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
 *   STILL no binning, 1088x1920: the WIDE view at full sensor resolution, used for one
 *         frame when a photo is taken at zoom 1.0 (see cam_ctrl_take_still).
 *
 * Zoom below 1.5 uses WIDE, 1.5 and above uses TELE; whatever is left over is done by the
 * pipeline's scaler (digital zoom). Changing mode restarts the camera stream, which takes
 * a fraction of a second, so it happens in its own task and the picture freezes briefly.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_ZOOM_MIN     1.0f
#define CAM_ZOOM_TELE    1.5f     /* from here on the un-binned mode is used */
#define CAM_ZOOM_MAX     6.0f

typedef enum { CAM_MODE_WIDE = 0, CAM_MODE_TELE, CAM_MODE_COUNT } cam_mode_t;

/* After the camera device is open (stream not started): builds the mode tables, programs
 * WIDE and sets up exposure control. `block` is one cache-line-aligned PSRAM region of
 * preview_bufs * preview_buf_len bytes: the preview uses it as that many frame buffers, a
 * high-resolution still re-uses the same memory as two 1088x1920 buffers. */
esp_err_t cam_ctrl_init(int video_fd, void *block, size_t preview_buf_len, int preview_bufs);

/* Starts streaming (camera task on core 1) and the mode-switch task. */
esp_err_t cam_ctrl_start(void);

/* Thread-safe. Returns the zoom that will be shown (clamped; while recording or saving a
 * photo the sensor mode cannot change, so the value stays within the current mode's range). */
float cam_ctrl_set_zoom(float zoom);
float cam_ctrl_get_zoom(void);

cam_mode_t cam_ctrl_mode(void);
const char *cam_ctrl_mode_name(void);      /* for the recipe sidecar */

/*
 * High-resolution still: at zoom 1.0 the preview shows binned pixels, so for a photo the
 * sensor is switched to an un-binned 1088x1920 mode of the same view, one frame is taken,
 * the effect chain runs on it at that size, and the preview resumes (about a second in
 * all). Not available when zoomed in (the preview already shows every sensor pixel there),
 * or with effects that need the previous frame.
 * The result is reported through the capture module's done callback, like any other still.
 * `dump` sends the JPEG over the serial port instead of saving it.
 */
bool cam_ctrl_still_available(void);
esp_err_t cam_ctrl_take_still(bool dump);

/* Development aid (serial remote): where the TELE window starts on the sensor. */
void cam_ctrl_set_tele_origin(uint16_t x0, uint16_t y0);

#ifdef __cplusplus
}
#endif
