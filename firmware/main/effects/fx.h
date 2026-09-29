/*
 * glitchESP effect engine — public interface.
 *
 * Pure C99, no ESP-IDF dependencies: the same files compile in tools/fxlab on a PC.
 * Frames are RGB565 (uint16_t per pixel, R in bits 15..11, G in 10..5, B in 4..0).
 *
 * Row-parallel contract: apply() produces rows [ctx->y0, ctx->y1) of `out` and may read any
 * row of `in`. Randomness must be derived from (seed, frame_no, row/band) — see fx_rng.h —
 * never from a generator that runs across rows, so that two cores producing two halves give
 * exactly the result one core would.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_MAX_PARAMS   8
#define FX_CHAIN_MAX    3

#define FX_COST_LIGHT   1     /* ~1 memcpy pass (scanline, wave) */
#define FX_COST_MEDIUM  2     /* 1-2 per-pixel passes (blocks) */
#define FX_COST_HEAVY   3     /* multi-tap per-pixel or sorting (chanshift, bitcrush, pixelsort) */

/*
 * On the ESP32-P4 this firmware executes code from PSRAM (XIP). Effect inner loops must live
 * in internal RAM or they fight the pixel data for cache and bus bandwidth (measured 3x).
 * On the PC harness the macro is empty.
 */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define FX_HOT IRAM_ATTR
#else
#define FX_HOT
#endif

typedef struct {
    uint16_t *px;          /* pixel data */
    uint16_t w, h;         /* size in pixels */
    uint32_t stride_px;    /* pixels per row (>= w) */
} fx_frame_t;

typedef struct {
    const char *id;        /* short machine name, e.g. "dx_r" */
    const char *name;      /* UI label */
    float min, max, def;
} fx_param_t;

/* Per-frame context. `seed` and `frame_no` fully determine the randomness. */
typedef struct {
    uint32_t seed;
    uint32_t frame_no;
    const fx_frame_t *prev;   /* previous *output* frame for temporal effects, may be NULL */
    void *scratch;            /* optional work memory, scratch_len bytes, may be NULL */
    size_t scratch_len;
    uint16_t y0, y1;          /* rows to produce: [y0, y1). The chain sets these. */
} fx_ctx_t;

typedef struct fx_desc {
    const char *id;           /* "chanshift" */
    const char *name;         /* "Channel shift" */
    uint8_t n_params;
    const fx_param_t *params;
    bool in_place;            /* apply() tolerates in == out */
    bool temporal;            /* uses ctx->prev */
    bool row_parallel;        /* apply() honours ctx->y0/y1 and can run split across cores */
    uint8_t cost;             /* FX_COST_*: guides the half-resolution preview decision */
    /* Fill `params` from a single 0..1 "amount" knob (the one-knob mapping). */
    void (*from_amount)(float amount, float *params);
    /* Optional: called once per frame before apply() (lookup tables etc.). */
    void (*prepare)(const float *params, const fx_ctx_t *ctx);
    /* Render rows [ctx->y0, ctx->y1) of `out` from `in`. Both frames have equal w/h. */
    void (*apply)(const fx_frame_t *in, fx_frame_t *out, const float *params, const fx_ctx_t *ctx);
} fx_desc_t;

/* ---- registry ---- */
int              fx_registry_count(void);
const fx_desc_t *fx_registry_get(int index);
const fx_desc_t *fx_registry_find(const char *id);
void             fx_params_defaults(const fx_desc_t *fx, float *params);
int              fx_param_index(const fx_desc_t *fx, const char *param_id);   /* -1 if unknown */

/* ---- chain ---- */
typedef struct {
    const fx_desc_t *fx;
    float params[FX_MAX_PARAMS];
    bool enabled;
} fx_slot_t;

typedef struct {
    fx_slot_t slots[FX_CHAIN_MAX];
    int count;
} fx_chain_t;

void fx_chain_clear(fx_chain_t *c);
int  fx_chain_add(fx_chain_t *c, const fx_desc_t *fx);            /* returns slot index or -1 */
void fx_chain_set_amount(fx_chain_t *c, float amount);            /* applies from_amount to every slot */

/*
 * Run the chain. `tmp` must be a frame of the same size as `in`/`out` (used for ping-pong
 * when more than one effect is enabled). With no enabled effect the input is copied to out.
 * `in`, `out` and `tmp` must be distinct buffers.
 */
void fx_chain_apply(const fx_chain_t *c, const fx_frame_t *in, fx_frame_t *out, fx_frame_t *tmp,
                    const fx_ctx_t *ctx);

/*
 * Optional parallel runner: the host installs a function that runs one row-parallel effect
 * split across cores (it must call fx->apply for every row exactly once). NULL = sequential.
 */
typedef void (*fx_parallel_fn)(const fx_desc_t *fx, const fx_frame_t *in, fx_frame_t *out,
                               const float *params, const fx_ctx_t *ctx);
void fx_set_parallel_runner(fx_parallel_fn fn);

/* ---- helpers shared by effects ---- */
static inline uint16_t fx_rgb565(unsigned r5, unsigned g6, unsigned b5)
{
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}
static inline unsigned fx_r5(uint16_t p) { return (p >> 11) & 0x1f; }
static inline unsigned fx_g6(uint16_t p) { return (p >> 5) & 0x3f; }
static inline unsigned fx_b5(uint16_t p) { return p & 0x1f; }
/* 0..255 luma approximation */
static inline unsigned fx_luma(uint16_t p)
{
    unsigned r = fx_r5(p) << 3, g = fx_g6(p) << 2, b = fx_b5(p) << 3;
    return (r * 77 + g * 151 + b * 28) >> 8;
}

void fx_frame_copy(const fx_frame_t *in, fx_frame_t *out);
void fx_frame_copy_rows(const fx_frame_t *in, fx_frame_t *out, int y0, int y1);

#ifdef __cplusplus
}
#endif
