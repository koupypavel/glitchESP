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
#define FX_COST_SOFT    5     /* a blur: full resolution adds nothing, so always preview at half (squint) */

/*
 * On the ESP32-P4 this firmware executes code from PSRAM (XIP). Effect inner loops must live
 * in internal RAM or they fight the pixel data for cache and bus bandwidth (measured 3x).
 * On the PC harness the macro is empty.
 */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_cache.h"
#define FX_HOT IRAM_ATTR
#else
#define FX_HOT
#endif

/*
 * The two cores do not see each other's cached writes to PSRAM. Per-frame state that one
 * core builds (analyze, prepare) and both cores read (apply) is safe in on-chip RAM (static
 * tables); if it lives in a fx_big_alloc() block, the writer calls fx_mem_publish() when it
 * is done and every apply() calls fx_mem_fetch() before reading it. Both take the block's
 * start and a length rounded up to 128 bytes. Nothing to do on the PC.
 */
#if defined(ESP_PLATFORM)
static inline void fx_mem_publish(const void *p, size_t n) { esp_cache_msync((void *)p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED); }
static inline void fx_mem_fetch(void *p, size_t n) { esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C); }
#else
static inline void fx_mem_publish(const void *p, size_t n) { (void)p; (void)n; }
static inline void fx_mem_fetch(void *p, size_t n) { (void)p; (void)n; }
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
    float step;            /* 0 = continuous; 1 with a 0..1 range = an on/off switch */
} fx_param_t;

/* Per-frame context. `seed` and `frame_no` fully determine the randomness. */
typedef struct {
    uint32_t seed;
    uint32_t frame_no;
    const fx_frame_t *prev;   /* what the chain's temporal effect produced last frame, may be NULL */
    fx_frame_t *keep;         /* where the chain stores that effect's output for the next frame (may
                                 be the same buffer as prev: it is written after the effect ran), or NULL */
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
    /* Optional: called once per frame before apply(), on one core, with the whole input frame
     * (measurements that every row needs: a histogram, a coarse copy of the picture). */
    void (*analyze)(const fx_frame_t *in, const float *params, const fx_ctx_t *ctx);
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
 * `in`, `out` and `tmp` must be distinct buffers. For a temporal effect, pass its history in
 * ctx->prev and ctx->keep.
 */
void fx_chain_apply(const fx_chain_t *c, const fx_frame_t *in, fx_frame_t *out, fx_frame_t *tmp,
                    const fx_ctx_t *ctx);

/*
 * Run the chain with only two buffers: the image starts in `a`, each effect renders into the
 * other buffer. Both are overwritten; returns the one holding the result (`a` if no effect
 * is enabled). For frames too large to afford a third buffer (high-resolution stills).
 */
fx_frame_t *fx_chain_apply_pingpong(const fx_chain_t *c, fx_frame_t *a, fx_frame_t *b, const fx_ctx_t *ctx);

/*
 * Optional parallel runner: the host installs a function that runs one row-parallel effect
 * split across cores (it must call fx->apply for every row exactly once). NULL = sequential.
 */
typedef void (*fx_parallel_fn)(const fx_desc_t *fx, const fx_frame_t *in, fx_frame_t *out,
                               const float *params, const fx_ctx_t *ctx);
void fx_set_parallel_runner(fx_parallel_fn fn);

/*
 * Row jobs outside apply(): work an effect's analyze() wants spread over both cores, such as
 * reading the whole frame. fx_rows_parallel() runs fn(arg, y0, y1) over [y0, y1), split across
 * the cores when the host installed a runner, inline otherwise. `reads` is the frame the
 * rows read (made visible to the other core first); `writes` is what row r writes, at
 * writes + r * write_row_bytes (made visible back when the job is done; write_row_bytes must
 * be a multiple of 128). fn must not touch anything else another core wrote this frame.
 */
typedef void (*fx_rows_fn)(void *arg, int y0, int y1);
typedef void (*fx_rows_runner_fn)(fx_rows_fn fn, void *arg, int y0, int y1, const fx_frame_t *reads,
                                  void *writes, size_t write_row_bytes);
void fx_set_rows_runner(fx_rows_runner_fn fn);
void fx_rows_parallel(fx_rows_fn fn, void *arg, int y0, int y1, const fx_frame_t *reads, void *writes,
                      size_t write_row_bytes);

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

/*
 * Resolution independence. Parameters measured in pixels (shifts, band heights, tile sizes,
 * wavelengths) are defined for a frame FX_REF_W pixels wide. Effects multiply them by
 * fx_scale(), so the same recipe looks the same on the 360-wide preview path, the 720-wide
 * full path and a 1088-wide still.
 */
#define FX_REF_W 720
static inline float fx_scale(const fx_frame_t *f) { return (float)f->w / (float)FX_REF_W; }
/* a pixel-unit parameter scaled to this frame, rounded, never below `min` */
static inline int fx_px(const fx_frame_t *f, float v, int min)
{
    float s = v * fx_scale(f);
    int r = (int)(s < 0 ? s - 0.5f : s + 0.5f);
    return r < min ? min : r;
}

/* Large working tables (>16 KB) must not live in on-chip RAM: allocate them through this
 * (PSRAM on the device, malloc on the PC). The block starts on a 128-byte boundary and its
 * length is rounded up to a multiple of 128 (see fx_mem_fetch). Returns NULL on failure.
 * Never freed. */
void *fx_big_alloc(size_t bytes);

void fx_frame_copy(const fx_frame_t *in, fx_frame_t *out);
void fx_frame_copy_rows(const fx_frame_t *in, fx_frame_t *out, int y0, int y1);

#ifdef __cplusplus
}
#endif
