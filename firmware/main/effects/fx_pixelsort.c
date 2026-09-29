/*
 * Pixel sorting (Kim Asendorf style). Along each row or column find runs of pixels whose
 * brightness lies inside [lo, hi] and sort the pixels of each run by brightness. Runs that
 * start/end at bright or dark edges produce the typical "melting" streaks.
 *
 * Sorting uses a counting sort over the 256 luma levels, so it is O(n) per run and needs no
 * recursion. Scratch lives in static buffers: this effect is not row-parallel.
 *
 * Vertical mode (the classic look) is cache-hostile if done column by column, so columns are
 * processed in tiles of TILE_COLS: gather the tile row by row (one contiguous chunk per row),
 * sort each column inside the tile, scatter it back row by row.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_LO, P_HI, P_VERTICAL, P_REVERSE, P_MIN_RUN, P_SKIP };

static const fx_param_t s_params[] = {
    { "lo",       "Luma low",         0, 255, 60 },
    { "hi",       "Luma high",        0, 255, 200 },
    { "vertical", "Vertical (1)",     0, 1, 1 },
    { "reverse",  "Dark to bright",   0, 1, 0 },
    { "min_run",  "Min run (px)",     2, 400, 12 },
    { "skip",     "Skip chance",      0, 1, 0.0f },
};

static void from_amount(float a, float *p)
{
    /* widen the window with amount: more pixels qualify -> longer streaks */
    p[P_LO] = 120.0f - a * 110.0f;
    p[P_HI] = 140.0f + a * 110.0f;
    p[P_VERTICAL] = 1;
    p[P_REVERSE] = 0;
    p[P_MIN_RUN] = 40.0f - a * 30.0f;
    p[P_SKIP] = 0.6f - a * 0.6f;
}

#define MAX_LINE   2048
#define TILE_COLS  16                       /* 16 columns x 2 bytes = 32 B contiguous per row */
static uint16_t s_line[MAX_LINE];
static uint8_t  s_luma[MAX_LINE];
static uint16_t s_sorted[MAX_LINE];
static uint16_t s_tile[TILE_COLS * MAX_LINE];   /* column-major tile: [col][row] */

/* counting sort of line[a..b) by luma, ascending (or descending when reverse) */
static void FX_HOT sort_run(int a, int b, int reverse)
{
    unsigned hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = a; i < b; i++) hist[s_luma[i]]++;
    unsigned pos = 0;
    if (!reverse) {
        for (int v = 0; v < 256; v++) { unsigned c = hist[v]; hist[v] = pos; pos += c; }
    } else {
        for (int v = 255; v >= 0; v--) { unsigned c = hist[v]; hist[v] = pos; pos += c; }
    }
    for (int i = a; i < b; i++) {
        s_sorted[hist[s_luma[i]]++] = s_line[i];
    }
    memcpy(&s_line[a], s_sorted, (size_t)(b - a) * sizeof(uint16_t));
}

static void FX_HOT process_line(int n, int lo, int hi, int reverse, int min_run)
{
    for (int i = 0; i < n; i++) s_luma[i] = (uint8_t)fx_luma(s_line[i]);
    int i = 0;
    while (i < n) {
        if (s_luma[i] < lo || s_luma[i] > hi) { i++; continue; }
        int j = i + 1;
        while (j < n && s_luma[j] >= lo && s_luma[j] <= hi) j++;
        if (j - i >= min_run) sort_run(i, j, reverse);
        i = j;
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    fx_rng_t rng;
    fx_rng_init(&rng, ctx, 0x5027);

    int lo = fx_clampi((int)p[P_LO], 0, 255), hi = fx_clampi((int)p[P_HI], 0, 255);
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int vertical = p[P_VERTICAL] >= 0.5f;
    int reverse = p[P_REVERSE] >= 0.5f;
    int min_run = fx_clampi((int)p[P_MIN_RUN], 2, MAX_LINE);
    float skip = fx_clampf(p[P_SKIP], 0, 1);
    int W = in->w, H = in->h;
    if (H > MAX_LINE) H = MAX_LINE;

    if (!vertical) {
        for (int y = 0; y < H; y++) {
            const uint16_t *src = in->px + (size_t)y * in->stride_px;
            uint16_t *dst = out->px + (size_t)y * out->stride_px;
            if (skip > 0 && fx_rng_f(&rng) < skip) { memcpy(dst, src, (size_t)W * 2); continue; }
            memcpy(s_line, src, (size_t)W * 2);
            process_line(W, lo, hi, reverse, min_run);
            memcpy(dst, s_line, (size_t)W * 2);
        }
        return;
    }

    /* vertical: tiles of TILE_COLS columns */
    for (int x0 = 0; x0 < W; x0 += TILE_COLS) {
        int cols = W - x0 < TILE_COLS ? W - x0 : TILE_COLS;
        /* gather: row y, columns x0..x0+cols -> tile[c][y] */
        for (int y = 0; y < H; y++) {
            const uint16_t *src = in->px + (size_t)y * in->stride_px + x0;
            for (int c = 0; c < cols; c++) s_tile[c * MAX_LINE + y] = src[c];
        }
        /* sort each column of the tile in place */
        for (int c = 0; c < cols; c++) {
            if (skip > 0 && fx_rng_f(&rng) < skip) continue;
            memcpy(s_line, &s_tile[c * MAX_LINE], (size_t)H * 2);
            process_line(H, lo, hi, reverse, min_run);
            memcpy(&s_tile[c * MAX_LINE], s_line, (size_t)H * 2);
        }
        /* scatter */
        for (int y = 0; y < H; y++) {
            uint16_t *dst = out->px + (size_t)y * out->stride_px + x0;
            for (int c = 0; c < cols; c++) dst[c] = s_tile[c * MAX_LINE + y];
        }
    }
}

const fx_desc_t fx_pixelsort = {
    .id = "pixelsort",
    .name = "Pixel sort",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = false,
    .cost = FX_COST_HEAVY,
    .from_amount = from_amount,
    .apply = apply,
};
