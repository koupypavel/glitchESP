/*
 * Dream: the look of the early "DeepDream" image hallucinations, where a network's
 * pattern-amplification turned every picture into eyes, dog snouts and fractal fur.
 *
 * The real thing runs a neural network backwards dozens of times per image, which this chip
 * cannot do at any frame rate. What it does instead is the ingredients of the look:
 *   - fur: every pixel is smeared along the direction of the nearest edge (five taps along
 *     the edge's tangent, taken from a coarse gradient field), which turns texture into
 *     combed, swirling strokes around every feature;
 *   - detail pushed away from its surroundings (an unsharp mask against a coarse average)
 *     and colour pushed away from grey;
 *   - rings: the brightness of the coarse picture is turned into contour lines, which wrap
 *     every bright or dark blob in concentric arcs, the way the dream pictures nest rings
 *     around everything;
 *   - eyes: the brightest and darkest blobs get an eye painted over them (white, iris with
 *     streaks, pupil, a highlight); the eyes follow the blobs from frame to frame and fade in
 *     and out, so the picture seems to look back.
 *
 * Row-parallel: analyze() averages the frame into a grid on both cores, blurs the grid,
 * takes its gradient and picks the eyes on one core; apply() only reads those and the input.
 * Performance: the texture is made in 2x2 blocks (five taps, the unsharp mask and the colour
 * work once per block); measured 88 ms a frame (11 fps) at the half-resolution preview,
 * of which 12 ms is the analysis.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_EYES, P_SIZE, P_FUR, P_DETAIL, P_RINGS, P_COLOR };

static const fx_param_t s_params[] = {
    { "eyes",   "Eyes",            0, 40, 14, 1 },
    { "size",   "Eye size (px)",   20, 160, 70, 0 },
    { "fur",    "Fur (px)",        0, 24, 8, 0 },
    { "detail", "Detail boost",    0, 2, 0.8f, 0 },
    { "rings",  "Rings",           0, 1, 0.5f, 0 },
    { "color",  "Colour x",        1, 2.5f, 1.5f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_EYES] = floorf(4.0f + a * 24.0f);
    p[P_SIZE] = 50.0f + a * 60.0f;
    p[P_FUR] = 3.0f + a * 12.0f;
    p[P_DETAIL] = 0.4f + a * 1.0f;
    p[P_RINGS] = 0.2f + a * 0.6f;
    p[P_COLOR] = 1.2f + a * 0.8f;
}

#define DR_MAX_W    1088
#define DR_MAX_GW   160
#define DR_MIN_CAP  8000
#define DR_MAX_EYES 40
#define DR_CELL     16          /* reference pixels per grid cell */

static struct {
    uint32_t *avg;              /* gh rows of `stride` cells, 0xYYRRGGBB */
    uint32_t *blur;             /* the same, 3x3 box blurred: what the detail is measured against */
    uint32_t *dir;              /* per cell: the edge tangent, (c + 128) | (s + 128) << 8, 64 = 1.0 */
    uint32_t *work;
    size_t cap, bytes;
    int n, gw, gh, stride, w, h;
    bool ok;
} s_g;

typedef struct {
    int16_t x, y;               /* centre, pixels */
    int16_t gx, gy;             /* the cell it sits on */
    uint16_t r;                 /* radius, pixels */
    uint8_t age;                /* 0 = free; grows to 16 as it fades in */
    uint8_t live;               /* seen this frame */
    uint32_t seed;
    uint16_t iris, iris_dark, white, pupil, shine, rim;     /* its colours */
} eye_t;

static eye_t s_eyes[DR_MAX_EYES];
static int s_n_eyes;
static uint8_t s_clamp[768];
static uint8_t s_band[256];     /* eye: normalized radius squared (0..255) -> what is painted there */
static uint32_t s_detail, s_sat;              /* 0..512, 256..640 */
static int s_fur;               /* half the smear length, pixels (0: none) */
static uint8_t s_ring[256];     /* coarse brightness -> brightness factor, 0..255 = 0..1.0 */
static uint8_t s_hair[64];      /* position across the fur -> brightness factor */
static float s_cos[64];         /* cos of 64 directions round the circle */

/* ---- grid ---- */

typedef struct { const fx_frame_t *in; int sy; } avg_arg_t;

static void FX_HOT average_rows(void *arg, int g0, int g1)
{
    const avg_arg_t *a = arg;
    const fx_frame_t *in = a->in;
    int W = in->w, H = in->h, n = s_g.n, gw = s_g.gw, sy = a->sy;
    uint32_t *sr = s_g.work + (g0 == 0 ? 0 : 3 * DR_MAX_GW), *sg = sr + DR_MAX_GW, *sb = sg + DR_MAX_GW;
    for (int gy = g0; gy < g1; gy++) {
        memset(sr, 0, 3 * DR_MAX_GW * sizeof(uint32_t));
        int y1 = (gy + 1) * n < H ? (gy + 1) * n : H, rows = 0;
        for (int y = gy * n; y < y1; y += sy, rows++) {
            const uint16_t *row = in->px + (size_t)y * in->stride_px;
            for (int gx = 0; gx < gw; gx++) {
                int x = gx * n, x1 = x + n < W ? x + n : W;
                uint32_t rb = 0, g = 0;
                for (; x < x1; x += 2) {
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
            uint32_t cnt = (uint32_t)((x1 - gx * n + 1) / 2) * (uint32_t)rows;
            uint32_t r = sr[gx] * 255u / (31u * cnt), g = sg[gx] * 255u / (63u * cnt), b = sb[gx] * 255u / (31u * cnt);
            uint32_t lum = (r * 77 + g * 151 + b * 28) >> 8;
            crow[gx] = (lum << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static inline uint32_t cell(const uint32_t *g, int gx, int gy)
{
    gx = fx_clampi(gx, 0, s_g.gw - 1);
    gy = fx_clampi(gy, 0, s_g.gh - 1);
    return g[(size_t)gy * s_g.stride + gx];
}

/* ---- eyes ---- */

static uint16_t rgb(int r, int g, int b)
{
    return fx_rgb565((unsigned)fx_clampi(r, 0, 255) >> 3, (unsigned)fx_clampi(g, 0, 255) >> 2, (unsigned)fx_clampi(b, 0, 255) >> 3);
}

static void eye_colours(eye_t *e)
{
    uint32_t h = fx_hash32(e->seed);
    /* iris: amber, green, blue or violet, like the dream dogs */
    static const uint8_t k_iris[4][3] = { { 230, 150, 30 }, { 120, 200, 40 }, { 60, 140, 230 }, { 170, 70, 210 } };
    const uint8_t *c = k_iris[h & 3];
    int v = (int)((h >> 8) & 63) - 32;
    e->iris = rgb(c[0] + v, c[1] + v, c[2] + v);
    e->iris_dark = rgb(c[0] * 2 / 5, c[1] * 2 / 5, c[2] * 2 / 5);
    e->white = rgb(245, 235, 205);
    e->pupil = rgb(12, 8, 10);
    e->shine = rgb(255, 255, 255);
    e->rim = rgb(50, 25, 20);
}

/* Find bright and dark blobs of the grid and keep the eyes on them from frame to frame. */
static void place_eyes(int want, int radius_px, const fx_ctx_t *ctx)
{
    int gw = s_g.gw, gh = s_g.gh, n = s_g.n;
    for (int i = 0; i < DR_MAX_EYES; i++) s_eyes[i].live = 0;

    /* candidates: cells that stand out from a ring two cells away, best first */
    enum { MAXC = 96 };
    int cx[MAXC], cy[MAXC], cs[MAXC], nc = 0;
    for (int gy = 2; gy < gh - 2; gy += 1) {
        for (int gx = 2; gx < gw - 2; gx++) {
            int l = (int)(cell(s_g.blur, gx, gy) >> 24);
            int ring = (int)(cell(s_g.blur, gx - 2, gy) >> 24) + (int)(cell(s_g.blur, gx + 2, gy) >> 24) +
                       (int)(cell(s_g.blur, gx, gy - 2) >> 24) + (int)(cell(s_g.blur, gx, gy + 2) >> 24) +
                       (int)(cell(s_g.blur, gx - 2, gy - 2) >> 24) + (int)(cell(s_g.blur, gx + 2, gy + 2) >> 24) +
                       (int)(cell(s_g.blur, gx + 2, gy - 2) >> 24) + (int)(cell(s_g.blur, gx - 2, gy + 2) >> 24);
            int score = l * 8 - ring;
            if (score < 0) score = -score;
            if (score < 8 * 12) continue;                       /* 12 brightness levels off its surroundings */
            bool peak = true;                                   /* a local maximum of the score */
            for (int dy = -1; dy <= 1 && peak; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int l2 = (int)(cell(s_g.blur, gx + dx, gy + dy) >> 24);
                    int r2 = 0;
                    static const int8_t k_ring[8][2] = { { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 }, { -2, -2 }, { 2, 2 }, { 2, -2 }, { -2, 2 } };
                    for (int k = 0; k < 8; k++) r2 += (int)(cell(s_g.blur, gx + dx + k_ring[k][0], gy + dy + k_ring[k][1]) >> 24);
                    int s2 = l2 * 8 - r2;
                    if (s2 < 0) s2 = -s2;
                    if (s2 > score) { peak = false; break; }
                }
            }
            if (!peak) continue;
            /* insert, sorted by score, into the best MAXC */
            int i = nc < MAXC ? nc++ : MAXC - 1;
            if (i == MAXC - 1 && cs[i] >= score) continue;
            while (i > 0 && cs[i - 1] < score) { cx[i] = cx[i - 1]; cy[i] = cy[i - 1]; cs[i] = cs[i - 1]; i--; }
            cx[i] = gx; cy[i] = gy; cs[i] = score;
        }
    }

    /* existing eyes follow the nearest candidate; the rest fade */
    uint8_t used[MAXC] = { 0 };
    for (int i = 0; i < DR_MAX_EYES; i++) {
        eye_t *e = &s_eyes[i];
        if (!e->age) continue;
        int best = -1, bd = 3 * 3 + 1;
        for (int c = 0; c < nc; c++) {
            if (used[c]) continue;
            int dx = cx[c] - e->gx, dy = cy[c] - e->gy, d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = c; }
        }
        if (best >= 0 && i < want) {
            used[best] = 1;
            e->gx = (int16_t)cx[best]; e->gy = (int16_t)cy[best];
            int tx = cx[best] * n + n / 2, ty = cy[best] * n + n / 2;
            e->x = (int16_t)(e->x + (tx - e->x) / 4);
            e->y = (int16_t)(e->y + (ty - e->y) / 4);
            e->live = 1;
            if (e->age < 16) e->age++;
        } else {
            e->age = e->age > 2 ? e->age - 2 : 0;
        }
    }
    /* new eyes on the strongest free candidates, away from the eyes there are */
    int have = 0;
    for (int i = 0; i < DR_MAX_EYES; i++) if (s_eyes[i].age) have++;
    for (int c = 0; c < nc && have < want; c++) {
        if (used[c]) continue;
        bool clear = true;
        for (int i = 0; i < DR_MAX_EYES && clear; i++) {
            if (!s_eyes[i].age) continue;
            int dx = cx[c] - s_eyes[i].gx, dy = cy[c] - s_eyes[i].gy;
            if (dx * dx + dy * dy < 5 * 5) clear = false;
        }
        if (!clear) continue;
        for (int i = 0; i < DR_MAX_EYES; i++) {
            eye_t *e = &s_eyes[i];
            if (e->age) continue;
            e->gx = (int16_t)cx[c]; e->gy = (int16_t)cy[c];
            e->x = (int16_t)(cx[c] * n + n / 2); e->y = (int16_t)(cy[c] * n + n / 2);
            e->seed = fx_hash32(ctx->seed ^ ((uint32_t)cx[c] * 0x9e3779b9u) ^ ((uint32_t)cy[c] * 0x85ebca6bu));
            e->age = 1; e->live = 1;
            eye_colours(e);
            have++;
            break;
        }
    }
    /* sizes: each eye its own, from its seed */
    s_n_eyes = 0;
    for (int i = 0; i < DR_MAX_EYES; i++) {
        eye_t *e = &s_eyes[i];
        if (!e->age) continue;
        uint32_t h = fx_hash32(e->seed ^ 0x77u);
        int r = radius_px * (70 + (int)(h & 63)) / 100;
        e->r = (uint16_t)(r * (int)e->age / 16);               /* grows in, shrinks out */
        s_n_eyes = i + 1;
    }
}

/* ---- analyze / apply ---- */

static void analyze(const fx_frame_t *in, const float *p, const fx_ctx_t *ctx)
{
    int W = in->w, H = in->h;
    s_g.ok = false;
    if (W > DR_MAX_W) return;
    if (!s_clamp[767]) {
        for (int i = 0; i < 768; i++) s_clamp[i] = (uint8_t)fx_clampi(i - 256, 0, 255);
        for (int i = 0; i < 64; i++) s_cos[i] = cosf((float)i * (6.2832f / 64.0f));
        /* an eye, from the centre out: pupil, iris, white, rim, then two rings around it;
         * 0..255 is the radius squared, 255 at 1.6 times the eye's radius */
        for (int i = 0; i < 256; i++) {
            s_band[i] = i < 20 ? 0 : i < 81 ? 1 : i < 100 ? 2 : i < 118 ? 3 : i < 143 ? 6 : i < 168 ? 5 : i < 201 ? 6 : i < 230 ? 5 : 6;
        }
    }

    int n = fx_px(in, DR_CELL, 4);
    n += n & 1;
    while ((W + n - 1) / n > DR_MAX_GW) n += 2;
    int gw = (W + n - 1) / n, gh = (H + n - 1) / n, stride = (gw + 31) & ~31;
    size_t need = (size_t)stride * (size_t)gh;
    if (need > s_g.cap) {
        size_t cap = need > DR_MIN_CAP ? need : DR_MIN_CAP;
        size_t plane = (cap + 31) & ~(size_t)31;
        uint32_t *mem = fx_big_alloc((3 * plane + 8 * DR_MAX_GW) * sizeof(uint32_t));
        if (!mem) return;
        s_g.work = mem;
        s_g.avg = mem + 8 * DR_MAX_GW;
        s_g.blur = s_g.avg + plane;
        s_g.dir = s_g.blur + plane;
        s_g.cap = cap;
    }
    s_g.n = n; s_g.gw = gw; s_g.gh = gh; s_g.stride = stride;
    s_g.bytes = (size_t)(s_g.dir - s_g.blur) * sizeof(uint32_t) + ((need * sizeof(uint32_t) + 127) & ~(size_t)127);

    avg_arg_t a = { in, n >= 4 ? 2 : 1 };
    fx_rows_parallel(average_rows, &a, 0, gh, in, s_g.avg, (size_t)stride * sizeof(uint32_t));

    /* 3x3 box blur of the grid */
    for (int gy = 0; gy < gh; gy++) {
        uint32_t *dst = s_g.blur + (size_t)gy * stride;
        for (int gx = 0; gx < gw; gx++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    uint32_t c = cell(s_g.avg, gx + dx, gy + dy);
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
                }
            }
            r /= 9; g /= 9; b /= 9;
            uint32_t lum = (r * 77 + g * 151 + b * 28) >> 8;
            dst[gx] = (lum << 24) | (r << 16) | (g << 8) | b;
        }
    }

    /* the edge tangent per cell, from the blurred grid's gradient; where there is no edge
     * worth the name the direction turns slowly across the picture instead of jittering */
    for (int gy = 0; gy < gh; gy++) {
        uint32_t *dst = s_g.dir + (size_t)gy * stride;
        for (int gx = 0; gx < gw; gx++) {
            int sx = ((int)(cell(s_g.blur, gx + 1, gy - 1) >> 24) + 2 * (int)(cell(s_g.blur, gx + 1, gy) >> 24) + (int)(cell(s_g.blur, gx + 1, gy + 1) >> 24)) -
                     ((int)(cell(s_g.blur, gx - 1, gy - 1) >> 24) + 2 * (int)(cell(s_g.blur, gx - 1, gy) >> 24) + (int)(cell(s_g.blur, gx - 1, gy + 1) >> 24));
            int sy = ((int)(cell(s_g.blur, gx - 1, gy + 1) >> 24) + 2 * (int)(cell(s_g.blur, gx, gy + 1) >> 24) + (int)(cell(s_g.blur, gx + 1, gy + 1) >> 24)) -
                     ((int)(cell(s_g.blur, gx - 1, gy - 1) >> 24) + 2 * (int)(cell(s_g.blur, gx, gy - 1) >> 24) + (int)(cell(s_g.blur, gx + 1, gy - 1) >> 24));
            float tx = (float)-sy, ty = (float)sx, m = sqrtf(tx * tx + ty * ty);
            float w = m / 40.0f;
            if (w > 1) w = 1;
            int ai = (gx * 2 + gy * 1 + (int)(ctx->frame_no / 8)) & 63;     /* a slow turn across the picture */
            float fx = s_cos[ai], fy = s_cos[(ai + 48) & 63];
            if (m > 0.5f) { tx /= m; ty /= m; } else { tx = fx; ty = fy; }
            if (tx * fx + ty * fy < 0) { fx = -fx; fy = -fy; }
            float dx = tx * w + fx * (1 - w), dy = ty * w + fy * (1 - w), dm = sqrtf(dx * dx + dy * dy);
            if (dm < 0.01f) { dx = 1; dy = 0; dm = 1; }
            int c = (int)(dx / dm * 64.0f), s2 = (int)(dy / dm * 64.0f);
            dst[gx] = (uint32_t)(c + 128) | ((uint32_t)(s2 + 128) << 8);
        }
    }

    int want = fx_clampi((int)lroundf(p[P_EYES]), 0, DR_MAX_EYES);
    place_eyes(want, fx_px(in, fx_clampf(p[P_SIZE], 20, 160) * 0.5f, 4), ctx);

    s_fur = fx_px(in, fx_clampf(p[P_FUR], 0, 24) * 0.5f, 0);
    s_detail = (uint32_t)(fx_clampf(p[P_DETAIL], 0, 2) * 256.0f);
    s_sat = (uint32_t)(fx_clampf(p[P_COLOR], 1, 2.5f) * 256.0f);
    /* contour lines: every 24 levels of coarse brightness a dark line, softened; the lines
     * come out as arcs around every blob */
    float rings = fx_clampf(p[P_RINGS], 0, 1);
    float hair = fx_clampf(p[P_DETAIL], 0, 2) * 0.12f;
    for (int i = 0; i < 64; i++) s_hair[i] = (uint8_t)(255.0f * (1.0f - hair * (0.5f + 0.5f * cosf((float)i * (6.2832f / 9.0f)))));
    for (int i = 0; i < 256; i++) {
        float ph = (float)i * (6.2832f / 24.0f);
        float v = 1.0f - rings * 0.45f * (0.5f + 0.5f * cosf(ph));
        s_ring[i] = (uint8_t)(v * 255.0f);
    }

    s_g.w = W; s_g.h = H;
    fx_mem_publish(s_g.blur, s_g.bytes);
    s_g.ok = true;
}

static inline uint32_t spread(uint16_t p) { return ((uint32_t)p | ((uint32_t)p << 16)) & 0x07E0F81F; }

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    int W = in->w, H = in->h;
    if (!s_g.ok || s_g.w != W || s_g.h != H) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    int n = s_g.n, gw = s_g.gw, gh = s_g.gh, stride = s_g.stride, half = n / 2, fur = s_fur;
    uint32_t detail = s_detail, sat = s_sat;
    const uint8_t *cl = s_clamp + 256;
    uint32_t rowc[DR_MAX_GW];
    int32_t rowd_c[DR_MAX_GW], rowd_s[DR_MAX_GW];
    uint16_t line[DR_MAX_W];
    fx_mem_fetch(s_g.blur, s_g.bytes);

    /* Rows go in pairs (y even, y + 1): one line serves both, so the texture is made of 2x2
     * blocks (at the half-resolution preview that is 4x4 on the screen, under a smear that is
     * wider than that anyway). A share that starts on an odd row renders the pair it belongs
     * to and keeps only its own row, so the result does not depend on the split. */
    for (int yy = ctx->y0; yy < ctx->y1; yy += 2 - (yy & 1)) {
        int y = yy & ~1;
        /* the blurred grid and the direction field along this row, between the two nearest
         * grid rows */
        int t = y - half, gy = 0;
        uint32_t wy = 0;
        if (t >= 0) { gy = t / n; wy = (uint32_t)((t - gy * n) * 256 / n); }
        if (gy >= gh - 1) { gy = gh - 1; wy = 0; }
        size_t ra = (size_t)gy * stride, rb = wy ? ra + stride : ra;
        const uint32_t *ca = s_g.blur + ra, *cb = s_g.blur + rb, *da = s_g.dir + ra, *db = s_g.dir + rb;
        uint32_t wa = 256 - wy;
        for (int gx = 0; gx < gw; gx++) {
            uint32_t a = ca[gx], b = cb[gx];
            uint32_t even = (((a & 0x00FF00FF) * wa + (b & 0x00FF00FF) * wy) >> 8) & 0x00FF00FF;
            uint32_t odd = (((a >> 8) & 0x00FF00FF) * wa + ((b >> 8) & 0x00FF00FF) * wy) & 0xFF00FF00;
            rowc[gx] = even | odd;
            int32_t ca_c = (int32_t)(da[gx] & 255) - 128, cb_c = (int32_t)(db[gx] & 255) - 128;
            int32_t ca_s = (int32_t)((da[gx] >> 8) & 255) - 128, cb_s = (int32_t)((db[gx] >> 8) & 255) - 128;
            if (ca_c * cb_c + ca_s * cb_s < 0) { cb_c = -cb_c; cb_s = -cb_s; }     /* a tangent has no sign */
            rowd_c[gx] = (ca_c * (int32_t)wa + cb_c * (int32_t)wy) >> 8;
            rowd_s[gx] = (ca_s * (int32_t)wa + cb_s * (int32_t)wy) >> 8;
        }

        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        int gx = 0;
        int32_t br = 0, bg = 0, bb = 0, dr = 0, dg = 0, db2 = 0, span_end = 0;
        int32_t dc = 0, ds = 0, ddc = 0, dds = 0;
        /* two pixels at a time: the preview shows every pixel doubled anyway */
        for (int x = 0; x < W; x += 2) {
            if (x >= span_end) {                              /* next span between two cell centres */
                int xc = x - half;
                if (xc < 0) { gx = 0; span_end = half < W ? half : W; }
                else { gx = xc / n; span_end = (gx + 1) * n + half; }
                if (span_end > W) span_end = W;
                int g1 = gx + 1 < gw ? gx + 1 : gx;
                uint32_t a = rowc[gx], b = rowc[g1];
                int32_t c1 = rowd_c[g1], s1 = rowd_s[g1], c0 = rowd_c[gx], s0 = rowd_s[gx];
                if (xc < 0 || gx >= gw - 1) { b = a; c1 = c0; s1 = s0; }
                if (c0 * c1 + s0 * s1 < 0) { c1 = -c1; s1 = -s1; }
                int32_t ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
                int32_t rec = 65536 / n;
                dr = (((int32_t)((b >> 16) & 255)) - ar) * rec;
                dg = (((int32_t)((b >> 8) & 255)) - ag) * rec;
                db2 = (((int32_t)(b & 255)) - ab) * rec;
                ddc = (c1 - c0) * rec; dds = (s1 - s0) * rec;
                int32_t off = xc < 0 ? 0 : xc - gx * n;
                br = (ar << 16) + dr * off; bg = (ag << 16) + dg * off; bb = (ab << 16) + db2 * off;
                dc = (c0 << 16) + ddc * off; ds = (s0 << 16) + dds * off;
            }
            int32_t r, g, b;
            if (fur) {
                /* five taps along the tangent: the fur */
                int32_t ox = (dc >> 16) * fur >> 6, oy = (ds >> 16) * fur >> 6;
                uint32_t acc = spread(src[x]) * 2;
                for (int k = 1; k <= 2; k++) {
                    int xa = x + ox * k, ya = y + oy * k, xb = x - ox * k, yb = y - oy * k;
                    if (xa < 0) xa = 0; else if (xa >= W) xa = W - 1;
                    if (ya < 0) ya = 0; else if (ya >= H) ya = H - 1;
                    if (xb < 0) xb = 0; else if (xb >= W) xb = W - 1;
                    if (yb < 0) yb = 0; else if (yb >= H) yb = H - 1;
                    acc += spread(in->px[(size_t)ya * in->stride_px + xa]) + spread(in->px[(size_t)yb * in->stride_px + xb]);
                }
                /* 6 units in all: /6 by multiplying with 43/256 per field */
                r = (int32_t)((((acc >> 11) & 0x3FF) * 43 >> 8) << 3);
                b = (int32_t)(((acc & 0x3FF) * 43 >> 8) << 3);
                g = (int32_t)(((acc >> 21) * 43 >> 8) << 2);
            } else {
                uint32_t px = src[x];
                r = (int32_t)((px >> 11) << 3); g = (int32_t)(((px >> 5) & 63) << 2); b = (int32_t)((px & 31) << 3);
            }
            /* detail: away from the local average; colour: away from grey; rings from the
             * coarse brightness */
            int32_t lb = ((br >> 16) * 77 + (bg >> 16) * 151 + (bb >> 16) * 28) >> 8;
            int32_t ring = s_ring[lb & 255];
            if (fur) {                                        /* hairs: stripes across the smear */
                int32_t perp = ((dc >> 16) * y - (ds >> 16) * x) >> 5;
                ring = ring * s_hair[perp & 63] >> 8;
            }
            r += ((r - (br >> 16)) * (int32_t)detail) >> 8;
            g += ((g - (bg >> 16)) * (int32_t)detail) >> 8;
            b += ((b - (bb >> 16)) * (int32_t)detail) >> 8;
            int32_t lum = (r * 77 + g * 151 + b * 28) >> 8;
            r = (lum + (((r - lum) * (int32_t)sat) >> 8)) * ring >> 8;
            g = (lum + (((g - lum) * (int32_t)sat) >> 8)) * ring >> 8;
            b = (lum + (((b - lum) * (int32_t)sat) >> 8)) * ring >> 8;
            r = cl[fx_clampi(r, -256, 511)]; g = cl[fx_clampi(g, -256, 511)]; b = cl[fx_clampi(b, -256, 511)];
            uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            line[x] = px;
            if (x + 1 < W) line[x + 1] = px;
            br += 2 * dr; bg += 2 * dg; bb += 2 * db2; dc += 2 * ddc; ds += 2 * dds;
        }

        /* the eyes on this row: painted into the picture, not stuck on it */
        for (int i = 0; i < s_n_eyes; i++) {
            const eye_t *e = &s_eyes[i];
            int r = e->r, R = r * 8 / 5, dy = y - e->y;
            if (!e->age || r < 3 || dy > R || dy < -R) continue;
            int x0 = e->x - R, x1 = e->x + R;
            if (x0 < 0) x0 = 0;
            if (x1 > W - 1) x1 = W - 1;
            uint32_t inv = (255u << 16) / (uint32_t)(R * R);
            int hx = e->x - r * 3 / 10, hy = e->y - r * 3 / 10, hr2 = r * r * 2 / 100 + 1;
            for (int x = x0; x <= x1; x++) {
                int dx = x - e->x, d2 = dx * dx + dy * dy;
                if (d2 > R * R) continue;
                unsigned band = s_band[(d2 * inv) >> 16];
                if (band == 6) continue;
                uint32_t here = spread(line[x]), m;
                if (band == 0) m = (spread(e->pupil) * 7 + here) >> 3;
                else if (band == 1) {
                    int ex = x - hx, ey = y - hy;
                    uint16_t c = ex * ex + ey * ey < hr2 ? e->shine :
                                 ((dx * 13 + dy * 7 + ((dx * dy) >> 3)) >> 4) & 1 ? e->iris : e->iris_dark;
                    m = (spread(c) * 3 + here) >> 2;
                } else if (band == 2) m = (spread(e->white) + here) >> 1;
                else if (band == 3) m = (spread(e->rim) + here) >> 1;
                else m = (spread(e->rim) + here * 3) >> 2;    /* the rings around */
                m &= 0x07E0F81F;
                line[x] = (uint16_t)(m | (m >> 16));
            }
        }
        memcpy(out->px + (size_t)yy * out->stride_px, line, (size_t)W * sizeof(uint16_t));
        if (yy == y && y + 1 < ctx->y1) memcpy(out->px + (size_t)(y + 1) * out->stride_px, line, (size_t)W * sizeof(uint16_t));
    }
}

const fx_desc_t fx_dream = {
    .id = "dream",
    .name = "Dream",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_SOFT,
    .from_amount = from_amount,
    .analyze = analyze,
    .apply = apply,
};
