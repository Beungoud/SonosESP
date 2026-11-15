// sq_sleep.cpp — Veille: the screen dims when nobody touches it, rests lower
// (or goes dark) at night, and a touch wakes it.
//
// Same settings and NVS keys as the P4 firmware (checkAutoDim / nightNow in
// ui_handlers.cpp), so the behaviour matches, but this build does not link
// ui_handlers.cpp and has its own little screen to set them.
//
// One difference: when the screen is very dark (night, or a dim level near
// zero), the touch that wakes it is swallowed. A child tapping a black panel
// in the dark wants light, not to have pressed whatever was under the finger.
#include "sq_ui.h"
#include "ui_common.h"
#include "config.h"
#include "display_driver.h"
#include <time.h>

// At or below this backlight level the panel looks off: the waking touch is
// not passed on to the button under it.
#define SQ_SLEEP_DARK_PCT  8

static bool    s_dimmed   = false;
static uint8_t s_level    = 100;     // what the backlight is at, as far as we set it
static bool    s_was_dark = false;   // the screen was dark when the last touch came

static void setLevel(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_level = (uint8_t)pct;
    display_set_brightness(s_level);
}

static void animCb(void*, int32_t v) { setLevel(v); }

static void fade(int from, int to, uint32_t ms, lv_anim_path_cb_t path) {
    lv_anim_delete(nullptr, animCb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, nullptr);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms ? ms : 1);
    lv_anim_set_exec_cb(&a, animCb);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

bool sqNightNow(void) {
    if (!night_enabled) return false;
    const time_t now = time(nullptr);
    if (now < 1600000000) return false;   // no NTP yet: 1970 is not a clock
    struct tm tm;
    localtime_r(&now, &tm);
    const int x = tm.tm_hour * 60 + tm.tm_min;
    return night_from_min < night_to_min ? (x >= night_from_min && x < night_to_min)
                                         : (x >= night_from_min || x < night_to_min);
}

void sqSleepLoad(void) {
    brightness_dimmed = wifiPrefs.getInt(NVS_KEY_BRIGHTNESS_DIM, DEFAULT_BRIGHTNESS_DIM);
    autodim_timeout   = wifiPrefs.getInt(NVS_KEY_AUTODIM, DEFAULT_AUTODIM_SEC);
    night_enabled     = wifiPrefs.getBool(NVS_KEY_NIGHT_ON, (bool)DEFAULT_NIGHT_ON);
    night_from_min    = wifiPrefs.getInt(NVS_KEY_NIGHT_FROM, DEFAULT_NIGHT_FROM);
    night_to_min      = wifiPrefs.getInt(NVS_KEY_NIGHT_TO, DEFAULT_NIGHT_TO);
    night_level       = wifiPrefs.getInt(NVS_KEY_NIGHT_LEVEL, 0);
    night_touch_level = wifiPrefs.getInt(NVS_KEY_NIGHT_TOUCH, DEFAULT_NIGHT_TOUCH);
    s_level = brightness_level;
    last_touch_time = millis();
}

// The level a woken screen comes back to.
static int awakeLevel(void) {
    return sqNightNow() ? constrain(night_touch_level, NIGHT_TOUCH_MIN, NIGHT_TOUCH_MAX) : brightness_level;
}

// The level a resting screen goes down to.
static int restLevel(void) {
    return sqNightNow() ? constrain(night_level, 0, 100) : constrain(brightness_dimmed, BRIGHTNESS_DIM_MIN, 100);
}

// Called from the loop when the touch sampler saw a new finger.
void sqSleepOnTouch(void) {
    last_touch_time = millis();
    s_was_dark = s_dimmed && s_level <= SQ_SLEEP_DARK_PCT;
    if (!s_dimmed) return;
    s_dimmed = false;
    if (s_was_dark) {
        // Nothing under the finger gets this touch: LVGL ignores the pointer
        // until it is lifted.
        lv_indev_t* in = lv_indev_get_next(nullptr);
        if (in) lv_indev_wait_release(in);
    }
    const int to = awakeLevel();
    Serial.printf("[SQ/SLEEP] wake %d -> %d%%%s\n", s_level, to, s_was_dark ? " (touch swallowed)" : "");
    if (sqNightNow()) fade(s_level, to, NIGHT_WAKE_FADE_MS, lv_anim_path_ease_out);   // never flash a dark room
    else              { lv_anim_delete(nullptr, animCb); setLevel(to); }
}

// Called from the loop on every pass.
void sqSleepTick(void) {
    if (autodim_timeout <= 0 || s_dimmed) return;
    if (millis() - last_touch_time < (uint32_t)autodim_timeout * 1000u) return;
    s_dimmed = true;
    Serial.printf("[SQ/SLEEP] dim %d -> %d%%%s\n", s_level, restLevel(), sqNightNow() ? " (night)" : "");
    fade(s_level, restLevel(), 1000, lv_anim_path_ease_in);
}

bool sqSleepDimmed(void) { return s_dimmed; }

// Brightness slider in Réglages: a change shows at once and counts as a touch.
void sqSleepBrightnessChanged(void) {
    s_dimmed = false;
    lv_anim_delete(nullptr, animCb);
    setLevel(brightness_level);
    last_touch_time = millis();
}

// ── Veille screen ───────────────────────────────────────────────────────────
static lv_obj_t* s_scr;
static lv_obj_t* s_delay_chips[4];
static lv_obj_t* s_dim_chips[3];
static lv_obj_t* s_night_sw;
static lv_obj_t* s_night_box;
static lv_obj_t* s_from_lbl;
static lv_obj_t* s_to_lbl;
static lv_obj_t* s_nlevel_chips[2];

static const int kDelays[4]   = {0, 30, 60, 300};
static const char* kDelayTxt[4] = {"Jamais", "30 s", "1 min", "5 min"};
static const int kDims[3]     = {BRIGHTNESS_DIM_MIN, 10, 25};
static const char* kDimTxt[3] = {"Très bas", "Bas", "Moyen"};
static const int kNight[2]    = {0, 5};
static const char* kNightTxt[2] = {"Éteint", "Veilleuse"};

static void chipStyle(lv_obj_t* c, bool on) {
    lv_obj_set_style_bg_color(c, lv_color_hex(on ? SQ_LILAC : SQ_RAISED), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(c, 0), lv_color_hex(on ? SQ_INK : SQ_TEXT), 0);
}

static lv_obj_t* chip(lv_obj_t* row, const char* text, lv_event_cb_t cb, int index) {
    lv_obj_t* b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 46);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    sqPressFeedback(b);
    lv_obj_t* l = sqLabel(b, text, &font_sq_16, SQ_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t)index);
    return b;
}

static lv_obj_t* section(lv_obj_t* list, const char* title) {
    lv_obj_t* c = sqBox(list, LV_PCT(100), LV_SIZE_CONTENT, SQ_SURFACE, 20);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    if (title) sqLabel(c, title, &font_sq_18, SQ_TEXT);
    return c;
}

static lv_obj_t* chipRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_scrollable(r, false);
    return r;
}

static void sleepRefresh(void);

static void delayClicked(lv_event_t* e) {
    autodim_timeout = kDelays[(int)(intptr_t)lv_event_get_user_data(e)];
    wifiPrefs.putInt(NVS_KEY_AUTODIM, autodim_timeout);
    sleepRefresh();
}

static void dimClicked(lv_event_t* e) {
    brightness_dimmed = kDims[(int)(intptr_t)lv_event_get_user_data(e)];
    wifiPrefs.putInt(NVS_KEY_BRIGHTNESS_DIM, brightness_dimmed);
    sleepRefresh();
}

static void nightLevelClicked(lv_event_t* e) {
    night_level = kNight[(int)(intptr_t)lv_event_get_user_data(e)];
    wifiPrefs.putInt(NVS_KEY_NIGHT_LEVEL, night_level);
    sleepRefresh();
}

static void nightToggled(lv_event_t*) {
    night_enabled = lv_obj_has_state(s_night_sw, LV_STATE_CHECKED);
    wifiPrefs.putBool(NVS_KEY_NIGHT_ON, night_enabled);
    sleepRefresh();
}

// user data: 0 = from -, 1 = from +, 2 = to -, 3 = to +
static void timeStep(lv_event_t* e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int* v = k < 2 ? &night_from_min : &night_to_min;
    *v = (*v + ((k & 1) ? NIGHT_STEP_MIN : -NIGHT_STEP_MIN) + 24 * 60) % (24 * 60);
    wifiPrefs.putInt(k < 2 ? NVS_KEY_NIGHT_FROM : NVS_KEY_NIGHT_TO, *v);
    sleepRefresh();
}

static lv_obj_t* timeStepper(lv_obj_t* parent, const char* caption, int minus_id) {
    lv_obj_t* r = chipRow(parent);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* cap = sqLabel(r, caption, &font_sq_16, SQ_MUTED);
    lv_obj_set_width(cap, 110);
    lv_obj_t* m = sqRoundButton(r, 46, SQ_RAISED, IC_MINUS, &font_sqi_26, SQ_TEXT);
    lv_obj_add_event_cb(m, timeStep, LV_EVENT_CLICKED, (void*)(intptr_t)minus_id);
    lv_obj_t* val = sqLabel(r, "", &font_sq_24, SQ_TEXT);
    lv_obj_set_width(val, 96);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* p = sqRoundButton(r, 46, SQ_RAISED, IC_PLUS, &font_sqi_26, SQ_TEXT);
    lv_obj_add_event_cb(p, timeStep, LV_EVENT_CLICKED, (void*)(intptr_t)(minus_id + 1));
    return val;
}

void sqSleepCreate(void) {
    s_scr = sqScreenCreate();
    sqHeader(s_scr, "Veille", SQ_LILAC, true);
    lv_obj_t* list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, SQ_HEADER_H);
    lv_obj_set_size(list, SQ_W, SQ_H - SQ_HEADER_H);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_bottom(list, 16, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* c = section(list, "Baisser l'écran après");
    lv_obj_t* r = chipRow(c);
    for (int i = 0; i < 4; ++i) s_delay_chips[i] = chip(r, kDelayTxt[i], delayClicked, i);

    c = section(list, "Lumière en veille");
    r = chipRow(c);
    for (int i = 0; i < 3; ++i) s_dim_chips[i] = chip(r, kDimTxt[i], dimClicked, i);

    c = section(list, nullptr);
    lv_obj_t* head = chipRow(c);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* hl = sqLabel(head, IC_MOON, &font_sqi_26, SQ_LILAC);
    (void)hl;
    lv_obj_t* ht = sqLabel(head, "Mode nuit", &font_sq_18, SQ_TEXT);
    lv_obj_set_flex_grow(ht, 1);
    s_night_sw = lv_switch_create(head);
    lv_obj_set_size(s_night_sw, 64, 34);
    lv_obj_set_style_bg_color(s_night_sw, lv_color_hex(SQ_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_night_sw, lv_color_hex(SQ_LILAC), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_night_sw, lv_color_hex(SQ_TEXT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_night_sw, 12);
    lv_obj_add_event_cb(s_night_sw, nightToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    s_night_box = lv_obj_create(c);
    lv_obj_remove_style_all(s_night_box);
    lv_obj_set_size(s_night_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_night_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_night_box, 10, 0);
    lv_obj_set_scrollable(s_night_box, false);
    s_from_lbl = timeStepper(s_night_box, "À partir de", 0);
    s_to_lbl   = timeStepper(s_night_box, "Jusqu'à", 2);
    sqLabel(s_night_box, "La nuit, l'écran est :", &font_sq_16, SQ_MUTED);
    r = chipRow(s_night_box);
    for (int i = 0; i < 2; ++i) s_nlevel_chips[i] = chip(r, kNightTxt[i], nightLevelClicked, i);

    sqRegisterScreen(SQ_SCR_SLEEP, s_scr, SQ_TAB_NONE);
}

static void sleepRefresh(void) {
    for (int i = 0; i < 4; ++i) chipStyle(s_delay_chips[i], autodim_timeout == kDelays[i]);
    int best = 0;   // nearest chip to whatever is stored (the P4 allows any level)
    for (int i = 1; i < 3; ++i)
        if (abs(brightness_dimmed - kDims[i]) < abs(brightness_dimmed - kDims[best])) best = i;
    for (int i = 0; i < 3; ++i) chipStyle(s_dim_chips[i], i == best);
    if (night_enabled) lv_obj_add_state(s_night_sw, LV_STATE_CHECKED);
    else               lv_obj_remove_state(s_night_sw, LV_STATE_CHECKED);
    sqSetHidden(s_night_box, !night_enabled);
    lv_label_set_text_fmt(s_from_lbl, "%02d:%02d", night_from_min / 60, night_from_min % 60);
    lv_label_set_text_fmt(s_to_lbl, "%02d:%02d", night_to_min / 60, night_to_min % 60);
    for (int i = 0; i < 2; ++i) chipStyle(s_nlevel_chips[i], (night_level > 0) == (i == 1));
}

void sqSleepOnShow(void) { sleepRefresh(); }

// The one-line summary on the Réglages card.
void sqSleepSummary(char* out, size_t n) {
    if (autodim_timeout <= 0) snprintf(out, n, "Écran toujours allumé");
    else if (autodim_timeout < 60) snprintf(out, n, "Baisse après %d s", autodim_timeout);
    else snprintf(out, n, "Baisse après %d min", autodim_timeout / 60);
    if (night_enabled) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, " · nuit %02dh%02d", night_from_min / 60, night_from_min % 60);
    }
}
