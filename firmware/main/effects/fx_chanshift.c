/*
 * Channel shift / RGB split: each color plane is sampled from an offset position, with an
 * optional per-band random jitter so the split "tears" along horizontal bands.
 *
 * Performance note: no modulo per pixel. For each row and channel the source row pointer
 * and a wrapped start index are computed once; the inner loop just increments and wraps.
 */
#include <math.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_DX_R, P_DY_R, P_DX_G, P_DY_G, P_DX_B, P_DY_B, P_JITTER, P_BAND };

static const fx_param_t s_params[] = {
    { "dx_r",   "Red X",     -96, 96, -12 },
    { "dy_r",   "Red Y",     -96, 96,   0 },
    { "dx_g",   "Green X",   -96, 96,   0 },
    { "dy_g",   "Green Y",   -96, 96,   0 },
    { "dx_b",   "Blue X",    -96, 96,  12 },
    { "dy_b",   "Blue Y",    -96, 96,   0 },
    { "jitter", "Band jitter", 0,  1, 0.3f },
    { "band",   "Band height", 4, 256, 32 },
};

static void from_amount(float a, float *p)
{
    p[P_DX_R] = -a * 40.0f;
    p[P_DY_R] = 0;
    p[P_DX_G] = 0;
    p[P_DY_G] = a * 6.0f;
    p[P_DX_B] = a * 40.0f;
    p[P_DY_B] = 0;
    p[P_JITTER] = a * 0.6f;
    p[P_BAND] = 32.0f - a * 20.0f;
}

static inline int wrap_start(int v, int w)
{
    v %= w;                       /* once per row per channel, not per pixel */
    return v < 0 ? v + w : v;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    fx_rng_t rng;
    fx_rng_init(&rng, ctx, 0x0C5A);

    int dxr = (int)lroundf(p[P_DX_R]), dyr = (int)lroundf(p[P_DY_R]);
    int dxg = (int)lroundf(p[P_DX_G]), dyg = (int)lroundf(p[P_DY_G]);
    int dxb = (int)lroundf(p[P_DX_B]), dyb = (int)lroundf(p[P_DY_B]);
    float jitter = fx_clampf(p[P_JITTER], 0, 1);
    int band = fx_clampi((int)p[P_BAND], 1, 4096);
    int jmax = (int)(jitter * 60.0f);
    int W = in->w, H = in->h;

    int jr = 0, jg = 0, jb = 0;
    for (int y = 0; y < H; y++) {
        if (jmax > 0 && (y % band) == 0) {
            if (fx_rng_f(&rng) < 0.6f) {
                jr = fx_rng_range(&rng, -jmax, jmax);
                jg = fx_rng_range(&rng, -jmax / 2, jmax / 2);
                jb = fx_rng_range(&rng, -jmax, jmax);
            } else {
                jr = jg = jb = 0;
            }
        }
        const uint16_t *sr = in->px + (size_t)fx_clampi(y - dyr, 0, H - 1) * in->stride_px;
        const uint16_t *sg = in->px + (size_t)fx_clampi(y - dyg, 0, H - 1) * in->stride_px;
        const uint16_t *sb = in->px + (size_t)fx_clampi(y - dyb, 0, H - 1) * in->stride_px;
        int xr = wrap_start(-dxr - jr, W);
        int xg = wrap_start(-dxg - jg, W);
        int xb = wrap_start(-dxb - jb, W);
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        for (int x = 0; x < W; x++) {
            dst[x] = (uint16_t)((sr[xr] & 0xF800) | (sg[xg] & 0x07E0) | (sb[xb] & 0x001F));
            if (++xr == W) xr = 0;
            if (++xg == W) xg = 0;
            if (++xb == W) xb = 0;
        }
    }
}

const fx_desc_t fx_chanshift = {
    .id = "chanshift",
    .name = "Channel shift",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .from_amount = from_amount,
    .apply = apply,
};
