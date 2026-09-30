/*
 * Camera control: sensor modes and zoom.
 *
 * The OV5647 has 2592x1944 pixels, but the picture on the 720x1280 screen can only ever be
 * a tall, narrow slice of it. Two sensor modes cover the useful range:
 *
 *   WIDE  2x2 binning, 640x960 frame. Four sensor pixels are averaged into one, so the
 *         frame spans the full sensor height (1920 rows). The middle 540x960 is enlarged
 *         1.33x to fill the screen. Widest view, least noise. This is zoom 1.0.
 *   TELE  no binning, 800x1280 frame: every sensor pixel is a screen pixel (the middle
 *         720x1280). A narrower view with real detail. This is zoom 1.5.
 *
 * Zoom below 1.5 uses WIDE, 1.5 and above uses TELE; whatever is left over is done by the
 * pipeline's scaler (digital zoom). Changing mode restarts the camera stream, which takes
 * a fraction of a second, so it happens in its own task and the picture freezes briefly.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_ZOOM_MIN     1.0f
#define CAM_ZOOM_TELE    1.5f     /* from here on the un-binned mode is used */
#define CAM_ZOOM_MAX     6.0f

typedef enum { CAM_MODE_WIDE = 0, CAM_MODE_TELE, CAM_MODE_COUNT } cam_mode_t;

/* After the camera device is open (stream not started): builds the mode tables, programs
 * WIDE, queues the `nbufs` frame buffers and sets up exposure control. */
esp_err_t cam_ctrl_init(int video_fd, void *const *bufs, int nbufs);

/* Starts streaming (camera task on core 1) and the mode-switch task. */
esp_err_t cam_ctrl_start(void);

/* Thread-safe. Returns the zoom that will be shown (clamped; while recording or saving a
 * photo the sensor mode cannot change, so the value stays within the current mode's range). */
float cam_ctrl_set_zoom(float zoom);
float cam_ctrl_get_zoom(void);

cam_mode_t cam_ctrl_mode(void);
const char *cam_ctrl_mode_name(void);      /* for the recipe sidecar */

/* Development aid (serial remote): where the TELE window starts on the sensor. */
void cam_ctrl_set_tele_origin(uint16_t x0, uint16_t y0);

#ifdef __cplusplus
}
#endif
