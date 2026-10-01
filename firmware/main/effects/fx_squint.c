/*
 * Squint: what a painter sees through half-closed eyes. Small detail is blurred away and the
 * picture is reduced to a few flat steps of light and dark (its "values"), so the big shapes
 * of light and shadow stand out.
 *
 * How: analyze() averages the frame into a coarse grid of cells (one cell per `blur`/2
 * pixels), softens that grid with its neighbours, and apply() draws every output pixel by
 * interpolating between the four nearest cells. Together that is close to a Gaussian blur for
 * the price of one pass. Each cell carries its brightness next to its colour; the
 * interpolated brightness is snapped to one of `values` steps and the colour is shifted by
 * the same amount, so the shapes get smooth outlines and keep a hint of colour.
 *
 * The steps are laid out between the darkest and the brightest parts of this picture (its
 * 3rd and 97th percentile), not between black and white: a dim room and a sunny street both
 * split into the same number of shapes.
 *
 * Row-parallel: the grid and the tables are built once per frame in analyze() (the averaging
 * itself runs on both cores through fx_rows_parallel); apply() only reads them.
 *
 * Performance: this effect is bound by memory, not arithmetic. Reading the frame for the
 * averages costs as much as everything else, so only every second row is read (every pixel
 * of it, two per load). The per-pixel work of apply() is done once per 2x2 block: the
 * picture is blurred over eight pixels or more and the outlines are soft, so nothing of that
 * shows, and what remains is mostly the cost of writing the frame.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_BLUR, P_VALUES, P_COLOR, P_SOFT, P_FLAT };

static const fx_param_t s_params[] = {
    { "blur",   "Blur (px)",   8, 64, 16, 0 },
    { "values", "Values",      2, 8, 4, 1 },
    { "color",  "Colour",      0, 1, 0.6f, 0 },
    { "soft",   "Soft edges",  0, 1, 0.3f, 0 },
    { "flat",   "Flat values", 0, 1, 1, 1 },
};

static void from_amount(float a, float *p)
{
    p[P_BLUR] = 8.0f + a * 40.0f;
    p[P_VALUES] = floorf(7.5f - a * 5.0f);      /* 7 -> 2 */
    p[P_COLOR] = 0.8f - a * 0.6f;
    p[P_SOFT] = 0.3f;
    p[P_FLAT] = 1;
}

#define SQ_MAX_W    1088        /* widest frame (a full-resolution still) */
#define SQ_MAX_GW   256         /* most cells per row */
#define SQ_MIN_CAP  16000       /* cells: enough for the smallest blur at every frame size (64 KB) */

/* The grid. Cell rows are padded to a multiple of 32 cells (128 bytes), so that a row
 * boundary is always a cache-line boundary: the two cores write different rows of it. */
static struct {
    uint32_t *cells;            /* gh rows of `stride` cells, each 0xYYRRGGBB (brightness, then colour) */
    uint32_t *work;             /* two sets of three sums per cell column (one per core), then a spare row */
    size_t cap;                 /* cells allocated */
    size_t bytes;               /* gh * stride * 4, rounded up to 128: what apply() fetches */
    int n, gw, gh, stride;      /* cell size in pixels, grid size, cells per padded row */
    int w, h;                   /* the frame the grid was built from */
    bool ok;
} s_g;

static int16_t s_delta[256];    /* brightness -> how far to shift it to reach its step */
static uint8_t s_clamp[768];    /* s_clamp[v + 256] = v limited to 0..255 */

/* One cell blended with its two neighbours: a/256 of each neighbour, the rest its own.
 * The four 8-bit fields are handled two at a time. */
static inline uint32_t mix3(uint32_t l, uint32_t c, uint32_t r, uint32_t a)
{
    uint32_t m = 256 - 2 * a;
    uint32_t even = ((((l & 0x00FF00FF) + (r & 0x00FF00FF)) * a + (c & 0x00FF00FF) * m) >> 8) & 0x00FF00FF;
    uint32_t odd = ((((l >> 8) & 0x00FF00FF) + ((r >> 8) & 0x00FF00FF)) * a + ((c >> 8) & 0x00FF00FF) * m) & 0xFF00FF00;
    return even | odd;
}

typedef struct {
    const fx_frame_t *in;
    int sx, sy;                 /* sampling steps across and down */
    int sat;                    /* colour: 0..256 */
} avg_arg_t;

/* Average the cells of grid rows [g0, g1): runs on both cores, each with its own sums. */
static void FX_HOT average_rows(void *arg, int g0, int g1)
{
    const avg_arg_t *a = arg;
    const fx_frame_t *in = a->in;
    int W = in->w, H = in->h, n = s_g.n, gw = s_g.gw, sx = a->sx, sy = a->sy, sat = a->sat;
    uint32_t *sr = s_g.work + (g0 == 0 ? 0 : 3 * SQ_MAX_GW), *sg = sr + SQ_MAX_GW, *sb = sg + SQ_MAX_GW;

    for (int gy = g0; gy < g1; gy++) {
        memset(sr, 0, 3 * SQ_MAX_GW * sizeof(uint32_t));
        int y1 = (gy + 1) * n < H ? (gy + 1) * n : H, rows = 0;
        for (int y = gy * n; y < y1; y += sy, rows++) {
            const uint16_t *row = in->px + (size_t)y * in->stride_px;
            for (int gx = 0; gx < gw; gx++) {
                int x = gx * n, x1 = x + n < W ? x + n : W;
                uint32_t rb = 0, g = 0;
                if (sx == 1 && !((uintptr_t)(row + x) & 3)) {      /* two pixels per load */
                    const uint32_t *r32 = (const uint32_t *)(row + x);
                    for (; x + 2 <= x1; x += 2, r32++) {
                        uint32_t w = *r32, p0 = w & 0xFFFF, p1 = w >> 16;
                        rb += (p0 & 0xF81F) + (p1 & 0xF81F);
                        g += (p0 & 0x07E0) + (p1 & 0x07E0);
                    }
                }
                for (; x < x1; x += sx) {
                    uint32_t px = row[x];
                    rb += px & 0xF81F;
                    g += px & 0x07E0;
                }
                sr[gx] += rb >> 11;
                sb[gx] += rb & 0x7FF;
                sg[gx] += g >> 5;
            }
        }
        uint32_t *crow = s_g.cells + (size_t)gy * s_g.stride;
        uint32_t last_cnt = 0, rec5 = 0, rec6 = 0;          /* sum -> 0..255 average, as 16.16 factors */
        for (int gx = 0; gx < gw; gx++) {
            int x1 = (gx + 1) * n < W ? (gx + 1) * n : W;
            uint32_t cnt = (uint32_t)((x1 - gx * n + sx - 1) / sx) * (uint32_t)rows;
            if (cnt != last_cnt) {                          /* only the last column differs */
                rec5 = (255u << 16) / (31u * cnt);
                rec6 = (255u << 16) / (63u * cnt);
                last_cnt = cnt;
            }
            int r = (int)((sr[gx] * rec5 + 0x8000u) >> 16);
            int g = (int)((sg[gx] * rec6 + 0x8000u) >> 16);
            int b = (int)((sb[gx] * rec5 + 0x8000u) >> 16);
            int lum = (r * 77 + g * 151 + b * 28) >> 8;
            r = lum + (r - lum) * sat / 256;                /* the colour setting: towards grey */
            g = lum + (g - lum) * sat / 256;
            b = lum + (b - lum) * sat / 256;
            crow[gx] = ((uint32_t)lum << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void analyze(const fx_frame_t *in, const float *p, const fx_ctx_t *ctx)
{
    (void)ctx;
    int W = in->w, H = in->h;
    s_g.ok = false;
    if (W > SQ_MAX_W) return;
    if (!s_clamp[767]) {
        for (int i = 0; i < 768; i++) s_clamp[i] = (uint8_t)fx_clampi(i - 256, 0, 255);
    }

    /* Cells of half the blur size, softened with a 1-2-1 blur. Below 16 px the cells stay
     * at 8 px and the softening is turned down instead. */
    float blur = fx_clampf(p[P_BLUR], 8, 64);
    uint32_t soften = blur >= 16 ? 64 : (uint32_t)((blur - 8) * 8);
    int n = fx_px(in, blur >= 16 ? blur / 2 : 8, 2);
    n += n & 1;                                             /* even: apply() works in pairs of pixels */
    while ((W + n - 1) / n > SQ_MAX_GW) n += 2;
    int gw = (W + n - 1) / n, gh = (H + n - 1) / n, stride = (gw + 31) & ~31;
    size_t need = (size_t)stride * (size_t)gh;
    if (need > s_g.cap) {
        size_t cap = need > SQ_MIN_CAP ? need : SQ_MIN_CAP;
        uint32_t *mem = fx_big_alloc((cap + 7 * SQ_MAX_GW) * sizeof(uint32_t));
        if (!mem) return;
        s_g.work = mem;
        s_g.cells = mem + 7 * SQ_MAX_GW;                    /* 7 * 1 KB keeps it on a 128-byte boundary */
        s_g.cap = cap;
    }
    s_g.n = n; s_g.gw = gw; s_g.gh = gh; s_g.stride = stride;
    s_g.bytes = (need * sizeof(uint32_t) + 127) & ~(size_t)127;

    /* Red and blue are summed in one word, which holds up to 64 pixels per row of a cell. */
    avg_arg_t a = { in, n > 64 ? 2 : 1, n >= 4 ? 2 : 1, (int)(fx_clampf(p[P_COLOR], 0, 1) * 256.0f + 0.5f) };
    fx_rows_parallel(average_rows, &a, 0, gh, in, s_g.cells, (size_t)stride * sizeof(uint32_t));

    /* Soften the grid: along the rows, then down the columns (a spare row keeps the values
     * of the row above as they were before it was overwritten). */
    if (soften) {
        for (int gy = 0; gy < gh; gy++) {
            uint32_t *row = s_g.cells + (size_t)gy * stride, prev = row[0];
            for (int gx = 0; gx < gw; gx++) {
                uint32_t cur = row[gx], next = row[gx + 1 < gw ? gx + 1 : gx];
                row[gx] = mix3(prev, cur, next, soften);
                prev = cur;
            }
        }
        uint32_t *above = s_g.work + 6 * SQ_MAX_GW;
        memcpy(above, s_g.cells, (size_t)gw * sizeof(uint32_t));
        for (int gy = 0; gy < gh; gy++) {
            uint32_t *row = s_g.cells + (size_t)gy * stride, *below = gy + 1 < gh ? row + stride : row;
            for (int gx = 0; gx < gw; gx++) {
                uint32_t cur = row[gx];
                row[gx] = mix3(above[gx], cur, below[gx], soften);
                above[gx] = cur;
            }
        }
    }

    /* The darkest and the brightest 3 % of the cells mark the range the steps are spread over. */
    uint32_t hist[64] = { 0 };
    for (int gy = 0; gy < gh; gy++) {
        const uint32_t *row = s_g.cells + (size_t)gy * stride;
        for (int gx = 0; gx < gw; gx++) hist[row[gx] >> 26]++;
    }
    uint32_t edge = (uint32_t)(gw * gh) * 3 / 100, acc = 0;
    int lo = 0, hi = 255;
    for (int i = 0; i < 64; i++) {
        acc += hist[i];
        if (acc > edge) { lo = i * 4; break; }
    }
    acc = 0;
    for (int i = 63; i >= 0; i--) {
        acc += hist[i];
        if (acc > edge) { hi = i * 4 + 3; break; }
    }
    if (hi - lo < 48) {                                     /* a flat scene: do not make shapes out of noise */
        int mid = (lo + hi) / 2;
        lo = fx_clampi(mid - 24, 0, 255 - 48);
        hi = lo + 48;
    }

    /* Brightness v -> its step. "Soft edges" widens the jump between two steps into a ramp
     * that covers part of a step (all of it at 1, which leaves the brightness untouched). */
    int k = fx_clampi((int)lroundf(p[P_VALUES]), 2, 8);
    bool flat = p[P_FLAT] >= 0.5f;
    float ramp = 0.04f + 0.96f * fx_clampf(p[P_SOFT], 0, 1);
    for (int v = 0; v < 256; v++) {
        float u = (float)(v - lo) / (float)(hi - lo) * (float)(k - 1);
        u = fx_clampf(u, 0, (float)(k - 1));
        float i = floorf(u), f = fx_clampf((u - i - 0.5f) / ramp + 0.5f, 0, 1);
        int yq = (int)lroundf((float)lo + (float)(hi - lo) * (i + f) / (float)(k - 1));
        s_delta[v] = flat ? (int16_t)(yq - v) : 0;
    }

    s_g.w = W; s_g.h = H;
    fx_mem_publish(s_g.cells, s_g.bytes);
    s_g.ok = true;
}

/* One finished pixel from a brightness and a colour (0..255 each). */
static inline uint16_t shade(int lum, int r, int g, int b)
{
    const uint8_t *cl = s_clamp + 256 + s_delta[lum];
    return (uint16_t)(((cl[r] >> 3) << 11) | ((cl[g] >> 2) << 5) | (cl[b] >> 3));
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    int W = in->w;
    if (!s_g.ok || s_g.w != W || s_g.h != in->h) {          /* no grid (no memory, or a frame too wide) */
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    int n = s_g.n, gw = s_g.gw, gh = s_g.gh, stride = s_g.stride, half = n / 2;
    int32_t recip2 = 2 * 65536 / n;         /* 16.16 step per pair of pixels, as a fraction of a cell */
    uint32_t rowc[SQ_MAX_GW];               /* this row's cells, already blended between two grid rows */
    uint16_t line[SQ_MAX_W];                /* the row is built on the stack and copied out in one go */
    fx_mem_fetch(s_g.cells, s_g.bytes);

    /* Rows go in pairs (y even, y + 1): one line serves both. A share that starts on an odd
     * row renders the pair it belongs to and keeps only its own row, so the result does not
     * depend on where the rows were split. */
    for (int y = ctx->y0; y < ctx->y1; y += 2 - (y & 1)) {
        int base = y & ~1;
        /* Cell centres sit at n/2, n/2 + n, ...: find the grid row above the middle of this pair
         * of rows and the weight of the one below. */
        int t = base - half, gy = 0;
        uint32_t wy = 0;
        if (t >= 0) {
            gy = t / n;
            wy = (uint32_t)((2 * (t - gy * n) + 2) * 128 / n);
        }
        if (gy >= gh - 1) { gy = gh - 1; wy = 0; }
        const uint32_t *ca = s_g.cells + (size_t)gy * stride, *cb = wy ? ca + stride : ca;
        uint32_t wa = 256 - wy;
        for (int gx = 0; gx < gw; gx++) {
            uint32_t a = ca[gx], b = cb[gx];                /* four 8-bit fields, blended two at a time */
            uint32_t even = (((a & 0x00FF00FF) * wa + (b & 0x00FF00FF) * wy) >> 8) & 0x00FF00FF;
            uint32_t odd = (((a >> 8) & 0x00FF00FF) * wa + ((b >> 8) & 0x00FF00FF) * wy) & 0xFF00FF00;
            rowc[gx] = even | odd;
        }

        int x = 0;
        uint32_t c = rowc[0];                               /* left of the first cell centre: flat */
        uint16_t px = shade((int)(c >> 24), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255));
        for (int end = half < W ? half : W; x < end; x++) line[x] = px;

        for (int gx = 0; gx + 1 < gw && x < W; gx++) {
            uint32_t a = rowc[gx], b = rowc[gx + 1];
            int32_t al = (int32_t)(a >> 24), ar = (int32_t)((a >> 16) & 255), ag = (int32_t)((a >> 8) & 255), ab = (int32_t)(a & 255);
            int32_t sl = ((int32_t)(b >> 24) - al) * recip2, sr = ((int32_t)((b >> 16) & 255) - ar) * recip2;
            int32_t sg = ((int32_t)((b >> 8) & 255) - ag) * recip2, sb = ((int32_t)(b & 255) - ab) * recip2;
            al = (al << 16) + sl / 2; ar = (ar << 16) + sr / 2;         /* the middle of the first pair */
            ag = (ag << 16) + sg / 2; ab = (ab << 16) + sb / 2;
            int end = x + n < W ? x + n : W;
            for (; x + 2 <= end; x += 2) {
                px = shade(al >> 16, ar >> 16, ag >> 16, ab >> 16);
                line[x] = px;
                line[x + 1] = px;
                al += sl; ar += sr; ag += sg; ab += sb;
            }
            if (x < end) line[x++] = shade(al >> 16, ar >> 16, ag >> 16, ab >> 16);     /* odd width */
        }

        c = rowc[gw - 1];                                   /* right of the last centre: flat */
        px = shade((int)(c >> 24), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255));
        for (; x < W; x++) line[x] = px;

        memcpy(out->px + (size_t)y * out->stride_px, line, (size_t)W * sizeof(uint16_t));
        if (y == base && y + 1 < ctx->y1) {
            memcpy(out->px + (size_t)(y + 1) * out->stride_px, line, (size_t)W * sizeof(uint16_t));
        }
    }
}

const fx_desc_t fx_squint = {
    .id = "squint",
    .name = "Squint",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = true,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_SOFT,
    .from_amount = from_amount,
    .analyze = analyze,
    .apply = apply,
};
