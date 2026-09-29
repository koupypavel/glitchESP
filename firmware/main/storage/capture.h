/*
 * Capture: snapshot the live frame, hardware-encode it to JPEG and write it to the microSD
 * card together with a JSON "recipe" sidecar. Runs its own task so the preview never stalls.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTURE_DIR         "/sdcard/GLITCH"
#define CAPTURE_JPEG_QUALITY 90

typedef struct {
    bool ok;
    char path[64];          /* e.g. /sdcard/GLITCH/IMG_0001.jpg */
    uint32_t jpeg_bytes;
    uint32_t encode_ms;
    uint32_t write_ms;
    const char *error;      /* human readable, valid when !ok */
} capture_result_t;

typedef void (*capture_done_cb_t)(const capture_result_t *res, void *user);

/* Mounts nothing itself: call bsp_sdcard_mount() before. sd_mounted tells us whether to try. */
esp_err_t capture_init(bool sd_mounted, capture_done_cb_t done_cb, void *user);

/* Trigger a shot. Returns ESP_ERR_INVALID_STATE if one is already in flight. */
esp_err_t capture_trigger(void);

bool capture_sd_available(void);
void capture_set_serial_dump(bool enable);   /* no SD card: dump JPEG as base64 over serial */
uint32_t capture_get_count(void);

#ifdef __cplusplus
}
#endif
