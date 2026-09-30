/*
 * On-device gallery: look through the photos and videos in /sdcard/GLITCH.
 *
 * Opening it stops the camera and borrows its frame-buffer memory for the JPEG decoder
 * (see cam_ctrl_pause); closing it restarts the preview. Photos of any size up to
 * 1088x1920 are decoded by the hardware JPEG decoder and scaled to the screen; videos
 * (Motion-JPEG AVI, as recorded here) show their first frame and can be played.
 * "Use look" reads the recipe from the picture's .json sidecar and makes it the current
 * effect setup.
 *
 * All the work happens in the gallery task; the functions below only post a command and
 * may be called from any task.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gallery_init(void);              /* task and command queue; before the UI is created */
void gallery_create_ui(lv_obj_t *screen);  /* hidden widgets; call with the LVGL lock held */

bool gallery_active(void);
void gallery_open(void);
void gallery_close(void);
void gallery_next(void);
void gallery_prev(void);
void gallery_play(void);                   /* start / pause a video */
void gallery_use_look(void);               /* apply the shown picture's recipe and go back to the camera */
void gallery_delete(void);                 /* remove the shown file and its sidecar (no confirmation here) */

#ifdef __cplusplus
}
#endif
