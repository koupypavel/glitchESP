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

#ifdef __cplusplus
}
#endif
