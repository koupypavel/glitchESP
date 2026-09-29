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

static const char *TAG = "pipeline";

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))
#define CROP_X          ((FP_CAM_W - FP_OUT_W) / 2)  /* 40 px: centered window */

typedef struct {
    size_t cache_line;
    uint8_t *tmp_buf;                 /* ping-pong buffer for multi-effect chains */
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
    /* effect time per (camera buffer, frame buffer) pair, to spot cache-conflict pairs */
    uint32_t pair_min[FP_CAM_BUFS][DISP_FB_NUM], pair_max[FP_CAM_BUFS][DISP_FB_NUM], pair_n[FP_CAM_BUFS][DISP_FB_NUM];
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

static void IRAM_ATTR copy_window(const uint16_t *src, uint32_t src_stride, uint16_t *dst)
{
    for (int y = 0; y < FP_OUT_H; y++) {
        memcpy(dst + (size_t)y * FP_OUT_W, src + (size_t)y * src_stride, FP_OUT_W * 2);
    }
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
        fx_frame_t in  = { (uint16_t *)cam_px, FP_OUT_W, FP_OUT_H, FP_CAM_W };
        fx_frame_t dst = { fb, FP_OUT_W, FP_OUT_H, FP_OUT_W };
        fx_frame_t tmp = { (uint16_t *)s_p.tmp_buf, FP_OUT_W, FP_OUT_H, FP_OUT_W };
        fx_ctx_t ctx = { .seed = recipe.seed, .frame_no = recipe.frame_no, .prev = NULL };
        fx_chain_apply(&recipe.chain, &in, &dst, &tmp, &ctx);
        s_p.fx_us = (uint32_t)(esp_timer_get_time() - t0);
        s_p.acc_fx_us += s_p.fx_us;
        if (cam_idx < FP_CAM_BUFS) {
            uint32_t *mn = &s_p.pair_min[cam_idx][fb_idx], *mx = &s_p.pair_max[cam_idx][fb_idx];
            if (s_p.pair_n[cam_idx][fb_idx]++ == 0 || s_p.fx_us < *mn) *mn = s_p.fx_us;
            if (s_p.fx_us > *mx) *mx = s_p.fx_us;
        }
    }
    /* the camera buffer is no longer needed */
    app_video_release_frame(cam_idx);

    /* Snapshot for capture: the clean frame before the UI is stamped on. */
    if (s_p.capture_pending && s_p.capture_dst) {
        memcpy(s_p.capture_dst, fb, FP_OUT_BYTES);
        esp_cache_msync(s_p.capture_dst, ALIGN_UP(FP_OUT_BYTES, s_p.cache_line), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        fp_snapshot_t snap = { .buf = s_p.capture_dst, .len = s_p.capture_dst_len, .seq = seq, .recipe = recipe };
        s_p.capture_pending = false;
        if (s_p.capture_cb) s_p.capture_cb(&snap, s_p.capture_user);
    }

    int64_t t1 = esp_timer_get_time();
    ui_lvgl_stamp(fb);
    s_p.acc_ui_us += (uint64_t)(esp_timer_get_time() - t1);
    display_submit_fb(fb_idx);

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
        ESP_LOGI(TAG, "cam %lu fps | wait %lu, copy %lu, fx %lu, ui %lu, total %lu us | luma %u expo %lu gain %lu/16 | %s",
                 (unsigned long)s_p.fps_value, (unsigned long)(s_p.acc_wait_us / n),
                 (unsigned long)(s_p.acc_copy_us / n), (unsigned long)(s_p.acc_fx_us / n),
                 (unsigned long)(s_p.acc_ui_us / n), (unsigned long)(s_p.acc_total_us / n),
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

uint32_t frame_pipeline_get_fps(void)   { return s_p.fps_value; }
uint32_t frame_pipeline_get_fx_us(void) { return s_p.fx_us; }
