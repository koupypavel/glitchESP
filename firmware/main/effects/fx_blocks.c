/*
 * Block shuffle: rectangular tiles are copied or swapped to random places, like corrupted
 * macroblocks in a broken video stream. Tiles near their origin look like "block drift",
 * far ones look like memory corruption.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_TILE, P_COUNT, P_MAX_DIST, P_SWAP, P_ASPECT };

static const fx_param_t s_params[] = {
    { "tile",     "Tile size (px)",   4, 256,  48 },
    { "count",    "Tiles moved",      0, 400,  40 },
    { "max_dist", "Max distance (px)",0, 1280, 200 },
    { "swap",     "Swap (1) / copy (0)", 0, 1, 0 },
    { "aspect",   "Width factor",     0.25f, 4, 2 },
};

static void from_amount(float a, float *p)
{
    p[P_TILE] = 64.0f - a * 40.0f;
    p[P_COUNT] = a * 120.0f;
    p[P_MAX_DIST] = 40.0f + a * 500.0f;
    p[P_SWAP] = 0;
    p[P_ASPECT] = 2.0f;
}

static void FX_HOT copy_rect(const uint16_t *src, uint32_t sstride, uint16_t *dst, uint32_t dstride, int w, int h)
{
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, (size_t)w * sizeof(uint16_t));
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    fx_rng_t rng;
    fx_rng_init(&rng, ctx, 0xB10C);

    fx_frame_copy(in, out);

    int th = fx_clampi(fx_px(in, p[P_TILE], 2), 2, 1024);
    int tw = fx_clampi(fx_px(in, p[P_TILE] * p[P_ASPECT], 2), 2, 2048);
    int count = fx_clampi((int)p[P_COUNT], 0, 4096);
    int maxd = fx_clampi(fx_px(in, p[P_MAX_DIST], 0), 0, 4096);
    int swap = p[P_SWAP] >= 0.5f;
    int W = in->w, H = in->h;
    if (tw > W) tw = W;
    if (th > H) th = H;

    for (int i = 0; i < count; i++) {
        int sx = fx_rng_range(&rng, 0, W - tw);
        int sy = fx_rng_range(&rng, 0, H - th);
        int dx = fx_clampi(sx + fx_rng_range(&rng, -maxd, maxd), 0, W - tw);
        int dy = fx_clampi(sy + fx_rng_range(&rng, -maxd, maxd), 0, H - th);
        /* snap destinations to the tile grid most of the time for a "codec" look */
        if (fx_rng_f(&rng) < 0.7f) {
            dx -= dx % tw; dy -= dy % th;
            if (dx + tw > W) dx = W - tw;
            if (dy + th > H) dy = H - th;
        }
        const uint16_t *s = in->px + (size_t)sy * in->stride_px + sx;   /* always read the clean input */
        uint16_t *d = out->px + (size_t)dy * out->stride_px + dx;
        if (swap) {
            const uint16_t *s2 = in->px + (size_t)dy * in->stride_px + dx;
            uint16_t *d2 = out->px + (size_t)sy * out->stride_px + sx;
            copy_rect(s2, in->stride_px, d2, out->stride_px, tw, th);
        }
        copy_rect(s, in->stride_px, d, out->stride_px, tw, th);
    }
}

const fx_desc_t fx_blocks = {
    .id = "blocks",
    .name = "Blocks",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = false,
    .cost = FX_COST_MEDIUM,
    .from_amount = from_amount,
    .apply = apply,
};
