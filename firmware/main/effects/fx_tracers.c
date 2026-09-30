/*
 * Tracers: temporal feedback. Each frame is combined with a decayed copy of the previous
 * *output*, so moving things leave fading copies behind (psychedelic trails).
 *
 *   mode 0 "echo"  : out = max(in, prev * decay) per channel — the live frame stays bright,
 *                    older frames fade out (light-trail look).
 *   mode 1 "blend" : out = in * (1-decay) + prev * decay — smooth ghosting / motion blur.
 *
 * "rainbow" makes the three channels decay at slowly cycling rates (driven by frame_no), so
 * the trails drift through hues over time.
 *
 * Needs ctx->prev (the previous output at the same size). Without it, it copies the input.
 * Row-parallel: rows are independent. Uses the packed RGB565 trick: spreading a pixel into
 * 0x07E0F81F (G in the high half, R|B in the low) lets one multiply scale all three channels.
 */
#include <math.h>
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_DECAY, P_MODE, P_RAINBOW, P_SPEED };

static const fx_param_t s_params[] = {
    { "decay",   "Trail length",     0, 1, 0.85f, 0 },
    { "mode",    "Blend instead of echo", 0, 1, 0, 1 },
    { "rainbow", "Hue drift",        0, 1, 0.5f, 0 },
    { "speed",   "Drift speed",      0, 1, 0.3f, 0 },
};

static void from_amount(float a, float *p)
{
    p[P_DECAY] = 0.6f + a * 0.38f;
    p[P_MODE] = 0;
    p[P_RAINBOW] = a;
    p[P_SPEED] = 0.2f + a * 0.5f;
}

/* per-frame channel multipliers in 1/32 steps, computed once in prepare() */
static unsigned s_kr, s_kg, s_kb;     /* 0..32 */
static int s_mode;

static void prepare(const float *p, const fx_ctx_t *ctx)
{
    float decay = fx_clampf(p[P_DECAY], 0, 0.995f);
    float rainbow = fx_clampf(p[P_RAINBOW], 0, 1);
    float phase = (float)ctx->frame_no * (0.02f + p[P_SPEED] * 0.15f);
    float dr = decay, dg = decay, db = decay;
    if (rainbow > 0.0f) {
        /* each channel's persistence wobbles around `decay`, 120 degrees apart */
        dr = decay * (1.0f - rainbow * 0.35f * (0.5f + 0.5f * sinf(phase)));
        dg = decay * (1.0f - rainbow * 0.35f * (0.5f + 0.5f * sinf(phase + 2.094f)));
        db = decay * (1.0f - rainbow * 0.35f * (0.5f + 0.5f * sinf(phase + 4.189f)));
    }
    s_kr = (unsigned)(dr * 32.0f + 0.5f);
    s_kg = (unsigned)(dg * 32.0f + 0.5f);
    s_kb = (unsigned)(db * 32.0f + 0.5f);
    s_mode = p[P_MODE] >= 0.5f;
}

static inline uint32_t spread(uint16_t p)
{
    return ((uint32_t)p | ((uint32_t)p << 16)) & 0x07E0F81Fu;   /* G<<16 | R<<11 | B */
}
static inline uint16_t pack(uint32_t s)
{
    return (uint16_t)((s | (s >> 16)) & 0xFFFF);
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    (void)p;
    const fx_frame_t *prev = ctx->prev;
    if (!prev || prev->w != in->w || prev->h != in->h) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
        return;
    }
    unsigned kr = s_kr, kg = s_kg, kb = s_kb;
    int W = in->w;

    for (int y = ctx->y0; y < ctx->y1; y++) {
        const uint16_t *src = in->px + (size_t)y * in->stride_px;
        const uint16_t *old = prev->px + (size_t)y * prev->stride_px;
        uint16_t *dst = out->px + (size_t)y * out->stride_px;
        if (!s_mode) {
            /* echo: per-channel max(in, prev*k) */
            for (int x = 0; x < W; x++) {
                uint16_t a = src[x], b = old[x];
                unsigned br = (fx_r5(b) * kr) >> 5, bg = (fx_g6(b) * kg) >> 5, bb = (fx_b5(b) * kb) >> 5;
                unsigned ar = fx_r5(a), ag = fx_g6(a), ab = fx_b5(a);
                dst[x] = fx_rgb565(ar > br ? ar : br, ag > bg ? ag : bg, ab > bb ? ab : bb);
            }
        } else {
            /* blend: in*(32-k) + prev*k, with the packed-field trick (R/B share kr, G uses kg) */
            unsigned krb = kr, ir = 32 - kr, ig = 32 - kg;
            for (int x = 0; x < W; x++) {
                uint32_t a = spread(src[x]), b = spread(old[x]);
                uint32_t rb = (((a & 0xF81Fu) * ir + (b & 0xF81Fu) * krb) >> 5) & 0xF81Fu;
                uint32_t g  = (((a >> 16) * ig + (b >> 16) * kg) >> 5) & 0x07E0u;
                dst[x] = (uint16_t)(rb | g);
            }
        }
    }
}

const fx_desc_t fx_tracers = {
    .id = "tracers",
    .name = "Tracers",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = true,
    .row_parallel = true,
    .cost = FX_COST_HEAVY,      /* per-pixel compare + prev copy: 103 ms full, 49 ms half */
    .from_amount = from_amount,
    .prepare = prepare,
    .apply = apply,
};
