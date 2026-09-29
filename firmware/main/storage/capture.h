/*
 * Capture: stills (hardware JPEG + JSON recipe sidecar) and Motion-JPEG AVI video, both
 * written to the microSD card by a dedicated task so the preview never stalls.
 *
 * Stills snapshot the live frame into an encoder buffer. Video encodes straight from the
 * panel frame buffer that was just submitted (held until the encoder is done).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTURE_DIR          "/sdcard/GLITCH"
#define CAPTURE_JPEG_QUALITY 90
#define VIDEO_JPEG_QUALITY   80
#define VIDEO_MAX_FRAMES     (30 * 60 * 20)   /* 30 min at 20 fps: index memory in PSRAM */

typedef struct {
    bool ok;
    char path[64];          /* e.g. /sdcard/GLITCH/IMG_0001.jpg */
    uint32_t jpeg_bytes;
    uint32_t encode_ms;
    uint32_t write_ms;
    const char *error;      /* human readable, valid when !ok */
} capture_result_t;

typedef struct {
    bool ok;
    char path[64];          /* /sdcard/GLITCH/VID_0001.avi */
    uint32_t frames;
    uint32_t duration_ms;
    uint32_t dropped;       /* frames skipped because the encoder was busy */
    const char *error;
} video_result_t;

typedef void (*capture_done_cb_t)(const capture_result_t *res, void *user);
typedef void (*video_done_cb_t)(const video_result_t *res, void *user);

/* Mounts nothing itself: call bsp_sdcard_mount() before. sd_mounted tells us whether to try. */
esp_err_t capture_init(bool sd_mounted, capture_done_cb_t done_cb, void *user);

/* Trigger a still. Returns ESP_ERR_INVALID_STATE if one is already in flight. */
esp_err_t capture_trigger(void);

/* ---- video ---- */
esp_err_t capture_video_start(video_done_cb_t done_cb, void *user);
esp_err_t capture_video_stop(void);
bool      capture_video_active(void);
uint32_t  capture_video_elapsed_ms(void);
/* Pipeline hook: a frame buffer was just submitted to the display. Encodes it if idle. */
void      capture_video_on_frame(int fb_idx, uint32_t seq);

bool capture_sd_available(void);
void capture_set_serial_dump(bool enable);   /* no SD card: dump JPEG as base64 over serial */
uint32_t capture_get_count(void);

#ifdef __cplusplus
}
#endif
