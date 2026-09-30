/*
 * Live view UI: full-screen camera canvas, status bar, capture flash and toast.
 * All lv_* calls happen inside the LVGL task or under bsp_display_lock().
 */
#pragma once

#include <stdbool.h>
#include "capture.h"

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

/* Thread-safe: operate the on-screen controls from another task (serial remote). */
void ui_live_set_zoom(float zoom);
bool ui_live_toggle_effect(const char *id);   /* false: unknown id, or the chain is full */
void ui_live_set_amount(float amount);

#ifdef __cplusplus
}
#endif
