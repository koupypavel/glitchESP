/*
 * Wave distortion: every output pixel is fetched from a position displaced by sine waves.
 * Horizontal wobble along y gives a "VHS tracking" look, vertical wobble along x a
 * "heat shimmer" look. The phase drifts with the frame number so the preview moves.
 *
 * Row-parallel: each row's noise is seeded from its own index. The horizontal-only case is
 * a rotated row copy (two memcpy segments); the vertical case is per-pixel.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_AMP_X, P_LEN_X, P_AMP_Y, P_LEN_Y, P_SPEED, P_NOISE };

static const fx_param_t s_params[] = {
    { "amp_x", "X amplitude (px)",   0, 200, 24 },
    { "len_x", "X wavelength (px)",  4, 2000, 180 },
    { "amp_y", "Y amplitude (px)",   0, 200, 0 },
    { "len_y", "Y wavelength (px)",  4, 2000, 240 },
    { "speed", "Drift per frame",    0, 1, 0.08f },
    { "noise", "Row noise",          0, 1, 0.2f },
};

static void from_amount(float a, float *p)
{
    p[P_AMP_X] = a * 80.0f;
    p[P_LEN_X] = 300.0f - a * 220.0f;
    p[P_AMP_Y] = 0;                 /* keep the fast horizontal path; vertical is an advanced param */
    p[P_LEN_Y] = 240.0f;
    p[P_SPEED] = 0.05f + a * 0.2f;
    p[P_NOISE] = a * 0.5f;
}

#define LUT_N 1024
static int16_t s_sin[LUT_N];       /* sin * 32767 */
static int s_lut_ready;

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    (void)p; (void)ctx;
    if (s_lut_ready) return;
    for (int i = 0; i < LUT_N; i++) {
        s_sin[i] = (int16_t)(sinf((float)i * (2.0f * 3.14159265f / LUT_N)) * 32767.0f);
    }
    s_lut_ready = 1;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w, H = in->h;
    int amp_x = (int)p[P_AMP_X], amp_y = (int)p[P_AMP_Y];
    float len_x = p[P_LEN_X] < 4 ? 4 : p[P_LEN_X];
    float len_y = p[P_LEN_Y] < 4 ? 4 : p[P_LEN_Y];
    float phase = (float)ctx->frame_no * p[P_SPEED];
    float noise = fx_clampf(p[P_NOISE], 0, 1);

    /* fixed-point phase steps in LUT units (16.16) */
    int32_t step_y = (int32_t)(LUT_N * 65536.0f / len_x);   /* along y, drives x displacement */
    int32_t step_x = (int32_t)(LUT_N * 65536.0f / len_y);   /* along x, drives y displacement */
    int32_t ph0 = (int32_t)(phase * LUT_N * 65536.0f / (2.0f * 3.14159265f));

    for (int y = ctx->y0; y < ctx->y1; y++) {
        int32_t ph = ph0 + step_y * y;
        int dxr = (amp_x * s_sin[(ph >> 16) & (LUT_N - 1)]) >> 15;
        if (noise > 0) {
            fx_rng_t rng;
            fx_rng_init_at(&rng, ctx, 0x3A7E, (uint32_t)y);
            if (fx_rng_f(&rng) < noise * 0.15f) {
                dxr += fx_rng_range(&rng, -amp_x - 8, amp_x + 8);   /* occasional torn row */
            }
        }
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        if (amp_y == 0) {
            /* fast path: pure horizontal displacement = a rotated row, two memcpy segments */
            int sx = dxr % W; if (sx < 0) sx += W;
            const uint16_t *src = in->px + (size_t)y * in->stride_px;
            memcpy(dst, src + sx, (size_t)(W - sx) * sizeof(uint16_t));
            memcpy(dst + (W - sx), src, (size_t)sx * sizeof(uint16_t));
        } else {
            int32_t phx = ph0 * 2;
            for (int x = 0; x < W; x++, phx += step_x) {
                int dyr = (amp_y * s_sin[(phx >> 16) & (LUT_N - 1)]) >> 15;
                int sx = x + dxr; if (sx < 0) sx += W; else if (sx >= W) sx -= W;
                int sy = y + dyr; if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;
                dst[x] = in->px[(size_t)sy * in->stride_px + sx];
            }
        }
    }
}

const fx_desc_t fx_wave = {
    .id = "wave",
    .name = "Wave",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_LIGHT,
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
