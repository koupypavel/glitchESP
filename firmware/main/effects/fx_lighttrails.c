/*
 * Light trails: the long-exposure look of traffic at night, live. Bright things leave a
 * glowing trail behind them as they move, while the rest of the picture stays as sharp and
 * current as without the effect.
 *
 * Temporal: the previous output (ctx->prev) is faded and kept wherever it is still brighter
 * than the live frame and bright enough to count as light; everywhere else the live pixel is
 * shown. That threshold is the difference to Tracers, whose echo also drags the dark parts
 * of the scene along. "Warm fade" lets the trails lose their blue first and their red last,
 * so they cool from white through orange to red, like a glowing filament.
 *
 * Row-parallel: rows are independent.
 */
#include <math.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_LENGTH, P_THRESH, P_WARM };

static const fx_param_t s_params[] = {
    { "length", "Trail length", 0, 1, 0.7f, 0 },
    { "thresh", "Only lights",  0, 1, 0.55f, 0 },
    { "warm",   "Warm fade",    0, 1, 0.5f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_LENGTH] = 0.4f + a * 0.58f;
    p[P_THRESH] = 0.7f - a * 0.35f;
    p[P_WARM] = 0.5f;
}

static unsigned s_kr, s_kg, s_kb;   /* per-channel fade per frame, in 1/256 */
static unsigned s_thr;              /* luma a faded pixel must keep to stay (0..255) */

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    (void)ctx;
    /* length 0..1 -> keeps 80 % .. 99 % per frame */
    float keep = 0.8f + 0.19f * fx_clampf(p[P_LENGTH], 0, 1);
    float warm = fx_clampf(p[P_WARM], 0, 1);
    s_kr = (unsigned)(256.0f * keep);
    s_kg = (unsigned)(256.0f * keep * (1.0f - 0.06f * warm));
    s_kb = (unsigned)(256.0f * keep * (1.0f - 0.14f * warm));
    s_thr = (unsigned)(40.0f + 180.0f * fx_clampf(p[P_THRESH], 0, 1));
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    const fx_frame_t *prev = ctx->prev;
    if (!prev || prev->w != in->w || prev->h != in->h) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    unsigned kr = s_kr, kg = s_kg, kb = s_kb, thr = s_thr;
    int W = in->w;
    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        const uint16_t *old = prev->px + (size_t)y * prev->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        for (int x = 0; x < W; x++) {
            uint16_t a = src[x], b = old[x];
            if (b == a || fx_luma(b) < thr) { dst[x] = a; continue; }    /* no light there before */
            /* the old light, faded */
            unsigned br = (fx_r5(b) * kr) >> 8, bg = (fx_g6(b) * kg) >> 8, bb = (fx_b5(b) * kb) >> 8;
            unsigned ar = fx_r5(a), ag = fx_g6(a), ab = fx_b5(a);
            dst[x] = fx_rgb565(ar > br ? ar : br, ag > bg ? ag : bg, ab > bb ? ab : bb);
        }
    }
}

const fx_desc_t fx_lighttrails = {
    .id = "lighttrails",
    .name = "Light trails",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = true,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
