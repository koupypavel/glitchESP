/*
 * Palette: the picture is redrawn with a handful of fixed colours (acid magenta/cyan/lime,
 * crimson and lime, CGA, Game Boy green, ...) and an ordered dither in horizontal lines,
 * the look of a camcorder tape fed through an old graphics card.
 *
 * Every pixel goes to the nearest palette colour. The dither adds a per-pixel offset from a
 * 4x4 pattern before the match, so flat areas break into lines of the two nearest colours.
 * "Hue" turns the picture before matching; a rotation keeps distances, so instead of
 * turning every pixel the palette is turned the other way when the tables are built.
 *
 * Performance: the match is a 4096-entry table (4 bits per channel), reached through three
 * per-channel tables that already contain the dither offset for each of the 16 pattern
 * cells: four loads per pixel, all in on-chip RAM. Row-parallel: rows are independent.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_PALETTE, P_HUE, P_DITHER, P_PATTERN, P_SIZE, P_CYCLE };

#define N_PALETTES 7
#define PAL_MAX    6

static const fx_param_t s_params[] = {
    { "palette", "Palette (0-6)",      0, N_PALETTES - 1, 0, 1 },
    { "hue",     "Hue turn (deg)",     0, 360, 0, 0 },
    { "dither",  "Dither",             0, 1, 0.7f, 0 },
    { "pattern", "Pattern (0 lines, 1 dots, 2 none)", 0, 2, 0, 1 },
    { "size",    "Pattern size (px)",  1, 8, 2, 1 },
    { "cycle",   "Hue drift (deg/frame)", 0, 20, 0, 0 },
};

/* The amount knob walks through the palettes, with a little more dither each time. */
static void from_amount(float a, float *p)
{
    float t = fx_clampf(a, 0, 0.999f) * N_PALETTES;
    int pal = (int)t;
    p[P_PALETTE] = (float)pal;
    p[P_HUE] = 0;
    p[P_DITHER] = 0.5f + 0.5f * (t - (float)pal);
    p[P_PATTERN] = 0;
    p[P_SIZE] = 2;
    p[P_CYCLE] = 0;
}

typedef struct { uint8_t n; uint8_t rgb[PAL_MAX][3]; } palette_t;

static const palette_t s_palettes[N_PALETTES] = {
    /* 0 acid: magenta, cyan, lime, dark, pink */
    { 5, { { 0xff, 0x50, 0xff }, { 0x50, 0xff, 0xe8 }, { 0xd0, 0xff, 0x30 }, { 0x28, 0x20, 0x38 }, { 0xff, 0x90, 0xb8 } } },
    /* 1 crimson: lime, steel blue, crimson, dark, white */
    { 5, { { 0xd8, 0xff, 0x20 }, { 0x88, 0x98, 0xb0 }, { 0xe0, 0x10, 0x48 }, { 0x1a, 0x14, 0x20 }, { 0xf4, 0xf4, 0xf0 } } },
    /* 2 CGA: black, cyan, magenta, white */
    { 4, { { 0x00, 0x00, 0x00 }, { 0x55, 0xff, 0xff }, { 0xff, 0x55, 0xff }, { 0xff, 0xff, 0xff } } },
    /* 3 Game Boy: four greens */
    { 4, { { 0x0f, 0x38, 0x0f }, { 0x30, 0x62, 0x30 }, { 0x8b, 0xac, 0x0f }, { 0x9b, 0xbc, 0x0f } } },
    /* 4 vapour: navy, pink, cyan, violet, cream */
    { 5, { { 0x1a, 0x10, 0x40 }, { 0xff, 0x6e, 0xc7 }, { 0x00, 0xf0, 0xff }, { 0x9d, 0x4e, 0xdd }, { 0xff, 0xf0, 0xc0 } } },
    /* 5 ember: black, wine, orange, gold, warm white */
    { 5, { { 0x00, 0x00, 0x00 }, { 0x80, 0x00, 0x20 }, { 0xff, 0x40, 0x00 }, { 0xff, 0xc0, 0x00 }, { 0xff, 0xf8, 0xe0 } } },
    /* 6 ink: black and white, the dither does the rest */
    { 2, { { 0x10, 0x10, 0x10 }, { 0xf8, 0xf8, 0xf8 } } },
};

/* 4x4 threshold patterns, values 0..15 */
static const uint8_t s_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};
static const uint8_t s_lines[4][4] = {      /* rows first: the horizontal line look */
    {  0,  2,  1,  3 },
    {  8, 10,  9, 11 },
    {  4,  6,  5,  7 },
    { 12, 14, 13, 15 },
};

/* per pattern cell: 5-bit (6-bit green) channel + dither offset -> 4-bit match index */
static uint8_t s_lut_r[16][32], s_lut_g[16][64], s_lut_b[16][32];
static uint16_t s_match[4096];               /* 4-4-4 colour -> nearest palette colour, RGB565 */
static const uint8_t (*s_pattern)[4] = s_lines;

static int s_built_pal = -1;
static float s_built_hue = -1000.0f, s_built_dither = -1;
static int s_built_pattern = -1;

static void build_match(int pal, float hue_deg)
{
    const palette_t *P = &s_palettes[pal];
    /* the palette turned by -hue (Rodrigues rotation around the grey axis) */
    float a = -hue_deg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a);
    const float k = 1.0f / 3.0f, sq = sqrtf(1.0f / 3.0f);
    float m[3][3] = {
        { c + (1 - c) * k, (1 - c) * k - sq * s, (1 - c) * k + sq * s },
        { (1 - c) * k + sq * s, c + (1 - c) * k, (1 - c) * k - sq * s },
        { (1 - c) * k - sq * s, (1 - c) * k + sq * s, c + (1 - c) * k },
    };
    int pr[PAL_MAX], pg[PAL_MAX], pb[PAL_MAX];
    uint16_t out[PAL_MAX];
    for (int i = 0; i < P->n; i++) {
        float r = P->rgb[i][0], g = P->rgb[i][1], b = P->rgb[i][2];
        pr[i] = (int)lroundf(m[0][0] * r + m[0][1] * g + m[0][2] * b);
        pg[i] = (int)lroundf(m[1][0] * r + m[1][1] * g + m[1][2] * b);
        pb[i] = (int)lroundf(m[2][0] * r + m[2][1] * g + m[2][2] * b);
        out[i] = fx_rgb565(P->rgb[i][0] >> 3, P->rgb[i][1] >> 2, P->rgb[i][2] >> 3);
    }
    for (int idx = 0; idx < 4096; idx++) {
        int r = ((idx >> 8) & 15) * 17, g = ((idx >> 4) & 15) * 17, b = (idx & 15) * 17;
        int best = 0, bd = 0x7fffffff;
        for (int i = 0; i < P->n; i++) {
            int dr = r - pr[i], dg = g - pg[i], db = b - pb[i];
            int d = dr * dr * 2 + dg * dg * 3 + db * db;      /* green weighs most, like the eye */
            if (d < bd) { bd = d; best = i; }
        }
        s_match[idx] = out[best];
    }
}

static void build_channels(float dither, int pattern)
{
    const uint8_t (*pat)[4] = pattern == 1 ? s_bayer : s_lines;
    s_pattern = pat;
    /* the offset spans about one palette step: up to +-64 of 255 at full dither */
    float amp = (pattern == 2 ? 0 : dither) * 128.0f;
    for (int t = 0; t < 16; t++) {
        int off = (int)lroundf(((float)t - 7.5f) * amp / 16.0f);
        for (int v = 0; v < 32; v++) {
            int v8 = (v << 3) | (v >> 2);
            int q = fx_clampi(v8 + off, 0, 255) >> 4;
            s_lut_r[t][v] = (uint8_t)q;
            s_lut_b[t][v] = (uint8_t)q;
        }
        for (int v = 0; v < 64; v++) {
            int v8 = (v << 2) | (v >> 4);
            s_lut_g[t][v] = (uint8_t)(fx_clampi(v8 + off, 0, 255) >> 4);
        }
    }
}

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    int pal = fx_clampi((int)lroundf(p[P_PALETTE]), 0, N_PALETTES - 1);
    int pattern = fx_clampi((int)lroundf(p[P_PATTERN]), 0, 2);
    float hue = p[P_HUE] + p[P_CYCLE] * (float)ctx->frame_no;
    hue = fmodf(hue, 360.0f);
    if (hue < 0) hue += 360.0f;
    float dither = fx_clampf(p[P_DITHER], 0, 1);
    if (pal != s_built_pal || fabsf(hue - s_built_hue) > 1.5f) {
        build_match(pal, hue);
        s_built_pal = pal;
        s_built_hue = hue;
    }
    if (pattern != s_built_pattern || fabsf(dither - s_built_dither) > 0.01f) {
        build_channels(dither, pattern);
        s_built_pattern = pattern;
        s_built_dither = dither;
    }
}

/* The pattern cell is `size` reference pixels (1, 2, 4 or 8 on this frame), so a line of the
 * dither is as tall in a photo as on the preview. The pattern repeats every 16 pixels at
 * most, so each row gets a 16-entry table of its cells' thresholds. */
static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    int cell = fx_px(in, p[P_SIZE], 1);
    int shift = cell >= 8 ? 3 : cell >= 4 ? 2 : cell >= 2 ? 1 : 0;
    const uint16_t *match = s_match;
    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        const uint8_t *prow = s_pattern[(y >> shift) & 3];
        const uint8_t *lr[16], *lg[16], *lb[16];
        for (int i = 0; i < 16; i++) {
            unsigned t = prow[(i >> shift) & 3];
            lr[i] = s_lut_r[t]; lg[i] = s_lut_g[t]; lb[i] = s_lut_b[t];
        }
        int w = in->w, x = 0;
        if ((((uintptr_t)src | (uintptr_t)dst) & 3) == 0) {
            const uint32_t *s32 = (const uint32_t *)src;
            uint32_t *d32 = (uint32_t *)dst;
            for (; x + 4 <= w; x += 4) {
                uint32_t w0 = s32[x >> 1], w1 = s32[(x >> 1) + 1];
                uint32_t pa = w0 & 0xffff, pb = w0 >> 16, pc = w1 & 0xffff, pd = w1 >> 16;
                int i = x & 15;
                uint32_t qa = match[((uint32_t)lr[i][pa >> 11] << 8) | ((uint32_t)lg[i][(pa >> 5) & 0x3f] << 4) | lb[i][pa & 0x1f]];
                uint32_t qb = match[((uint32_t)lr[i + 1][pb >> 11] << 8) | ((uint32_t)lg[i + 1][(pb >> 5) & 0x3f] << 4) | lb[i + 1][pb & 0x1f]];
                uint32_t qc = match[((uint32_t)lr[i + 2][pc >> 11] << 8) | ((uint32_t)lg[i + 2][(pc >> 5) & 0x3f] << 4) | lb[i + 2][pc & 0x1f]];
                uint32_t qd = match[((uint32_t)lr[i + 3][pd >> 11] << 8) | ((uint32_t)lg[i + 3][(pd >> 5) & 0x3f] << 4) | lb[i + 3][pd & 0x1f]];
                d32[x >> 1] = qa | (qb << 16);
                d32[(x >> 1) + 1] = qc | (qd << 16);
            }
        }
        for (; x < w; x++) {
            uint16_t px = src[x];
            int i = x & 15;
            dst[x] = match[((unsigned)lr[i][px >> 11] << 8) | ((unsigned)lg[i][(px >> 5) & 0x3f] << 4) | lb[i][px & 0x1f]];
        }
    }
}

const fx_desc_t fx_palette = {
    .id = "palette",
    .name = "Palette",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = true,
    .temporal = false,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
