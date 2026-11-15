// sq_art.cpp — artwork for the square UI (see sq_work.h).
//
// The ESP32-S3 has no JPEG codec, so covers are decoded in software. Two things
// keep that cheap:
//   - JPEGDEC decodes straight to 1/2, 1/4 or 1/8 scale, so a 640px Spotify
//     cover becomes ~160px without ever existing at full size;
//   - each image is decoded once, at the size it is shown, and kept in PSRAM.
// Progressive JPEGs (JPEGDEC cannot scale those) go through stb_image.
#include "sq_work.h"
#include "ui_common.h"
#include "ui_network_guard.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#undef INTELSHORT
#undef INTELLONG
#undef MOTOSHORT
#undef MOTOLONG
#include <JPEGDEC.h>
#undef INTELSHORT
#undef INTELLONG
#undef MOTOSHORT
#undef MOTOLONG
#include <PNGdec.h>

extern bool decodeJPEGProgressiveStb(const uint8_t* buf, size_t len, uint16_t** out, int* out_w,
                                     int* out_h);

volatile uint32_t sq_art_version = 0;

#define ART_SLOTS        96
#define ART_MAX_BYTES    (512 * 1024)
#define ART_QUEUE_LEN    24

struct ArtSlot {
    uint32_t       key;        // hash(url) ^ size
    int            size;
    bool           keep;       // also kept in flash (favourites)
    uint32_t       last_use;   // millis() of the last get
    uint32_t       retry_ms;   // FAILED only: when to try again, 0 = never
    enum { EMPTY, QUEUED, READY, FAILED } state;
    uint16_t*      pixels;     // PSRAM, size*size
    lv_image_dsc_t dsc;
};

struct ArtReq {
    uint32_t key;
    int      size;
    bool     keep;
    char     url[384];
};

static ArtSlot*          s_slots = nullptr;   // PSRAM
static SemaphoreHandle_t s_lock  = nullptr;
static QueueHandle_t     s_queue = nullptr;
static uint8_t*          s_dl    = nullptr;   // download buffer, PSRAM

static uint32_t artKey(const char* url, int size) {
    uint32_t h = 2166136261u;
    for (const char* p = url; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h ^ ((uint32_t)size * 2654435761u);
}

// Picks a slot for a new key: an empty one, else the least recently used
// READY/FAILED one. Caller holds the lock.
static ArtSlot* slotFor(uint32_t key) {
    ArtSlot* victim = nullptr;
    for (int i = 0; i < ART_SLOTS; ++i) {
        ArtSlot& s = s_slots[i];
        if (s.state == ArtSlot::EMPTY) return &s;
        if (s.state == ArtSlot::QUEUED) continue;
        if (!victim || s.last_use < victim->last_use) victim = &s;
    }
    if (victim && victim->pixels) { heap_caps_free(victim->pixels); victim->pixels = nullptr; }
    return victim;
}

// ── Flash copies ────────────────────────────────────────────────────────────
// The favourites' covers are kept in LittleFS, decoded (RGB565 at the size the
// tiles show them), so they are there at boot without a download or a decode.
// /art/<key>.565: a 4-byte header (size, 0x565) then size*size pixels.
//
// Flash is touched only by fsTask: reading or writing it turns the cache off,
// and the artwork task's stack is in PSRAM, which is unreachable meanwhile.
#define ART_DIR          "/art"
#define ART_MAX_STORED   128
#define ART_FS_MIN_FREE  (256 * 1024)

struct FsReq {
    enum { LOAD, SAVE, PRUNE } op;
    uint32_t  key;
    int       size;
    char*     url;      // LOAD: PSRAM copy, for the download if the file is gone
    uint16_t* px;       // SAVE: PSRAM copy of the pixels, freed by fsTask
    uint32_t* keys;     // PRUNE: the keys to keep (PSRAM), freed by fsTask
    int       n;
};

static QueueHandle_t s_fsq = nullptr;
static uint32_t      s_stored[ART_MAX_STORED];
static int           s_stored_n = 0;
static volatile bool s_fs_ready = false;   // the directory has been listed

static bool storedHas(uint32_t key) {
    for (int i = 0; i < s_stored_n; ++i) if (s_stored[i] == key) return true;
    return false;
}
static void storedAdd(uint32_t key) {
    if (!storedHas(key) && s_stored_n < ART_MAX_STORED) s_stored[s_stored_n++] = key;
}
static void storedDel(uint32_t key) {
    for (int i = 0; i < s_stored_n; ++i)
        if (s_stored[i] == key) { s_stored[i] = s_stored[--s_stored_n]; return; }
}

static void artPath(char* out, size_t n, uint32_t key) { snprintf(out, n, ART_DIR "/%08lx.565", (unsigned long)key); }

static const lv_image_dsc_t* artGet(const char* url, int size, bool keep);

uint32_t sqArtKey(const char* url, int size) { return artKey(url, size); }

bool sqArtStored(const char* url, int size) {
    if (!url || !*url || !s_lock) return false;
    const uint32_t key = artKey(url, size);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = storedHas(key);
    xSemaphoreGive(s_lock);
    return has;
}

const lv_image_dsc_t* sqArtGet(const char* url, int size) { return artGet(url, size, false); }
const lv_image_dsc_t* sqArtGetKept(const char* url, int size) { return artGet(url, size, true); }

void sqArtPrune(const uint32_t* keep, int n) {
    if (!s_fsq) return;
    FsReq r = {};
    r.op = FsReq::PRUNE;
    r.n = n;
    r.keys = (uint32_t*)heap_caps_malloc((n ? n : 1) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!r.keys) return;
    if (n) memcpy(r.keys, keep, n * sizeof(uint32_t));
    if (xQueueSend(s_fsq, &r, 0) != pdTRUE) heap_caps_free(r.keys);
}

static void slotFill(ArtSlot& s, uint16_t* px, int size) {
    s.pixels = px;
    s.dsc = {};
    s.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s.dsc.header.w = size;
    s.dsc.header.h = size;
    s.dsc.header.stride = size * 2;
    s.dsc.data = reinterpret_cast<const uint8_t*>(px);
    s.dsc.data_size = size * size * 2;
    s.state = ArtSlot::READY;
}

static const lv_image_dsc_t* artGet(const char* url, int size, bool keep) {
    if (!url || !*url || !s_slots) return nullptr;
    if (strncmp(url, "http", 4) != 0 && url[0] != '/') return nullptr;   // "unknown", "", ...
    // Until the flash copies are listed, a kept picture would be downloaded
    // again for nothing: the caller asks again when sq_art_version moves.
    if (keep && !s_fs_ready) return nullptr;
    uint32_t key = artKey(url, size);
    const lv_image_dsc_t* out = nullptr;
    bool enqueue = false;
    bool from_flash = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ArtSlot* hit = nullptr;
    for (int i = 0; i < ART_SLOTS; ++i)
        if (s_slots[i].state != ArtSlot::EMPTY && s_slots[i].key == key) { hit = &s_slots[i]; break; }
    if (hit) {
        hit->last_use = millis();
        if (hit->state == ArtSlot::READY) out = &hit->dsc;
        if (hit->state == ArtSlot::FAILED && hit->retry_ms && (int32_t)(millis() - hit->retry_ms) >= 0) {
            hit->state = ArtSlot::QUEUED;   // a network failure: try once more
            enqueue = true;
        }
    } else {
        ArtSlot* s = slotFor(key);
        if (s) {
            memset(s, 0, sizeof(*s));
            s->key = key;
            s->size = size;
            s->state = ArtSlot::QUEUED;
            s->keep = keep;
            s->last_use = millis();
            enqueue = true;
            from_flash = keep && storedHas(key);
        }
    }
    if (hit && keep) hit->keep = true;
    xSemaphoreGive(s_lock);
    if (enqueue && from_flash) {
        FsReq f = {};
        f.op = FsReq::LOAD;
        f.key = key;
        f.size = size;
        f.url = (char*)heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM);
        if (f.url) strcpy(f.url, url);
        if (f.url && xQueueSend(s_fsq, &f, 0) == pdTRUE) return out;
        if (f.url) heap_caps_free(f.url);
        // Queue full: fall through to the network.
    }
    if (enqueue) {
        ArtReq r = {};
        r.key = key;
        r.size = size;
        r.keep = keep;
        strlcpy(r.url, url, sizeof(r.url));
        if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < ART_SLOTS; ++i)
                if (s_slots[i].key == key && s_slots[i].state == ArtSlot::QUEUED) s_slots[i].state = ArtSlot::EMPTY;
            xSemaphoreGive(s_lock);
        }
    }
    return out;
}

// ── Download ────────────────────────────────────────────────────────────────
// A Stream that writes into a fixed buffer, for HTTPClient::writeToStream().
struct BufSink : public Stream {
    uint8_t* buf; size_t cap; size_t len = 0; bool overflow = false;
    BufSink(uint8_t* b, size_t c) : buf(b), cap(c) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (len + n > cap) { overflow = true; return 0; }
        memcpy(buf + len, d, n); len += n; return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
};
static int downloadOnce(const char* url, uint8_t* buf, int cap) {
    String full = url;
    if (full.startsWith("/")) {
        // Sonos-relative (/getaa?...): served by the speaker itself.
        SonosDevice* d = sonos.getCurrentDevice();
        if (!d) return -1;
        full = "http://" + d->ip.toString() + ":1400" + full;
    }
    HTTPClient http;
    WiFiClientSecure tls;
    WiFiClient plain;
    bool https = full.startsWith("https://");
    if (https) {
        tls.setInsecure();   // public cover art: integrity is not worth a CA bundle here
        if (!http.begin(tls, full)) return -1;
    } else {
        if (!http.begin(plain, full)) return -1;
    }
    http.setTimeout(8000);
    http.setConnectTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[SQ/ART] GET %d (%s) - internal %u/%u, psram %u/%u, wifi %d\n", code,
                      code < 0 ? http.errorToString(code).c_str() : "http",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (int)WiFi.status());
        http.end();
        return code < 0 ? code : -code;   // always negative: never a byte count
    }
    // writeToStream() undoes chunked transfer encoding, which reading the raw
    // socket did not (Radio France covers came back as a single byte).
    BufSink sink(buf, cap);
    int declared = http.getSize();
    int w = http.writeToStream(&sink);
    http.end();
    if (sink.len < 64)
        Serial.printf("[SQ/ART] Tiny body: http=%d declared=%d wrote=%d len=%u\n", code, declared, w,
                      (unsigned)sink.len);
    if (sink.overflow) return -1;
    return w < 0 ? w : (int)sink.len;
}

#if UI_SQUARE
// S3: the radio is on-chip; nothing to serialise.
static int download(const char* url, uint8_t* buf, int cap) { return downloadOnce(url, buf, cap); }
#else
// P4: the radio is an ESP32-C6 on an SDIO link, and overlapping transfers or a
// transfer started with too little DMA memory crash that link (see
// ui_network_guard.h and ui_album_art.cpp). Downloads take the same turns as
// every other request: SDIO cooldowns, the network mutex, a DMA floor.
static int download(const char* url, uint8_t* buf, int cap) {
    const bool https = !strncmp(url, "https://", 8);
    sdioPreWait("SQART", https ? SDIO_WAIT_HTTPS_COOLDOWN : 0);
    for (int i = 0; i < 50 && heap_caps_get_free_size(MALLOC_CAP_DMA) < ART_MIN_DMA_PRE_BURST; ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (heap_caps_get_free_size(MALLOC_CAP_DMA) < ART_MIN_DMA_PRE_BURST) {
        Serial.printf("[SQ/ART] DMA too low (%u) - skipped\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        return -2;
    }
    if (!network_mutex ||
        xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_ART_MS)) != pdTRUE)
        return -3;
    int n = downloadOnce(url, buf, cap);
    last_network_end_ms = millis();
    last_art_download_end_ms = millis();
    if (https) last_https_end_ms = millis();
    xSemaphoreGive(network_mutex);
    return n;
}
#endif

// ── Decode ──────────────────────────────────────────────────────────────────
struct JpegCtx { uint16_t* dst; int w, h; };

static int jpegDraw(JPEGDRAW* d) {
    JpegCtx* c = (JpegCtx*)d->pUser;
    for (int y = 0; y < d->iHeight; ++y) {
        int dy = d->y + y;
        if (dy >= c->h) break;
        int w = d->iWidthUsed;
        if (d->x + w > c->w) w = c->w - d->x;
        if (w <= 0) continue;
        memcpy(c->dst + dy * c->w + d->x, d->pPixels + y * d->iWidth, w * 2);
    }
    return 1;
}

// Centre-crop to a square and box/bilinear-scale to size x size.
static uint16_t* fitSquare(const uint16_t* src, int sw, int sh, int size) {
    uint16_t* out = (uint16_t*)heap_caps_malloc(size * size * 2, MALLOC_CAP_SPIRAM);
    if (!out) return nullptr;
    int side = sw < sh ? sw : sh;
    int ox = (sw - side) / 2, oy = (sh - side) / 2;
    for (int y = 0; y < size; ++y) {
        int y0 = oy + y * side / size, y1 = oy + (y + 1) * side / size;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < size; ++x) {
            int x0 = ox + x * side / size, x1 = ox + (x + 1) * side / size;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint16_t* row = src + yy * sw;
                for (int xx = x0; xx < x1; ++xx) {
                    uint16_t p = row[xx];
                    r += p >> 11; g += (p >> 5) & 0x3F; b += p & 0x1F; n++;
                }
            }
            out[y * size + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
    return out;
}

// PNG (radio logos): decoded at full size, then scaled like a JPEG.
struct PngCtx { PNG* png; uint16_t* dst; int w, h; uint16_t* line; };
static int pngDraw(PNGDRAW* d) {
    PngCtx* c = (PngCtx*)d->pUser;
    // Never let the decoder write into the image directly: a line it reports
    // wider than the header said, or a row past the bottom, overran the block
    // and corrupted the heap (crash a few seconds after opening Favoris).
    if (d->y < 0 || d->y >= c->h || d->iWidth <= 0 || d->iWidth > 1024) return 1;
    c->png->getLineAsRGB565(d, c->line, PNG_RGB565_LITTLE_ENDIAN, 0x00161339);
    memcpy(c->dst + d->y * c->w, c->line, (d->iWidth < c->w ? d->iWidth : c->w) * 2);
    return 1;
}

static uint16_t* decodePng(const uint8_t* buf, int len, int size) {
    PNG* png = (PNG*)heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
    if (!png) return nullptr;
    new (png) PNG();
    uint16_t* full = nullptr;
    int w = 0, h = 0;
    // Byte 28 of the IHDR is the interlace method; PNGdec has no Adam7 support.
    const bool interlaced = len > 28 && buf[28] != 0;
    uint16_t* line = (uint16_t*)heap_caps_malloc(1024 * 2, MALLOC_CAP_SPIRAM);
    if (!interlaced && line && png->openRAM((uint8_t*)buf, len, pngDraw) == PNG_SUCCESS) {
        w = png->getWidth(); h = png->getHeight();
        int ch = 1;
        switch (png->getPixelType()) {
            case PNG_PIXEL_TRUECOLOR:       ch = 3; break;
            case PNG_PIXEL_GRAY_ALPHA:      ch = 2; break;
            case PNG_PIXEL_TRUECOLOR_ALPHA: ch = 4; break;
            default:                        ch = 1; break;
        }
        const int pitch = (w * ch * png->getBpp() + 7) / 8;
        const bool fits = 2 * (pitch + 1) <= PNG_MAX_BUFFERED_PIXELS;   // see platformio.ini
        if (!fits) Serial.printf("[SQ/ART] PNG %dx%d too wide for the decoder - skipped\n", w, h);
        if (fits && w > 0 && h > 0 && w <= 1024 && h <= 1024)
            full = (uint16_t*)heap_caps_calloc(w * h, 2, MALLOC_CAP_SPIRAM);
        if (full) {
            PngCtx ctx = {png, full, w, h, line};
            if (png->decode(&ctx, 0) != PNG_SUCCESS) { heap_caps_free(full); full = nullptr; }
        }
        png->close();
    }
    if (line) heap_caps_free(line);
    if (interlaced) Serial.println("[SQ/ART] Interlaced PNG - skipped");
    png->~PNG();
    heap_caps_free(png);
    if (!full) return nullptr;
    uint16_t* out = fitSquare(full, w, h, size);
    heap_caps_free(full);
    return out;
}

static uint16_t* decode(const uint8_t* buf, int len, int size) {
    if (len >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        return decodePng(buf, len, size);
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        Serial.printf("[SQ/ART] Unknown image format (%d bytes) - skipped\n", len);
        return nullptr;
    }
    JPEGDEC* jpeg = (JPEGDEC*)heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
    if (!jpeg) return nullptr;
    new (jpeg) JPEGDEC();
    uint16_t* full = nullptr;
    int fw = 0, fh = 0;
    bool progressive = false;
    bool too_big = false;
    if (jpeg->openRAM((uint8_t*)buf, len, jpegDraw)) {
        progressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
        // stb decodes a progressive JPEG at full size and keeps every DCT
        // coefficient too: ~9 bytes a pixel, 18MB for a 1400px cover. Its
        // attempts drained the PSRAM the Wi-Fi and lwIP buffers live in, and
        // every connection failed for a while after. Small ones only.
        if (progressive && (jpeg->getWidth() > 640 || jpeg->getHeight() > 640)) {
            too_big = true;
            progressive = false;
            Serial.printf("[SQ/ART] Progressive JPEG %dx%d - too big to decode, skipped\n",
                          jpeg->getWidth(), jpeg->getHeight());
        }
        if (!progressive && !too_big) {
            int w = jpeg->getWidth(), h = jpeg->getHeight();
            int side = w < h ? w : h;
            int opt = 0, div = 1;
            // Largest reduction that still leaves at least `size` pixels.
            if (side / 8 >= size) { opt = JPEG_SCALE_EIGHTH; div = 8; }
            else if (side / 4 >= size) { opt = JPEG_SCALE_QUARTER; div = 4; }
            else if (side / 2 >= size) { opt = JPEG_SCALE_HALF; div = 2; }
            fw = (w + div - 1) / div;
            fh = (h + div - 1) / div;
            full = (uint16_t*)heap_caps_calloc(fw * fh, 2, MALLOC_CAP_SPIRAM);
            if (full) {
                JpegCtx ctx = {full, fw, fh};
                jpeg->setUserPointer(&ctx);
                jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
                if (!jpeg->decode(0, 0, opt)) { heap_caps_free(full); full = nullptr; }
            }
        }
        jpeg->close();
    }
    jpeg->~JPEGDEC();
    heap_caps_free(jpeg);
    if (progressive && !decodeJPEGProgressiveStb(buf, len, &full, &fw, &fh)) full = nullptr;
    if (!full) return nullptr;
    uint16_t* out = fitSquare(full, fw, fh, size);
    heap_caps_free(full);
    return out;
}

// Spotify serves each cover in several sizes behind the same id, the size
// being part of the id's prefix. Its API hands out the 640px one, which for a
// 48px row is a 150KB download and a full decode per thumbnail: a search
// result page meant dozens of them. Ask for the smallest size that covers
// the box instead.
static void spotifySized(char* url, size_t cap, int size) {
    struct Sz { const char* p640; const char* p300; const char* p64; };
    static const Sz kinds[] = {
        {"ab67616d0000b273", "ab67616d00001e02", "ab67616d00004851"},   // album covers
        {"ab6761610000e5eb", "ab67616100005174", "ab6761610000f178"},   // artist pictures (320/160)
    };
    char* id = strstr(url, "i.scdn.co/image/");
    if (!id) return;
    id += strlen("i.scdn.co/image/");
    for (const Sz& k : kinds) {
        if (strncmp(id, k.p640, 16) != 0) continue;
        memcpy(id, size <= 64 ? k.p64 : k.p300, 16);
        return;
    }
    (void)cap;
}

static void artTask(void*) {
    ArtReq r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE) continue;
        char original[sizeof(r.url)];
        strlcpy(original, r.url, sizeof(original));
        spotifySized(r.url, sizeof(r.url), r.size);
        if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); }
        uint32_t t0 = millis();
        int n = download(r.url, s_dl, ART_MAX_BYTES);
        if (n <= 0 && strcmp(original, r.url) != 0) {   // that size does not exist: the 640px one
            strlcpy(r.url, original, sizeof(r.url));
            n = download(r.url, s_dl, ART_MAX_BYTES);
        }
        uint16_t* px = n > 0 ? decode(s_dl, n, r.size) : nullptr;
        if (!px) Serial.printf("[SQ/ART] Failed (%d) %.80s\n", n, r.url);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < ART_SLOTS; ++i) {
            ArtSlot& s = s_slots[i];
            if (s.key != r.key || s.state != ArtSlot::QUEUED) continue;
            if (px) {
                if (r.keep || s.keep) {
                    // A copy for fsTask: the slot's own pixels may be evicted
                    // before the write happens.
                    FsReq f = {};
                    f.op = FsReq::SAVE;
                    f.key = r.key;
                    f.size = r.size;
                    f.px = (uint16_t*)heap_caps_malloc(r.size * r.size * 2, MALLOC_CAP_SPIRAM);
                    if (f.px) {
                        memcpy(f.px, px, r.size * r.size * 2);
                        if (xQueueSend(s_fsq, &f, 0) != pdTRUE) heap_caps_free(f.px);
                    }
                }
                slotFill(s, px, r.size);
                px = nullptr;
            } else {
                s.state = ArtSlot::FAILED;
                // Network trouble (timeouts, refused, busy radio) is worth another
                // go a minute later; an HTTP error or an undecodable picture is not.
                s.retry_ms = (n < 0 && n > -100) ? millis() + 60000 : 0;
            }
            break;
        }
        xSemaphoreGive(s_lock);
        if (px) heap_caps_free(px);   // slot was recycled meanwhile
        sq_art_version++;
        if (n > 0) Serial.printf("[SQ/ART] %d B -> %dpx in %lums (stack low %u)\n", n, r.size,
                                 millis() - t0, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

// ── Flash task ──────────────────────────────────────────────────────────────
static void fsList(void) {
    if (!LittleFS.exists(ART_DIR)) LittleFS.mkdir(ART_DIR);
    File dir = LittleFS.open(ART_DIR);
    int n = 0;
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            const char* name = f.name();
            const char* base = strrchr(name, '/');
            base = base ? base + 1 : name;
            uint32_t key = strtoul(base, nullptr, 16);
            f.close();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            storedAdd(key);
            xSemaphoreGive(s_lock);
            ++n;
        }
        dir.close();
    }
    Serial.printf("[SQ/FS] %d cover(s) in flash\n", n);
}

static void fsLoad(FsReq& r) {
    char path[32];
    artPath(path, sizeof(path), r.key);
    uint16_t* px = nullptr;
    File f = LittleFS.open(path, "r");
    if (f) {
        uint16_t hdr[2] = {0, 0};
        const size_t bytes = (size_t)r.size * r.size * 2;
        if (f.read(reinterpret_cast<uint8_t*>(hdr), 4) == 4 && hdr[0] == r.size && hdr[1] == 0x565 && (size_t)f.size() == bytes + 4) {
            px = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            if (px && f.read(reinterpret_cast<uint8_t*>(px), bytes) != bytes) { heap_caps_free(px); px = nullptr; }
        }
        f.close();
    }
    const bool read_ok = px != nullptr;
    bool done = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!px) storedDel(r.key);
    for (int i = 0; i < ART_SLOTS && px; ++i) {
        ArtSlot& s = s_slots[i];
        if (s.key != r.key || s.state != ArtSlot::QUEUED) continue;
        slotFill(s, px, r.size);
        px = nullptr;
        done = true;
        break;
    }
    xSemaphoreGive(s_lock);
    if (px) heap_caps_free(px);   // the slot went away meanwhile
    if (done) { sq_art_version++; return; }
    if (read_ok || !r.url) return;   // the file is fine, nobody wants it any more
    // Missing or damaged file: download it like any other picture.
    Serial.printf("[SQ/FS] %s unusable - downloading again\n", path);
    LittleFS.remove(path);
    ArtReq q = {};
    q.key = r.key;
    q.size = r.size;
    q.keep = true;
    strlcpy(q.url, r.url, sizeof(q.url));
    if (xQueueSend(s_queue, &q, pdMS_TO_TICKS(500)) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < ART_SLOTS; ++i)
            if (s_slots[i].key == r.key && s_slots[i].state == ArtSlot::QUEUED) s_slots[i].state = ArtSlot::EMPTY;
        xSemaphoreGive(s_lock);
    }
}

static void fsSave(FsReq& r) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = storedHas(r.key);
    const bool full = s_stored_n >= ART_MAX_STORED;
    xSemaphoreGive(s_lock);
    if (have || full) return;
    const size_t bytes = (size_t)r.size * r.size * 2;
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < bytes + ART_FS_MIN_FREE) {
        Serial.println("[SQ/FS] flash nearly full - cover not kept");
        return;
    }
    char path[32];
    artPath(path, sizeof(path), r.key);
    const uint32_t t0 = millis();
    File f = LittleFS.open(path, "w");
    if (!f) return;
    const uint16_t hdr[2] = {(uint16_t)r.size, 0x565};
    bool ok = f.write(reinterpret_cast<const uint8_t*>(hdr), 4) == 4 &&
              f.write(reinterpret_cast<const uint8_t*>(r.px), bytes) == bytes;
    f.close();
    if (!ok) { LittleFS.remove(path); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    storedAdd(r.key);
    xSemaphoreGive(s_lock);
    sq_art_version++;
    Serial.printf("[SQ/FS] kept cover %08lx (%u B) in %lums\n", (unsigned long)r.key, (unsigned)bytes,
                  (unsigned long)(millis() - t0));
}

static void fsPrune(FsReq& r) {
    uint32_t gone[ART_MAX_STORED];
    int g = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_stored_n; ++i) {
        bool keep = false;
        for (int k = 0; k < r.n && !keep; ++k) keep = r.keys[k] == s_stored[i];
        if (!keep) gone[g++] = s_stored[i];
    }
    for (int i = 0; i < g; ++i) storedDel(gone[i]);
    xSemaphoreGive(s_lock);
    for (int i = 0; i < g; ++i) {
        char path[32];
        artPath(path, sizeof(path), gone[i]);
        LittleFS.remove(path);
    }
    if (g) Serial.printf("[SQ/FS] removed %d cover(s) no longer in the favourites\n", g);
}

static void fsTask(void*) {
    fsList();
    s_fs_ready = true;
    sq_art_version++;
    FsReq r;
    for (;;) {
        if (xQueueReceive(s_fsq, &r, portMAX_DELAY) != pdTRUE) continue;
        switch (r.op) {
            case FsReq::LOAD:  fsLoad(r); break;
            case FsReq::SAVE:  fsSave(r); break;
            case FsReq::PRUNE: fsPrune(r); break;
        }
        if (r.url)  heap_caps_free(r.url);
        if (r.px)   heap_caps_free(r.px);
        if (r.keys) heap_caps_free(r.keys);
    }
}

bool sqArtFlashReady(void) { return s_fs_ready; }

void sqArtStart(void) {
    s_slots = (ArtSlot*)heap_caps_calloc(ART_SLOTS, sizeof(ArtSlot), MALLOC_CAP_SPIRAM);
    s_dl    = (uint8_t*)heap_caps_malloc(ART_MAX_BYTES, MALLOC_CAP_SPIRAM);
    s_lock  = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(ART_QUEUE_LEN, sizeof(ArtReq));
    s_fsq   = xQueueCreate(32, sizeof(FsReq));
    // Internal-RAM stack: this task reads and writes flash (see above).
    xTaskCreatePinnedToCore(fsTask, "SqArtFs", 6144, nullptr, 1, nullptr, 0);
    // PSRAM stack: this task never writes flash, and mbedTLS allocates on the
    // heap (in PSRAM too, see main_square.cpp), so internal RAM is not needed.
    // Measured high-water mark: ~3KB used of 24KB with an https download.
    static StaticTask_t tcb;
    const size_t stack_words = 12288;
    StackType_t* stack = (StackType_t*)heap_caps_malloc(stack_words * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(artTask, "SqArt", stack_words, nullptr, 1, stack, &tcb, 0);
}

// The Sonos polling task asks this before each poll. On the P4 an artwork
// download competes with SOAP for the SDIO link to the Wi-Fi chip; the S3's
// radio is on-chip, so the square build never needs to hold polling back.
bool isAlbumArtPending() { return false; }
