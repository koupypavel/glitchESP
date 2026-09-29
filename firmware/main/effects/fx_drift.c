/*
 * Drifting (PsychonautWiki): "textures and shapes warp, flow, melt, morph, breathe".
 * Two effects share one sampler here:
 *
 *   drift   — flowing / morphing: a slow sum-of-sines displacement field whose phases advance
 *             at unrelated rates, so the picture keeps deforming without ever repeating.
 *   breathe — objects contract and expand rhythmically: a radial zoom about the centre that
 *             oscillates in and out, with a little wobble on top.
 *
 * The displacement is evaluated on a coarse grid (every GRID pixels) and linearly interpolated
 * across the row in fixed point, so the per-pixel work is two adds, a clamp and one load.
 * Row-parallel: each output row only depends on the (read-only) input.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

#define LUT_N   1024
#define GRID    8         /* field evaluated every GRID px; dy is held constant per block */
#define MAX_GRID (2048 / GRID + 2)

static int16_t s_sin[LUT_N];
static int s_lut_ready;

static void lut_init(void)
{
    if (s_lut_ready) return;
    for (int i = 0; i < LUT_N; i++) {
        s_sin[i] = (int16_t)(sinf((float)i * (2.0f * 3.14159265f / LUT_N)) * 32767.0f);
    }
    s_lut_ready = 1;
}

/* sin(t) for t in turns (1.0 = full circle), result in 1/32767 */
static inline int sin_turn(float turns)
{
    int idx = (int)(turns * LUT_N) & (LUT_N - 1);
    return s_sin[idx];
}

/* Sample rows [y0,y1) of `out` from `in` at (x + dx, y + dy), with dx/dy given per grid
 * column by `field(x, y, ctx, dx16, dy16)` in 16.16 pixels. */
typedef void (*field_fn)(int x, int y, int W, int H, const float *p, uint32_t frame, int32_t *dx16, int32_t *dy16);

static void FX_HOT warp_rows(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx, field_fn field)
{
    int W = in->w, H = in->h;
    int ngrid = W / GRID + 2;
    int32_t gdx[MAX_GRID], gdy[MAX_GRID];

    for (int y = ctx->y0; y < ctx->y1; y++) {
        for (int g = 0; g < ngrid; g++) {
            field(g * GRID, y, W, H, p, ctx->frame_no, &gdx[g], &gdy[g]);
        }
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        for (int g = 0; g + 1 < ngrid && g * GRID < W; g++) {
            int32_t dx = gdx[g];
            int32_t sdx = (gdx[g + 1] - gdx[g]) / GRID;
            /* one source row per block: a row switch per pixel would be a PSRAM miss per pixel */
            int sy = y + ((gdy[g] + gdy[g + 1]) >> 17);
            if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;
            const uint16_t *srow = in->px + (size_t)sy * in->stride_px;
            int x0 = g * GRID, x1 = x0 + GRID < W ? x0 + GRID : W;
            for (int x = x0; x < x1; x++) {
                int sx = x + (dx >> 16);
                if (sx < 0) sx = 0; else if (sx >= W) sx = W - 1;
                dst[x] = srow[sx];
                dx += sdx;
            }
        }
    }
}

/* ---------------- drift: flowing / morphing ---------------- */

enum { D_AMP, D_SCALE, D_SPEED, D_MORPH };

static const fx_param_t s_drift_params[] = {
    { "amp",   "Amplitude (px)",     0, 120, 30 },
    { "scale", "Feature size (px)", 40, 800, 260 },
    { "speed", "Speed",              0, 1, 0.3f },
    { "morph", "Morph (irregular)",  0, 1, 0.5f },
};

static void drift_from_amount(float a, float *p)
{
    p[D_AMP] = 8.0f + a * 70.0f;
    p[D_SCALE] = 320.0f - a * 180.0f;
    p[D_SPEED] = 0.15f + a * 0.5f;
    p[D_MORPH] = a;
}

static void drift_field(int x, int y, int W, int H, const float *p, uint32_t frame, int32_t *dx16, int32_t *dy16)
{
    /* parameters are in full-resolution pixels; scale for the half-res path */
    float k = (float)W / 720.0f;
    (void)H;
    float amp = p[D_AMP] * k, inv = 1.0f / ((p[D_SCALE] < 40 ? 40 : p[D_SCALE]) * k);
    float t = (float)frame * (0.002f + p[D_SPEED] * 0.02f);
    float m = p[D_MORPH];
    /* two incommensurate waves per axis; morph adds a slower cross-term */
    int sx = sin_turn(y * inv + t) + sin_turn(x * inv * 0.61f - t * 1.37f + 0.25f)
           + (int)(m * sin_turn((x + y) * inv * 0.31f + t * 0.53f));
    int sy = sin_turn(x * inv * 0.83f - t * 0.91f) + sin_turn(y * inv * 0.47f + t * 1.13f + 0.5f)
           + (int)(m * sin_turn((x - y) * inv * 0.27f - t * 0.71f));
    *dx16 = (int32_t)(amp * 65536.0f / 3.0f / 32767.0f * sx);
    *dy16 = (int32_t)(amp * 65536.0f / 3.0f / 32767.0f * sy);
}

static void drift_apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    warp_rows(in, out, p, ctx, drift_field);
}

static void drift_prepare(const float *p, const fx_ctx_t *ctx) { (void)p; (void)ctx; lut_init(); }

const fx_desc_t fx_drift = {
    .id = "drift",
    .name = "Drift",
    .n_params = sizeof(s_drift_params) / sizeof(s_drift_params[0]),
    .params = s_drift_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,      /* block-row sampling: ~50 ms full, ~15 ms half */
    .from_amount = drift_from_amount,
    .prepare = drift_prepare,
    .apply = drift_apply,
};

/* ---------------- breathe: rhythmic contraction / expansion ---------------- */

enum { B_DEPTH, B_RATE, B_WOBBLE };

static const fx_param_t s_breathe_params[] = {
    { "depth",  "Zoom depth (%)",   0, 30, 8 },
    { "rate",   "Breaths per min",  2, 60, 12 },
    { "wobble", "Wobble",           0, 1, 0.3f },
};

static void breathe_from_amount(float a, float *p)
{
    p[B_DEPTH] = 3.0f + a * 20.0f;
    p[B_RATE] = 8.0f + a * 20.0f;
    p[B_WOBBLE] = a * 0.6f;
}

/* frame rate assumption for the rhythm: ~19 fps */
static void breathe_field(int x, int y, int W, int H, const float *p, uint32_t frame, int32_t *dx16, int32_t *dy16)
{
    float depth = p[B_DEPTH] * 0.01f;
    float turns = (float)frame * (p[B_RATE] / 60.0f / 19.0f);
    /* 0..depth: only ever expanded, so the sampler never leaves the frame (no edge bands) */
    float s = depth * (0.5f + 0.5f * (float)sin_turn(turns) / 32767.0f);
    float w = p[B_WOBBLE] * 6.0f * ((float)W / 720.0f);
    /* zoom about the centre: sample from (x - (x-cx)*s); positive s = expanded (zoomed in) */
    float cx = 0.5f * W, cy = 0.5f * H;
    float dx = -(x - cx) * s + w * (float)sin_turn(y * 0.004f + turns * 3.0f) / 32767.0f;
    float dy = -(y - cy) * s + w * (float)sin_turn(x * 0.005f - turns * 2.0f) / 32767.0f;
    *dx16 = (int32_t)(dx * 65536.0f);
    *dy16 = (int32_t)(dy * 65536.0f);
}

static void breathe_apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    warp_rows(in, out, p, ctx, breathe_field);
}

const fx_desc_t fx_breathe = {
    .id = "breathe",
    .name = "Breathe",
    .n_params = sizeof(s_breathe_params) / sizeof(s_breathe_params[0]),
    .params = s_breathe_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,
    .from_amount = breathe_from_amount,
    .prepare = drift_prepare,
    .apply = breathe_apply,
};
