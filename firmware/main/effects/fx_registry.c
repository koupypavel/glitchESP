#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "fx.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

/* Effects register themselves through these externs (one per file). */
extern const fx_desc_t fx_chanshift;
extern const fx_desc_t fx_scanline;
extern const fx_desc_t fx_bitcrush;
extern const fx_desc_t fx_blocks;
extern const fx_desc_t fx_wave;
extern const fx_desc_t fx_pixelsort;
extern const fx_desc_t fx_tracers;
extern const fx_desc_t fx_hueshift;
extern const fx_desc_t fx_kaleido;
extern const fx_desc_t fx_diffract;
extern const fx_desc_t fx_drift;
extern const fx_desc_t fx_breathe;

static const fx_desc_t *const s_registry[] = {
    &fx_chanshift,
    &fx_scanline,
    &fx_bitcrush,
    &fx_blocks,
    &fx_wave,
    &fx_pixelsort,
    &fx_tracers,
    &fx_hueshift,
    &fx_kaleido,
    &fx_diffract,
    &fx_drift,
    &fx_breathe,
};

static fx_parallel_fn s_parallel;

void *fx_big_alloc(size_t bytes)
{
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
    return malloc(bytes);
#endif
}

int fx_registry_count(void)
{
    return (int)(sizeof(s_registry) / sizeof(s_registry[0]));
}

const fx_desc_t *fx_registry_get(int index)
{
    if (index < 0 || index >= fx_registry_count()) return NULL;
    return s_registry[index];
}

const fx_desc_t *fx_registry_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < fx_registry_count(); i++) {
        if (strcmp(s_registry[i]->id, id) == 0) return s_registry[i];
    }
    return NULL;
}

void fx_params_defaults(const fx_desc_t *fx, float *params)
{
    for (int i = 0; i < FX_MAX_PARAMS; i++) {
        params[i] = (i < fx->n_params) ? fx->params[i].def : 0.0f;
    }
}

int fx_param_index(const fx_desc_t *fx, const char *param_id)
{
    for (int i = 0; i < fx->n_params; i++) {
        if (strcmp(fx->params[i].id, param_id) == 0) return i;
    }
    return -1;
}

void FX_HOT fx_frame_copy_rows(const fx_frame_t *in, fx_frame_t *out, int y0, int y1)
{
    if (in->px == out->px) return;
    size_t row = (size_t)in->w * sizeof(uint16_t);
    for (int y = y0; y < y1; y++) {
        memcpy(out->px + (size_t)y * out->stride_px, in->px + (size_t)y * in->stride_px, row);
    }
}

void FX_HOT fx_frame_copy(const fx_frame_t *in, fx_frame_t *out)
{
    fx_frame_copy_rows(in, out, 0, in->h);
}

void fx_set_parallel_runner(fx_parallel_fn fn)
{
    s_parallel = fn;
}

/* ---- chain ---- */

void fx_chain_clear(fx_chain_t *c)
{
    memset(c, 0, sizeof(*c));
}

int fx_chain_add(fx_chain_t *c, const fx_desc_t *fx)
{
    if (!fx || c->count >= FX_CHAIN_MAX) return -1;
    fx_slot_t *s = &c->slots[c->count];
    s->fx = fx;
    s->enabled = true;
    fx_params_defaults(fx, s->params);
    return c->count++;
}

void fx_chain_set_amount(fx_chain_t *c, float amount)
{
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    for (int i = 0; i < c->count; i++) {
        if (c->slots[i].fx && c->slots[i].fx->from_amount) {
            c->slots[i].fx->from_amount(amount, c->slots[i].params);
        }
    }
}

static void run_effect(const fx_desc_t *fx, const fx_frame_t *in, fx_frame_t *out,
                       const float *params, const fx_ctx_t *ctx_in)
{
    fx_ctx_t ctx = *ctx_in;
    ctx.y0 = 0;
    ctx.y1 = in->h;
    if (fx->prepare) fx->prepare(params, &ctx);
    if (fx->row_parallel && s_parallel) {
        s_parallel(fx, in, out, params, &ctx);
    } else {
        fx->apply(in, out, params, &ctx);
    }
}

static void copy_apply(const fx_frame_t *in, fx_frame_t *out, const float *params, const fx_ctx_t *ctx)
{
    (void)params;
    fx_frame_copy_rows(in, out, ctx->y0, ctx->y1);
}

/* A plain copy dressed as an effect, so it goes through the same parallel runner. */
static const fx_desc_t s_copy_fx = { .id = "copy", .name = "copy", .row_parallel = true, .apply = copy_apply };

/*
 * One stage of a chain. The history (ctx->prev in, ctx->keep out) belongs to the first
 * temporal effect: it sees what it produced itself on the previous frame, and its output is
 * stored before any later effect touches it. Feeding it the chain's final output instead
 * would run every later effect over the trail again on each frame, which compounds (hue,
 * contrast, displacement) until the picture dissolves.
 */
static void run_stage(const fx_slot_t *s, const fx_frame_t *src, fx_frame_t *dst, const fx_ctx_t *ctx, bool *history_used)
{
    fx_ctx_t c = *ctx;
    bool owns_history = s->fx->temporal && !*history_used;
    if (!owns_history) c.prev = NULL;
    run_effect(s->fx, src, dst, s->params, &c);
    if (owns_history) {
        *history_used = true;
        if (ctx->keep) run_effect(&s_copy_fx, dst, ctx->keep, NULL, ctx);
    }
}

fx_frame_t *fx_chain_apply_pingpong(const fx_chain_t *c, fx_frame_t *a, fx_frame_t *b, const fx_ctx_t *ctx)
{
    fx_frame_t *src = a, *dst = b;
    bool history_used = false;
    for (int i = 0; i < c->count; i++) {
        const fx_slot_t *s = &c->slots[i];
        if (!s->enabled || !s->fx) continue;
        run_stage(s, src, dst, ctx, &history_used);
        fx_frame_t *t = src; src = dst; dst = t;
    }
    return src;
}

void FX_HOT fx_chain_apply(const fx_chain_t *c, const fx_frame_t *in, fx_frame_t *out, fx_frame_t *tmp,
                           const fx_ctx_t *ctx)
{
    int enabled = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->slots[i].enabled && c->slots[i].fx) enabled++;
    }
    if (enabled == 0) {
        fx_frame_copy(in, out);
        return;
    }

    /* Ping-pong between `tmp` and `out`, counted back from the end so that the last effect
     * writes `out` and no effect ever reads the buffer it is writing: with three effects the
     * order is out, tmp, out; with two it is tmp, out. */
    const fx_frame_t *src = in;
    int left = enabled;
    bool history_used = false;
    for (int i = 0; i < c->count; i++) {
        const fx_slot_t *s = &c->slots[i];
        if (!s->enabled || !s->fx) continue;
        left--;
        fx_frame_t *dst = (left % 2 == 0) ? out : tmp;
        run_stage(s, src, dst, ctx, &history_used);
        src = dst;
    }
}
