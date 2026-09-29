/*
 * Bit-crush / posterize with optional ordered (Bayer 4x4) dither.
 * RGB565 has 5/6/5 bits; "bits" is how many of them survive per channel.
 *
 * Performance note: integer division is ~35 cycles on the ESP32-P4, so the per-channel
 * quantization is precomputed into lookup tables (one per dither threshold) once per frame.
 */
#include <math.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BITS_R, P_BITS_G, P_BITS_B, P_DITHER };

static const fx_param_t s_params[] = {
    { "bits_r", "Red bits",   1, 5, 2 },
    { "bits_g", "Green bits", 1, 6, 2 },
    { "bits_b", "Blue bits",  1, 5, 2 },
    { "dither", "Dither",     0, 1, 1 },
};

static void from_amount(float a, float *p)
{
    float bits = 5.0f - a * 4.0f;     /* 5 -> 1 */
    p[P_BITS_R] = bits;
    p[P_BITS_G] = bits + 1.0f;
    p[P_BITS_B] = bits;
    p[P_DITHER] = 1.0f;
}

/* 4x4 Bayer matrix, values 0..15 */
static const uint8_t s_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* Quantize an n-bit channel value to `keep` bits, with dither threshold t in 0..15. */
static inline unsigned crush(unsigned v, unsigned nbits, unsigned keep, unsigned t, int dither)
{
    if (keep >= nbits) return v;
    unsigned drop = nbits - keep;
    unsigned step = 1u << drop;
    unsigned maxv = (1u << nbits) - 1;
    if (dither) {
        v += (t * step) >> 4;
        if (v > maxv) v = maxv;
    }
    unsigned q = v >> drop;
    unsigned maxq = (1u << keep) - 1;
    return (q * maxv) / (maxq ? maxq : 1);
}

/* lut[t][v] for the 16 dither thresholds: 5-bit channels need 32 entries, green 64 */
static uint8_t s_lut_r[16][32], s_lut_g[16][64], s_lut_b[16][32];

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)ctx;
    unsigned br = (unsigned)fx_clampi((int)lroundf(p[P_BITS_R]), 1, 5);
    unsigned bg = (unsigned)fx_clampi((int)lroundf(p[P_BITS_G]), 1, 6);
    unsigned bb = (unsigned)fx_clampi((int)lroundf(p[P_BITS_B]), 1, 5);
    int dither = p[P_DITHER] >= 0.5f;

    for (unsigned t = 0; t < 16; t++) {
        for (unsigned v = 0; v < 32; v++) {
            s_lut_r[t][v] = (uint8_t)crush(v, 5, br, t, dither);
            s_lut_b[t][v] = (uint8_t)crush(v, 5, bb, t, dither);
        }
        for (unsigned v = 0; v < 64; v++) {
            s_lut_g[t][v] = (uint8_t)crush(v, 6, bg, t, dither);
        }
    }

    for (int y = 0; y < (int)in->h; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        const uint8_t *brow = s_bayer[y & 3];
        const uint8_t *lr[4] = { s_lut_r[brow[0]], s_lut_r[brow[1]], s_lut_r[brow[2]], s_lut_r[brow[3]] };
        const uint8_t *lg[4] = { s_lut_g[brow[0]], s_lut_g[brow[1]], s_lut_g[brow[2]], s_lut_g[brow[3]] };
        const uint8_t *lb[4] = { s_lut_b[brow[0]], s_lut_b[brow[1]], s_lut_b[brow[2]], s_lut_b[brow[3]] };
        int x = 0, w = in->w;
        for (; x + 4 <= w; x += 4) {
            for (int k = 0; k < 4; k++) {
                uint16_t px = src[x + k];
                dst[x + k] = fx_rgb565(lr[k][px >> 11], lg[k][(px >> 5) & 0x3f], lb[k][px & 0x1f]);
            }
        }
        for (; x < w; x++) {
            uint16_t px = src[x];
            int k = x & 3;
            dst[x] = fx_rgb565(lr[k][px >> 11], lg[k][(px >> 5) & 0x3f], lb[k][px & 0x1f]);
        }
    }
}

const fx_desc_t fx_bitcrush = {
    .id = "bitcrush",
    .name = "Bit crush",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = true,
    .temporal = false,
    .from_amount = from_amount,
    .apply = apply,
};
