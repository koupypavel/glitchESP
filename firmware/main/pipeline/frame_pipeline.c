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
#define ZOOM_MIN_Q8     256                           /* 1x: the whole view of the wide sensor mode */
#define ZOOM_MAX_Q8     (8 * 256)
#define HALF_W          (FP_OUT_W / 2)
#define HALF_H          (FP_OUT_H / 2)
#define HALF_BYTES      (HALF_W * HALF_H * 2)

typedef struct {
    size_t cache_line;
    uint8_t *tmp_buf;                 /* ping-pong buffer for multi-effect chains */
    uint16_t *half_in, *half_tmp, *half_out[2];   /* 360x640 working frames; half_out ping-pongs */
    int half_cur;                                  /* which half_out holds the latest output */
    uint16_t *prev_full;                           /* previous full-res clean output (temporal fx) */
    uint16_t *zoom_buf;                            /* zoomed full-res input for the effect chain */
    volatile uint32_t zoom_q8;                     /* zoom, 256 = 1x */
    volatile uint32_t view_base_w;                 /* camera pixels across the screen at 1x (sensor mode dependent) */
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
    .recipe = { .amount = 0.5f, .seed = 1, .zoom = 1.0f },
    .zoom_q8 = ZOOM_MIN_Q8,
    .view_base_w = FP_OUT_W,
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
    s_p.zoom_buf = heap_caps_aligned_calloc(s_p.cache_line, 1, len, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_p.half_in && s_p.half_tmp && s_p.half_out[0] && s_p.half_out[1] && s_p.prev_full && s_p.zoom_buf,
                        ESP_ERR_NO_MEM, TAG, "half/prev/zoom buffers");

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

/*
 * Bilinear scaler for digital zoom: any rectangle of the camera frame -> any output size.
 *
 * Done in two separable steps so the expensive part is shared between output rows:
 *   1. horizontal: a source row is resampled to the output width once, into a small line
 *      buffer on the stack (on-chip RAM, so it is fast to re-read);
 *   2. vertical: each output row is a weighted mix of two such lines.
 * When zooming in, several output rows fall between the same two source rows and reuse
 * the same two lines, so step 1 runs only about (output rows / zoom) times.
 *
 * Pixels are mixed in a "spread" form: the RGB565 value is copied into a 32-bit word with
 * green moved to the upper half (mask 0x07E0F81F). That leaves 5 empty bits above every
 * channel, so one multiplication by a 0..32 weight scales all three channels at once.
 */
#define SPREAD_MASK 0x07E0F81Fu
#define SPREAD(p)   ((((uint32_t)(p)) | ((uint32_t)(p) << 16)) & SPREAD_MASK)

typedef struct {
    const uint16_t *src; uint32_t src_stride; int src_h;   /* whole camera frame */
    int32_t y0_fp, step_y_fp;                               /* 16.16 source row of output row 0, step */
    uint16_t *dst; int dst_w;
    const uint16_t *xi;                                     /* per output column: left source pixel... */
    const uint8_t *xw;                                      /* ...and the 0..31 weight of its right neighbour */
} scale_arg_t;

static uint16_t s_scale_xi[FP_OUT_W];
static uint8_t  s_scale_xw[FP_OUT_W];

static inline void scale_hline(const scale_arg_t *s, int row, uint32_t *line)
{
    const uint16_t *r = s->src + (size_t)row * s->src_stride;
    const uint16_t *xi = s->xi; const uint8_t *xw = s->xw;
    for (int x = 0; x < s->dst_w; x++) {
        uint32_t i = xi[x], w = xw[x];
        uint32_t a = SPREAD(r[i]), b = SPREAD(r[i + 1]);
        line[x] = ((a * (32 - w) + b * w) >> 5) & SPREAD_MASK;
    }
}

static void IRAM_ATTR scale_rows(void *arg, int y0, int y1)
{
    const scale_arg_t *s = arg;
    uint32_t line_a[FP_OUT_W], line_b[FP_OUT_W];            /* 5.8 KB of stack per core */
    uint32_t *l0 = line_a, *l1 = line_b;
    int have0 = -1, have1 = -1;                             /* source rows held in l0 / l1 */
    int max_row = s->src_h - 2;
    for (int y = y0; y < y1; y++) {
        int32_t sy = s->y0_fp + y * s->step_y_fp;
        if (sy < 0) sy = 0;
        int r = sy >> 16;
        uint32_t w1 = ((uint32_t)sy >> 11) & 31;
        if (r > max_row) { r = max_row; w1 = 31; }
        if (have0 != r) {
            if (have1 == r) {                               /* moved down one row: reuse the lower line */
                uint32_t *t = l0; l0 = l1; l1 = t;
                have0 = r; have1 = -1;
            } else {
                scale_hline(s, r, l0);
                have0 = r;
            }
        }
        if (have1 != r + 1) {
            scale_hline(s, r + 1, l1);
            have1 = r + 1;
        }
        uint32_t w0 = 32 - w1;
        uint32_t *d32 = (uint32_t *)(s->dst + (size_t)y * s->dst_w);
        for (int x = 0; x < s->dst_w; x += 2) {
            uint32_t p = ((l0[x] * w0 + l1[x] * w1) >> 5) & SPREAD_MASK;
            uint32_t q = ((l0[x + 1] * w0 + l1[x + 1] * w1) >> 5) & SPREAD_MASK;
            d32[x >> 1] = ((p | (p >> 16)) & 0xffff) | ((q | (q >> 16)) << 16);
        }
    }
}

/* The part of the camera frame that is shown: centered, FP_OUT aspect, shrinking with zoom. */
typedef struct { int x, y, w, h; } view_rect_t;

static view_rect_t view_rect(uint32_t cam_w, uint32_t cam_h, uint32_t base_w, uint32_t zoom_q8)
{
    view_rect_t v;
    v.w = (int)((base_w * 256u) / zoom_q8);
    v.h = (int)(((uint64_t)base_w * 256u * FP_OUT_H) / ((uint64_t)zoom_q8 * FP_OUT_W));
    /* a zoom the current sensor mode cannot cover (mid mode switch): show all it has */
    if (v.w > (int)cam_w) { v.h = (int)((int64_t)v.h * cam_w / v.w); v.w = (int)cam_w; }
    if (v.h > (int)cam_h) { v.w = (int)((int64_t)v.w * cam_h / v.h); v.h = (int)cam_h; }
    v.w &= ~1;
    v.h &= ~1;
    v.x = (((int)cam_w - v.w) / 2) & ~1;                    /* even: keeps 32-bit reads aligned */
    v.y = ((int)cam_h - v.h) / 2;
    return v;
}

/*
 * Fast path for the wide sensor mode at zoom 1.0: exactly 3 source pixels -> 4 output pixels
 * in both directions (540x960 -> 720x1280).
 *
 * Sampling the source every 0.75 pixel gives positions 0, 0.75, 1.5, 2.25, then the pattern
 * repeats, so the only blend weights are 0, 1/4, 1/2 and 3/4. Those need no multiplication:
 * the average of two RGB565 pixels is
 *     (a & b) + (((a ^ b) & 0xF7DE) >> 1)
 * (shared bits, plus half of the differing bits with each channel's lowest bit masked so
 * nothing spills into the neighbouring channel), and a 1/4 : 3/4 mix is two averages. The
 * same expression works on a 32-bit word holding two pixels. One average rounds down and
 * the other up so the picture does not get darker.
 */
#define AVG_MASK 0xF7DEF7DEu
static inline __attribute__((always_inline)) uint32_t avg_dn(uint32_t a, uint32_t b) { return (a & b) + (((a ^ b) & AVG_MASK) >> 1); }
static inline __attribute__((always_inline)) uint32_t avg_up(uint32_t a, uint32_t b) { return (a | b) - (((a ^ b) & AVG_MASK) >> 1); }

typedef struct {
    const uint16_t *src; uint32_t src_stride;       /* top-left pixel of the view */
    int src_w, src_h;                               /* view size (multiples of 3) */
    uint16_t *dst; int dst_w;
} scale43_arg_t;

/* One source row -> dst_w packed pixels (two per word). */
static inline __attribute__((always_inline)) void scale43_hline(const scale43_arg_t *s, int row, uint32_t *line)
{
    const uint16_t *r = s->src + (size_t)row * s->src_stride;
    int groups = s->src_w / 3;
    for (int g = 0; g < groups; g++, r += 3) {
        uint32_t s0 = r[0], s1 = r[1], s2 = r[2], s3 = r[3];
        uint32_t o1 = avg_up(avg_dn(s0, s1), s1);           /* 1/4 s0 + 3/4 s1 */
        uint32_t o2 = avg_dn(s1, s2);
        uint32_t o3 = avg_dn(avg_up(s2, s3), s2);           /* 3/4 s2 + 1/4 s3 */
        line[2 * g]     = s0 | (o1 << 16);
        line[2 * g + 1] = o2 | (o3 << 16);
    }
}

static void IRAM_ATTR scale43_rows(void *arg, int y0, int y1)
{
    const scale43_arg_t *s = arg;
    uint32_t line_a[FP_OUT_W / 2], line_b[FP_OUT_W / 2];    /* two scaled source rows, 2.9 KB of stack */
    uint32_t *la = line_a, *lb = line_b;
    int have_a = -1, have_b = -1;
    int words = s->dst_w / 2, last = s->src_h - 1;
    for (int y = y0; y < y1; y++) {
        int phase = y & 3;
        int ra = (y >> 2) * 3 + (phase ? phase - 1 : 0);    /* upper source row of the pair */
        int rb = ra + 1 > last ? last : ra + 1;
        if (have_a != ra) {
            if (have_b == ra) { uint32_t *t = la; la = lb; lb = t; have_a = ra; have_b = -1; }
            else              { scale43_hline(s, ra, la); have_a = ra; }
        }
        uint32_t *d = (uint32_t *)(s->dst + (size_t)y * s->dst_w);
        if (phase == 0) {
            memcpy(d, la, (size_t)words * 4);
            continue;
        }
        if (have_b != rb) { scale43_hline(s, rb, lb); have_b = rb; }
        if (phase == 1)      for (int i = 0; i < words; i++) d[i] = avg_up(avg_dn(la[i], lb[i]), lb[i]);
        else if (phase == 2) for (int i = 0; i < words; i++) d[i] = avg_dn(la[i], lb[i]);
        else                 for (int i = 0; i < words; i++) d[i] = avg_dn(avg_up(la[i], lb[i]), la[i]);
    }
}

/*
 * The companion for the half-resolution effect path: 3 source pixels -> 2 output pixels
 * (540x960 -> 360x640). Output pixels sit at source positions 0.25 and 1.75 of each group
 * of three, i.e. 3/4 : 1/4 mixes again. Rows are mixed first (two pixels per word, straight
 * from the camera frame), then the mixed row is squeezed horizontally.
 */
static void IRAM_ATTR scale32_rows(void *arg, int y0, int y1)
{
    const scale43_arg_t *s = arg;
    uint32_t line[(FP_OUT_W * 3 / 4) / 2];                  /* one mixed source row, up to 540 px */
    int words = s->src_w / 2, groups = s->src_w / 3;
    for (int y = y0; y < y1; y++) {
        const uint16_t *ra = s->src + (size_t)((y >> 1) * 3 + (y & 1)) * s->src_stride;
        const uint32_t *a = (const uint32_t *)ra, *b = (const uint32_t *)(ra + s->src_stride);
        if (!(y & 1)) for (int i = 0; i < words; i++) line[i] = avg_dn(avg_up(a[i], b[i]), a[i]);   /* 3/4 a + 1/4 b */
        else          for (int i = 0; i < words; i++) line[i] = avg_up(avg_dn(a[i], b[i]), b[i]);   /* 1/4 a + 3/4 b */
        const uint16_t *p = (const uint16_t *)line;
        uint32_t *d = (uint32_t *)(s->dst + (size_t)y * s->dst_w);
        for (int g = 0; g < groups; g++, p += 3) {
            uint32_t s0 = p[0], s1 = p[1], s2 = p[2];
            d[g] = avg_dn(avg_up(s0, s1), s0) | (avg_up(avg_dn(s1, s2), s2) << 16);
        }
    }
}

/* Scale `v` (a rectangle of the camera frame) to a dst_w x dst_h image. */
static void scale_view(const uint16_t *cam, uint32_t cam_w, uint32_t cam_h, const view_rect_t *v,
                       uint16_t *dst, int dst_w, int dst_h)
{
    if (v->w * 2 == dst_w * 3 && v->h * 2 == dst_h * 3 && v->w % 6 == 0 && !(v->x & 1) && !(cam_w & 1) && dst_w <= FP_OUT_W / 2) {
        scale43_arg_t a32 = { cam + (size_t)v->y * cam_w + v->x, cam_w, v->w, v->h, dst, dst_w };
        fx_parallel_rows(scale32_rows, &a32, 0, dst_h, dst, (uint32_t)dst_w);
        return;
    }
    if (v->w * 4 == dst_w * 3 && v->h * 4 == dst_h * 3 && v->w % 3 == 0 && v->x + v->w < (int)cam_w) {
        scale43_arg_t a43 = { cam + (size_t)v->y * cam_w + v->x, cam_w, v->w, v->h, dst, dst_w };
        fx_parallel_rows(scale43_rows, &a43, 0, dst_h, dst, (uint32_t)dst_w);
        return;
    }
    /* Pixel centres: output pixel i sits at source position x + (i + 0.5) * step - 0.5. */
    int32_t step_x = (int32_t)(((int64_t)v->w << 16) / dst_w);
    int32_t step_y = (int32_t)(((int64_t)v->h << 16) / dst_h);
    int32_t x0 = (v->x << 16) + step_x / 2 - 0x8000;
    int32_t x_max = ((int32_t)(cam_w - 1) << 16) - 1;       /* so that pixel i + 1 always exists */
    for (int i = 0; i < dst_w; i++) {
        int32_t sx = x0 + i * step_x;
        if (sx < 0) sx = 0;
        if (sx > x_max) sx = x_max;
        s_scale_xi[i] = (uint16_t)(sx >> 16);
        s_scale_xw[i] = (uint8_t)((sx >> 11) & 31);
    }
    scale_arg_t a = {
        .src = cam, .src_stride = cam_w, .src_h = (int)cam_h,
        .y0_fp = (v->y << 16) + step_y / 2 - 0x8000, .step_y_fp = step_y,
        .dst = dst, .dst_w = dst_w, .xi = s_scale_xi, .xw = s_scale_xw,
    };
    fx_parallel_rows(scale_rows, &a, 0, dst_h, dst, (uint32_t)dst_w);
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

    if (cam_w < 64 || cam_h < 64) {
        ESP_LOGE(TAG, "unexpected camera frame %ux%u", (unsigned)cam_w, (unsigned)cam_h);
        app_video_release_frame(cam_idx);
        return;
    }
    uint32_t seq = s_p.seq + 1;
    /* What is shown: a centered rectangle of the camera frame. At 1x it is exactly
     * 720x1280 and is used in place; anything else goes through the scaler. */
    uint32_t zoom_q8 = s_p.zoom_q8;
    view_rect_t view = view_rect(cam_w, cam_h, s_p.view_base_w, zoom_q8);
    bool scaled = view.w != FP_OUT_W || view.h != FP_OUT_H;
    const uint16_t *cam = (const uint16_t *)camera_buf;
    const uint16_t *cam_px = cam + (size_t)view.y * cam_w + view.x;   /* top-left of the view, stride cam_w */

    /* Private copy of the recipe so the UI can change it at any time. */
    fp_recipe_t recipe;
    taskENTER_CRITICAL(&s_p.lock);
    recipe = s_p.recipe;
    taskEXIT_CRITICAL(&s_p.lock);
    recipe.frame_no = seq;
    recipe.zoom = (float)zoom_q8 / 256.0f;

    /* software auto-exposure meters the part of the camera frame that is shown */
    auto_exposure_feed(cam_px, view.w, view.h, (int)cam_w);

    /* Render into a free panel frame buffer. */
    int fb_idx = display_acquire_fb();
    uint16_t *fb = display_fb(fb_idx);
    int64_t t0 = esp_timer_get_time();
    if (!chain_active(&recipe.chain)) {
        if (scaled) scale_view(cam, cam_w, cam_h, &view, fb, FP_OUT_W, FP_OUT_H);
        else        copy_window(cam_px, cam_w, fb);
        s_p.fx_us = 0;
        s_p.acc_copy_us += (uint64_t)(esp_timer_get_time() - t0);
    } else {
        fp_quality_t q = s_p.quality;
        bool half = (q == FP_QUALITY_HALF) || (q == FP_QUALITY_AUTO && chain_prefers_half(&recipe.chain));
        bool temporal = chain_is_temporal(&recipe.chain);
        /* A photo is worth one slow frame: render it at full resolution. (Not with temporal
         * effects, whose trail lives in the half-resolution history.) */
        if (half && s_p.capture_pending && !temporal) half = false;
        fx_ctx_t ctx = { .seed = recipe.seed, .frame_no = recipe.frame_no, .prev = NULL };
        if (half) {
            if (scaled) scale_view(cam, cam_w, cam_h, &view, s_p.half_in, HALF_W, HALF_H);
            else        downscale_2x(cam_px, cam_w, s_p.half_in);
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
            fx_frame_t in  = { (uint16_t *)cam_px, FP_OUT_W, FP_OUT_H, cam_w };
            if (scaled) {
                scale_view(cam, cam_w, cam_h, &view, s_p.zoom_buf, FP_OUT_W, FP_OUT_H);
                in.px = s_p.zoom_buf;
                in.stride_px = FP_OUT_W;
            }
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
        ESP_LOGI(TAG, "cam %lu fps | zoom %.2f | wait %lu, copy %lu, fx %lu%s, ui %lu, total %lu us | int free %u KB | luma %u expo %lu gain %lu/16 | %s",
                 (unsigned long)s_p.fps_value, (double)recipe.zoom, (unsigned long)(s_p.acc_wait_us / n),
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
    out->frame_no = s_p.seq;          /* the frame on screen: same random pattern if re-rendered */
}

bool frame_pipeline_chain_is_temporal(void)
{
    fp_recipe_t r;
    frame_pipeline_get_recipe(&r);
    return chain_is_temporal(&r.chain);
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

float frame_pipeline_set_zoom(float zoom)
{
    uint32_t q = (uint32_t)(zoom * 256.0f + 0.5f);
    if (q < ZOOM_MIN_Q8) q = ZOOM_MIN_Q8;
    if (q > ZOOM_MAX_Q8) q = ZOOM_MAX_Q8;
    s_p.zoom_q8 = q;
    taskENTER_CRITICAL(&s_p.lock);
    s_p.recipe.zoom = (float)q / 256.0f;
    taskEXIT_CRITICAL(&s_p.lock);
    return (float)q / 256.0f;
}

float frame_pipeline_get_zoom(void) { return (float)s_p.zoom_q8 / 256.0f; }

void frame_pipeline_set_view_base(uint32_t cam_px_across_at_1x)
{
    s_p.view_base_w = cam_px_across_at_1x;
}

void frame_pipeline_set_quality(fp_quality_t q) { s_p.quality = q; }
bool frame_pipeline_last_was_half(void)         { return s_p.last_half; }

uint32_t frame_pipeline_get_fps(void)   { return s_p.fps_value; }
uint32_t frame_pipeline_get_fx_us(void) { return s_p.fx_us; }
