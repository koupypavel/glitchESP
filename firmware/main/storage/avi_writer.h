/*
 * Minimal Motion-JPEG AVI writer: one video stream, JPEG frames appended as they come,
 * index and header finalized on close (frame rate is measured, not assumed).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sd_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sd_writer_t sd;
    bool open;
    uint32_t w, h;
    uint32_t frames;
    uint32_t max_frame_bytes;
    uint32_t movi_list_pos;  /* file offset of the 'LIST' that holds 'movi' */
    uint32_t *index;         /* pairs: offset (relative to 'movi' fourcc), size */
    uint32_t index_cap;      /* in frames */
    int64_t t_start_us;
} avi_writer_t;

esp_err_t avi_open(avi_writer_t *a, const char *path, uint32_t w, uint32_t h, uint32_t max_frames);
esp_err_t avi_write_frame(avi_writer_t *a, const uint8_t *jpeg, uint32_t len);
/* Finalizes header + index; fps = frames / elapsed. Returns duration in ms via *duration_ms. */
esp_err_t avi_close(avi_writer_t *a, uint32_t *duration_ms);

#ifdef __cplusplus
}
#endif
