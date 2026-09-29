/*
 * Direct frame-buffer display path (no LVGL in the video loop).
 *
 * The MIPI-DSI DPI panel owns three 720x1280 RGB565 frame buffers in PSRAM. The camera task
 * acquires a free one, writes the finished video frame into it, stamps the UI layer on top
 * and submits it; the driver switches to it at the next vsync without copying. A callback
 * from the driver tells us when the previously shown buffer may be reused.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_W       720
#define DISP_H       1280
#define DISP_BPP     2
#define DISP_FB_NUM  3

esp_err_t display_init(void);

/* Block until a frame buffer is free, return its index (0..DISP_FB_NUM-1). */
int display_acquire_fb(void);
uint16_t *display_fb(int idx);

/* Hand a filled buffer to the panel. Returns after the driver has queued the switch. */
esp_err_t display_submit_fb(int idx);

esp_lcd_panel_handle_t display_panel(void);

#ifdef __cplusplus
}
#endif
