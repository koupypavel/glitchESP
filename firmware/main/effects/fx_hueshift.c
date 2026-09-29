/*
 * Hue shift / colour enhancement ("colour shifting", "colour enhancement" in psychedelic
 * replication terms): every colour is rotated around the hue wheel by an angle that drifts
 * with time, with optional saturation and contrast boost.
 *
 * A 3x3 hue-rotation matrix per pixel would be far too slow on the P4, so the whole
 * RGB565 -> RGB565 mapping is a 64K-entry lookup table (128 KB, internal RAM). The table is
 * rebuilt only when the angle has moved by a few degrees; the per-pixel cost is one load.
 * Row-parallel: rows are independent.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_ANGLE, P_SPEED, P_SAT, P_CONTRAST };

static const fx_param_t s_params[] = {
    { "angle",    "Hue offset (deg)",   0, 360, 0 },
    { "speed",    "Drift (deg/frame)",  0,  30, 3 },
    { "sat",      "Saturation x",     0.5f, 3, 1.6f },
    { "contrast", "Contrast x",       0.5f, 2, 1.1f },
};

static void from_amount(float a, float *p)
{
    p[P_ANGLE] = 0;
    p[P_SPEED] = 1.0f + a * 12.0f;
    p[P_SAT] = 1.0f + a * 1.5f;
    p[P_CONTRAST] = 1.0f + a * 0.4f;
}

static uint16_t *s_lut;            /* 65536 entries, allocated with fx_big_alloc (PSRAM on device) */
static float s_lut_angle = -1000.0f, s_lut_sat = -1, s_lut_con = -1;

/* Build the table: hue rotation (Rodrigues form around the grey axis) + saturation + contrast. */
static void build_lut(float angle_deg, float sat, float con)
{
    float a = angle_deg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a);
    const float k = 1.0f / 3.0f, sq = sqrtf(1.0f / 3.0f);
    /* hue rotation matrix m */
    float m[3][3] = {
        { c + (1 - c) * k, (1 - c) * k - sq * s, (1 - c) * k + sq * s },
        { (1 - c) * k + sq * s, c + (1 - c) * k, (1 - c) * k - sq * s },
        { (1 - c) * k - sq * s, (1 - c) * k + sq * s, c + (1 - c) * k },
    };
    /* saturation: lerp between luma and colour */
    const float lr = 0.299f, lg = 0.587f, lb = 0.114f;
    float sm[3][3] = {
        { lr + (1 - lr) * sat, lg - lg * sat, lb - lb * sat },
        { lr - lr * sat, lg + (1 - lg) * sat, lb - lb * sat },
        { lr - lr * sat, lg - lg * sat, lb + (1 - lb) * sat },
    };
    /* combined = m * sm, scaled by contrast, in 8.8 fixed point */
    int32_t f[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float v = 0;
            for (int t = 0; t < 3; t++) v += m[i][t] * sm[t][j];
            f[i][j] = (int32_t)(v * con * 256.0f);
        }
    }
    int32_t bias = (int32_t)((1.0f - con) * 128.0f * 256.0f);   /* contrast pivots around mid grey */

    for (unsigned p = 0; p < 65536; p++) {
        int r = (int)((p >> 11) & 0x1f) << 3, g = (int)((p >> 5) & 0x3f) << 2, b = (int)(p & 0x1f) << 3;
        int32_t nr = (f[0][0] * r + f[0][1] * g + f[0][2] * b + bias) >> 8;
        int32_t ng = (f[1][0] * r + f[1][1] * g + f[1][2] * b + bias) >> 8;
        int32_t nb = (f[2][0] * r + f[2][1] * g + f[2][2] * b + bias) >> 8;
        nr = nr < 0 ? 0 : (nr > 255 ? 255 : nr);
        ng = ng < 0 ? 0 : (ng > 255 ? 255 : ng);
        nb = nb < 0 ? 0 : (nb > 255 ? 255 : nb);
        s_lut[p] = fx_rgb565((unsigned)nr >> 3, (unsigned)ng >> 2, (unsigned)nb >> 3);
    }
}

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    if (!s_lut) {
        s_lut = fx_big_alloc(65536 * sizeof(uint16_t));
        if (!s_lut) return;
        s_lut_angle = -1000.0f;
    }
    float angle = fmodf(p[P_ANGLE] + (float)ctx->frame_no * p[P_SPEED], 360.0f);
    float sat = fx_clampf(p[P_SAT], 0.0f, 4.0f), con = fx_clampf(p[P_CONTRAST], 0.25f, 3.0f);
    /* rebuild when the hue moved >= 4 degrees or another parameter changed */
    float d = fabsf(angle - s_lut_angle);
    if (d > 180.0f) d = 360.0f - d;
    if (d >= 4.0f || sat != s_lut_sat || con != s_lut_con) {
        build_lut(angle, sat, con);
        s_lut_angle = angle; s_lut_sat = sat; s_lut_con = con;
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    const uint16_t *lut = s_lut;
    int W = in->w;
    if (!lut) { fx_frame_copy_rows(in, out, ctx->y0, ctx->y1); return; }
    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        int x = 0;
        if ((((uintptr_t)src | (uintptr_t)dst) & 3) == 0) {
            const uint32_t *s32 = (const uint32_t *)src;
            uint32_t *d32 = (uint32_t *)dst;
            for (; x + 2 <= W; x += 2) {
                uint32_t v = s32[x >> 1];
                d32[x >> 1] = (uint32_t)lut[v & 0xffff] | ((uint32_t)lut[v >> 16] << 16);
            }
        }
        for (; x < W; x++) dst[x] = lut[src[x]];
    }
}

const fx_desc_t fx_hueshift = {
    .id = "hueshift",
    .name = "Hue drift",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = true,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_LIGHT,
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
