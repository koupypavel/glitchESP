/*
 * Scanline displacement / smear: the image is cut into horizontal bands. Each band is either
 * shifted sideways by a random amount (wrapping), repeated from the row above the band that
 * started a "hold" (which smears the picture vertically), or left intact.
 *
 * Row-parallel: every band's decision is a pure function of (seed, frame, band index). A hold
 * started in band s covers bands s .. s+hold_len-1, so a band looks back at most hold_len
 * decisions instead of carrying state from row to row.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BAND, P_MAX_OFF, P_SHIFT_PROB, P_HOLD_PROB, P_HOLD_LEN };

static const fx_param_t s_params[] = {
    { "band",       "Band height",     1, 128,  6, 0 },
    { "max_off",    "Max shift (px)",  0, 720, 120, 0 },
    { "shift_prob", "Shift chance",    0,   1, 0.35f, 0 },
    { "hold_prob",  "Smear chance",    0,   1, 0.15f, 0 },
    { "hold_len",   "Smear length",    1,  64, 8, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_BAND] = 8.0f - a * 5.0f;
    p[P_MAX_OFF] = a * 300.0f;
    p[P_SHIFT_PROB] = 0.1f + a * 0.6f;
    p[P_HOLD_PROB] = a * 0.35f;
    p[P_HOLD_LEN] = 2.0f + a * 30.0f;
}

typedef struct { int hold; int shift; } band_decision_t;

static band_decision_t decide(const fx_ctx_t *ctx, int band_idx, float hold_prob, float shift_prob, int max_off)
{
    fx_rng_t rng;
    fx_rng_init_at(&rng, ctx, 0x5CA1, (uint32_t)band_idx);
    band_decision_t d = { 0, 0 };
    float r = fx_rng_f(&rng);
    if (r < hold_prob) {
        d.hold = 1;
    } else if (r < hold_prob + shift_prob) {
        d.shift = fx_rng_range(&rng, -max_off, max_off);
    }
    return d;
}

static void FX_HOT copy_row_shifted(const uint16_t *src, uint16_t *dst, int w, int shift)
{
    shift %= w; if (shift < 0) shift += w;
    if (shift == 0) {
        memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        return;
    }
    /* dst[x] = src[x - shift] with wrap */
    memcpy(dst + shift, src, (size_t)(w - shift) * sizeof(uint16_t));
    memcpy(dst, src + (w - shift), (size_t)shift * sizeof(uint16_t));
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int band = fx_clampi(fx_px(in, p[P_BAND], 1), 1, 4096);
    int max_off = fx_clampi(fx_px(in, p[P_MAX_OFF], 0), 0, (int)in->w - 1);
    float shift_prob = fx_clampf(p[P_SHIFT_PROB], 0, 1);
    float hold_prob = fx_clampf(p[P_HOLD_PROB], 0, 1);
    int hold_len = fx_clampi((int)p[P_HOLD_LEN], 1, 4096);
    int w = in->w;

    int cur_band = -1;
    int src_y = 0, shift = 0;           /* per-band: which source row and how much shift */
    int hold_active = 0;

    for (int y = ctx->y0; y < ctx->y1; y++) {
        int b = y / band;
        if (b != cur_band) {
            cur_band = b;
            /* is a hold covering this band? look back up to hold_len bands */
            hold_active = 0;
            int look = hold_len < b + 1 ? hold_len : b + 1;
            for (int k = 0; k < look; k++) {
                band_decision_t d = decide(ctx, b - k, hold_prob, shift_prob, max_off);
                if (d.hold) {
                    hold_active = 1;
                    src_y = (b - k) * band - 1;
                    if (src_y < 0) src_y = 0;
                    shift = 0;
                    break;
                }
            }
            if (!hold_active) {
                shift = decide(ctx, b, hold_prob, shift_prob, max_off).shift;
            }
        }
        const uint16_t *src = in->px + (size_t)(hold_active ? src_y : y) * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        copy_row_shifted(src, dst, w, shift);
    }
}

const fx_desc_t fx_scanline = {
    .id = "scanline",
    .name = "Scanline smear",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_LIGHT,
    .from_amount = from_amount,
    .apply = apply,
};
