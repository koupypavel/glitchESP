#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_private/esp_cache_private.h"
#include "frame_pipeline.h"
#include "app_video.h"
#include "auto_exposure.h"
#include "display.h"
#include "ui_lvgl.h"
#include "fx_parallel.h"
#include "capture.h"

static const char *TAG = "pipeline";

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
#define CROP_X          ((FP_CAM_W - FP_OUT_W) / 2)  /* 40 px: centered window */
#define HALF_W          (FP_OUT_W / 2)
#define HALF_H          (FP_OUT_H / 2)
#define HALF_BYTES      (HALF_W * HALF_H * 2)

typedef struct {
    size_t cache_line;
    uint8_t *tmp_buf;                 /* ping-pong buffer for multi-effect chains */
    uint16_t *half_in, *half_tmp, *half_out[2];   /* 360x640 working frames; half_out ping-pongs */
    int half_cur;                                  /* which half_out holds the latest output */
    uint16_t *prev_full;                           /* previous full-res clean output (temporal fx) */
    bool prev_full_valid, prev_half_valid;
    volatile fp_quality_t quality;
    volatile bool last_half;
    uint32_t seq;

    /* recipe shared with the UI; guarded by `lock` */
    portMUX_TYPE lock;
    fp_recipe_t recipe;
    fp_frame_t last;                  /* descriptor of the last submitted frame */

    /* capture request */
    volatile bool capture_pending;
    uint8_t *capture_dst;
    size_t capture_dst_len;
    fp_capture_cb_t capture_cb;
    void *capture_user;

    /* stats */
    uint32_t fps_count, fps_value;
    int64_t fps_t0_us;
    volatile uint32_t fx_us;
    int64_t last_end_us;
    uint64_t acc_wait_us, acc_fx_us, acc_copy_us, acc_ui_us, acc_total_us;
} pipeline_t;

static pipeline_t s_p = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .recipe = { .amount = 0.5f, .seed = 1 },
};

esp_err_t frame_pipeline_init(void)
{
    ESP_RETURN_ON_ERROR(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_p.cache_line), TAG, "cache align");
    size_t len = ALIGN_UP(FP_OUT_BYTES, s_p.cache_line);
    s_p.tmp_buf = heap_caps_aligned_calloc(s_p.cache_line, 1, len, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_p.tmp_buf, ESP_ERR_NO_MEM, TAG, "tmp buffer");
    s_p.half_in  = heap_caps_aligned_calloc(s_p.cache_line, 1, HALF_BYTES, MALLOC_CAP_SPIRAM);
    s_p.half_tmp = heap_caps_aligned_calloc(s_p.cache_line, 1, HALF_BYTES, MALLOC_CAP_SPIRAM);
    s_p.half_out[0] = heap_caps_aligned_calloc(s_p.cache_line, 1, HALF_BYTES, MALLOC_CAP_SPIRAM);
    s_p.half_out[1] = heap_caps_aligned_calloc(s_p.cache_line, 1, HALF_BYTES, MALLOC_CAP_SPIRAM);
    s_p.prev_full = heap_caps_aligned_calloc(s_p.cache_line, 1, len, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_p.half_in && s_p.half_tmp && s_p.half_out[0] && s_p.half_out[1] && s_p.prev_full,
                        ESP_ERR_NO_MEM, TAG, "half/prev buffers");

    fx_chain_clear(&s_p.recipe.chain);
    s_p.recipe.seed = esp_random();
    s_p.fps_t0_us = esp_timer_get_time();
    app_video_set_auto_release(false);
    ESP_LOGI(TAG, "ready: %dx%d window of %dx%d straight into the panel frame buffers, %d effects registered",
             FP_OUT_W, FP_OUT_H, FP_CAM_W, FP_CAM_H, fx_registry_count());
    return ESP_OK;
}

static bool chain_active(const fx_chain_t *c)
{
    for (int i = 0; i < c->count; i++) {
        if (c->slots[i].enabled && c->slots[i].fx) return true;
    }
    return false;
}

typedef struct { const uint16_t *src; uint32_t src_stride; uint16_t *dst; } rows_arg_t;

static void IRAM_ATTR copy_rows(void *a, int y0, int y1)
{
    rows_arg_t *r = a;
    for (int y = y0; y < y1; y++) {
        memcpy(r->dst + (size_t)y * FP_OUT_W, r->src + (size_t)y * r->src_stride, FP_OUT_W * 2);
    }
}

static void copy_window(const uint16_t *src, uint32_t src_stride, uint16_t *dst)
{
    rows_arg_t a = { src, src_stride, dst };
    fx_parallel_rows(copy_rows, &a, 0, FP_OUT_H, dst, FP_OUT_W);
}

/* 2x2 point-sampled downscale of the camera window: reads every other row/pixel. */
static void IRAM_ATTR downscale_rows(void *a, int y0, int y1)
{
    rows_arg_t *r = a;
    const uint16_t *src = r->src; uint32_t src_stride = r->src_stride; uint16_t *dst = r->dst;
    for (int y = y0; y < y1; y++) {
        const uint32_t *s32 = (const uint32_t *)(src + (size_t)(2 * y) * src_stride);
        uint32_t *d32 = (uint32_t *)(dst + (size_t)y * HALF_W);
        for (int x = 0; x < HALF_W; x += 2) {
            uint32_t a = s32[x], b = s32[x + 1];            /* 4 source pixels: a.lo a.hi b.lo b.hi */
            d32[x >> 1] = (a & 0xffff) | (b << 16);        /* keep a.lo and b.lo */
        }
    }
}

static void downscale_2x(const uint16_t *src, uint32_t src_stride, uint16_t *dst)
{
    rows_arg_t a = { src, src_stride, dst };
    fx_parallel_rows(downscale_rows, &a, 0, HALF_H, dst, HALF_W);
}

/* 2x pixel-doubling upscale into a 720x1280 frame buffer, 32-bit writes. */
static void IRAM_ATTR upscale_rows(void *a, int y0, int y1)
{
    rows_arg_t *r = a;
    const uint16_t *src = r->src; uint16_t *dst = r->dst;
    for (int y = y0; y < y1; y++) {
        const uint16_t *s = src + (size_t)y * HALF_W;
        uint32_t *d0 = (uint32_t *)(dst + (size_t)(2 * y) * FP_OUT_W);
        for (int x = 0; x < HALF_W; x++) {
            uint32_t p = s[x];
            d0[x] = p | (p << 16);
        }
        memcpy(d0 + HALF_W, d0, FP_OUT_W * 2);              /* second row = copy of the first */
    }
}

static void upscale_2x(const uint16_t *src, uint16_t *dst)
{
    rows_arg_t a = { src, HALF_W, dst };
    /* split in source rows; each produces two output rows (out stride for cache ops: 2 rows) */
    fx_parallel_rows(upscale_rows, &a, 0, HALF_H, dst, FP_OUT_W * 2);
}

static bool chain_is_temporal(const fx_chain_t *c)
{
    for (int i = 0; i < c->count; i++) {
        if (c->slots[i].enabled && c->slots[i].fx && c->slots[i].fx->temporal) return true;
    }
    return false;
}

static bool chain_prefers_half(const fx_chain_t *c)
{
    /* With two cores a single row-parallel effect stays under ~60 ms at full resolution, so
     * half resolution (35 ms of scaling overhead) only pays for chains and for the effects
     * that cannot be split across cores. */
    int cost = 0;
    for (int i = 0; i < c->count; i++) {
        const fx_desc_t *fx = c->slots[i].fx;
        if (!c->slots[i].enabled || !fx) continue;
        cost += fx->row_parallel ? fx->cost : fx->cost * 2;
        if (fx->temporal) cost += 2;   /* at half res the previous frame is free (ping-pong) */
    }
    return cost >= 5;
}

void frame_pipeline_on_camera_frame(uint8_t *camera_buf, uint8_t cam_idx,
                                    uint32_t cam_w, uint32_t cam_h, size_t camera_buf_len, void *user_data)
{
    (void)camera_buf_len; (void)user_data;
    int64_t t_start = esp_timer_get_time();
    if (s_p.last_end_us) s_p.acc_wait_us += (uint64_t)(t_start - s_p.last_end_us);

    if (cam_w != FP_CAM_W || cam_h != FP_CAM_H) {
        ESP_LOGE(TAG, "unexpected camera frame %ux%u", (unsigned)cam_w, (unsigned)cam_h);
        app_video_release_frame(cam_idx);
        return;
    }
    uint32_t seq = s_p.seq + 1;
    const uint16_t *cam_px = (const uint16_t *)camera_buf + CROP_X;   /* centered window, stride 800 */

    /* Private copy of the recipe so the UI can change it at any time. */
    fp_recipe_t recipe;
    taskENTER_CRITICAL(&s_p.lock);
    recipe = s_p.recipe;
    taskEXIT_CRITICAL(&s_p.lock);
    recipe.frame_no = seq;

    /* software auto-exposure looks at the clean camera window */
    auto_exposure_feed(cam_px, FP_OUT_W, FP_OUT_H, FP_CAM_W);

    /* Render into a free panel frame buffer. */
    int fb_idx = display_acquire_fb();
    uint16_t *fb = display_fb(fb_idx);
    int64_t t0 = esp_timer_get_time();
    if (!chain_active(&recipe.chain)) {
        copy_window(cam_px, FP_CAM_W, fb);
        s_p.fx_us = 0;
        s_p.acc_copy_us += (uint64_t)(esp_timer_get_time() - t0);
    } else {
        fp_quality_t q = s_p.quality;
        bool half = (q == FP_QUALITY_HALF) || (q == FP_QUALITY_AUTO && chain_prefers_half(&recipe.chain));
        bool temporal = chain_is_temporal(&recipe.chain);
        fx_ctx_t ctx = { .seed = recipe.seed, .frame_no = recipe.frame_no, .prev = NULL };
        if (half) {
            downscale_2x(cam_px, FP_CAM_W, s_p.half_in);
            int cur = s_p.half_cur ^ 1;                          /* write the other buffer */
            fx_frame_t prev = { s_p.half_out[s_p.half_cur], HALF_W, HALF_H, HALF_W };
            if (temporal && s_p.prev_half_valid) ctx.prev = &prev;
            fx_frame_t in  = { s_p.half_in,  HALF_W, HALF_H, HALF_W };
            fx_frame_t dst = { s_p.half_out[cur], HALF_W, HALF_H, HALF_W };
            fx_frame_t tmp = { s_p.half_tmp, HALF_W, HALF_H, HALF_W };
            fx_chain_apply(&recipe.chain, &in, &dst, &tmp, &ctx);
            s_p.half_cur = cur;
            s_p.prev_half_valid = true;
            s_p.prev_full_valid = false;
            upscale_2x(s_p.half_out[cur], fb);
        } else {
            fx_frame_t prev = { s_p.prev_full, FP_OUT_W, FP_OUT_H, FP_OUT_W };
            if (temporal && s_p.prev_full_valid) ctx.prev = &prev;
            fx_frame_t in  = { (uint16_t *)cam_px, FP_OUT_W, FP_OUT_H, FP_CAM_W };
            fx_frame_t dst = { fb, FP_OUT_W, FP_OUT_H, FP_OUT_W };
            fx_frame_t tmp = { (uint16_t *)s_p.tmp_buf, FP_OUT_W, FP_OUT_H, FP_OUT_W };
            fx_chain_apply(&recipe.chain, &in, &dst, &tmp, &ctx);
            if (temporal) {
                /* keep a clean copy (before the UI is stamped on) for the next frame */
                copy_window(fb, FP_OUT_W, s_p.prev_full);
                s_p.prev_full_valid = true;
            } else {
                s_p.prev_full_valid = false;
            }
            s_p.prev_half_valid = false;
        }
        s_p.last_half = half;
        s_p.fx_us = (uint32_t)(esp_timer_get_time() - t0);
        s_p.acc_fx_us += s_p.fx_us;
    }
    /* the camera buffer is no longer needed */
    app_video_release_frame(cam_idx);

    /* Snapshot for capture: the clean frame before the UI is stamped on. */
    if (s_p.capture_pending && s_p.capture_dst) {
        memcpy(s_p.capture_dst, fb, FP_OUT_BYTES);
        esp_cache_msync(s_p.capture_dst, FP_OUT_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);   /* encoder buffer is not line-aligned */
        fp_snapshot_t snap = { .buf = s_p.capture_dst, .len = s_p.capture_dst_len, .seq = seq, .recipe = recipe };
        s_p.capture_pending = false;
        if (s_p.capture_cb) s_p.capture_cb(&snap, s_p.capture_user);
    }

    int64_t t1 = esp_timer_get_time();
    bool recording = capture_video_active();
    if (!recording) ui_lvgl_stamp(fb);            /* the video must stay clean */
    s_p.acc_ui_us += (uint64_t)(esp_timer_get_time() - t1);
    display_submit_fb(fb_idx);
    if (recording) capture_video_on_frame(fb_idx, seq);

    s_p.last.px = fb;
    s_p.last.stride_px = FP_OUT_W;
    s_p.last.seq = seq;
    s_p.last.cam_idx = -1;
    s_p.last.recipe = recipe;
    s_p.seq = seq;

    /* stats + a line every 2 s so performance can be read over serial */
    s_p.fps_count++;
    int64_t now = esp_timer_get_time();
    s_p.acc_total_us += (uint64_t)(now - t_start);
    s_p.last_end_us = now;
    if (now - s_p.fps_t0_us >= 2000000) {
        uint32_t n = s_p.fps_count ? s_p.fps_count : 1;
        char chain_str[64] = "";
        for (int i = 0; i < recipe.chain.count; i++) {
            if (recipe.chain.slots[i].enabled && recipe.chain.slots[i].fx) {
                strlcat(chain_str, recipe.chain.slots[i].fx->id, sizeof(chain_str));
                strlcat(chain_str, " ", sizeof(chain_str));
            }
        }
        s_p.fps_value = s_p.fps_count / 2;
        ae_state_t ae; auto_exposure_get(&ae);
        ESP_LOGI(TAG, "cam %lu fps | wait %lu, copy %lu, fx %lu%s, ui %lu, total %lu us | int free %u KB | luma %u expo %lu gain %lu/16 | %s",
                 (unsigned long)s_p.fps_value, (unsigned long)(s_p.acc_wait_us / n),
                 (unsigned long)(s_p.acc_copy_us / n), (unsigned long)(s_p.acc_fx_us / n),
                 s_p.last_half ? " (half)" : "",
                 (unsigned long)(s_p.acc_ui_us / n), (unsigned long)(s_p.acc_total_us / n),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 ae.measured_luma, (unsigned long)ae.exposure_lines, (unsigned long)ae.gain_x16,
                 chain_str[0] ? chain_str : "(no fx)");
        s_p.acc_wait_us = s_p.acc_fx_us = s_p.acc_copy_us = s_p.acc_ui_us = s_p.acc_total_us = 0;
        s_p.fps_count = 0;
        s_p.fps_t0_us = now;
    }
}

const fp_frame_t *frame_pipeline_acquire_latest(void)
{
    return s_p.seq ? &s_p.last : NULL;
}

esp_err_t frame_pipeline_request_capture(uint8_t *dst, size_t dst_len, fp_capture_cb_t cb, void *user)
{
    ESP_RETURN_ON_FALSE(dst && dst_len >= FP_OUT_BYTES, ESP_ERR_INVALID_ARG, TAG, "bad capture buffer");
    ESP_RETURN_ON_FALSE(!s_p.capture_pending, ESP_ERR_INVALID_STATE, TAG, "capture already pending");
    s_p.capture_dst = dst;
    s_p.capture_dst_len = dst_len;
    s_p.capture_cb = cb;
    s_p.capture_user = user;
    s_p.capture_pending = true;
    return ESP_OK;
}

/* ---- recipe control ---- */

void frame_pipeline_set_chain(const fx_chain_t *chain)
{
    taskENTER_CRITICAL(&s_p.lock);
    s_p.recipe.chain = *chain;
    fx_chain_set_amount(&s_p.recipe.chain, s_p.recipe.amount);
    taskEXIT_CRITICAL(&s_p.lock);
}

void frame_pipeline_get_recipe(fp_recipe_t *out)
{
    taskENTER_CRITICAL(&s_p.lock);
    *out = s_p.recipe;
    taskEXIT_CRITICAL(&s_p.lock);
}

void frame_pipeline_set_amount(float amount)
{
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    taskENTER_CRITICAL(&s_p.lock);
    s_p.recipe.amount = amount;
    fx_chain_set_amount(&s_p.recipe.chain, amount);
    taskEXIT_CRITICAL(&s_p.lock);
}

void frame_pipeline_set_seed(uint32_t seed)
{
    taskENTER_CRITICAL(&s_p.lock);
    s_p.recipe.seed = seed;
    taskEXIT_CRITICAL(&s_p.lock);
}

uint32_t frame_pipeline_reroll(void)
{
    uint32_t seed = esp_random();
    frame_pipeline_set_seed(seed);
    return seed;
}

void frame_pipeline_set_quality(fp_quality_t q) { s_p.quality = q; }
bool frame_pipeline_last_was_half(void)         { return s_p.last_half; }

uint32_t frame_pipeline_get_fps(void)   { return s_p.fps_value; }
uint32_t frame_pipeline_get_fx_us(void) { return s_p.fx_us; }
