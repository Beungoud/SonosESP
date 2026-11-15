// sq_setup.cpp — Premier démarrage: what the panel does the first time it is
// on the network, shown instead of a player with nothing in it.
//
// It finds the speakers, the Spotify account(s) and the favourites, then keeps
// the favourites' covers in flash. All of that happens anyway at every boot;
// the first time it is slow (no flash copy of anything), so it gets a screen
// that says so. Done once: an NVS flag, also set when a favourites copy is
// already in flash (a panel updated from a version without this screen).
#include "sq_ui.h"
#include "sq_work.h"
#include "ui_common.h"

#define NVS_KEY_SQ_SETUP   "sq_setup_done"
#define SETUP_SPK_GIVEUP   45000    // no speaker by then: say so, go on
#define SETUP_SVC_GIVEUP   60000
#define SETUP_ART_STALL    30000    // no new cover for that long: the rest failed
#define SETUP_TIMEOUT      150000   // never keep a child waiting longer than this

enum RowState { ROW_WAIT, ROW_BUSY, ROW_DONE, ROW_SKIP };

struct Row {
    lv_obj_t* box;
    lv_obj_t* dot;        // mint disc with a check, or the spinner
    lv_obj_t* check;
    lv_obj_t* spin;
    lv_obj_t* value;
    RowState  state;
};

static lv_obj_t* s_scr;
static lv_obj_t* s_title;
static lv_obj_t* s_sub;
static lv_obj_t* s_bar;
static Row       s_rows[4];
static uint32_t  s_started = 0;
static uint32_t  s_finish_at = 0;
static int       s_art_seen = -1;
static uint32_t  s_art_moved = 0;

bool sqSetupNeeded(void) { return !wifiPrefs.getBool(NVS_KEY_SQ_SETUP, false); }
void sqSetupMarkDone(void) { wifiPrefs.putBool(NVS_KEY_SQ_SETUP, true); }

static void rowSet(Row& r, RowState st, const char* value) {
    if (value) lv_label_set_text(r.value, value);
    if (r.state == st) return;
    r.state = st;
    const bool done = st == ROW_DONE;
    const bool busy = st == ROW_BUSY;
    sqSetHidden(r.check, !done);
    sqSetHidden(r.spin, !busy);
    lv_obj_set_style_bg_opa(r.dot, done || st == ROW_SKIP ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(r.dot, lv_color_hex(done ? SQ_MINT : SQ_RAISED), 0);
    lv_obj_set_style_border_width(r.box, busy ? 3 : 0, 0);
    lv_obj_set_style_text_color(r.value, lv_color_hex(busy ? 0xFFE58A : SQ_MUTED), 0);
}

static void rowCreate(lv_obj_t* list, Row& r, const char* title) {
    const int h = SQ_TALL ? 62 : 52, d = SQ_TALL ? 40 : 36;
    r.box = sqBox(list, LV_PCT(100), h, SQ_ROW, SQ_TALL ? 20 : 18);
    lv_obj_set_style_border_color(r.box, lv_color_hex(SQ_YELLOW), 0);
    lv_obj_set_style_pad_left(r.box, SQ_TALL ? 10 : 8, 0);
    lv_obj_set_style_pad_right(r.box, SQ_TALL ? 16 : 14, 0);
    lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r.box, 12, 0);

    r.dot = sqBox(r.box, d, d, SQ_MINT, d / 2);
    r.check = sqIcon(r.dot, IC_CHECK, &font_sqi_20, SQ_INK);
    lv_obj_center(r.check);
    r.spin = lv_spinner_create(r.dot);
    lv_spinner_set_anim_params(r.spin, 1000, 90);
    lv_obj_set_size(r.spin, d, d);
    lv_obj_center(r.spin);
    lv_obj_set_style_arc_width(r.spin, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(r.spin, lv_color_hex(SQ_RAISED), LV_PART_MAIN);
    lv_obj_set_style_arc_width(r.spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r.spin, lv_color_hex(SQ_YELLOW), LV_PART_INDICATOR);

    lv_obj_t* t = sqLabel(r.box, title, SQ_TALL ? &font_sq_20 : &font_sq_18, SQ_TEXT);
    lv_obj_set_flex_grow(t, 1);
    r.value = sqLabel(r.box, "", SQ_TALL ? &font_sq_16 : &font_sq_14, SQ_MUTED);
    r.state = ROW_DONE;   // forces the first rowSet to style it
    rowSet(r, ROW_WAIT, "");
}

void sqSetupCreate(void) {
    s_scr = sqScreenCreate();
    lv_obj_t* col = lv_obj_create(s_scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, SQ_W, SQ_H);
    lv_obj_set_style_pad_hor(col, 28, 0);
    lv_obj_set_style_pad_top(col, SQ_TALL ? 56 : 28, 0);
    lv_obj_set_style_pad_bottom(col, SQ_TALL ? 32 : 24, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(col, false);

    // Greeting: the speaker disc beside the words on the square, above them
    // (bigger, with two candy dots) on the portrait panel.
    lv_obj_t* head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, SQ_TALL ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, SQ_TALL ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 18, 0);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_set_scrollable(head, false);
    const int disc = SQ_TALL ? 150 : 84;
    lv_obj_t* d = sqBox(head, disc, disc, SQ_SKY, disc / 2);
    lv_obj_t* ic = sqIcon(d, IC_SPEAKER, SQ_TALL ? &font_sqi_44 : &font_sqi_32, SQ_INK);
    lv_obj_center(ic);
    if (SQ_TALL) {
        lv_obj_t* a = sqBox(d, 44, 44, SQ_YELLOW, 22);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 4, 6);
        lv_obj_t* b = sqBox(d, 26, 26, SQ_PINK, 13);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 2, -14);
    }
    lv_obj_t* words = lv_obj_create(head);
    lv_obj_remove_style_all(words);
    lv_obj_set_size(words, SQ_TALL ? LV_PCT(100) : LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (!SQ_TALL) lv_obj_set_flex_grow(words, 1);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(words, SQ_TALL ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          SQ_TALL ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(words, 2, 0);
    lv_obj_set_style_pad_top(words, SQ_TALL ? 20 : 0, 0);
    lv_obj_set_scrollable(words, false);
    s_title = sqLabel(words, "Bonjour !", &font_sq_34, SQ_TEXT);
    s_sub = sqLabel(words, SQ_TALL ? "Je prépare le panneau.\nÇa ne se fait qu'une fois."
                                   : "Je prépare le panneau. Ça ne se fait qu'une fois.",
                    SQ_TALL ? &font_sq_20 : &font_sq_16, SQ_MUTED);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_sub, SQ_TALL ? LV_PCT(100) : SQ_W - 56 - disc - 18);
    if (SQ_TALL) lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* list = lv_obj_create(col);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(list, SQ_TALL ? 30 : 22, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, SQ_TALL ? 12 : 10, 0);
    lv_obj_set_scrollable(list, false);
    rowCreate(list, s_rows[0], "Enceintes");
    rowCreate(list, s_rows[1], "Compte Spotify");
    rowCreate(list, s_rows[2], "Favoris Sonos");
    rowCreate(list, s_rows[3], "Enregistrement");

    lv_obj_t* fill = lv_obj_create(col);
    lv_obj_remove_style_all(fill);
    lv_obj_set_width(fill, 1);
    lv_obj_set_flex_grow(fill, 1);

    s_bar = lv_bar_create(col);
    lv_obj_set_size(s_bar, LV_PCT(100), SQ_TALL ? 16 : 14);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_radius(s_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(SQ_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(SQ_YELLOW), LV_PART_INDICATOR);
    lv_obj_t* foot = sqLabel(col, "Pour recommencer plus tard : Réglages, puis Rafraîchir.",
                             SQ_TALL ? &font_sq_16 : &font_sq_14, SQ_MUTED);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_label_set_long_mode(foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_margin_top(foot, 12, 0);
    if (SQ_TALL) lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);

    sqRegisterScreen(SQ_SCR_SETUP, s_scr, SQ_TAB_NONE);
}

void sqSetupOnShow(void) {
    if (!s_started) s_started = millis();
}

// ~4 Hz while the screen is up.
void sqSetupTick(void) {
    if (!s_started) s_started = millis();
    const uint32_t age = millis() - s_started;
    char v[32];

    // Speakers
    const int spk = sonos.getDeviceCount();
    if (sq_sonos_ready && spk > 0) {
        snprintf(v, sizeof(v), spk == 1 ? "1 trouvée" : "%d trouvées", spk);
        rowSet(s_rows[0], ROW_DONE, v);
    } else if (age > SETUP_SPK_GIVEUP) {
        rowSet(s_rows[0], ROW_SKIP, "aucune pour l'instant");
    } else {
        rowSet(s_rows[0], ROW_BUSY, WiFi.status() == WL_CONNECTED ? "je cherche…" : "Wi-Fi…");
    }

    // Spotify account(s)
    SvcSource acc[6];
    const int na = sqSpotifyAccounts(acc, 6);
    if (na > 0) {
        snprintf(v, sizeof(v), na == 1 ? "trouvé" : "%d trouvés", na);
        rowSet(s_rows[1], ROW_DONE, v);
    } else if (musicServices.state() == SVC_READY || age > SETUP_SVC_GIVEUP || s_rows[0].state == ROW_SKIP) {
        rowSet(s_rows[1], ROW_SKIP, "pas trouvé");
    } else {
        rowSet(s_rows[1], s_rows[0].state == ROW_DONE ? ROW_BUSY : ROW_WAIT, "");
    }

    // Favourites
    const int nf = sqFavsCount();
    if (sq_favs_from_speaker) {
        snprintf(v, sizeof(v), "%d", nf);
        rowSet(s_rows[2], ROW_DONE, v);
    } else if (s_rows[0].state == ROW_SKIP) {
        rowSet(s_rows[2], ROW_SKIP, "plus tard");
    } else {
        rowSet(s_rows[2], s_rows[0].state == ROW_DONE ? ROW_BUSY : ROW_WAIT, "");
    }

    // Covers, kept in flash: asking for each one starts its download.
    int total = 0, kept = 0;
    if (s_rows[2].state == ROW_DONE && sqArtFlashReady()) {
        for (int i = 0; i < nf; ++i) {
            SqFav f;
            if (!sqFavsGet(i, &f) || !f.art[0]) continue;
            ++total;
            if (sqArtStored(f.art, SQ_FAV_ART_PX)) ++kept;
            else sqArtGetKept(f.art, SQ_FAV_ART_PX);
        }
        if (kept != s_art_seen) { s_art_seen = kept; s_art_moved = millis(); }
        snprintf(v, sizeof(v), "pochettes %d/%d", kept, total);
        if (kept >= total)                                rowSet(s_rows[3], ROW_DONE, total ? v : "rien à garder");
        else if (millis() - s_art_moved > SETUP_ART_STALL) rowSet(s_rows[3], ROW_DONE, v);
        else                                              rowSet(s_rows[3], ROW_BUSY, v);
    } else if (s_rows[2].state == ROW_SKIP) {
        rowSet(s_rows[3], ROW_SKIP, "plus tard");
    } else {
        rowSet(s_rows[3], ROW_WAIT, "");
    }

    int done = 0;
    for (int i = 0; i < 4; ++i) if (s_rows[i].state == ROW_DONE || s_rows[i].state == ROW_SKIP) ++done;
    int permille = done * 250;
    if (s_rows[3].state == ROW_BUSY && total) permille += 250 * kept / total;
    lv_bar_set_value(s_bar, permille, LV_ANIM_ON);

    if (!s_finish_at && (done == 4 || age > SETUP_TIMEOUT)) {
        lv_label_set_text(s_title, "C'est prêt !");
        lv_label_set_text(s_sub, s_rows[0].state == ROW_DONE ? "On peut écouter de la musique."
                                                             : "Je continuerai à chercher les enceintes.");
        lv_bar_set_value(s_bar, 1000, LV_ANIM_ON);
        s_finish_at = millis() + 1800;
    }
    if (s_finish_at && (int32_t)(millis() - s_finish_at) >= 0) {
        sqSetupMarkDone();
        s_finish_at = 0;
        sqShow(SQ_SCR_PLAYER);
    }
}
