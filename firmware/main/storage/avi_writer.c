#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "avi_writer.h"

static const char *TAG = "avi";

#define AVIF_HASINDEX       0x00000010
#define AVIIF_KEYFRAME      0x00000010
#define AVI_HEADER_BYTES    224   /* RIFF(12) + LIST hdrl(8+192) + LIST movi header(12) */

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* Build the fixed-size header into hdr[256], returns its length. Sizes/frame counts are
 * placeholders until avi_close(). */
static size_t build_header(const avi_writer_t *a, uint8_t *hdr, uint32_t riff_size, uint32_t movi_size, uint32_t fps_x1000)
{
    uint8_t *p = hdr;
    memcpy(p, "RIFF", 4); put32(p + 4, riff_size); memcpy(p + 8, "AVI ", 4); p += 12;
    memcpy(p, "LIST", 4); put32(p + 4, 4 + 8 + 56 + 12 + 8 + 56 + 8 + 40); memcpy(p + 8, "hdrl", 4); p += 12;
    /* avih */
    memcpy(p, "avih", 4); put32(p + 4, 56); p += 8;
    uint32_t usec = fps_x1000 ? (uint32_t)(1000000000ULL / fps_x1000) : 66666;
    put32(p + 0, usec);                         /* dwMicroSecPerFrame */
    put32(p + 4, a->max_frame_bytes * (fps_x1000 / 1000 + 1));   /* dwMaxBytesPerSec */
    put32(p + 8, 0);                            /* dwPaddingGranularity */
    put32(p + 12, AVIF_HASINDEX);               /* dwFlags */
    put32(p + 16, a->frames);                   /* dwTotalFrames */
    put32(p + 20, 0);                           /* dwInitialFrames */
    put32(p + 24, 1);                           /* dwStreams */
    put32(p + 28, a->max_frame_bytes);          /* dwSuggestedBufferSize */
    put32(p + 32, a->w);
    put32(p + 36, a->h);
    memset(p + 40, 0, 16);                      /* dwReserved[4] */
    p += 56;
    /* LIST strl */
    memcpy(p, "LIST", 4); put32(p + 4, 4 + 8 + 56 + 8 + 40); memcpy(p + 8, "strl", 4); p += 12;
    /* strh */
    memcpy(p, "strh", 4); put32(p + 4, 56); p += 8;
    memcpy(p + 0, "vids", 4);
    memcpy(p + 4, "MJPG", 4);
    put32(p + 8, 0);                            /* dwFlags */
    put16(p + 12, 0); put16(p + 14, 0);         /* wPriority, wLanguage */
    put32(p + 16, 0);                           /* dwInitialFrames */
    put32(p + 20, 1000);                        /* dwScale */
    put32(p + 24, fps_x1000 ? fps_x1000 : 15000); /* dwRate: fps = rate/scale */
    put32(p + 28, 0);                           /* dwStart */
    put32(p + 32, a->frames);                   /* dwLength */
    put32(p + 36, a->max_frame_bytes);          /* dwSuggestedBufferSize */
    put32(p + 40, 0xFFFFFFFFu);                 /* dwQuality */
    put32(p + 44, 0);                           /* dwSampleSize */
    put16(p + 48, 0); put16(p + 50, 0); put16(p + 52, (uint16_t)a->w); put16(p + 54, (uint16_t)a->h);
    p += 56;
    /* strf: BITMAPINFOHEADER */
    memcpy(p, "strf", 4); put32(p + 4, 40); p += 8;
    put32(p + 0, 40);
    put32(p + 4, a->w);
    put32(p + 8, a->h);
    put16(p + 12, 1); put16(p + 14, 24);
    memcpy(p + 16, "MJPG", 4);
    put32(p + 20, a->w * a->h * 3);
    put32(p + 24, 0); put32(p + 28, 0); put32(p + 32, 0); put32(p + 36, 0);
    p += 40;
    /* LIST movi */
    memcpy(p, "LIST", 4); put32(p + 4, movi_size); memcpy(p + 8, "movi", 4); p += 12;

    return (size_t)(p - hdr);
}

esp_err_t avi_open(avi_writer_t *a, const char *path, uint32_t w, uint32_t h, uint32_t max_frames)
{
    memset(a, 0, sizeof(*a));
    a->w = w; a->h = h;
    a->index_cap = max_frames;
    a->index = heap_caps_malloc((size_t)max_frames * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(a->index, ESP_ERR_NO_MEM, TAG, "index");
    esp_err_t ret = sdw_open(&a->sd, path);
    if (ret != ESP_OK) {
        heap_caps_free(a->index); a->index = NULL;
        return ret;
    }
    uint8_t hdr[256];
    size_t n = build_header(a, hdr, 0, 4, 0);
    ret = sdw_write(&a->sd, hdr, n);
    if (ret != ESP_OK) {
        sdw_close(&a->sd);
        heap_caps_free(a->index); a->index = NULL;
        return ret;
    }
    a->open = true;
    a->movi_list_pos = sdw_tell(&a->sd) - 12;   /* the 'LIST....movi' header we just wrote */
    a->t_start_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t avi_write_frame(avi_writer_t *a, const uint8_t *jpeg, uint32_t len)
{
    if (!a->open) return ESP_ERR_INVALID_STATE;
    if (a->frames >= a->index_cap) return ESP_ERR_NO_MEM;
    uint32_t pos = sdw_tell(&a->sd);
    uint8_t ch[8];
    memcpy(ch, "00dc", 4); put32(ch + 4, len);
    if (sdw_write(&a->sd, ch, 8) != ESP_OK) return ESP_FAIL;
    if (sdw_write(&a->sd, jpeg, len) != ESP_OK) return ESP_FAIL;
    if (len & 1) { uint8_t z = 0; sdw_write(&a->sd, &z, 1); }
    a->index[a->frames * 2] = pos - (a->movi_list_pos + 8);   /* relative to 'movi' fourcc */
    a->index[a->frames * 2 + 1] = len;
    a->frames++;
    if (len > a->max_frame_bytes) a->max_frame_bytes = len;
    return ESP_OK;
}

esp_err_t avi_close(avi_writer_t *a, uint32_t *duration_ms)
{
    if (!a->open) return ESP_ERR_INVALID_STATE;
    int64_t dur_us = esp_timer_get_time() - a->t_start_us;
    uint32_t fps_x1000 = dur_us > 0 ? (uint32_t)((uint64_t)a->frames * 1000000000ULL / (uint64_t)dur_us) : 15000;
    if (a->frames < 2 || fps_x1000 < 1000 || fps_x1000 > 60000) fps_x1000 = 15000;   /* sane range 1..60 fps */

    uint32_t movi_end = sdw_tell(&a->sd);
    uint32_t movi_size = movi_end - (a->movi_list_pos + 8);

    /* idx1 */
    uint8_t ch[8];
    memcpy(ch, "idx1", 4); put32(ch + 4, a->frames * 16);
    sdw_write(&a->sd, ch, 8);
    for (uint32_t i = 0; i < a->frames; i++) {
        uint8_t e[16];
        memcpy(e, "00dc", 4); put32(e + 4, AVIIF_KEYFRAME);
        put32(e + 8, a->index[i * 2]); put32(e + 12, a->index[i * 2 + 1]);
        sdw_write(&a->sd, e, 16);
    }
    uint32_t end = sdw_tell(&a->sd);
    uint8_t hdr[256];
    size_t n = build_header(a, hdr, end - 8, movi_size, fps_x1000);
    esp_err_t ret = sdw_rewrite_start(&a->sd, hdr, n);
    esp_err_t cret = sdw_close(&a->sd);
    if (ret == ESP_OK) ret = cret;
    a->open = false;
    heap_caps_free(a->index);
    a->index = NULL;
    if (duration_ms) *duration_ms = (uint32_t)(dur_us / 1000);
    ESP_LOGI(TAG, "closed: %lu frames, %.1f fps, %lu KB", (unsigned long)a->frames, fps_x1000 / 1000.0,
             (unsigned long)(end / 1024));
    return ret;
}
