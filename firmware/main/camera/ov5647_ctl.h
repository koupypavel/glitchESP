/*
 * Direct OV5647 control over the shared board I2C bus (SCCB address 0x36), alongside the
 * esp_video driver: manual exposure/gain, frame timing, and register diagnostics.
 *
 * Why: the sensor mode table programs a slow readout (~16 fps) and its built-in AEC does not
 * adapt in this configuration. We run exposure/gain manually and pick faster frame timing.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame timing actually used (line length in pixel clocks, frame length in lines). */
#define OV5647_HTS_FAST     2394   /* readout window is 2109 px wide + blanking; 1896 stalls the sensor */
#define OV5647_VTS_FAST     1470   /* readout window is 1447 lines tall; 1320 stalls the sensor */
#define OV5647_VTS_DEFAULT  1732
#define OV5647_EXPO_MAX(vts)  ((vts) - 8)

/* White balance (sensor side). Gains are 12-bit, 0x400 = 1x, in the order R, G, B (high byte
 * first). CURRENT reads back what the auto white balance has settled on. */
#define OV5647_REG_WB_CTRL     0x5180
#define OV5647_WB_MANUAL_EN    0x08
#define OV5647_REG_WB_MANUAL   0x5186   /* 6 bytes */
#define OV5647_REG_WB_CURRENT  0x5190   /* 6 bytes, read only */

esp_err_t ov5647_ctl_init(void);
esp_err_t ov5647_ctl_read(uint16_t reg, uint8_t *val);
esp_err_t ov5647_ctl_write(uint16_t reg, uint8_t val);

esp_err_t ov5647_ctl_set_manual(bool manual_aec, bool manual_agc);
esp_err_t ov5647_ctl_set_exposure_lines(uint32_t lines);
/* gain in 1/16 steps: 16 = 1x, 32 = 2x ... up to 1023 (~64x) */
esp_err_t ov5647_ctl_set_gain_x16(uint32_t gain_x16);
esp_err_t ov5647_ctl_set_timing(uint16_t hts, uint16_t vts);   /* 0 = leave unchanged */

void ov5647_ctl_dump(void);

#ifdef __cplusplus
}
#endif
