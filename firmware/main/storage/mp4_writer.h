/*
 * Minimal MP4 writer for one H.264 video track.
 *
 * Layout: ftyp, then one mdat box that receives the frames as they come, then the moov box
 * (the index) at the end. Frames go in as the encoder delivers them ("Annex B": NAL units
 * separated by start codes) and are rewritten to the MP4 form (each NAL prefixed with its
 * length); the parameter sets (SPS/PPS) are taken out of the stream and stored once in the
 * index. All frames form a single chunk, so the index needs only a size per frame and the
 * list of key frames. The frame rate is measured: duration / frames.
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
    uint32_t frames, cap;
    uint32_t *sizes;                /* bytes of each frame as stored */
    uint32_t *keys;                 /* frame numbers (1-based) of the key frames */
    uint32_t n_keys;
    uint8_t sps[96], pps[48];
    uint8_t sps_len, pps_len;
    int64_t t_start_us;
} mp4_writer_t;

esp_err_t mp4_open(mp4_writer_t *m, const char *path, uint32_t w, uint32_t h, uint32_t max_frames);
/* `data` is modified in place (start codes become lengths). */
esp_err_t mp4_write_frame(mp4_writer_t *m, uint8_t *data, uint32_t len);
esp_err_t mp4_close(mp4_writer_t *m, uint32_t *duration_ms);

#ifdef __cplusplus
}
#endif
