/*
 * Slit scan: a narrow slit sweeps across the picture and only the part under the slit is
 * refreshed from the camera. Everything else is left as it was when the slit last passed,
 * so the picture is assembled from many different moments. Things that move while the
 * slit passes over them come out stretched, squashed or bent.
 *
 * Temporal: the untouched part of the picture is the effect's own previous output
 * (ctx->prev). Where the slit is follows from the frame number alone, so the effect keeps
 * no state and the rows can be rendered in any order, on either core.
 */
#include <string.h>
#include "fx.h"
#include "fx_rng.h"

enum { P_SPEED, P_SIDEWAYS, P_BOUNCE };

static const fx_param_t s_params[] = {
    { "speed",    "Slit speed (px/frame)", 1, 64, 8, 1 },
    { "sideways", "Sideways",              0, 1, 0, 1 },
    { "bounce",   "Back and forth",        0, 1, 0, 1 },
};

static void from_amount(float a, float *p)
{
    p[P_SPEED] = 32.0f - a * 28.0f;     /* slower slit = moments further apart = stronger */
    p[P_SIDEWAYS] = 0;
    p[P_BOUNCE] = 0;
}

/* Start of the slit along an axis of length L at this frame. */
static int slit_pos(uint32_t frame, int speed, int L, int bounce)
{
    if (!bounce) return (int)(((uint64_t)frame * (uint32_t)speed) % (uint32_t)L);
    int span = L - speed;
    if (span <= 0) return 0;
    int t = (int)(((uint64_t)frame * (uint32_t)speed) % (uint32_t)(2 * span));
    return t < span ? t : 2 * span - t;
}

static void FX_HOT apply(const fx_frame_t *in, fx_frame_t *out, const float *p, const fx_ctx_t *ctx)
{
    const fx_frame_t *prev = ctx->prev;
    if (!prev || prev->w != in->w || prev->h != in->h) {
        fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);      /* nothing to keep yet */
        return;
    }
    int W = in->w, H = in->h;
    int speed = fx_px(in, p[P_SPEED], 1);
    int sideways = p[P_SIDEWAYS] >= 0.5f, bounce = p[P_BOUNCE] >= 0.5f;
    size_t row_bytes = (size_t)W * sizeof(uint16_t);

    if (!sideways) {
        /* a horizontal slit moving down: whole rows come either from now or from before */
        int pos = slit_pos(ctx->frame_no, speed, H, bounce);
        for (int y = ctx->y0; y < ctx->y1; y++) {
            int d = y - pos;
            if (d < 0) d += H;                              /* the slit wraps past the bottom */
            const fx_frame_t *from = d < speed ? in : prev;
            memcpy(out->px + (size_t)y * out->stride_px, from->px + (size_t)y * from->stride_px, row_bytes);
        }
    } else {
        /* a vertical slit moving right: every row is the old row with a fresh piece in it */
        int pos = slit_pos(ctx->frame_no, speed, W, bounce);
        int n1 = pos + speed <= W ? speed : W - pos;        /* piece before the right edge */
        int n2 = speed - n1;                                /* rest wraps to the left edge */
        for (int y = ctx->y0; y < ctx->y1; y++) {
            const uint16_t *now = in->px + (size_t)y * in->stride_px;
            uint16_t *dst = out->px + (size_t)y * out->stride_px;
            memcpy(dst, prev->px + (size_t)y * prev->stride_px, row_bytes);
            memcpy(dst + pos, now + pos, (size_t)n1 * sizeof(uint16_t));
            if (n2 > 0) memcpy(dst, now, (size_t)n2 * sizeof(uint16_t));
        }
    }
}

const fx_desc_t fx_slitscan = {
    .id = "slitscan",
    .name = "Slit scan",
    .n_params = sizeof(s_params) / sizeof(s_params[0]),
    .params = s_params,
    .in_place = false,
    .temporal = true,
    .row_parallel = true,
    .cost = FX_COST_LIGHT,
    .from_amount = from_amount,
    .apply = apply,
};
