/*
 * Fast sequential file writer for the microSD card.
 *
 * Measured on this board (40 MHz, 4-bit SDMMC), writing 4 MB:
 *     fwrite() through a 32 KB stdio buffer ......... 1.8 MB/s
 *     write() of 64 KB from a cache-aligned PSRAM buffer 2.8 MB/s
 *     write() of 32 KB from an on-chip DMA buffer ..... 5.2 MB/s
 * The card driver can only hand a buffer straight to the hardware when it is DMA-capable
 * and aligned; anything else is copied through a temporary buffer in small pieces. So this
 * writer collects data in one 32 KB on-chip DMA buffer and always writes whole buffers at
 * file offsets that are multiples of 32 KB.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;                 /* -1 when closed */
    uint8_t *buf;
    size_t fill;            /* bytes waiting in buf */
    uint32_t pos;           /* logical file position (bytes appended so far) */
    bool failed;            /* a write came up short: everything after it is refused */
} sd_writer_t;

esp_err_t sdw_prealloc(void);           /* get the buffer now, while a block that size is easy to find */
esp_err_t sdw_open(sd_writer_t *w, const char *path);      /* one file at a time */
esp_err_t sdw_write(sd_writer_t *w, const void *data, size_t len);      /* append */
static inline uint32_t sdw_tell(const sd_writer_t *w) { return w->pos; }
/* Overwrite bytes at the start of the file (headers that are only known at the end).
 * Flushes first; appending afterwards is not supported. */
esp_err_t sdw_rewrite_start(sd_writer_t *w, const void *data, size_t len);
esp_err_t sdw_close(sd_writer_t *w);                                     /* flush + close */

#ifdef __cplusplus
}
#endif
