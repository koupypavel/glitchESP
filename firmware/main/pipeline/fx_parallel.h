/*
 * Two-core runner for row-parallel effects: core 0 (a worker task) renders the top half of
 * the rows while the caller (camera task on core 1) renders the bottom half.
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the worker task and installs itself as the engine's parallel runner. */
esp_err_t fx_parallel_init(void);

/* Run fn(arg, y0, y1) over [y0, y1) split across both cores; blocks until both halves finish.
 * `out`/`out_stride_px` describe the buffer written so cache lines can be written back /
 * invalidated across cores (rows are out rows). */
typedef void (*fx_row_fn)(void *arg, int y0, int y1);
void fx_parallel_rows(fx_row_fn fn, void *arg, int y0, int y1, uint16_t *out, uint32_t out_stride_px);

#ifdef __cplusplus
}
#endif
