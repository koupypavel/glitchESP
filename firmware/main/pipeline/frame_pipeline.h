/*
 * Frame pipeline: camera frame -> view rectangle -> [scale] -> [effect chain] -> panel
 * frame buffer (+ UI layer stamped on top).
 *
 * The camera delivers RGB565 frames whose size depends on the sensor mode (640x960 binned or
 * 800x1280, see camera/cam_ctrl.h). A centered rectangle with the screen's aspect ratio is
 * shown; it is copied when it is exactly 720x1280 and scaled otherwise. Everything is
 * rendered straight into one of the panel's own frame buffers, which the panel then
 * switches to, and the camera buffer goes back to the driver within the same callback.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "fx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FP_CAM_W        800
#define FP_CAM_H        1280
#define FP_OUT_W        720
#define FP_OUT_H        1280
#define FP_OUT_BPP      2                         /* RGB565 */
#define FP_OUT_BYTES    (FP_OUT_W * FP_OUT_H * FP_OUT_BPP)
#define FP_RING_LEN     3                         /* effect output buffers */
#define FP_CAM_BUFS     5                         /* camera buffers; display holds <= 3 so the driver always has >= 2 */

/* Everything needed to reproduce a frame's look. */
typedef struct {
    fx_chain_t chain;
    float amount;          /* last value of the one-knob control, 0..1 */
    uint32_t seed;
    uint32_t frame_no;     /* value fed to the effects' RNG for this frame */
    float zoom;            /* digital zoom the frame was taken with, 1.0 = none */
} fp_recipe_t;

/* A displayable frame: FP_OUT_W x FP_OUT_H pixels starting at `px`, rows `stride_px` apart. */
typedef struct {
    uint16_t *px;
    uint32_t stride_px;
    uint32_t seq;          /* camera frame sequence number */
    int cam_idx;           /* camera buffer index this frame lives in, or -1 for a ring buffer */
    fp_recipe_t recipe;    /* recipe that produced this frame */
} fp_frame_t;

/* Snapshot handed to the capture path: a contiguous 720x1280 copy plus its recipe. */
typedef struct {
    uint8_t *buf;
    size_t len;
    uint32_t seq;
    fp_recipe_t recipe;
} fp_snapshot_t;

typedef void (*fp_capture_cb_t)(const fp_snapshot_t *snapshot, void *user);

esp_err_t frame_pipeline_init(void);

/* Camera callback compatible with app_video_register_frame_operation_cb(). */
void frame_pipeline_on_camera_frame(uint8_t *camera_buf, uint8_t camera_buf_index,
                                    uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                    size_t camera_buf_len, void *user_data);

/* Latest completed display frame (NULL if none yet). Safe to call from the LVGL task. */
const fp_frame_t *frame_pipeline_acquire_latest(void);

/* Request a snapshot of the next completed frame into `dst` (len >= FP_OUT_BYTES). */
esp_err_t frame_pipeline_request_capture(uint8_t *dst, size_t dst_len, fp_capture_cb_t cb, void *user);

/* The next requested capture is taken after the UI layer is stamped on (a screenshot). */
void frame_pipeline_capture_with_ui(bool with_ui);

/* ---- recipe control (thread-safe, callable from the UI) ---- */
void frame_pipeline_set_recipe(const fp_recipe_t *r);     /* chain with its parameters, amount, seed (presets) */
void frame_pipeline_set_chain(const fx_chain_t *chain);   /* replaces the chain, keeps amount/seed */
void frame_pipeline_get_recipe(fp_recipe_t *out);        /* frame_no = the frame on screen */
bool frame_pipeline_chain_is_temporal(void);              /* an active effect needs the previous frame */
void frame_pipeline_set_amount(float amount);             /* re-maps every slot via from_amount */
void frame_pipeline_set_seed(uint32_t seed);
uint32_t frame_pipeline_reroll(void);                     /* new random seed, returns it */

/* ---- view: which part of the camera frame is shown ----
 * The shown rectangle is centered, has the screen's aspect ratio, and is
 * (view base / zoom) camera pixels wide. The view base depends on the sensor mode (how many
 * of its pixels span the screen at 1x), so camera/cam_ctrl.c sets both; use cam_ctrl_set_zoom()
 * from the UI. */
float frame_pipeline_set_zoom(float zoom);                /* returns the value set */
float frame_pipeline_get_zoom(void);
void frame_pipeline_set_view_base(uint32_t cam_px_across_at_1x);

/* ---- preview quality ---- */
typedef enum { FP_QUALITY_AUTO = 0, FP_QUALITY_FULL, FP_QUALITY_HALF } fp_quality_t;
void frame_pipeline_set_quality(fp_quality_t q);
bool frame_pipeline_last_was_half(void);   /* true if the last frame ran the effects at 360x640 */

/* ---- stats ---- */
uint32_t frame_pipeline_get_fps(void);      /* camera frames processed per second */
uint32_t frame_pipeline_get_fx_us(void);    /* effect stage duration of the last frame, microseconds */

#ifdef __cplusplus
}
#endif
