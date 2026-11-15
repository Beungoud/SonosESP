// sq_ota.cpp — Mise à jour: firmware updates over Wi-Fi for the kids UI builds
// (square S3 and P4 portrait), from the GitHub releases of GITHUB_REPO.
//
// Checking runs on the worker (SQ_JOB_OTA_CHECK): once a couple of minutes
// after Wi-Fi comes up, then twice a day, and on demand from the screen.
// Nothing is announced to the child: the Réglages card and this screen say a
// version is waiting, and a grown-up installs it.
//
// Installing does not happen under the running UI. The URL is saved to NVS and
// the panel restarts; setup() then downloads and flashes it before anything
// else starts (no Sonos tasks, no artwork, no TLS session but this one), which
// is the same choice the P4 firmware made for the same reasons: all the RAM,
// and nothing else on the network. The flag is cleared before the download
// starts, so a failure boots the old firmware instead of looping.
#include "sq_ui.h"
#include "sq_work.h"
#include "ui_common.h"
#include "config.h"
#include "display_driver.h"
#include "ui_network_guard.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_heap_caps.h>

// The release asset for this build. Exact names: see the release workflows.
#if UI_SQUARE
#define SQ_OTA_ASSET "firmware-square.bin"
#elif !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define SQ_OTA_ASSET "firmware-4inch-r3-kids.bin"
#else
#define SQ_OTA_ASSET "firmware-4inch-kids.bin"
#endif

volatile uint32_t sq_ota_version = 0;

static volatile SqOtaState s_state = SQ_OTA_IDLE;
static char s_latest[40] = "";
static char s_url[384]   = "";

SqOtaState  sqOtaState(void) { return s_state; }
const char* sqOtaLatest(void) { return s_latest; }

static void setState(SqOtaState st) {
    s_state = st;
    sq_ota_version++;
}

// "2.4.0" > "2.3.1". The numeric part only: a "-nightly.abc" suffix is ignored.
static bool semverNewer(const char* cand, const char* cur) {
    auto parse = [](const char* v, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        int part = 0, val = 0;
        for (; *v && part < 3; ++v) {
            if (*v >= '0' && *v <= '9') val = val * 10 + (*v - '0');
            else if (*v == '.') { out[part++] = val; val = 0; }
            else break;
        }
        if (part < 3) out[part] = val;
    };
    int a[3], b[3];
    parse(cand, a);
    parse(cur, b);
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// A Stream that fills a fixed buffer, for HTTPClient::writeToStream().
struct SqBufSink : public Stream {
    uint8_t* buf; size_t cap; size_t len = 0; bool overflow = false;
    SqBufSink(uint8_t* b, size_t c) : buf(b), cap(c) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (len + n > cap) { overflow = true; return 0; }
        memcpy(buf + len, d, n); len += n; return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
};

// ── Check (worker task) ─────────────────────────────────────────────────────
void sqOtaCheckNow(void) {
    if (WiFi.status() != WL_CONNECTED) { setState(SQ_OTA_FAILED); return; }
    setState(SQ_OTA_CHECKING);
    const bool nightly = ota_channel == 1;
    String url = String("https://api.github.com/repos/") + GITHUB_REPO +
                 (nightly ? "/releases?per_page=5" : "/releases/latest");

    // Only what is used: a release's notes alone can be tens of KB.
    JsonDocument filter;
    JsonObject f = nightly ? filter[0].to<JsonObject>() : filter.to<JsonObject>();
    f["tag_name"] = true;
    f["published_at"] = true;
    f["assets"][0]["name"] = true;
    f["assets"][0]["browser_download_url"] = true;

    JsonDocument doc;
    DeserializationError err = DeserializationError::EmptyInput;
    int code = -1;
#if !UI_SQUARE
    // P4: the Wi-Fi chip sits behind SDIO and takes one transfer at a time.
    sdioPreWait("SQOTA", SDIO_WAIT_HTTPS_COOLDOWN);
    if (!network_mutex || xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        setState(SQ_OTA_FAILED);
        return;
    }
#endif
    // The whole body first, then the parse: reading the JSON straight off the
    // TLS socket stopped part way (IncompleteInput) on GitHub's answer.
    // writeToStream() also undoes chunked encoding.
    uint8_t* body = (uint8_t*)heap_caps_malloc(OTA_MAX_JSON_BYTES, MALLOC_CAP_SPIRAM);
    size_t body_len = 0;
    if (body) {
        WiFiClientSecure tls;
        tls.setInsecure();   // the firmware itself is checked by Update (image header, SHA)
        HTTPClient http;
        if (http.begin(tls, url)) {
            http.addHeader("Accept", "application/vnd.github+json");
            http.addHeader("User-Agent", "SonosESP/" FIRMWARE_VERSION);
            http.setTimeout(15000);
            code = http.GET();
            if (code == 200) {
                SqBufSink sink(body, OTA_MAX_JSON_BYTES);
                const int declared = http.getSize();
                const int w = http.writeToStream(&sink);
                body_len = sink.overflow ? 0 : sink.len;
                Serial.printf("[SQ/OTA] body %u B (declared %d, wrote %d%s)\n", (unsigned)sink.len, declared, w,
                              sink.overflow ? ", too big" : "");
            }
            http.end();
        }
    }
#if !UI_SQUARE
    last_network_end_ms = millis();
    last_https_end_ms = millis();
    xSemaphoreGive(network_mutex);
#endif
    if (body_len) err = deserializeJson(doc, (const char*)body, body_len, DeserializationOption::Filter(filter));
    if (body) heap_caps_free(body);
    if (code == 404 && !nightly) {
        // /releases/latest answers 404 when the repo has no release at all.
        Serial.printf("[SQ/OTA] no release in %s yet\n", GITHUB_REPO);
        s_latest[0] = 0;
        setState(SQ_OTA_UP_TO_DATE);
        return;
    }
    if (code != 200 || err) {
        Serial.printf("[SQ/OTA] check failed: http %d, json %s\n", code, err.c_str());
        setState(SQ_OTA_FAILED);
        return;
    }

    JsonVariant rel;
    if (nightly) {
        const char* best = nullptr;
        for (JsonVariant r : doc.as<JsonArray>()) {
            const char* tag = r["tag_name"] | "";
            const char* pub = r["published_at"] | "";
            if (!strstr(tag, "-nightly")) continue;
            if (!best || strcmp(pub, best) > 0) { best = pub; rel = r; }
        }
    } else {
        rel = doc.as<JsonVariant>();
        if (strstr(rel["tag_name"] | "", "-nightly")) rel = JsonVariant();
    }
    if (rel.isNull()) {
        Serial.printf("[SQ/OTA] no %s release in %s\n", nightly ? "nightly" : "stable", GITHUB_REPO);
        s_latest[0] = 0;
        setState(SQ_OTA_UP_TO_DATE);
        return;
    }
    const char* tag = rel["tag_name"] | "";
    strlcpy(s_latest, tag[0] == 'v' ? tag + 1 : tag, sizeof(s_latest));
    s_url[0] = 0;
    for (JsonVariant a : rel["assets"].as<JsonArray>())
        if (!strcmp(a["name"] | "", SQ_OTA_ASSET)) strlcpy(s_url, a["browser_download_url"] | "", sizeof(s_url));

    const bool newer = nightly ? strcmp(s_latest, FIRMWARE_VERSION) != 0 : semverNewer(s_latest, FIRMWARE_VERSION);
    Serial.printf("[SQ/OTA] latest %s (installed %s), asset %s: %s\n", s_latest, FIRMWARE_VERSION, SQ_OTA_ASSET,
                  s_url[0] ? "yes" : "missing");
    setState(!newer ? SQ_OTA_UP_TO_DATE : s_url[0] ? SQ_OTA_AVAILABLE : SQ_OTA_NO_ASSET);
}

// ── Install: save, restart, flash at boot ──────────────────────────────────
static void installClicked(lv_event_t*) {
    if (s_state != SQ_OTA_AVAILABLE || !s_url[0]) return;
    wifiPrefs.putString(NVS_KEY_OTA_URL, s_url);
    wifiPrefs.putBool(NVS_KEY_OTA_PENDING, true);
    Serial.printf("[SQ/OTA] install %s at next boot\n", s_latest);
    sqToast("Je redémarre pour la mise à jour…");
    lv_refr_now(nullptr);
    delay(800);
    ESP.restart();
}

bool sqOtaBootPending(void) { return wifiPrefs.getBool(NVS_KEY_OTA_PENDING, false); }

static lv_obj_t* s_boot_status;
static lv_obj_t* s_boot_bar;

static void bootPump(void) {
    static uint32_t last = 0;
    if (millis() - last < 80) return;
    last = millis();
    lv_timer_handler();
}

static void bootSay(const char* text) {
    lv_label_set_text(s_boot_status, text);
    lv_refr_now(nullptr);
}

// One pass of the download, resuming at `written` with a Range request. The
// URL is always GitHub's own: its redirect target is a signed URL that expires.
// Returns true when the whole image is in.
static bool downloadPass(const char* url, size_t* written, size_t* total, uint8_t* buf, size_t cap) {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(tls, url)) return false;
    http.addHeader("User-Agent", "SonosESP/" FIRMWARE_VERSION);
    http.setTimeout(15000);
    if (*written) http.addHeader("Range", String("bytes=") + *written + "-");
    const int code = http.GET();
    if (*written && code == 200) {
        // The server ignored the range: start over.
        Serial.println("[SQ/OTA] no resume - starting again");
        Update.abort();
        *written = 0;
    }
    if (code != 200 && code != 206) {
        Serial.printf("[SQ/OTA] GET %d\n", code);
        http.end();
        return false;
    }
    if (*written == 0) {
        const int len = http.getSize();
        if (len <= 0) { http.end(); return false; }
        *total = (size_t)len;
        if (!Update.begin(*total)) {
            Serial.printf("[SQ/OTA] Update.begin: %s\n", Update.errorString());
            http.end();
            return false;
        }
    }
    WiFiClient* s = http.getStreamPtr();
    uint32_t last_data = millis();
    while (*written < *total) {
        const size_t avail = s->available();
        if (avail) {
            const size_t want = avail < cap ? avail : cap;
            const int n = s->readBytes(buf, want);
            if (n <= 0) break;
            if (Update.write(buf, n) != (size_t)n) {
                Serial.printf("[SQ/OTA] write: %s\n", Update.errorString());
                http.end();
                return false;
            }
            *written += n;
            last_data = millis();
            lv_bar_set_value(s_boot_bar, (int)((uint64_t)*written * 100 / *total), LV_ANIM_OFF);
            bootPump();
        } else {
            if (!s->connected() || millis() - last_data > 15000) break;
            bootPump();
            delay(2);
        }
    }
    http.end();
    return *written >= *total;
}

void sqOtaRunAtBoot(void) {
    String url = wifiPrefs.getString(NVS_KEY_OTA_URL, "");
    wifiPrefs.putBool(NVS_KEY_OTA_PENDING, false);   // one go only: a failure boots the old firmware
    wifiPrefs.remove(NVS_KEY_OTA_URL);

    lv_obj_t* scr = sqScreenCreate();
    lv_obj_t* col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, SQ_W - 56, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 18, 0);
    const int disc = SQ_TALL ? 150 : 110;
    lv_obj_t* d = sqBox(col, disc, disc, SQ_ORANGE, disc / 2);
    lv_obj_t* ic = sqIcon(d, IC_DOWNLOAD, &font_sqi_44, SQ_INK);
    lv_obj_center(ic);
    sqLabel(col, "Mise à jour", &font_sq_34, SQ_TEXT);
    s_boot_status = sqLabel(col, "", &font_sq_18, SQ_MUTED);
    lv_obj_set_width(s_boot_status, LV_PCT(100));
    lv_label_set_long_mode(s_boot_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    s_boot_bar = lv_bar_create(col);
    lv_obj_set_size(s_boot_bar, LV_PCT(100), 16);
    lv_bar_set_range(s_boot_bar, 0, 100);
    lv_obj_set_style_radius(s_boot_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_boot_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(SQ_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(SQ_ORANGE), LV_PART_INDICATOR);
    lv_obj_t* warn = sqLabel(col, "Ne débranche pas le panneau.", &font_sq_16, SQ_MUTED);
    (void)warn;
    lv_screen_load(scr);
    display_set_brightness(brightness_level > 30 ? brightness_level : 30);
    bootSay("Connexion au Wi-Fi…");

    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { bootPump(); delay(10); }

    bool ok = false;
    if (!url.length()) {
        bootSay("Rien à installer.");
    } else if (WiFi.status() != WL_CONNECTED) {
        bootSay("Pas de Wi-Fi : la mise à jour attendra.");
    } else {
        bootSay("Je télécharge la nouvelle version…");
        Serial.printf("[SQ/OTA] downloading %s\n", url.c_str());
        const size_t cap = 4096;
        uint8_t* buf = (uint8_t*)malloc(cap);
        size_t written = 0, total = 0;
        for (int attempt = 0; buf && attempt < 5 && !ok; ++attempt) {
            if (attempt) { Serial.printf("[SQ/OTA] retry %d at %u/%u\n", attempt, (unsigned)written, (unsigned)total); delay(1500); }
            ok = downloadPass(url.c_str(), &written, &total, buf, cap);
        }
        free(buf);
        if (ok) {
            bootSay("J'installe…");
            ok = Update.end(true) && Update.isFinished();
            if (!ok) Serial.printf("[SQ/OTA] end: %s\n", Update.errorString());
        } else if (written) {
            Update.abort();
        }
        bootSay(ok ? "C'est fini ! Je redémarre." : "La mise à jour n'a pas marché. Je garde l'ancienne version.");
    }
    Serial.printf("[SQ/OTA] %s\n", ok ? "done" : "failed");
    const uint32_t t1 = millis();
    while (millis() - t1 < (ok ? 1500u : 4000u)) { bootPump(); delay(10); }
    ESP.restart();
}

// ── Mise à jour screen ──────────────────────────────────────────────────────
static lv_obj_t* s_scr;
static lv_obj_t* s_status;
static lv_obj_t* s_check_btn;
static lv_obj_t* s_install_btn;
static lv_obj_t* s_install_lbl;
static lv_obj_t* s_chan[2];
static uint32_t  s_seen = 0xFFFFFFFF;

static void checkClicked(lv_event_t*) {
    if (s_state == SQ_OTA_CHECKING) return;
    if (!sqWorkPost(SQ_JOB_OTA_CHECK)) return;
    setState(SQ_OTA_CHECKING);
}

static void channelClicked(lv_event_t* e) {
    const int ch = (int)(intptr_t)lv_event_get_user_data(e);
    if (ch == ota_channel) return;
    ota_channel = ch;
    wifiPrefs.putInt(NVS_KEY_OTA_CHANNEL, ota_channel);
    s_latest[0] = 0;
    s_url[0] = 0;
    setState(SQ_OTA_IDLE);
    checkClicked(nullptr);
}

void sqOtaCreate(void) {
    s_scr = sqScreenCreate();
    sqHeader(s_scr, "Mise à jour", SQ_ORANGE, true);
    lv_obj_t* list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, SQ_HEADER_H);
    lv_obj_set_size(list, SQ_W, SQ_H - SQ_HEADER_H);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_bottom(list, 16, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* card = sqBox(list, LV_PCT(100), LV_SIZE_CONTENT, SQ_SURFACE, 20);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    sqLabel(card, "Version installée : " FIRMWARE_VERSION, &font_sq_18, SQ_TEXT);
    s_status = sqLabel(card, "", &font_sq_16, SQ_MUTED);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_scrollable(row, false);
    s_check_btn = sqPillButton(row, 56, SQ_RAISED, SQ_ORANGE, IC_REFRESH, "Chercher", &font_sq_18);
    lv_obj_add_event_cb(s_check_btn, checkClicked, LV_EVENT_CLICKED, nullptr);
    s_install_btn = sqPillButton(row, 56, SQ_ORANGE, SQ_INK, IC_DOWNLOAD, "Installer", &font_sq_18);
    lv_obj_set_flex_grow(s_install_btn, 1);
    s_install_lbl = lv_obj_get_child(s_install_btn, 1);
    lv_obj_add_event_cb(s_install_btn, installClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* cc = sqBox(list, LV_PCT(100), LV_SIZE_CONTENT, SQ_SURFACE, 20);
    lv_obj_set_style_pad_all(cc, 12, 0);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cc, 10, 0);
    sqLabel(cc, "Versions", &font_sq_18, SQ_TEXT);
    lv_obj_t* chips = lv_obj_create(cc);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 8, 0);
    lv_obj_set_scrollable(chips, false);
    static const char* kChan[2] = {"Stables", "D'essai"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* b = lv_button_create(chips);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 46);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 23, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        sqPressFeedback(b);
        lv_obj_t* l = sqLabel(b, kChan[i], &font_sq_16, SQ_TEXT);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, channelClicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_chan[i] = b;
    }

    lv_obj_t* note = sqLabel(list, "Pendant l'installation, le panneau redémarre deux fois. Ne le débranche pas.\n"
                                   "Source : github.com/" GITHUB_REPO,
                             &font_sq_14, SQ_MUTED);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);

    sqRegisterScreen(SQ_SCR_UPDATE, s_scr, SQ_TAB_NONE);
}

// The line under "Mise à jour" in Réglages, and the status on this screen.
void sqOtaSummary(char* out, size_t n) {
    switch (s_state) {
        case SQ_OTA_IDLE:       snprintf(out, n, "Version " FIRMWARE_VERSION); break;
        case SQ_OTA_CHECKING:   snprintf(out, n, "Je regarde s'il y a du nouveau…"); break;
        case SQ_OTA_UP_TO_DATE: snprintf(out, n, "Le panneau est à jour (" FIRMWARE_VERSION ")"); break;
        case SQ_OTA_AVAILABLE:  snprintf(out, n, "Nouvelle version %s prête à installer", s_latest); break;
        case SQ_OTA_NO_ASSET:   snprintf(out, n, "Version %s sortie, mais pas encore pour ce panneau", s_latest); break;
        case SQ_OTA_FAILED:     snprintf(out, n, "Impossible de joindre GitHub"); break;
    }
}

void sqOtaRefresh(void) {
    if (s_seen == sq_ota_version) return;
    s_seen = sq_ota_version;
    char buf[96];
    sqOtaSummary(buf, sizeof(buf));
    lv_label_set_text(s_status, buf);
    sqSetHidden(s_install_btn, s_state != SQ_OTA_AVAILABLE);
    if (s_state == SQ_OTA_AVAILABLE) lv_label_set_text_fmt(s_install_lbl, "Installer %s", s_latest);
    if (s_state == SQ_OTA_CHECKING) lv_obj_add_state(s_check_btn, LV_STATE_DISABLED);
    else                            lv_obj_remove_state(s_check_btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s_check_btn, s_state == SQ_OTA_CHECKING ? LV_OPA_50 : LV_OPA_COVER, 0);
    for (int i = 0; i < 2; ++i) {
        const bool on = i == ota_channel;
        lv_obj_set_style_bg_color(s_chan[i], lv_color_hex(on ? SQ_ORANGE : SQ_RAISED), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_chan[i], 0), lv_color_hex(on ? SQ_INK : SQ_TEXT), 0);
    }
}

void sqOtaOnShow(void) {
    s_seen = 0xFFFFFFFF;
    sqOtaRefresh();
    if (s_state == SQ_OTA_IDLE || s_state == SQ_OTA_FAILED) checkClicked(nullptr);
}
