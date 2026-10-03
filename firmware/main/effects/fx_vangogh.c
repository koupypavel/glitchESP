/*
 * Van Gogh: the picture redrawn in short, thick, directed brush strokes.
 *
 * This is not a neural "style transfer" (far out of reach of this chip) but the recipe a
 * painter's hand follows:
 *   - one dab of paint per small cell of the picture, in that cell's average colour, with the
 *     colour pushed and every dab a little lighter or darker than its neighbours;
 *   - the dab is longer than wide and lies along the edges of what it paints (the direction
 *     comes from the brightness gradient of the coarse picture);
 *   - where the picture is flat (sky, walls) there is no edge to follow, and the strokes
 *     fall into slow swirls around a few centres instead: the Starry Night sky;
 *   - thick paint: each stroke has a lit and a shaded flank and a dark groove around it.
 *
 * How: analyze() averages the frame into a grid of cells (both cores, as in Squint), then
 * turns every cell into a stroke record: centre (jittered), direction, four ready shades of
 * its colour. apply() paints the strokes as overlapping ellipses, row by row: for each row
 * it walks the strokes of the five cell rows around it and fills each stroke's span on this
 * row. Across the stroke the shade goes from lit flank to plain to shaded flank, and its
 * rim is the dark groove. Strokes are laid in two layers picked at random, always in the
 * same order, so a pixel gets the same stroke whichever core draws its row. What no stroke
 * covers shows the underpainting (the groove shade of its cell).
 *
 * The stroke positions depend on the seed only, so the paint does not boil from frame to
 * frame; the swirl centres drift slowly with the frame number ("flow"), which makes the
 * strokes in flat areas turn like a living painting.
 *
 * Row-parallel: apply() only reads the stroke records, never the input frame.
 * Performance: a first version looked up, for every pixel, the nearest stroke among the 21
 * cells around it: 312 ms a frame at half resolution. Painting each stroke along the span
 * where the row cuts its ellipse (one square root per stroke and row) writes each pixel
 * less than twice: 104 ms (9 fps) at the default size, 80 ms (12 fps) with large strokes.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_SIZE, P_LENGTH, P_SWIRL, P_COLOR, P_RELIEF, P_FLOW };

static const fx_param_t s_params[] = {
    { "size",   "Stroke size (px)", 8, 40, 16, 0 },
    { "length", "Stroke length",    1, 5, 3, 0 },
    { "swirl",  "Swirl",            0, 1, 0.5f, 0 },
    { "color",  "Colour x",         1, 2.5f, 1.7f, 0 },
    { "relief", "Thick paint",      0, 1, 0.6f, 0 },
    { "flow",   "Flow",             0, 1, 0.3f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_SIZE] = 10.0f + a * 22.0f;
    p[P_LENGTH] = 2.0f + a * 2.0f;
    p[P_SWIRL] = 0.3f + a * 0.5f;
    p[P_COLOR] = 1.3f + a * 0.8f;
    p[P_RELIEF] = 0.6f;
    p[P_FLOW] = 0.3f;
}

#define VG_MAX_W    1088        /* widest frame (a full-resolution still) */
#define VG_MAX_GW   288         /* most cells per row */
#define VG_MIN_CAP  16000       /* cells allocated at least: every preview size fits */
#define VG_VORTICES 5

typedef struct {
    uint16_t shade[4];          /* groove, shaded flank, plain, lit flank */
    int16_t cx, cy;             /* centre, pixels */
    int8_t c, s;                /* direction, 64 = 1.0 */
    uint8_t ry;                 /* rows it reaches above and below its centre */
    uint8_t layer;              /* 0 or 1: which of the two layers it is laid in */
} stroke_t;                     /* 16 bytes: eight to a cache line */

static struct {
    uint32_t *avg;              /* gh rows of `stride` cells, 0xYYRRGGBB */
    uint32_t *work;             /* two sets of three sums per cell column (one per core) */
    stroke_t *strokes;          /* same layout as avg */
    size_t cap;                 /* cells allocated */
    size_t bytes;               /* of strokes, rounded up to 128: what apply() fetches */
    int n, gw, gh, stride;
    int w, h;
    int k2;                     /* (length / width) squared */
    int32_t r2, rim, flank;     /* stroke outline, where its groove starts, where its flanks start */
    bool ok;
} s_g;

typedef struct { const fx_frame_t *in; int sy; } avg_arg_t;

/* Average the cells of grid rows [g0, g1): runs on both cores, each with its own sums. */
static void FX_HOT average_rows(void *arg, int g0, int g1)
{
    const avg_arg_t *a = arg;
    const fx_frame_t *in = a->in;
    int W = in->w, H = in->h, n = s_g.n, gw = s_g.gw, sy = a->sy;
    uint32_t *sr = s_g.work + (g0 == 0 ? 0 : 3 * VG_MAX_GW), *sg = sr + VG_MAX_GW, *sb = sg + VG_MAX_GW;

    for (int gy = g0; gy < g1; gy++) {
        memset(sr, 0, 3 * VG_MAX_GW * sizeof(uint32_t));
        int y1 = (gy + 1) * n < H ? (gy + 1) * n : H, rows = 0;
        for (int y = gy * n; y < y1; y += sy, rows++) {
            const uint16_t *row = in->px + (size_t)y * in->stride_px;
            for (int gx = 0; gx < gw; gx++) {
                int x = gx * n, x1 = x + n < W ? x + n : W;
                uint32_t rb = 0, g = 0;
                for (; x < x1; x++) {
                    uint32_t px = row[x];
                    rb += px & 0xF81F;
                    g += px & 0x07E0;
                }
                sr[gx] += rb >> 11;
                sb[gx] += rb & 0x7FF;
                sg[gx] += g >> 5;
            }
        }
        uint32_t *crow = s_g.avg + (size_t)gy * s_g.stride;
        for (int gx = 0; gx < gw; gx++) {
            int x1 = (gx + 1) * n < W ? (gx + 1) * n : W;
            uint32_t cnt = (uint32_t)(x1 - gx * n) * (uint32_t)rows;
            uint32_t r = sr[gx] * 255u / (31u * cnt), g = sg[gx] * 255u / (63u * cnt), b = sb[gx] * 255u / (31u * cnt);
            uint32_t lum = (r * 77 + g * 151 + b * 28) >> 8;
            crow[gx] = (lum << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static inline uint16_t paint(float r, float g, float b, float k, float lift)
{
    int ri = fx_clampi((int)(r * k + lift), 0, 255), gi = fx_clampi((int)(g * k + lift), 0, 255);
    int bi = fx_clampi((int)(b * k + lift), 0, 255);
    return fx_rgb565((unsigned)ri >> 3, (unsigned)gi >> 2, (unsigned)bi >> 3);
}

typedef struct {
    float vx[VG_VORTICES], vy[VG_VORTICES], vs[VG_VORTICES];
    float t, edge_thr, color, relief, A, B;
    uint32_t seed;
    int W;
} build_arg_t;

/* Turn the averaged cells into stroke records. Runs on one core, from internal RAM like the
 * pixel loops: there are thousands of cells and a dozen floating-point steps for each. */
static void FX_HOT build_strokes(const build_arg_t *g)
{
    int n = s_g.n, gw = s_g.gw, gh = s_g.gh, stride = s_g.stride;
    float A = g->A, B = g->B, color = g->color, relief = g->relief, inv_thr = 1.0f / g->edge_thr;
    float cell = (float)n / (float)g->W;
    float k0 = 1.0f - 0.5f * relief, k1 = 1.0f - 0.2f * relief, k3 = 1.0f + 0.15f * relief, lift3 = 26.0f * relief;

    for (int gy = 0; gy < gh; gy++) {
        stroke_t *row = s_g.strokes + (size_t)gy * stride;
        const uint32_t *arow = s_g.avg + (size_t)gy * stride;
        const uint32_t *up = gy > 0 ? arow - stride : arow, *dn = gy + 1 < gh ? arow + stride : arow;
        float py = ((float)gy + 0.5f) * cell, roll = 0.25f * sinf(py * 7.0f + g->t);
        for (int gx = 0; gx < gw; gx++) {
            /* direction: along the edge where there is one ... */
            int xl = gx > 0 ? gx - 1 : 0, xr = gx + 1 < gw ? gx + 1 : gw - 1;
            int ul = (int)(up[xl] >> 24), uc = (int)(up[gx] >> 24), ur = (int)(up[xr] >> 24);
            int ml = (int)(arow[xl] >> 24), mr = (int)(arow[xr] >> 24);
            int dl = (int)(dn[xl] >> 24), dc = (int)(dn[gx] >> 24), dr = (int)(dn[xr] >> 24);
            float sx = (float)((ur + 2 * mr + dr) - (ul + 2 * ml + dl));
            float sy = (float)((dl + 2 * dc + dr) - (ul + 2 * uc + ur));
            float mag = __builtin_sqrtf(sx * sx + sy * sy);
            float tx = 1, ty = 0;
            if (mag > 0.5f) { float im = 1.0f / mag; tx = -sy * im; ty = sx * im; }
            /* ... and around the swirl centres where there is none */
            float px = ((float)gx + 0.5f) * cell;
            float fx = 0.6f, fy = roll;                                 /* a gentle roll underneath */
            for (int i = 0; i < VG_VORTICES; i++) {
                float dx = px - g->vx[i], dy = py - g->vy[i], k = g->vs[i] / (dx * dx + dy * dy + 0.02f);
                fx -= dy * k;
                fy += dx * k;
            }
            float w = mag * inv_thr;
            if (w > 1) w = 1;
            float fm = (1 - w) / __builtin_sqrtf(fx * fx + fy * fy + 1e-6f);
            if (tx * fx + ty * fy < 0) w = -w;                          /* same way round before blending */
            float dx = tx * w + fx * fm, dy = ty * w + fy * fm;
            float dm = dx * dx + dy * dy;
            if (dm < 1e-4f) { dx = 1; dy = 0; } else { dm = 1.0f / __builtin_sqrtf(dm); dx *= dm; dy *= dm; }
            if (dy - dx < 0) { dx = -dx; dy = -dy; }                    /* the lit flank faces up and left */

            uint32_t h = fx_hash32(g->seed ^ ((uint32_t)gy * 0x9e3779b9u) ^ ((uint32_t)gx * 0x85ebca6bu));
            stroke_t *s = &row[gx];
            s->cx = (int16_t)(gx * n + n / 2 + (((int)(h & 255) - 128) * n * 2 / 5 >> 7));
            s->cy = (int16_t)(gy * n + n / 2 + (((int)((h >> 8) & 255) - 128) * n * 2 / 5 >> 7));
            s->c = (int8_t)(int)(dx * 64.0f + (dx < 0 ? -0.5f : 0.5f));
            s->s = (int8_t)(int)(dy * 64.0f + (dy < 0 ? -0.5f : 0.5f));
            /* its bounding box: |along| * A + |across| * B is a little more than the ellipse needs */
            float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int ry = (int)(A * ay + B * ax) + 1;
            s->ry = (uint8_t)(ry > 127 ? 127 : ry);
            s->layer = (uint8_t)((h >> 30) & 1);

            /* the paint: colour pushed, every dab a little off its neighbours */
            uint32_t c = arow[gx];
            float lum = (float)(c >> 24), r = (float)((c >> 16) & 255), gr = (float)((c >> 8) & 255), b = (float)(c & 255);
            float bright = 1.0f + ((float)((h >> 16) & 255) * (1.0f / 255.0f) - 0.5f) * 0.22f;
            float tint = ((float)(h >> 24) * (1.0f / 255.0f) - 0.5f) * 20.0f;
            r = (lum + (r - lum) * color) * bright + tint;
            gr = (lum + (gr - lum) * color) * bright;
            b = (lum + (b - lum) * color) * bright - tint;
            s->shade[0] = paint(r, gr, b, k0, 0);
            s->shade[1] = paint(r, gr, b, k1, 0);
            s->shade[2] = paint(r, gr, b, 1.0f, 0);
            s->shade[3] = paint(r, gr, b, k3, lift3);
        }
    }
}

static void analyze(const fx_frame_t *in, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w, H = in->h;
    s_g.ok = false;
    if (W > VG_MAX_W) return;

    int n = fx_px(in, fx_clampf(p[P_SIZE], 8, 40) * 0.5f, 4);
    while ((W + n - 1) / n > VG_MAX_GW) n++;
    int gw = (W + n - 1) / n, gh = (H + n - 1) / n, stride = (gw + 31) & ~31;
    size_t need = (size_t)stride * (size_t)gh;
    if (need > s_g.cap) {
        size_t cap = need > VG_MIN_CAP ? need : VG_MIN_CAP;
        uint32_t *mem = fx_big_alloc((cap + 8 * VG_MAX_GW) * sizeof(uint32_t));
        stroke_t *st = fx_big_alloc(cap * sizeof(stroke_t));
        if (!mem || !st) return;
        s_g.work = mem;
        s_g.avg = mem + 8 * VG_MAX_GW;                      /* stays on a 128-byte boundary */
        s_g.strokes = st;
        s_g.cap = cap;
    }
    s_g.n = n; s_g.gw = gw; s_g.gh = gh; s_g.stride = stride;
    s_g.bytes = (need * sizeof(stroke_t) + 127) & ~(size_t)127;

    avg_arg_t a = { in, n >= 4 ? 2 : 1 };
    fx_rows_parallel(average_rows, &a, 0, gh, in, s_g.avg, (size_t)stride * sizeof(uint32_t));

    /* Swirl centres: fixed by the seed, each slowly circling its place. Coordinates in
     * frame widths. */
    build_arg_t g;
    float aspect = (float)H / (float)W;
    g.t = (float)ctx->frame_no * fx_clampf(p[P_FLOW], 0, 1) * 0.06f;
    for (int i = 0; i < VG_VORTICES; i++) {
        uint32_t h = fx_hash32(ctx->seed ^ (0x51ed27u * (uint32_t)(i + 1)));
        float ph = (float)(h >> 24) * (6.2832f / 256.0f);
        g.vx[i] = (float)(h & 255) / 255.0f + 0.08f * cosf(g.t + ph);
        g.vy[i] = (float)((h >> 8) & 255) / 255.0f * aspect + 0.08f * sinf(g.t * 0.8f + ph);
        g.vs[i] = (h & 0x10000) ? 0.12f : -0.12f;
    }

    float len = fx_clampf(p[P_LENGTH], 1, 5), relief = fx_clampf(p[P_RELIEF], 0, 1);
    s_g.k2 = (int)lroundf(len * len);
    /* Half length A and half width B of a stroke: strokes cover 1.7 times the picture, so few
     * gaps stay open. Thresholds in the units apply() measures in: 1/8 pixel squared along
     * the stroke, 1/64 pixel across it. */
    float kk = sqrtf((float)s_g.k2);
    float B = (float)n * sqrtf(1.7f / (3.14159f * kk)), A = B * kk;
    s_g.r2 = (int32_t)(A * A * 64.0f);
    s_g.rim = (int32_t)((float)s_g.r2 * (1.0f - 0.4f * relief));
    s_g.flank = (int32_t)(B * 64.0f * 0.38f);

    g.edge_thr = 16.0f + 320.0f * fx_clampf(p[P_SWIRL], 0, 1);     /* gradient needed to overrule the swirl */
    g.color = fx_clampf(p[P_COLOR], 1, 2.5f);
    g.relief = relief;
    g.A = A; g.B = B;
    g.seed = ctx->seed;
    g.W = W;
    build_strokes(&g);

    s_g.w = W; s_g.h = H;
    fx_mem_publish(s_g.strokes, s_g.bytes);
    s_g.ok = true;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    int W = in->w;
    if (!s_g.ok || s_g.w != W || s_g.h != in->h) {          /* no grid (no memory, or a frame too wide) */
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    int n = s_g.n, gw = s_g.gw, gh = s_g.gh, stride = s_g.stride, k2 = s_g.k2;
    int32_t rim = s_g.rim, flank = s_g.flank;
    float kf = (float)k2, rq = (float)s_g.r2 * 64.0f;       /* the outline in 1/64 pixel, squared */
    uint16_t line[VG_MAX_W];                /* the row is painted on the stack and copied out */
    fx_mem_fetch(s_g.strokes, s_g.bytes);

    int gy = ctx->y0 / n, ynext = (gy + 1) * n;
    for (int y = ctx->y0; y < ctx->y1; y++) {
        if (y >= ynext) { gy++; ynext += n; }
        int r0 = gy > 1 ? gy - 2 : 0, r1 = gy + 2 < gh ? gy + 2 : gh - 1;

        /* underpainting */
        const stroke_t *own = s_g.strokes + (size_t)gy * stride;
        for (int gx = 0, x = 0; gx < gw; gx++) {
            uint16_t c = own[gx].shade[0];
            for (int end = x + n < W ? x + n : W; x < end; x++) line[x] = c;
        }

        for (int layer = 0; layer < 2; layer++) {
            for (int r = r0; r <= r1; r++) {
                const stroke_t *srow = s_g.strokes + (size_t)r * stride;
                for (int c = 0; c < gw; c++) {
                    const stroke_t *s = &srow[c];
                    int dy = y - s->cy, ry = s->ry;
                    if (s->layer != layer || dy > ry || dy < -ry) continue;
                    /* Where this row cuts the ellipse: (a0 + cs*dx)^2 + k2*(b0 - sn*dx)^2 <= R^2,
                     * a quadratic in dx (a: along the stroke, b: across, 1/64 pixel). */
                    int32_t cs = s->c, sn = s->s, a0 = dy * sn, b0 = dy * cs;
                    float P = (float)(cs * cs + k2 * sn * sn), Q = (float)(a0 * cs - k2 * b0 * sn);
                    float disc = Q * Q - P * ((float)a0 * (float)a0 + kf * (float)b0 * (float)b0 - rq);
                    if (disc < 0) continue;
                    float root = __builtin_sqrtf(disc), ip = 1.0f / P;
                    int x0 = s->cx + (int)((-Q - root) * ip + 64.0f) - 63;      /* rounded up */
                    int x1 = s->cx + (int)((-Q + root) * ip + 64.0f) - 64;      /* rounded down */
                    if (x0 < 0) x0 = 0;
                    if (x1 > W - 1) x1 = W - 1;
                    int32_t dx = x0 - s->cx, a = a0 + dx * cs, b = b0 - dx * sn;
                    uint16_t sh0 = s->shade[0], sh1 = s->shade[1], sh2 = s->shade[2], sh3 = s->shade[3];
                    for (int x = x0; x <= x1; x++, a += cs, b -= sn) {
                        int32_t aa = a >> 3, bb = b >> 3;
                        line[x] = aa * aa + k2 * bb * bb > rim ? sh0 : b > flank ? sh3 : b < -flank ? sh1 : sh2;
                    }
                }
            }
        }
        memcpy(out->px + (size_t)y * out->stride_px, line, (size_t)W * sizeof(uint16_t));
    }
}

const fx_desc_t fx_vangogh = {
    .id = "vangogh",
    .name = "Van Gogh",
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
