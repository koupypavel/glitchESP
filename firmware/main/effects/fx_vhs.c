/*
 * VHS: what a worn tape does to a picture.
 *
 *  - colour bleed: on tape the colour signal has far less bandwidth than brightness, so
 *    colours smear to the right. Here green (which carries most of the brightness) is kept
 *    sharp while red and blue run through a one-pole low-pass along the row;
 *  - wobble: the rows wander sideways on a slow sine plus a little jitter;
 *  - tracking bar: a band of torn, noisy rows that drifts up the picture;
 *  - head switch: the bottom rows are skewed and noisy, where the heads change over;
 *  - snow: short white dashes scattered over the picture;
 *  - every other row is a little darker.
 *
 * Row-parallel: everything about a row comes from (seed, frame, row).
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BLEED, P_WOBBLE, P_SNOW, P_TRACK, P_HEAD };

static const fx_param_t s_params[] = {
    { "bleed",  "Colour bleed (px)", 0, 40, 10, 0 },
    { "wobble", "Wobble (px)",       0, 24, 3, 0 },
    { "snow",   "Snow",              0, 1, 0.25f, 0 },
    { "track",  "Tracking bar",      0, 1, 0.5f, 0 },
    { "head",   "Head switch",       0, 1, 0.5f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_BLEED] = 4.0f + a * 24.0f;
    p[P_WOBBLE] = a * 10.0f;
    p[P_SNOW] = a * 0.6f;
    p[P_TRACK] = a;
    p[P_HEAD] = a;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w, H = in->h;
    float k = fx_scale(in);
    int bleed = fx_px(in, p[P_BLEED], 0);
    float wobble = p[P_WOBBLE] * k;
    float snow = fx_clampf(p[P_SNOW], 0, 1);
    float track = fx_clampf(p[P_TRACK], 0, 1);
    float head = fx_clampf(p[P_HEAD], 0, 1);

    /* low-pass strength for red/blue: reaches a step after roughly `bleed` pixels */
    int alpha = bleed > 0 ? 256 / (1 + bleed / 2) : 256;
    if (alpha < 8) alpha = 8;

    int head_rows = (int)(H * 0.07f * head);                  /* skewed band at the bottom */
    int head_start = H - head_rows;
    int bar_h = (int)(60.0f * k * track);                     /* tracking bar height */
    int bar_top = bar_h > 0 ? H - (int)((ctx->frame_no * (uint32_t)(7.0f * k + 1.0f)) % (uint32_t)(H + bar_h)) : H;
    float phase = (float)ctx->frame_no * 0.21f;
    float inv_len = 6.2832f / (180.0f * k);                   /* wobble wavelength along y */

    for (int y = ctx->y0; y < ctx->y1; y++) {
        fx_rng_t rng;
        fx_rng_init_at(&rng, ctx, 0x0785, (uint32_t)y);

        /* how far this row is pushed sideways */
        int shift = (int)(wobble * sinf((float)y * inv_len + phase));
        if (wobble > 0 && (fx_rng_u32(&rng) & 7) == 0) shift += fx_rng_range(&rng, -1, 1);
        bool torn = false;
        if (y >= head_start && head_rows > 0) {
            shift += (y - head_start) * 3 + fx_rng_range(&rng, 0, (int)(12 * k) + 1);
            torn = true;
        }
        if (bar_h > 0 && y >= bar_top && y < bar_top + bar_h) {
            shift += fx_rng_range(&rng, -(int)(40 * k) - 1, (int)(40 * k) + 1);
            torn = true;
        }

        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        uint16_t dim = (y & 1) ? 0x18E3 : 0;                  /* odd rows lose 1/8 of each channel */

        int sx = -shift;
        uint16_t first = src[sx < 0 ? 0 : (sx >= W ? W - 1 : sx)];
        int r_acc = (int)fx_r5(first) << 8, b_acc = (int)fx_b5(first) << 8;
        for (int x = 0; x < W; x++, sx++) {
            uint16_t s = src[sx < 0 ? 0 : (sx >= W ? W - 1 : sx)];
            r_acc += (((int)fx_r5(s) << 8) - r_acc) * alpha >> 8;
            b_acc += (((int)fx_b5(s) << 8) - b_acc) * alpha >> 8;
            uint16_t o = (uint16_t)(((r_acc >> 8) << 11) | (s & 0x07E0) | (b_acc >> 8));
            dst[x] = (uint16_t)(o - ((o >> 3) & dim));
        }

        /* snow: a few white dashes; torn rows get many */
        int dashes = 0;
        float chance = snow * 0.08f + (torn ? 0.6f : 0.0f);
        if (fx_rng_f(&rng) < chance) dashes = 1 + (int)(fx_rng_u32(&rng) % (torn ? 6u : 2u));
        for (int d = 0; d < dashes; d++) {
            int x0 = fx_rng_range(&rng, 0, W - 1);
            int len = fx_rng_range(&rng, 2, (int)(10 * k) + 2);
            uint16_t c = (fx_rng_u32(&rng) & 3) ? 0xFFFF : 0xBDF7;
            for (int x = x0; x < x0 + len && x < W; x++) dst[x] = c;
        }
    }
}

const fx_desc_t fx_vhs = {
    .id = "vhs",
    .name = "VHS",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,      /* two filters and a pack per pixel */
    .from_amount = from_amount,
    .apply = apply,
};
