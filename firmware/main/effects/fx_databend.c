/*
 * Databend: the look of a JPEG file with a few damaged bytes.
 *
 * A JPEG stores the picture as a row-by-row stream of small blocks, and each block's
 * brightness and colour are stored as the *difference* from the block before it. Damage
 * one byte and the decoder loses its place: from that block on, to the end of the file or
 * to the next restart marker, the picture is shifted sideways and its colours are off by
 * a constant amount. A second damaged byte adds another shift on top.
 *
 * This effect draws that result directly instead of really corrupting a file: a handful
 * of "breaks" are placed along the block stream, and each one changes the running state
 * (sideways shift, colour offset, or a stuck block that smears across the row) for
 * everything after it. Doing it this way keeps it fast, repeatable from the seed, and
 * usable at any frame size.
 *
 * Row-parallel: prepare() lays out the breaks once per frame; apply() looks up the state
 * for the blocks it draws.
 */
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BREAKS, P_SHIFT, P_COLOR, P_HOLD, P_BLOCK };

static const fx_param_t s_params[] = {
    { "breaks", "Breaks",         1, 40, 10, 1 },
    { "shift",  "Shift",          0, 1, 0.5f, 0 },
    { "color",  "Colour damage",  0, 1, 0.5f, 0 },
    { "hold",   "Hold (frames)",  1, 60, 6, 1 },
    { "block",  "Block size (px)", 8, 64, 16, 1 },
};

static void from_amount(float a, float *p)
{
    p[P_BREAKS] = 2.0f + a * 30.0f;
    p[P_SHIFT] = a;
    p[P_COLOR] = 0.2f + a * 0.8f;
    p[P_HOLD] = 12.0f - a * 9.0f;
    p[P_BLOCK] = 16.0f;
}

#define MAX_BREAKS 40

typedef struct {
    uint16_t at;                /* where along the block stream, 0..65535 */
    int16_t shift;              /* running sideways shift, in 1/256 of the width */
    int8_t dr, dg, db;          /* running colour offset, in RGB565 steps */
    uint8_t smear;              /* the block at the break is repeated to the end of its run */
} break_t;

static break_t s_breaks[MAX_BREAKS];
static int s_n;

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    int n = fx_clampi((int)p[P_BREAKS], 1, MAX_BREAKS);
    float shift = fx_clampf(p[P_SHIFT], 0, 1), color = fx_clampf(p[P_COLOR], 0, 1);
    int hold = fx_clampi((int)p[P_HOLD], 1, 1000);

    /* the damage stays put for `hold` frames, then a new file is "damaged" */
    fx_ctx_t held = *ctx;
    held.frame_no = ctx->frame_no / (uint32_t)hold;
    fx_rng_t rng;
    fx_rng_init(&rng, &held, 0xDA7A);

    for (int i = 0; i < n; i++) s_breaks[i].at = (uint16_t)(fx_rng_u32(&rng) >> 16);
    for (int i = 1; i < n; i++) {                          /* insertion sort by position */
        uint16_t v = s_breaks[i].at;
        int j = i - 1;
        while (j >= 0 && s_breaks[j].at > v) { s_breaks[j + 1].at = s_breaks[j].at; j--; }
        s_breaks[j + 1].at = v;
    }

    int sh = 0, dr = 0, dg = 0, db = 0;
    int max_shift = (int)(shift * 96.0f), max_c = (int)(color * 10.0f);
    for (int i = 0; i < n; i++) {
        uint32_t kind = fx_rng_u32(&rng) % 10u;
        if (kind == 0) {                                   /* a restart marker: decoder recovers */
            sh = dr = dg = db = 0;
        } else {
            if (max_shift) sh += fx_rng_range(&rng, -max_shift, max_shift);
            if (max_c) {
                dr = fx_clampi(dr + fx_rng_range(&rng, -max_c, max_c), -14, 14);
                dg = fx_clampi(dg + fx_rng_range(&rng, -2 * max_c, 2 * max_c), -28, 28);
                db = fx_clampi(db + fx_rng_range(&rng, -max_c, max_c), -14, 14);
            }
        }
        s_breaks[i].shift = (int16_t)sh;
        s_breaks[i].dr = (int8_t)dr;
        s_breaks[i].dg = (int8_t)dg;
        s_breaks[i].db = (int8_t)db;
        s_breaks[i].smear = kind == 1 && color > 0;
    }
    s_n = n;
}

/* dst[0..n) = src[sx..] (wrapping at W) with a colour offset added. */
static void FX_HOT run(uint16_t *dst, const uint16_t *src, int sx, int n, int W, const break_t *b)
{
    if (b->smear) {
        uint16_t c = src[sx];
        for (int i = 0; i < n; i++) dst[i] = c;
        return;
    }
    if (!b->dr && !b->dg && !b->db) {
        int first = W - sx < n ? W - sx : n;
        memcpy(dst, src + sx, (size_t)first * sizeof(uint16_t));
        if (n > first) memcpy(dst + first, src, (size_t)(n - first) * sizeof(uint16_t));
        return;
    }
    int dr = b->dr, dg = b->dg, db = b->db;
    for (int i = 0; i < n; i++) {
        uint16_t s = src[sx];
        if (++sx == W) sx = 0;
        int r = (int)fx_r5(s) + dr, g = (int)fx_g6(s) + dg, bl = (int)fx_b5(s) + db;
        r = r < 0 ? 0 : (r > 31 ? 31 : r);
        g = g < 0 ? 0 : (g > 63 ? 63 : g);
        bl = bl < 0 ? 0 : (bl > 31 ? 31 : bl);
        dst[i] = fx_rgb565((unsigned)r, (unsigned)g, (unsigned)bl);
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    static const break_t clean = { 0 };
    int W = in->w, H = in->h;
    int B = fx_clampi(fx_px(in, p[P_BLOCK], 4), 4, 256);
    int cols = (W + B - 1) / B, rows = (H + B - 1) / B;
    uint32_t total = (uint32_t)cols * (uint32_t)rows;

    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        uint32_t row_first = (uint32_t)(y / B) * (uint32_t)cols;       /* stream index of this row's first block */

        /* the state in force at the start of the row: the last break at or before it */
        int i = 0;
        const break_t *state = &clean;
        while (i < s_n && ((uint32_t)s_breaks[i].at * total >> 16) <= row_first) state = &s_breaks[i++];

        int x = 0;
        while (x < W) {
            /* this run ends at the next break inside the row, or at the row's end */
            int x_end = W;
            if (i < s_n) {
                uint32_t blk = (uint32_t)s_breaks[i].at * total >> 16;
                if (blk < row_first + (uint32_t)cols) x_end = (int)(blk - row_first) * B;
            }
            if (x_end > W) x_end = W;
            if (x_end > x) {
                int sx = (x + state->shift * W / 256) % W;
                if (sx < 0) sx += W;
                run(dst + x, src, sx, x_end - x, W, state);
                x = x_end;
            }
            if (x < W) state = &s_breaks[i++];
        }
    }
}

const fx_desc_t fx_databend = {
    .id = "databend",
    .name = "Databend",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_MEDIUM,
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
