/*
 * Small deterministic PRNG for effects (xorshift32 + hash seeding). Header-only.
 *
 * Effects seed a fresh generator per band or per row with fx_rng_init_at(), so every row's
 * randomness depends only on (seed, frame_no, salt, row/band index). That keeps a frame
 * reproducible from its sidecar and identical whether it was rendered on one core or two.
 */
#pragma once

#include <stdint.h>
#include "fx.h"

typedef struct { uint32_t s; } fx_rng_t;

static inline uint32_t fx_hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Seed for the whole frame (use only for things that are not row dependent). */
static inline void fx_rng_init(fx_rng_t *r, const fx_ctx_t *ctx, uint32_t salt)
{
    uint32_t s = fx_hash32(ctx->seed ^ (ctx->frame_no * 0x9e3779b9U) ^ salt);
    r->s = s ? s : 0x1234567U;
}

/* Seed for one row / band / tile: independent of what happened on other rows. */
static inline void fx_rng_init_at(fx_rng_t *r, const fx_ctx_t *ctx, uint32_t salt, uint32_t index)
{
    uint32_t s = fx_hash32(ctx->seed ^ (ctx->frame_no * 0x9e3779b9U) ^ salt ^ (index * 0x85ebca6bU));
    r->s = s ? s : 0x1234567U;
}

static inline uint32_t fx_rng_u32(fx_rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}

/* uniform float in [0,1) */
static inline float fx_rng_f(fx_rng_t *r)
{
    return (float)(fx_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

/* uniform int in [lo, hi] */
static inline int fx_rng_range(fx_rng_t *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(fx_rng_u32(r) % (uint32_t)(hi - lo + 1));
}

static inline int fx_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fx_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
