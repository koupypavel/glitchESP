/*
 * LVGL glue for the direct frame-buffer path. LVGL renders widgets into a full-screen
 * "UI layer" in PSRAM via a partial-render flush callback; the camera task stamps that
 * layer onto every video frame. Two colour keys make the layer cheap to composite:
 *   UI_KEY_CLEAR : nothing here, show the video
 *   UI_KEY_DIM   : darken the video 50% (translucent panels)
 * Any other value is copied as-is.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_KEY_CLEAR   0x0001
#define UI_KEY_DIM     0x0002
/* LVGL colours that land exactly on those keys after the RGB565 conversion */
#define UI_COLOR_CLEAR lv_color_hex(0x000008)
#define UI_COLOR_DIM   lv_color_hex(0x000010)

esp_err_t ui_lvgl_init(void);          /* LVGL, display, touch, task on core 0 */
bool ui_lvgl_lock(uint32_t timeout_ms);
void ui_lvgl_unlock(void);

/* Stamp the UI layer onto a 720x1280 RGB565 frame buffer (called from the camera task). */
void ui_lvgl_stamp(uint16_t *fb);

/* Idle handling: poke() on any user action (touch does it by itself). After `after_ms`
 * without one, dim_if_idle() turns the backlight down; the next poke() turns it back up. */
void ui_lvgl_poke(void);
uint32_t ui_lvgl_idle_ms(void);
bool ui_lvgl_dimmed(void);
void ui_lvgl_dim_if_idle(uint32_t after_ms);

/* Full-white flash for the next `frames` frames (shutter feedback). */
void ui_lvgl_flash(int frames);

#ifdef __cplusplus
}
#endif
