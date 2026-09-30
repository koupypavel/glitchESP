/*
 * Software auto-exposure for the OV5647 in manual mode.
 * Measures the mean brightness of the displayed frame (subsampled) and steers sensor
 * exposure lines first, gain second, toward a target. Called from the camera task.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t exposure_lines;    /* current */
    uint32_t gain_x16;          /* current, 16 = 1x */
    uint32_t max_exposure_lines;
    uint32_t max_gain_x16;
    uint8_t  target_luma;       /* 0..255 */
    uint8_t  measured_luma;
    bool     locked;            /* true = hold current values (manual mode) */
} ae_state_t;

/* Puts the sensor in manual mode with the given starting point. */
esp_err_t auto_exposure_init(uint32_t start_exposure_lines, uint32_t start_gain_x16, uint32_t max_exposure_lines);

/* Feed one frame (RGB565, w x h with stride). Cheap: samples every 16th pixel of every 16th row. */
void auto_exposure_feed(const uint16_t *px, int w, int h, int stride_px);

/* The sensor mode changed (its registers were rewritten): back to manual mode, new exposure
 * ceiling, and the current exposure rescaled by num/den (line time changed) so brightness
 * carries over. Values are written again after the first frames of the new stream. */
void auto_exposure_restart(uint32_t max_exposure_lines, uint32_t num, uint32_t den);
void auto_exposure_set_max_exposure(uint32_t lines);
void auto_exposure_set_locked(bool locked);
void auto_exposure_get(ae_state_t *out);

#ifdef __cplusplus
}
#endif
