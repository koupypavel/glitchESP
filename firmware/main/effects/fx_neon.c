/*
 * Neon: the outlines of the picture glow like neon tubes on a dark background. Strongest at
 * night, where shop signs, windows and car lights already have hard edges.
 *
 * Per pixel, the brightness difference between the neighbours two pixels to the left and
 * right, plus the one between above and below, is the edge strength (two pixels apart on a
 * 720-wide frame, so the lines come out a few pixels thick, like tubes). The edge is coloured from a hue wheel that runs across the picture and
 * slowly turns with time, and laid over the original picture darkened by "Background".
 *
 * Row-parallel: each output row reads three input rows.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_THRESH, P_GLOW, P_BG, P_SPEED, P_SPREAD };

static const fx_param_t s_params[] = {
    { "thresh", "Edge threshold", 0, 1, 0.15f, 0 },
    { "glow",   "Glow",           0.5f, 4.0f, 2.0f, 0 },
    { "bg",     "Background",     0, 1, 0.25f, 0 },
    { "speed",  "Colour speed",   0, 1, 0.3f, 0 },
    { "spread", "Colour spread",  0, 1, 0.5f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_THRESH] = 0.3f - a * 0.22f;
    p[P_GLOW] = 1.2f + a * 2.3f;
    p[P_BG] = 0.5f - a * 0.4f;
    p[P_SPEED] = 0.3f;
    p[P_SPREAD] = 0.3f + a * 0.5f;
}

#define NEON_MAX_W 1088
#define NEON_RING  7                                 /* brightness rows kept: y - d .. y + d, d <= 3 */

static uint8_t s_hr[256], s_hg[256], s_hb[256];     /* hue wheel, 0..255 per channel */
static uint8_t s_edge[512];                          /* gradient (0..510) -> edge strength 0..255 */
static uint8_t s_bg5[32], s_bg6[64];                 /* channel -> darkened background, 0..255 */
static int s_bg_q8;
static uint32_t s_phase, s_spread_q8;

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    if (!s_hr[1] && !s_hg[1]) {
        for (int i = 0; i < 256; i++) {
            float h = (float)i / 256.0f * 6.2832f;
            /* saturated neon colours: no muddy mid tones */
            s_hr[i] = (uint8_t)(255.0f * fx_clampf(0.5f + 0.8f * cosf(h), 0, 1));
            s_hg[i] = (uint8_t)(255.0f * fx_clampf(0.5f + 0.8f * cosf(h - 2.094f), 0, 1));
            s_hb[i] = (uint8_t)(255.0f * fx_clampf(0.5f + 0.8f * cosf(h + 2.094f), 0, 1));
        }
    }
    float thr = fx_clampf(p[P_THRESH], 0, 1) * 100.0f;
    float glow = fx_clampf(p[P_GLOW], 0.5f, 4.0f);
    for (int g = 0; g < 512; g++) s_edge[g] = (uint8_t)fx_clampf(((float)g - thr) * glow, 0, 255);
    s_bg_q8 = (int)(fx_clampf(p[P_BG], 0, 1) * 256.0f);
    for (int i = 0; i < 32; i++) s_bg5[i] = (uint8_t)((i * 255 / 31 * s_bg_q8) >> 8);
    for (int i = 0; i < 64; i++) s_bg6[i] = (uint8_t)((i * 255 / 63 * s_bg_q8) >> 8);
    s_phase = (uint32_t)((float)ctx->frame_no * (0.5f + 6.0f * fx_clampf(p[P_SPEED], 0, 1)));
    s_spread_q8 = (uint32_t)(32.0f + 480.0f * fx_clampf(p[P_SPREAD], 0, 1));   /* hue steps per 256 px */
}

static void luma_row(const uint16_t *row, uint8_t *out, int w)
{
    for (int x = 0; x < w; x++) out[x] = (uint8_t)fx_luma(row[x]);
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    int W = in->w, H = in->h;
    if (W > NEON_MAX_W) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    /* each input row's brightness is worked out once and kept while three output rows use it */
    uint8_t ring[NEON_RING][NEON_MAX_W];
    int tag[NEON_RING];
    for (int i = 0; i < NEON_RING; i++) tag[i] = -1;
    int d = fx_px(in, 2, 1);                           /* distance of the neighbours compared */
    if (d > (NEON_RING - 1) / 2) d = (NEON_RING - 1) / 2;
    float sc = 1.0f / fx_scale(in);
    uint32_t hue_step = (uint32_t)((float)s_spread_q8 * sc);    /* per pixel, 8.8 */

    for (int y = ctx->y0; y < ctx->y1; y++) {
        int rows[3] = { y >= d ? y - d : 0, y, y + d < H ? y + d : H - 1 };
        uint8_t *l[3];
        for (int k = 0; k < 3; k++) {
            int slot = rows[k] % NEON_RING;
            if (tag[slot] != rows[k]) {
                luma_row(in->px + (size_t)rows[k] * in->stride_px, ring[slot], W);
                tag[slot] = rows[k];
            }
            l[k] = ring[slot];
        }
        const uint8_t *up = l[0], *mid = l[1], *dn = l[2];
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        uint32_t hue = (s_phase << 8) + (uint32_t)y * hue_step;          /* 8.8 */
        for (int x = 0; x < W; x++, hue += hue_step) {
            int xl = x >= d ? x - d : 0, xr = x + d < W ? x + d : W - 1;
            int gx = (int)mid[xr] - (int)mid[xl], gy = (int)dn[x] - (int)up[x];
            unsigned e = s_edge[(gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)];
            uint16_t px = src[x];
            unsigned h = (hue >> 8) & 255;
            /* background: the picture, darkened */
            unsigned r = s_bg5[fx_r5(px)], gg = s_bg6[fx_g6(px)], b = s_bg5[fx_b5(px)];
            /* plus the glowing edge (added, so crossing edges get brighter) */
            r += (s_hr[h] * e) >> 8; gg += (s_hg[h] * e) >> 8; b += (s_hb[h] * e) >> 8;
            dst[x] = fx_rgb565((r > 255 ? 255 : r) >> 3, (gg > 255 ? 255 : gg) >> 2, (b > 255 ? 255 : b) >> 3);
        }
    }
}

const fx_desc_t fx_neon = {
    .id = "neon",
    .name = "Neon",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_SOFT,       /* edges glow anyway: preview at half resolution, photos at full */
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
