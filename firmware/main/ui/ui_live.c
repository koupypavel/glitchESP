#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "ui_lvgl.h"
#include "ui_live.h"
#include "frame_pipeline.h"
#include "fx.h"
#include "settings.h"
#include "cam_ctrl.h"
#include "presets.h"
#include "gallery.h"
#include "ui_editor.h"
#include "battery.h"

static const char *TAG = "ui_live";

#define IDLE_DIM_MS 60000

#define BAR_H   394     /* bottom control bar: four rows of chips, amount slider, tool buttons */

/* colours that map exactly onto the UI layer keys (see ui_lvgl.h) */
#define COLOR_KEY_CLEAR   lv_color_hex(0x000008)   /* RGB565 0x0001 */
#define COLOR_KEY_DIM     lv_color_hex(0x000010)   /* RGB565 0x0002 */

static lv_obj_t *s_status;
static lv_obj_t *s_toast;
static lv_obj_t *s_slider;
#define MAX_CHIPS   32
static lv_obj_t *s_chip[MAX_CHIPS];
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_settings;   /* modal panel, NULL when closed */
static lv_obj_t *s_zoom_label;
static lv_obj_t *s_bar, *s_zoom_btn[2];
static lv_obj_t *s_handle, *s_handle_label;    /* tab that hides / shows the bar */
static bool s_bar_hidden;
static lv_obj_t *s_presets;    /* modal panel, NULL when closed */
static lv_obj_t *s_preset_label[PRESET_SLOTS];

/* ---- toast ---- */

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;
}

static void toast_show(const char *msg, uint32_t ms)
{
    lv_label_set_text(s_toast, msg);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/* ---- effect controls ---- */

/* The chips changed: build the chain they describe. Effects that stay keep their
 * parameters (they may have been edited by hand); new ones start from the amount knob. */
static void rebuild_chain(void)
{
    fp_recipe_t cur, next;
    frame_pipeline_get_recipe(&cur);
    next = cur;
    fx_chain_clear(&next.chain);
    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        if (!s_chip[i] || !lv_obj_has_state(s_chip[i], LV_STATE_CHECKED)) continue;
        const fx_desc_t *fx = fx_registry_get(i);
        int slot = fx_chain_add(&next.chain, fx);
        if (slot < 0) {
            lv_obj_remove_state(s_chip[i], LV_STATE_CHECKED);
            toast_show("max 3 effects", 1200);
            continue;
        }
        const fx_slot_t *old = NULL;
        for (int k = 0; k < cur.chain.count; k++) {
            if (cur.chain.slots[k].fx == fx) old = &cur.chain.slots[k];
        }
        if (old) memcpy(next.chain.slots[slot].params, old->params, sizeof(old->params));
        else if (fx->from_amount) fx->from_amount(cur.amount, next.chain.slots[slot].params);
    }
    frame_pipeline_set_recipe(&next);
    ui_editor_sync();
}

static int chips_checked(void)
{
    int n = 0;
    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        if (s_chip[i] && lv_obj_has_state(s_chip[i], LV_STATE_CHECKED)) n++;
    }
    return n;
}

/* A chip that was just switched on may have to go back off: a fourth effect is refused,
 * and so is a second effect that needs the previous frame (there is one history buffer). */
static bool chip_over_limit(lv_obj_t *chip)
{
    if (!lv_obj_has_state(chip, LV_STATE_CHECKED)) return false;
    const char *why = NULL;
    if (chips_checked() > FX_CHAIN_MAX) why = "max 3 effects";
    int temporal = 0, mine = -1;
    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        if (s_chip[i] == chip) mine = i;
        if (s_chip[i] && lv_obj_has_state(s_chip[i], LV_STATE_CHECKED) && fx_registry_get(i)->temporal) temporal++;
    }
    if (!why && mine >= 0 && fx_registry_get(mine)->temporal && temporal > 1) why = "Tracers and Slit scan: one at a time";
    if (!why) return false;
    lv_obj_remove_state(chip, LV_STATE_CHECKED);
    toast_show(why, 1500);
    return true;
}

static void chip_event_cb(lv_event_t *e)
{
    if (!chip_over_limit(lv_event_get_target(e))) rebuild_chain();
}

/* The one knob: every parameter of every active effect follows it again. */
static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    frame_pipeline_set_amount((float)lv_slider_get_value(s) / 100.0f);
    ui_editor_sync();
}

static void editor_open_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_editor_open(NULL)) toast_show("turn on an effect first", 1500);
}

static void reroll_event_cb(lv_event_t *e)
{
    (void)e;
    uint32_t seed = frame_pipeline_reroll();
    char msg[40];
    snprintf(msg, sizeof(msg), "seed %08lx", (unsigned long)seed);
    toast_show(msg, 900);
}

/* ---- presets ---- */

/* Make the chips and the slider show `r`, and hand it to the pipeline as it is. */
static void apply_recipe(const fp_recipe_t *r)
{
    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        if (!s_chip[i]) continue;
        bool on = false;
        for (int k = 0; k < r->chain.count; k++) {
            if (r->chain.slots[k].fx == fx_registry_get(i) && r->chain.slots[k].enabled) on = true;
        }
        if (on) lv_obj_add_state(s_chip[i], LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_chip[i], LV_STATE_CHECKED);
    }
    lv_slider_set_value(s_slider, (int32_t)(r->amount * 100.0f + 0.5f), LV_ANIM_OFF);
    frame_pipeline_set_recipe(r);
    ui_editor_sync();
}

static void presets_close(void)
{
    if (s_presets) {
        lv_obj_delete(s_presets);
        s_presets = NULL;
        memset(s_preset_label, 0, sizeof(s_preset_label));
    }
}

static void preset_refresh_row(int slot)
{
    if (!s_preset_label[slot]) return;
    char text[96];
    presets_describe(slot, text, sizeof(text));
    lv_label_set_text_fmt(s_preset_label[slot], "%d   %s", slot + 1, text);
}

static bool preset_load(int slot)
{
    fp_recipe_t r;
    char msg[48];
    bool ok = presets_load(slot, &r) == ESP_OK;
    if (ok) {
        apply_recipe(&r);
        presets_close();
        snprintf(msg, sizeof(msg), "preset %d", slot + 1);
    } else {
        snprintf(msg, sizeof(msg), "preset %d is empty", slot + 1);
    }
    toast_show(msg, 1200);
    return ok;
}

static bool preset_save(int slot)
{
    fp_recipe_t r;
    char msg[48];
    frame_pipeline_get_recipe(&r);
    bool ok = presets_save(slot, &r) == ESP_OK;
    preset_refresh_row(slot);
    snprintf(msg, sizeof(msg), ok ? "saved as preset %d" : "could not save preset %d", slot + 1);
    toast_show(msg, 1200);
    return ok;
}

static void preset_load_cb(lv_event_t *e)  { preset_load((int)(intptr_t)lv_event_get_user_data(e)); }
static void preset_save_cb(lv_event_t *e)  { preset_save((int)(intptr_t)lv_event_get_user_data(e)); }
static void presets_close_cb(lv_event_t *e) { (void)e; presets_close(); }

/* One row per slot: tap the description to recall it, the disk button stores the current look. */
static void presets_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_presets) return;
    s_presets = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_presets, FP_OUT_W - 60, 150 + PRESET_SLOTS * 72);
    lv_obj_align(s_presets, LV_ALIGN_CENTER, 0, -90);
    lv_obj_set_style_bg_color(s_presets, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(s_presets, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_presets, lv_color_hex(0xE0007A), 0);
    lv_obj_set_style_border_width(s_presets, 2, 0);
    lv_obj_set_style_radius(s_presets, 12, 0);
    lv_obj_set_style_pad_all(s_presets, 16, 0);
    lv_obj_set_flex_flow(s_presets, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_presets, 8, 0);
    lv_obj_remove_flag(s_presets, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_presets);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Presets");

    for (int i = 0; i < PRESET_SLOTS; i++) {
        lv_obj_t *row = lv_obj_create(s_presets);
        lv_obj_set_size(row, LV_PCT(100), 64);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *load = lv_button_create(row);
        lv_obj_set_size(load, FP_OUT_W - 60 - 32 - 4 - 84, 60);      /* panel minus padding, border, save button */
        lv_obj_align(load, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(load, lv_color_hex(0x303030), 0);
        lv_obj_add_event_cb(load, preset_load_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(load);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        s_preset_label[i] = l;
        preset_refresh_row(i);

        lv_obj_t *save = lv_button_create(row);
        lv_obj_set_size(save, 76, 60);
        lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(save, lv_color_hex(0x2060C0), 0);
        lv_obj_add_event_cb(save, preset_save_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *sl = lv_label_create(save);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
        lv_label_set_text(sl, LV_SYMBOL_SAVE);
        lv_obj_center(sl);
    }

    lv_obj_t *close = lv_button_create(s_presets);
    lv_obj_set_size(close, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(close, lv_color_hex(0xE0007A), 0);
    lv_obj_t *cl = lv_label_create(close);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, presets_close_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- zoom ---- */

static const float k_zoom_steps[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f };   /* 1.5 and up: un-binned sensor mode */
#define ZOOM_STEPS ((int)(sizeof(k_zoom_steps) / sizeof(k_zoom_steps[0])))

static void zoom_show(float z)
{
    int tenths = (int)(z * 10.0f + 0.5f);
    lv_label_set_text_fmt(s_zoom_label, "%d.%dx", tenths / 10, tenths % 10);
}

static void zoom_step(int dir)
{
    float z = cam_ctrl_get_zoom();
    int idx = 0;
    for (int i = 1; i < ZOOM_STEPS; i++) {
        if (fabsf(k_zoom_steps[i] - z) < fabsf(k_zoom_steps[idx] - z)) idx = i;
    }
    idx += dir;
    if (idx < 0) idx = 0;
    if (idx >= ZOOM_STEPS) idx = ZOOM_STEPS - 1;
    zoom_show(cam_ctrl_set_zoom(k_zoom_steps[idx]));
}

static void zoom_event_cb(lv_event_t *e)
{
    zoom_step((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *zoom_button(lv_obj_t *parent, const char *symbol, int dir, int y_ofs)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, y_ofs);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, zoom_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
    return b;
}

/* "+" and "-" on the right edge with the current factor between them. */
static void create_zoom_controls(lv_obj_t *parent)
{
    s_zoom_btn[0] = zoom_button(parent, LV_SYMBOL_PLUS, +1, -150);
    s_zoom_btn[1] = zoom_button(parent, LV_SYMBOL_MINUS, -1, -10);

    s_zoom_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_zoom_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_zoom_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_zoom_label, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(s_zoom_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_zoom_label, 6, 0);
    lv_obj_set_style_pad_ver(s_zoom_label, 4, 0);
    lv_obj_set_style_radius(s_zoom_label, 6, 0);
    lv_obj_set_width(s_zoom_label, 64);
    lv_obj_set_style_text_align(s_zoom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_zoom_label, LV_ALIGN_RIGHT_MID, -10, -80);
    zoom_show(cam_ctrl_get_zoom());
}

/* ---- settings panel ---- */

static void settings_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    settings_t cfg = *settings_get();
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (which == 0) cfg.flip_h = on;
    if (which == 1) cfg.flip_v = on;
    if (which == 2) cfg.photo_hires = on;
    if (which == 3) cfg.idle_dim = on;
    if (which == 4) cfg.sound = on;
    settings_set(&cfg);
}

static const uint8_t k_burst_shots[] = { 1, 3, 5, 10 };

static void settings_burst_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    settings_t cfg = *settings_get();
    uint32_t i = lv_dropdown_get_selected(dd);
    cfg.burst = k_burst_shots[i < sizeof(k_burst_shots) ? i : 0];
    settings_set(&cfg);
}

/* A label on the left, a drop-down on the right. */
static lv_obj_t *add_dropdown_row(lv_obj_t *parent, const char *text, const char *options, uint32_t selected,
                                  lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_width(dd, 220);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static void settings_video_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    settings_t cfg = *settings_get();
    cfg.video_h264 = lv_dropdown_get_selected(dd) == 1;
    settings_set(&cfg);
}

static void settings_low_light_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    settings_t cfg = *settings_get();
    cfg.low_light = (uint8_t)lv_dropdown_get_selected(dd);
    settings_set(&cfg);
}

static void settings_quality_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    settings_t cfg = *settings_get();
    cfg.quality = (uint8_t)lv_dropdown_get_selected(dd);
    settings_set(&cfg);
}

static void settings_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_settings) {
        lv_obj_delete(s_settings);
        s_settings = NULL;
    }
}

static lv_obj_t *add_switch_row(lv_obj_t *parent, const char *text, bool on, int id)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, settings_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)id);
    return sw;
}

static void settings_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_settings) return;
    const settings_t *cfg = settings_get();
    s_settings = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_settings, FP_OUT_W - 60, 780);
    lv_obj_align(s_settings, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_settings, lv_color_hex(0xE0007A), 0);
    lv_obj_set_style_border_width(s_settings, 2, 0);
    lv_obj_set_style_radius(s_settings, 12, 0);
    lv_obj_set_style_pad_all(s_settings, 16, 0);
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_settings, 6, 0);

    lv_obj_t *title = lv_label_create(s_settings);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Settings");

    add_switch_row(s_settings, "Mirror left/right", cfg->flip_h, 0);
    add_switch_row(s_settings, "Flip up/down", cfg->flip_v, 1);
    add_switch_row(s_settings, "Full-resolution photos (1x)", cfg->photo_hires, 2);
    add_switch_row(s_settings, "Dim screen when idle", cfg->idle_dim, 3);
    add_switch_row(s_settings, "Shutter sound", cfg->sound, 4);

    uint32_t burst_idx = 0;
    for (uint32_t i = 0; i < sizeof(k_burst_shots); i++) {
        if (k_burst_shots[i] == cfg->burst) burst_idx = i;
    }
    add_dropdown_row(s_settings, "Burst", "Off\n3 photos\n5 photos\n10 photos", burst_idx, settings_burst_cb);
    add_dropdown_row(s_settings, "Video format", "MJPEG (plays here)\nH.264 (smaller)", cfg->video_h264 ? 1 : 0,
                     settings_video_cb);
    add_dropdown_row(s_settings, "Low light", "Auto\nOff\nAlways", cfg->low_light, settings_low_light_cb);
    add_dropdown_row(s_settings, "Preview quality", "Auto\nFull\nHalf", cfg->quality, settings_quality_cb);

    lv_obj_t *close = lv_button_create(s_settings);
    lv_obj_set_size(close, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(close, lv_color_hex(0xE0007A), 0);
    lv_obj_t *cl = lv_label_create(close);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ver = lv_label_create(s_settings);                /* firmware version, from version.txt */
    lv_obj_set_style_text_color(ver, lv_color_hex(0x909090), 0);
    lv_label_set_text_fmt(ver, "glitchESP %s", esp_app_get_description()->version);
}

/* Control bar geometry: four rows of four effect chips, the amount slider, five tools. */
#define BAR_PAD     12
#define CHIP_COLS   4
#define CHIP_GAP    8
#define CHIP_W      ((FP_OUT_W - 2 * BAR_PAD - 10 - (CHIP_COLS - 1) * CHIP_GAP) / CHIP_COLS)
#define CHIP_H      56
#define CHIP_ROWS   4
#define CHIP_ROWS_H (CHIP_ROWS * CHIP_H + (CHIP_ROWS - 1) * CHIP_GAP)
#define AMOUNT_H    44
#define TOOL_N      5
#define TOOL_GAP    10
#define TOOL_W      ((FP_OUT_W - 2 * BAR_PAD - (TOOL_N - 1) * TOOL_GAP) / TOOL_N)
#define TOOL_H      60
#define HANDLE_W    120
#define HANDLE_H    44

static void gallery_open_cb(lv_event_t *e) { (void)e; gallery_open(); }

/* Icon button on the bar's bottom line; `pos` counts from the right edge. */
static void tool_button(lv_obj_t *bar, const char *symbol, uint32_t color, lv_event_cb_t cb, int pos)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_size(b, TOOL_W, TOOL_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -pos * (TOOL_W + TOOL_GAP), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}

/* Hide or show the control bar. The handle stays: a tab on top of the bar, or at the
 * bottom edge of the screen when the bar is away. */
static void set_bar_hidden(bool hidden, bool remember)
{
    s_bar_hidden = hidden;
    /* the status line and the zoom buttons go with the bar: only the handle stays */
    lv_obj_t *objs[] = { s_bar, s_status, s_zoom_btn[0], s_zoom_btn[1], s_zoom_label };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (!objs[i]) continue;
        if (hidden) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_handle, LV_ALIGN_BOTTOM_MID, 0, hidden ? 0 : -BAR_H);
    lv_label_set_text(s_handle_label, hidden ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    if (hidden) ui_editor_close();
    if (remember) {
        settings_t cfg = *settings_get();
        if (cfg.bar_hidden != hidden) {
            cfg.bar_hidden = hidden;
            settings_set(&cfg);
        }
    }
}

static void handle_cb(lv_event_t *e)
{
    (void)e;
    set_bar_hidden(!s_bar_hidden, true);
}

static void create_control_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = s_bar = lv_obj_create(parent);
    lv_obj_set_size(bar, FP_OUT_W, BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_KEY_DIM, 0);      /* video darkened underneath */
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, BAR_PAD, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* effect chips: a fixed grid, so every chip is the same comfortable size. Four rows are
     * visible; with more effects than that the grid scrolls, a row at a time. */
    lv_obj_t *row = lv_obj_create(bar);
    lv_obj_set_size(row, LV_PCT(100), CHIP_ROWS_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(row, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, CHIP_GAP, 0);
    lv_obj_set_style_pad_row(row, CHIP_GAP, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(row, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(row, fx_registry_count() > CHIP_ROWS * CHIP_COLS ? LV_SCROLLBAR_MODE_ON : LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE0007A), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(row, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(row, 10, 0);                 /* room for the scroll bar */

    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        const fx_desc_t *fx = fx_registry_get(i);
        lv_obj_t *b = lv_button_create(row);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_size(b, CHIP_W, CHIP_H);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xE0007A), LV_STATE_CHECKED);
        lv_obj_set_style_pad_hor(b, 2, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_label_set_text(l, fx->name);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, chip_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_chip[i] = b;
    }

    /* the amount knob on its own line */
    lv_obj_t *cap = lv_label_create(bar);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_white(), 0);
    lv_label_set_text(cap, "amount");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, CHIP_ROWS_H + 10 + (AMOUNT_H - 24) / 2);

    s_slider = lv_slider_create(bar);
    lv_obj_set_size(s_slider, FP_OUT_W - 2 * BAR_PAD - 104 - 16, 24);
    lv_obj_align(s_slider, LV_ALIGN_TOP_LEFT, 104, CHIP_ROWS_H + 10 + (AMOUNT_H - 24) / 2);
    lv_slider_set_range(s_slider, 0, 100);
    lv_slider_set_value(s_slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0xE0007A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0xE0007A), LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* tools along the bottom */
    tool_button(bar, LV_SYMBOL_SETTINGS, 0x505050, settings_open_cb, 0);
    tool_button(bar, LV_SYMBOL_LIST, 0x2060C0, presets_open_cb, 1);
    tool_button(bar, LV_SYMBOL_IMAGE, 0x2060C0, gallery_open_cb, 2);
    tool_button(bar, LV_SYMBOL_REFRESH, 0x2060C0, reroll_event_cb, 3);
    tool_button(bar, LV_SYMBOL_EDIT, 0xE0007A, editor_open_cb, 4);

    /* the handle that hides and shows the bar */
    s_handle = lv_button_create(parent);
    lv_obj_set_size(s_handle, HANDLE_W, HANDLE_H);
    lv_obj_set_style_bg_color(s_handle, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(s_handle, 10, 0);
    s_handle_label = lv_label_create(s_handle);
    lv_obj_set_style_text_font(s_handle_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_handle_label);
    lv_obj_add_event_cb(s_handle, handle_cb, LV_EVENT_CLICKED, NULL);
    set_bar_hidden(settings_get()->bar_hidden, false);
}

/* Battery for the status line, and what to do when it runs low: warn once at 10 %, and
 * close a recording before the board browns out, so the video file stays readable. */
static const char *battery_text(char *buf, size_t len)
{
    static bool s_warned;
    battery_info_t b;
    battery_get(&b);
    if (b.state == BATTERY_UNKNOWN || b.state == BATTERY_NONE) return "";
    if (b.low && !s_warned) {
        s_warned = true;
        toast_show("battery low", 3000);
    }
    if (!b.low) s_warned = false;
    if (b.critical && capture_video_active()) {
        capture_video_stop();
        toast_show("battery empty: recording stopped", 4000);
    }
    snprintf(buf, len, "   %s%d%%", b.state == BATTERY_CHARGING ? "+" : "", b.percent);
    return buf;
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    char bat[16];
    int night = cam_ctrl_night_level();
    lv_label_set_text_fmt(s_status, "cam %lu fps%s   fx %lu ms   %s   shots %lu%s",
                          (unsigned long)frame_pipeline_get_fps(), night ? (night > 1 ? " night 2" : " night") : "",
                          (unsigned long)(frame_pipeline_get_fx_us() / 1000),
                          capture_sd_available() ? "SD ok" : "no SD",
                          (unsigned long)capture_get_count(), battery_text(bat, sizeof(bat)));
    static int s_tick;
    if (++s_tick % 5 == 0) capture_sd_poll();               /* a card put in after boot */
    /* a minute without input turns the backlight down; recording counts as input */
    if (capture_video_active()) ui_lvgl_poke();
    else if (settings_get()->idle_dim) ui_lvgl_dim_if_idle(IDLE_DIM_MS);
}

void ui_live_create(void)
{
    lv_obj_t *scr = lv_screen_active();

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_status, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_status, 8, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(s_status, FP_OUT_W);
    lv_label_set_text(s_status, "starting camera...");

    create_control_bar(scr);
    create_zoom_controls(scr);
    gallery_create_ui(scr);

    s_toast = lv_label_create(scr);
    lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(BAR_H + 70));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(status_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "live screen created (%d effects)", fx_registry_count());
}

/* ---- the same controls, driven from another task (serial remote) ---- */

void ui_live_set_zoom(float zoom)
{
    if (!ui_lvgl_lock(200)) return;
    zoom_show(cam_ctrl_set_zoom(zoom));
    ui_lvgl_unlock();
}

bool ui_live_toggle_effect(const char *id)
{
    bool ok = false;
    if (!ui_lvgl_lock(200)) return false;
    for (int i = 0; i < fx_registry_count() && i < MAX_CHIPS; i++) {
        if (!s_chip[i] || strcmp(fx_registry_get(i)->id, id) != 0) continue;
        bool turn_on = !lv_obj_has_state(s_chip[i], LV_STATE_CHECKED);
        if (turn_on) lv_obj_add_state(s_chip[i], LV_STATE_CHECKED);
        else         lv_obj_remove_state(s_chip[i], LV_STATE_CHECKED);
        ok = !chip_over_limit(s_chip[i]);
        if (ok) rebuild_chain();
        break;
    }
    ui_lvgl_unlock();
    return ok;
}

void ui_live_set_visible(bool visible)
{
    lv_obj_t *objs[] = { s_status, s_bar, s_handle, s_zoom_btn[0], s_zoom_btn[1], s_zoom_label };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (!objs[i]) continue;
        if (visible) lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (visible) set_bar_hidden(s_bar_hidden, false);      /* the bar itself follows its own switch */
    if (!visible) {
        presets_close();
        settings_close_cb(NULL);
        ui_editor_close();
    }
}

void ui_live_apply_recipe(const fp_recipe_t *r)
{
    apply_recipe(r);
}

void ui_live_toast(const char *msg, uint32_t ms)
{
    toast_show(msg, ms);
}

bool ui_live_editor(const char *fx_id, bool open)
{
    bool ok = true;
    if (!ui_lvgl_lock(200)) return false;
    if (open) ok = ui_editor_open(fx_id && fx_id[0] ? fx_registry_find(fx_id) : NULL);
    else      ui_editor_close();
    ui_lvgl_unlock();
    return ok;
}

bool ui_live_set_param(const char *fx_id, const char *param_id, float value)
{
    const fx_desc_t *fx = fx_registry_find(fx_id);
    int k = fx ? fx_param_index(fx, param_id) : -1;
    if (k < 0 || !ui_lvgl_lock(200)) return false;
    bool ok = frame_pipeline_set_param(fx, k, value);
    ui_editor_sync();
    ui_lvgl_unlock();
    return ok;
}

void ui_live_set_bar_hidden(bool hidden)
{
    if (!ui_lvgl_lock(200)) return;
    set_bar_hidden(hidden, true);
    ui_lvgl_unlock();
}

void ui_live_toggle_bar(void)
{
    if (!ui_lvgl_lock(200)) return;
    set_bar_hidden(!s_bar_hidden, true);
    ui_lvgl_unlock();
}

void ui_live_adjust_amount(int steps)
{
    if (!ui_lvgl_lock(200)) return;
    int32_t v = lv_slider_get_value(s_slider) + steps * 2;       /* 2 % per detent */
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
    frame_pipeline_set_amount((float)v / 100.0f);
    ui_editor_sync();
    char msg[24];
    snprintf(msg, sizeof(msg), "amount %d%%", (int)v);
    toast_show(msg, 700);
    ui_lvgl_unlock();
}

void ui_live_reroll(void)
{
    if (!ui_lvgl_lock(200)) return;
    reroll_event_cb(NULL);
    ui_lvgl_unlock();
}

/* Step to the next stored preset (wrapping); with none stored it says so. */
void ui_live_next_preset(void)
{
    static int s_last = -1;
    if (!ui_lvgl_lock(200)) return;
    int found = -1;
    for (int i = 1; i <= PRESET_SLOTS && found < 0; i++) {
        int slot = (s_last + i + PRESET_SLOTS) % PRESET_SLOTS;
        if (presets_exists(slot)) found = slot;
    }
    if (found >= 0) {
        s_last = found;
        preset_load(found);
    } else {
        toast_show("no presets saved", 1200);
    }
    ui_lvgl_unlock();
}

bool ui_live_preset(int slot, bool save)
{
    if (!ui_lvgl_lock(200)) return false;
    bool ok = save ? preset_save(slot) : preset_load(slot);
    ui_lvgl_unlock();
    return ok;
}

void ui_live_show_panel(int panel, bool show)
{
    if (!ui_lvgl_lock(200)) return;
    if (panel == 0) {
        if (show) presets_open_cb(NULL);
        else      presets_close();
    } else {
        if (show) settings_open_cb(NULL);
        else      settings_close_cb(NULL);
    }
    ui_lvgl_unlock();
}

void ui_live_set_amount(float amount)
{
    if (!ui_lvgl_lock(200)) return;
    lv_slider_set_value(s_slider, (int32_t)(amount * 100.0f), LV_ANIM_OFF);
    frame_pipeline_set_amount(amount);
    ui_lvgl_unlock();
}

void ui_live_on_capture_started(void)
{
    ui_lvgl_flash(2);
}

void ui_live_on_capture_done(const capture_result_t *res)
{
    char msg[96];
    const char *name = strrchr(res->path, '/');
    name = name ? name + 1 : res->path;
    if (res->ok && res->burst > 1) {
        snprintf(msg, sizeof(msg), "saved %lu photos, last %s", (unsigned long)res->burst, name);
    } else if (res->ok) {
        snprintf(msg, sizeof(msg), "saved %s  (%lu KB, %lu ms)", name,
                 (unsigned long)(res->jpeg_bytes / 1024), (unsigned long)(res->encode_ms + res->write_ms));
    } else if (res->burst > 0) {
        snprintf(msg, sizeof(msg), "saved %lu photos, then: %s", (unsigned long)res->burst, res->error ? res->error : "?");
    } else {
        snprintf(msg, sizeof(msg), "capture failed: %s", res->error ? res->error : "?");
    }
    if (ui_lvgl_lock(200)) {
        toast_show(msg, 2500);
        ui_lvgl_unlock();
    }
}

void ui_live_on_video_started(void)
{
    if (ui_lvgl_lock(200)) {
        toast_show("REC  (hold BOOT to stop)", 1500);
        ui_lvgl_unlock();
    }
}

void ui_live_on_video_done(const video_result_t *res)
{
    char msg[128];
    if (res->ok) {
        const char *name = strrchr(res->path, '/');
        snprintf(msg, sizeof(msg), "saved %s  %lu s, %lu frames%s", name ? name + 1 : res->path,
                 (unsigned long)(res->duration_ms / 1000), (unsigned long)res->frames,
                 res->dropped ? " (some dropped)" : "");
    } else {
        snprintf(msg, sizeof(msg), "video failed: %s", res->error ? res->error : "?");
    }
    if (ui_lvgl_lock(200)) {
        toast_show(msg, 3000);
        ui_lvgl_unlock();
    }
}

void ui_live_on_video_error(const char *msg)
{
    if (ui_lvgl_lock(200)) {
        toast_show(msg, 2000);
        ui_lvgl_unlock();
    }
}
