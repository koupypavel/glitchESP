/* Persistent user settings (NVS): orientation, preview quality, JPEG quality. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool flip_h;          /* sensor horizontal mirror */
    bool flip_v;          /* sensor vertical flip */
    uint8_t quality;      /* 0 auto, 1 full, 2 half (fp_quality_t) */
    uint8_t jpeg_quality; /* 50..100 */
    bool photo_hires;     /* at zoom 1: re-read the sensor un-binned for a 1088x1920 photo */
} settings_t;

esp_err_t settings_init(void);            /* load from NVS (defaults if absent) */
const settings_t *settings_get(void);
void settings_set(const settings_t *s);   /* store + apply to camera/pipeline */
void settings_apply(void);                /* push current values to the hardware */

#ifdef __cplusplus
}
#endif
