/*
 * Scanline displacement / smear: the image is cut into horizontal bands. Each band is either
 * shifted sideways by a random amount (wrapping), repeated from the band above ("hold",
 * which smears the picture vertically), or left intact.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BAND, P_MAX_OFF, P_SHIFT_PROB, P_HOLD_PROB, P_HOLD_LEN };

static const fx_param_t s_params[] = {
    { "band",       "Band height",     1, 128,  6 },
    { "max_off",    "Max shift (px)",  0, 720, 120 },
    { "shift_prob", "Shift chance",    0,   1, 0.35f },
    { "hold_prob",  "Smear chance",    0,   1, 0.15f },
    { "hold_len",   "Smear length",    1,  64, 8 },
};

static void from_amount(float a, float *p)
{
    p[P_BAND] = 8.0f - a * 5.0f;
    p[P_MAX_OFF] = a * 300.0f;
    p[P_SHIFT_PROB] = 0.1f + a * 0.6f;
    p[P_HOLD_PROB] = a * 0.35f;
    p[P_HOLD_LEN] = 2.0f + a * 30.0f;
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
    fx_rng_t rng;
    fx_rng_init(&rng, ctx, 0x5CA1);

    int band = fx_clampi((int)p[P_BAND], 1, 4096);
    int max_off = fx_clampi((int)p[P_MAX_OFF], 0, (int)in->w - 1);
    float shift_prob = fx_clampf(p[P_SHIFT_PROB], 0, 1);
    float hold_prob = fx_clampf(p[P_HOLD_PROB], 0, 1);
    int hold_len = fx_clampi((int)p[P_HOLD_LEN], 1, 4096);
    int w = in->w;

    int hold_src_y = -1;    /* row being repeated while smearing */
    int hold_left = 0;
    int shift = 0;

    for (int y = 0; y < (int)in->h; y++) {
        if ((y % band) == 0 && hold_left <= 0) {
            float r = fx_rng_f(&rng);
            if (r < hold_prob) {
                hold_src_y = y > 0 ? y - 1 : 0;
                hold_left = hold_len * band;
            } else if (r < hold_prob + shift_prob) {
                shift = fx_rng_range(&rng, -max_off, max_off);
            } else {
                shift = 0;
            }
        }
        const uint16_t *src;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        if (hold_left > 0) {
            src = in->px + (size_t)hold_src_y * in->stride_px;
            hold_left--;
            copy_row_shifted(src, dst, w, shift);
        } else {
            src = in->px + (size_t)y * in->stride_px;
            copy_row_shifted(src, dst, w, shift);
        }
    }
}

const fx_desc_t fx_scanline = {
    .id = "scanline",
    .name = "Scanline smear",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .from_amount = from_amount,
    .apply = apply,
};
