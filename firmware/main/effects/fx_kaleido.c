/*
 * Kaleidoscope / symmetrical texture repetition: the frame is mirrored across its vertical
 * axis (2-way), and optionally also across the horizontal axis (4-way). A slow "sweep" moves
 * the mirror axis so the symmetry breathes.
 *
 * Row-parallel: each output row depends on one input row (the mirrored one for the bottom
 * half in 4-way mode), and mirroring within a row is local.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_WAYS, P_AXIS, P_SWEEP, P_FLIP };

static const fx_param_t s_params[] = {
    { "ways",  "2-way (0) / 4-way (1)", 0, 1, 0 },
    { "axis",  "Axis position",         0.1f, 0.9f, 0.5f },
    { "sweep", "Axis sweep",            0, 1, 0.3f },
    { "flip",  "Mirror side",           0, 1, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_WAYS] = a >= 0.5f ? 1 : 0;
    p[P_AXIS] = 0.5f;
    p[P_SWEEP] = a * 0.6f;
    p[P_FLIP] = 0;
}

/* dst[0..n) = reversed src[0..n) */
static void FX_HOT reverse_copy(const uint16_t *src, uint16_t *dst, int n)
{
    const uint16_t *s = src + n - 1;
    int i = 0;
    for (; i + 2 <= n; i += 2, s -= 2) {
        dst[i] = s[0];
        dst[i + 1] = s[-1];
    }
    for (; i < n; i++, s--) dst[i] = *s;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w, H = in->h;
    int four = p[P_WAYS] >= 0.5f;
    int flip = p[P_FLIP] >= 0.5f;
    float axis = fx_clampf(p[P_AXIS], 0.1f, 0.9f);
    float sweep = fx_clampf(p[P_SWEEP], 0, 1);
    if (sweep > 0) axis += 0.3f * sweep * sinf((float)ctx->frame_no * 0.03f);
    axis = fx_clampf(axis, 0.1f, 0.9f);
    int ax = (int)(axis * W);            /* columns [0, ax) are the source side */
    int ay = H / 2;

    for (int y = ctx->y0; y < ctx->y1; y++) {
        int sy = y;
        if (four && y >= ay) sy = 2 * ay - 1 - y;          /* bottom half mirrors the top */
        if (sy < 0) sy = 0;
        const uint16_t *src = in->px + (size_t)sy * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        if (!flip) {
            /* keep the left part, mirror it into the right part (clip at the edge) */
            memcpy(dst, src, (size_t)ax * 2);
            int n = W - ax;
            if (n > ax) {
                reverse_copy(src, dst + ax, ax);
                memcpy(dst + 2 * ax, src, (size_t)(n - ax) * 2);   /* repeat if the axis is left of centre */
            } else {
                reverse_copy(src + ax - n, dst + ax, n);
            }
        } else {
            /* keep the right part, mirror it into the left */
            int n = W - ax;                 /* width of the kept right part */
            memcpy(dst + ax, src + ax, (size_t)n * 2);
            if (ax > n) {
                reverse_copy(src + ax, dst + ax - n, n);
                memcpy(dst, src + ax, (size_t)(ax - n) * 2);
            } else {
                reverse_copy(src + ax, dst, ax);
            }
        }
    }
}

const fx_desc_t fx_kaleido = {
    .id = "kaleido",
    .name = "Kaleido",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_LIGHT,
    .from_amount = from_amount,
    .apply = apply,
};
