/*
 * Starburst: the star filter of night photography. Every bright light sends out rays, four
 * or eight of them, in its own colour, fading with distance.
 *
 * How: analyze() maps the frame's highlights onto a coarse grid (one cell per 6 pixels of a
 * 720-wide frame) and then sweeps that grid in each ray direction keeping a decaying
 * maximum, which drags every bright cell into a streak. All directions, columns and
 * diagonals included, are done in two passes down and up the grid, a row at a time. apply() interpolates the ray map up
 * to the frame and adds it to the picture.
 *
 * Row-parallel: the grid is built once per frame (the highlight scan on both cores through
 * fx_rows_parallel), apply() only reads it.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_THRESH, P_LENGTH, P_DIAG, P_STRENGTH };

static const fx_param_t s_params[] = {
    { "thresh",   "Threshold",    0.4f, 0.98f, 0.8f, 0 },
    { "length",   "Ray length",   10, 240, 90, 0 },
    { "diag",     "Eight rays",   0, 1, 0, 1 },
    { "strength", "Brightness",   0.2f, 2.0f, 1.0f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_THRESH] = 0.92f - a * 0.3f;
    p[P_LENGTH] = 30.0f + a * 170.0f;
    p[P_DIAG] = a >= 0.5f ? 1 : 0;
    p[P_STRENGTH] = 0.7f + a * 0.8f;
}

#define SB_CELL     6                   /* grid cell, pixels on a 720-wide frame: also the width of a ray */
#define SB_MAX_GW   192
#define SB_MAX_GH   330
#define SB_ROW_MAX  640                 /* bytes per grid row: 3 x 192, rounded up to 128 */

/* the grid: highlights, then rays, 3 channels of 0..255 each; rows padded to 128 bytes */
static struct {
    uint8_t *lit, *ray;                 /* gh rows of `row` bytes */
    int n, gw, gh, row;
    int w, h;
    size_t bytes;
    bool ok;
} s_g;

static uint8_t s_clamp[256 + 512];      /* s_clamp[v] = min(v, 255) */
static uint8_t s_e5[32], s_e6[64];      /* 5- and 6-bit channel -> 0..255 */
static int s_add_q8;                    /* strength, 8.8 fixed point */

typedef struct { const fx_frame_t *in; int thresh; } scan_arg_t;

/* Highlights of grid rows [g0, g1): the brightest pixel of each cell, if over the threshold,
 * scaled by how far over it is. Only every second pixel of every second row is looked at. */
static void FX_HOT scan_rows(void *arg, int g0, int g1)
{
    const scan_arg_t *a = arg;
    const fx_frame_t *in = a->in;
    int n = s_g.n, gw = s_g.gw, W = in->w, H = in->h, thr = a->thresh;
    int span = 255 - thr;
    for (int gy = g0; gy < g1; gy++) {
        uint8_t *lit = s_g.lit + (size_t)gy * s_g.row;
        int y0 = gy * n, y1 = y0 + n < H ? y0 + n : H;
        for (int gx = 0; gx < gw; gx++) {
            int x0 = gx * n, x1 = x0 + n < W ? x0 + n : W;
            unsigned best = 0;
            uint16_t bp = 0;
            for (int y = y0; y < y1; y += 2) {
                const uint16_t *row = in->px + (size_t)y * in->stride_px;
                for (int x = x0; x < x1; x += 2) {
                    unsigned l = fx_luma(row[x]);
                    if (l > best) { best = l; bp = row[x]; }
                }
            }
            uint8_t *c = lit + gx * 3;
            if ((int)best <= thr) { c[0] = c[1] = c[2] = 0; continue; }
            unsigned k = (unsigned)(((int)best - thr) * 256 / span);      /* 0..256 */
            c[0] = (uint8_t)(((fx_r5(bp) * 255 / 31) * k) >> 8);
            c[1] = (uint8_t)(((fx_g6(bp) * 255 / 63) * k) >> 8);
            c[2] = (uint8_t)(((fx_b5(bp) * 255 / 31) * k) >> 8);
        }
    }
}

static inline uint8_t mx(uint8_t a, uint32_t b) { return a > b ? a : (uint8_t)b; }

/* s = max(s * d, lit) and ray = max(ray, s) over n values: one step of a sweep. At night
 * most of the grid is dark, so values are looked at four at a time and skipped when there
 * is neither a ray passing nor a light (the rows are padded with zeros to a multiple of 4). */
static inline void step_state(uint8_t *st, const uint8_t *lit, uint8_t *ray, int n, uint32_t dq)
{
    const uint32_t *sw = (const uint32_t *)(const void *)st, *lw = (const uint32_t *)(const void *)lit;
    for (int k = 0; k < n; k += 4) {
        if (!(sw[k >> 2] | lw[k >> 2])) continue;
        for (int j = k; j < k + 4; j++) {
            uint32_t v = ((uint32_t)st[j] * dq) >> 8;
            if (lit[j] > v) v = lit[j];
            st[j] = (uint8_t)v;
            if (v > ray[j]) ray[j] = (uint8_t)v;
        }
    }
}

static inline bool all_zero(const uint8_t *row, int n)
{
    const uint32_t *w = (const uint32_t *)(const void *)row;
    uint32_t any = 0;
    for (int k = 0; k < (n + 3) / 4; k++) any |= w[k];
    return !any;
}

/* Along one row, both ways: three channels interleaved, so a step is 3 values. */
static void FX_HOT sweep_row(const uint8_t *lit, uint8_t *ray, int gw, uint32_t dq)
{
    for (int dir = 0; dir < 2; dir++) {
        uint32_t s0 = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < gw; i++) {
            const uint8_t *l = lit + (dir ? gw - 1 - i : i) * 3;
            uint8_t *r = ray + (dir ? gw - 1 - i : i) * 3;
            s0 = (s0 * dq) >> 8; if (l[0] > s0) s0 = l[0]; if (s0 > r[0]) r[0] = (uint8_t)s0;
            s1 = (s1 * dq) >> 8; if (l[1] > s1) s1 = l[1]; if (s1 > r[1]) r[1] = (uint8_t)s1;
            s2 = (s2 * dq) >> 8; if (l[2] > s2) s2 = l[2]; if (s2 > r[2]) r[2] = (uint8_t)s2;
        }
    }
}

/*
 * All rays in two passes over the grid, top to bottom and back, a row at a time. A pass
 * keeps one state row per direction (straight, and diagonally to either side, which is the
 * state row moved by one cell before each step). Working on copies of the current row in
 * on-chip memory, the grid in PSRAM is read and written only a few times in all; sweeping
 * it once per direction cost more than the rest of the effect together.
 */
static void FX_HOT sweep_all(uint32_t dq, uint32_t dq_diag, bool diag)
{
    int gw = s_g.gw, gh = s_g.gh, n = gw * 3;
    if (gw < 2 || n > SB_ROW_MAX) return;               /* frames are always far wider than two cells */
    uint32_t lr4[SB_ROW_MAX / 4], rr4[SB_ROW_MAX / 4], sv4[SB_ROW_MAX / 4], sa4[SB_ROW_MAX / 4], sb4[SB_ROW_MAX / 4];
    uint8_t *lr = (uint8_t *)lr4, *rr = (uint8_t *)rr4, *sv = (uint8_t *)sv4, *sa = (uint8_t *)sa4, *sb = (uint8_t *)sb4;
    memset(lr4, 0, sizeof(lr4));
    memset(rr4, 0, sizeof(rr4));
    for (int pass = 0; pass < 2; pass++) {
        memset(sv4, 0, sizeof(sv4));
        memset(sa4, 0, sizeof(sa4));
        memset(sb4, 0, sizeof(sb4));
        for (int i = 0; i < gh; i++) {
            int gy = pass == 0 ? i : gh - 1 - i;
            memcpy(lr, s_g.lit + (size_t)gy * s_g.row, (size_t)n);
            if (pass == 0) {
                memset(rr, 0, (size_t)n);
                if (!all_zero(lr, n)) sweep_row(lr, rr, gw, dq);
            } else {
                memcpy(rr, s_g.ray + (size_t)gy * s_g.row, (size_t)n);
            }
            step_state(sv, lr, rr, n, dq);
            if (diag) {
                memmove(sa + 3, sa, (size_t)(n - 3));       /* towards the right */
                sa[0] = sa[1] = sa[2] = 0;
                memmove(sb, sb + 3, (size_t)(n - 3));       /* towards the left */
                sb[n - 3] = sb[n - 2] = sb[n - 1] = 0;
                step_state(sa, lr, rr, n, dq_diag);
                step_state(sb, lr, rr, n, dq_diag);
            }
            memcpy(s_g.ray + (size_t)gy * s_g.row, rr, (size_t)n);
        }
    }
}

static void analyze(const fx_frame_t *in, const float *p, const fx_ctx_t *ctx)
{
    (void)ctx;
    s_g.ok = false;
    if (!s_clamp[511]) {
        for (int i = 0; i < 768; i++) s_clamp[i] = (uint8_t)(i < 255 ? i : 255);
        for (int i = 0; i < 32; i++) s_e5[i] = (uint8_t)(i * 255 / 31);
        for (int i = 0; i < 64; i++) s_e6[i] = (uint8_t)(i * 255 / 63);
    }
    int n = fx_px(in, SB_CELL, 2);
    while ((in->w + n - 1) / n > SB_MAX_GW || (in->h + n - 1) / n > SB_MAX_GH) n++;
    int gw = (in->w + n - 1) / n, gh = (in->h + n - 1) / n;
    int row = (gw * 3 + 127) & ~127;
    size_t bytes = (size_t)row * gh;
    if (!s_g.lit) {
        size_t cap = (size_t)SB_ROW_MAX * SB_MAX_GH;
        s_g.lit = fx_big_alloc(cap * 2);                     /* highlights, rays */
        if (!s_g.lit) return;
        s_g.ray = s_g.lit + cap;
    }
    s_g.n = n; s_g.gw = gw; s_g.gh = gh; s_g.row = row;

    scan_arg_t a = { in, (int)(fx_clampf(p[P_THRESH], 0.4f, 0.98f) * 255.0f) };
    fx_rows_parallel(scan_rows, &a, 0, gh, in, s_g.lit, (size_t)row);

    /* decay per cell so that a ray fades to a tenth over `length` pixels */
    float len_cells = fx_clampf(p[P_LENGTH], 10, 240) * fx_scale(in) / (float)n;
    float d = expf(logf(0.1f) / (len_cells > 1 ? len_cells : 1));
    int dq = (int)(d * 256.0f), dq_diag = (int)(powf(d, 1.4142f) * 256.0f);
    sweep_all((uint32_t)dq, (uint32_t)dq_diag, p[P_DIAG] >= 0.5f);
    s_add_q8 = (int)(fx_clampf(p[P_STRENGTH], 0.2f, 2.0f) * 256.0f);
    s_g.w = in->w; s_g.h = in->h;
    s_g.bytes = bytes;
    fx_mem_publish(s_g.ray, s_g.bytes);
    s_g.ok = true;
}

/* Add rays to pixels [x0, x1) of a row, interpolating from cell c0 to cell c1 (already
 * scaled, 0..255 per channel); v0 is where along that step x0 lies, in 1/65536 of a cell. */
static inline void add_span(const uint16_t *src, uint16_t *dst, int x0, int x1, const uint8_t *c0, const uint8_t *c1,
                            int32_t step, int32_t v0)
{
    if (!(c0[0] | c0[1] | c0[2] | c1[0] | c1[1] | c1[2])) {         /* no ray here */
        if (src != dst) memcpy(dst + x0, src + x0, (size_t)(x1 - x0) * 2);
        return;
    }
    int32_t dr = (c1[0] - c0[0]) * step, dg = (c1[1] - c0[1]) * step, db = (c1[2] - c0[2]) * step;
    int32_t r = (c0[0] << 16) + (int32_t)(((int64_t)(c1[0] - c0[0]) * v0)), g = (c0[1] << 16) + (int32_t)(((int64_t)(c1[1] - c0[1]) * v0));
    int32_t b = (c0[2] << 16) + (int32_t)(((int64_t)(c1[2] - c0[2]) * v0));
    for (int x = x0; x < x1; x++, r += dr, g += dg, b += db) {
        uint16_t px = src[x];
        dst[x] = fx_rgb565(s_clamp[s_e5[fx_r5(px)] + (r >> 16)] >> 3, s_clamp[s_e6[fx_g6(px)] + (g >> 16)] >> 2,
                           s_clamp[s_e5[fx_b5(px)] + (b >> 16)] >> 3);
    }
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    int W = in->w;
    if (!s_g.ok || s_g.w != W || s_g.h != in->h) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    fx_mem_fetch(s_g.ray, s_g.bytes);
    int n = s_g.n, gw = s_g.gw, gh = s_g.gh, half = n / 2;
    int32_t step = 65536 / n;
    int32_t add = s_add_q8;
    uint8_t rowc[SB_ROW_MAX];                                   /* this row's rays, blended and scaled */

    for (int y = ctx->y0; y < ctx->y1; y++) {
        int t = y - half, gy = 0;
        int32_t wy = 0;
        if (t >= 0) { gy = t / n; wy = ((t - gy * n) * 256) / n; }
        if (gy >= gh - 1) { gy = gh - 1; wy = 0; }
        const uint8_t *ra = s_g.ray + (size_t)gy * s_g.row;
        const uint8_t *rb = wy ? ra + s_g.row : ra;
        unsigned any = 0;
        for (int k = 0; k < gw * 3; k++) {
            int32_t v = ((((int32_t)ra[k] * (256 - wy) + (int32_t)rb[k] * wy) >> 8) * add) >> 8;
            rowc[k] = (uint8_t)(v > 255 ? 255 : v);
            any |= rowc[k];
        }
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        if (!any) {                                             /* no ray crosses this row */
            if (src != dst) memcpy(dst, src, (size_t)W * 2);
            continue;
        }
        /* cell centres sit at half, half + n, ...: flat before the first and after the last */
        int x = half < W ? half : W;
        add_span(src, dst, 0, x, rowc, rowc, 0, 0);
        for (int gx = 0; gx + 1 < gw && x < W; gx++) {
            int x1 = x + n < W ? x + n : W;
            add_span(src, dst, x, x1, rowc + gx * 3, rowc + gx * 3 + 3, step, 0);
            x = x1;
        }
        if (x < W) add_span(src, dst, x, W, rowc + (gw - 1) * 3, rowc + (gw - 1) * 3, 0, 0);
    }
}

const fx_desc_t fx_starburst = {
    .id = "starburst",
    .name = "Starburst",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = true,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_SOFT,       /* the rays are soft: preview at half resolution, photos at full */
    .from_amount = from_amount,
    .analyze = analyze,
    .apply = apply,
};
