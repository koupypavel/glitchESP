#include <string.h>
#include <math.h>
#include "fx.h"

/* Effects register themselves through these externs (one per file). */
extern const fx_desc_t fx_chanshift;
extern const fx_desc_t fx_scanline;
extern const fx_desc_t fx_bitcrush;
extern const fx_desc_t fx_blocks;
extern const fx_desc_t fx_wave;
extern const fx_desc_t fx_pixelsort;

static const fx_desc_t *const s_registry[] = {
    &fx_chanshift,
    &fx_scanline,
    &fx_bitcrush,
    &fx_blocks,
    &fx_wave,
    &fx_pixelsort,
};

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

void FX_HOT fx_frame_copy(const fx_frame_t *in, fx_frame_t *out)
{
    if (in->px == out->px) return;
    size_t row = (size_t)in->w * sizeof(uint16_t);
    if (in->stride_px == in->w && out->stride_px == out->w) {
        memcpy(out->px, in->px, row * in->h);
        return;
    }
    for (unsigned y = 0; y < in->h; y++) {
        memcpy(out->px + (size_t)y * out->stride_px, in->px + (size_t)y * in->stride_px, row);
    }
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

    /* Ping-pong: the last enabled effect writes to `out`, the others alternate tmp/out. */
    const fx_frame_t *src = in;
    int done = 0;
    for (int i = 0; i < c->count; i++) {
        const fx_slot_t *s = &c->slots[i];
        if (!s->enabled || !s->fx) continue;
        done++;
        fx_frame_t *dst;
        if (done == enabled) {
            dst = out;
        } else {
            dst = (src == tmp) ? out : tmp;   /* never write into the buffer we read from */
        }
        s->fx->apply(src, dst, s->params, ctx);
        src = dst;
    }
}
