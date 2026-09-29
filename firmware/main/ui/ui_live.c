#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "ui_lvgl.h"
#include "ui_live.h"
#include "frame_pipeline.h"
#include "fx.h"
#include "settings.h"

static const char *TAG = "ui_live";

#define BAR_H   215     /* bottom control bar height (two rows of chips + slider) */

/* colours that map exactly onto the UI layer keys (see ui_lvgl.h) */
#define COLOR_KEY_CLEAR   lv_color_hex(0x000008)   /* RGB565 0x0001 */
#define COLOR_KEY_DIM     lv_color_hex(0x000010)   /* RGB565 0x0002 */

static lv_obj_t *s_status;
static lv_obj_t *s_toast;
static lv_obj_t *s_slider;
static lv_obj_t *s_chip[12];
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_settings;   /* modal panel, NULL when closed */

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

static void rebuild_chain(void)
{
    fx_chain_t chain;
    fx_chain_clear(&chain);
    for (int i = 0; i < fx_registry_count() && i < 12; i++) {
        if (s_chip[i] && lv_obj_has_state(s_chip[i], LV_STATE_CHECKED)) {
            if (fx_chain_add(&chain, fx_registry_get(i)) < 0) {
                lv_obj_remove_state(s_chip[i], LV_STATE_CHECKED);
                toast_show("max 3 effects", 1200);
            }
        }
    }
    frame_pipeline_set_chain(&chain);
}

static void chip_event_cb(lv_event_t *e)   { (void)e; rebuild_chain(); }

static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    frame_pipeline_set_amount((float)lv_slider_get_value(s) / 100.0f);
}

static void reroll_event_cb(lv_event_t *e)
{
    (void)e;
    uint32_t seed = frame_pipeline_reroll();
    char msg[40];
    snprintf(msg, sizeof(msg), "seed %08lx", (unsigned long)seed);
    toast_show(msg, 900);
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
    lv_obj_set_size(s_settings, FP_OUT_W - 60, 420);
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

    lv_obj_t *row = lv_obj_create(s_settings);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, "Preview quality");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, "Auto\nFull\nHalf");
    lv_dropdown_set_selected(dd, cfg->quality);
    lv_obj_set_width(dd, 200);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(dd, settings_quality_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *close = lv_button_create(s_settings);
    lv_obj_set_size(close, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(close, lv_color_hex(0xE0007A), 0);
    lv_obj_t *cl = lv_label_create(close);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);
}

static void create_control_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, FP_OUT_W, BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_KEY_DIM, 0);      /* video darkened underneath */
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 12, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(bar);
    lv_obj_set_size(row, LV_PCT(100), 112);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(row, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < fx_registry_count() && i < 12; i++) {
        const fx_desc_t *fx = fx_registry_get(i);
        lv_obj_t *b = lv_button_create(row);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_height(b, 50);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xE0007A), LV_STATE_CHECKED);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_label_set_text(l, fx->name);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, chip_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_chip[i] = b;
    }

    lv_obj_t *dice = lv_button_create(row);
    lv_obj_set_height(dice, 50);
    lv_obj_set_style_bg_color(dice, lv_color_hex(0x2060C0), 0);
    lv_obj_t *dl = lv_label_create(dice);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
    lv_label_set_text(dl, LV_SYMBOL_REFRESH);
    lv_obj_center(dl);
    lv_obj_add_event_cb(dice, reroll_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *gear = lv_button_create(row);
    lv_obj_set_height(gear, 50);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x505050), 0);
    lv_obj_t *gl = lv_label_create(gear);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_16, 0);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cap = lv_label_create(bar);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_white(), 0);
    lv_label_set_text(cap, "amount");
    lv_obj_align(cap, LV_ALIGN_BOTTOM_LEFT, 0, -8);

    s_slider = lv_slider_create(bar);
    lv_obj_set_size(s_slider, FP_OUT_W - 24 - 110, 24);
    lv_obj_align(s_slider, LV_ALIGN_BOTTOM_RIGHT, 0, -12);
    lv_slider_set_range(s_slider, 0, 100);
    lv_slider_set_value(s_slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0xE0007A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0xE0007A), LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_label_set_text_fmt(s_status, "cam %lu fps   fx %lu ms   %s   shots %lu",
                          (unsigned long)frame_pipeline_get_fps(),
                          (unsigned long)(frame_pipeline_get_fx_us() / 1000),
                          capture_sd_available() ? "SD ok" : "no SD",
                          (unsigned long)capture_get_count());
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

    s_toast = lv_label_create(scr);
    lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(BAR_H + 30));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(status_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "live screen created (%d effects)", fx_registry_count());
}

void ui_live_on_capture_started(void)
{
    ui_lvgl_flash(2);
}

void ui_live_on_capture_done(const capture_result_t *res)
{
    char msg[96];
    if (res->ok) {
        const char *name = strrchr(res->path, '/');
        snprintf(msg, sizeof(msg), "saved %s  (%lu KB, %lu ms)", name ? name + 1 : res->path,
                 (unsigned long)(res->jpeg_bytes / 1024), (unsigned long)(res->encode_ms + res->write_ms));
    } else {
        snprintf(msg, sizeof(msg), "capture failed: %s", res->error ? res->error : "?");
    }
    if (ui_lvgl_lock(200)) {
        toast_show(msg, 2500);
        ui_lvgl_unlock();
    }
}
