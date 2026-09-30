/*
 * Live view UI: full-screen camera canvas, status bar, capture flash and toast.
 * All lv_* calls happen inside the LVGL task or under bsp_display_lock().
 */
#pragma once

#include <stdbool.h>
#include "capture.h"
#include "frame_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the live screen. Call while holding bsp_display_lock(). */
void ui_live_create(void);

/* Thread-safe: shows the white flash and later the result toast. */
void ui_live_on_capture_started(void);
void ui_live_on_capture_done(const capture_result_t *res);
void ui_live_on_video_started(void);
void ui_live_on_video_done(const video_result_t *res);
void ui_live_on_video_error(const char *msg);

/* Call with the LVGL lock held (from LVGL callbacks, or between ui_lvgl_lock/unlock). */
void ui_live_set_visible(bool visible);                 /* hide the camera controls (gallery) */
void ui_live_apply_recipe(const fp_recipe_t *recipe);   /* chips, slider and pipeline follow it */
void ui_live_toast(const char *msg, uint32_t ms);

/* Thread-safe: operate the on-screen controls from another task (serial remote). */
void ui_live_set_zoom(float zoom);
bool ui_live_toggle_effect(const char *id);   /* false: unknown id, or the chain is full */
void ui_live_set_amount(float amount);
bool ui_live_editor(const char *fx_id, bool open);      /* parameter editor; "" = first active effect */
bool ui_live_set_param(const char *fx_id, const char *param_id, float value);
void ui_live_set_bar_hidden(bool hidden);               /* the control bar; the choice is remembered */
bool ui_live_preset(int slot, bool save);     /* slot 0..PRESET_SLOTS-1: store the current look, or recall */
void ui_live_show_panel(int panel, bool show); /* open / close a panel: 0 presets, 1 settings */

#ifdef __cplusplus
}
#endif
