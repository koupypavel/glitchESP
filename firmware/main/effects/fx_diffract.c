/*
 * Diffraction ("rainbows and spectrums of colour embedded within the brighter parts of the
 * visual field"): bright areas sprout spectral streaks along the row. Each colour channel
 * trails with its own delay and decay, so the halo is ordered like a spectrum: white core,
 * then the short blue band, green, and the long red tail.
 *
 * Implemented as a running decay ("glow") swept left-to-right and right-to-left per row:
 *   glow_c(x) = max(glow_c(x-1) * decay_c, bright(x - delay_c))
 * which is O(1) per pixel. Row-parallel: rows are independent.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_THRESH, P_LENGTH, P_SPREAD, P_INTENSITY, P_BOTH };

static const fx_param_t s_params[] = {
    { "thresh",    "Bright threshold", 0, 255, 170 },
    { "length",    "Streak length",    0, 1, 0.6f },
    { "spread",    "Colour spread",    0, 24, 6 },
    { "intensity", "Intensity",        0, 2, 1.0f },
    { "both",      "Both directions",  0, 1, 1 },
};

static void from_amount(float a, float *p)
{
    p[P_THRESH] = 200.0f - a * 90.0f;
    p[P_LENGTH] = 0.4f + a * 0.55f;
    p[P_SPREAD] = 2.0f + a * 14.0f;
    p[P_INTENSITY] = 0.6f + a * 1.0f;
    p[P_BOTH] = 1;
}

#define MAXW 2048

/* One sweep updates all three channel glows (accumulators stay in registers).
 * dir = +1: left to right, -1: right to left. glow arrays accumulate (max). */
static void FX_HOT sweep3(const uint8_t *bright, uint8_t *gr, uint8_t *gg, uint8_t *gb, int W, int dir,
                          int spread, unsigned dr, unsigned dg, unsigned db)
{
    unsigned r = 0, g = 0, b = 0;
    if (dir > 0) {
        for (int x = 0; x < W; x++) {
            unsigned bb = bright[x];
            unsigned bg2 = x >= spread ? bright[x - spread] : 0;
            unsigned br = x >= 2 * spread ? bright[x - 2 * spread] : 0;
            b = (b * db) >> 8; if (bb > b) b = bb;
            g = (g * dg) >> 8; if (bg2 > g) g = bg2;
            r = (r * dr) >> 8; if (br > r) r = br;
            if (b > gb[x]) gb[x] = (uint8_t)b;
            if (g > gg[x]) gg[x] = (uint8_t)g;
            if (r > gr[x]) gr[x] = (uint8_t)r;
        }
    } else {
        for (int x = W - 1; x >= 0; x--) {
            unsigned bb = bright[x];
            unsigned bg2 = x + spread < W ? bright[x + spread] : 0;
            unsigned br = x + 2 * spread < W ? bright[x + 2 * spread] : 0;
            b = (b * db) >> 8; if (bb > b) b = bb;
            g = (g * dg) >> 8; if (bg2 > g) g = bg2;
            r = (r * dr) >> 8; if (br > r) r = br;
            if (b > gb[x]) gb[x] = (uint8_t)b;
            if (g > gg[x]) gg[x] = (uint8_t)g;
            if (r > gr[x]) gr[x] = (uint8_t)r;
        }
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w;
    if (W > MAXW) W = MAXW;
    int thresh = fx_clampi((int)p[P_THRESH], 0, 254);
    float len = fx_clampf(p[P_LENGTH], 0, 0.98f);
    float k = fx_scale(in);
    int spread = fx_clampi(fx_px(in, p[P_SPREAD], 0), 0, 128);
    int inten = (int)(fx_clampf(p[P_INTENSITY], 0, 4) * 64.0f);      /* 6.6 fixed */
    int both = p[P_BOTH] >= 0.5f;
    /* decays per channel: red trails longest, blue shortest */
    /* (the decay is per pixel, so its exponent follows the frame scale: same streak length) */
    unsigned dr = (unsigned)(powf(0.90f + 0.098f * len, 1.0f / k) * 256.0f);
    unsigned dg = (unsigned)(powf(0.85f + 0.13f * len, 1.0f / k) * 256.0f);
    unsigned db = (unsigned)(powf(0.78f + 0.18f * len, 1.0f / k) * 256.0f);
    unsigned gain = 255u * 256u / (unsigned)(255 - thresh);           /* maps thresh..255 -> 0..255 */

    uint8_t bright[MAXW], gr[MAXW], gg[MAXW], gb[MAXW];

    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;

        unsigned any = 0;
        for (int x = 0; x < W; x++) {
            int l = (int)fx_luma(src[x]) - thresh;
            unsigned v = l <= 0 ? 0 : ((unsigned)l * gain) >> 8;
            if (v > 255) v = 255;
            bright[x] = (uint8_t)v;
            any |= v;
        }
        if (!any) {
            /* nothing bright on this row: plain copy */
            memcpy(dst, src, (size_t)W * sizeof(uint16_t));
            continue;
        }
        memset(gr, 0, (size_t)W); memset(gg, 0, (size_t)W); memset(gb, 0, (size_t)W);
        sweep3(bright, gr, gg, gb, W, +1, spread, dr, dg, db);
        if (both) sweep3(bright, gr, gg, gb, W, -1, spread, dr, dg, db);
        for (int x = 0; x < W; x++) {
            uint16_t px = src[x];
            unsigned r = (fx_r5(px) << 3) + ((gr[x] * inten) >> 6);
            unsigned g = (fx_g6(px) << 2) + ((gg[x] * inten) >> 6);
            unsigned b = (fx_b5(px) << 3) + ((gb[x] * inten) >> 6);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            dst[x] = fx_rgb565(r >> 3, g >> 2, b >> 3);
        }
    }
}

const fx_desc_t fx_diffract = {
    .id = "diffract",
    .name = "Diffraction",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,      /* ~60 ops/px on bright rows: runs at half resolution */
    .from_amount = from_amount,
    .apply = apply,
};
