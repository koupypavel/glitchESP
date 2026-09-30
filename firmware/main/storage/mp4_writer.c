#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mp4_writer.h"

static const char *TAG = "mp4";

#define FTYP_BYTES      32
#define MDAT_HDR_BYTES  8
#define TIMESCALE       30000       /* ticks per second in the index */

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void be16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }

/* ---- file start: ftyp + the mdat header (its size is only known at the end) ---- */

static size_t build_start(uint8_t *b, uint32_t mdat_size)
{
    be32(b, FTYP_BYTES); memcpy(b + 4, "ftyp", 4);
    memcpy(b + 8, "isom", 4); be32(b + 12, 0x200);
    memcpy(b + 16, "isomiso2avc1mp41", 16);
    be32(b + FTYP_BYTES, mdat_size); memcpy(b + FTYP_BYTES + 4, "mdat", 4);
    return FTYP_BYTES + MDAT_HDR_BYTES;
}

esp_err_t mp4_open(mp4_writer_t *m, const char *path, uint32_t w, uint32_t h, uint32_t max_frames)
{
    memset(m, 0, sizeof(*m));
    m->w = w; m->h = h;
    m->cap = max_frames;
    m->sizes = heap_caps_malloc((size_t)max_frames * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    m->keys = heap_caps_malloc((size_t)max_frames * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    esp_err_t ret = m->sizes && m->keys ? sdw_open(&m->sd, path) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK) {
        uint8_t start[FTYP_BYTES + MDAT_HDR_BYTES];
        ret = sdw_write(&m->sd, start, build_start(start, MDAT_HDR_BYTES));
        if (ret != ESP_OK) sdw_close(&m->sd);
    }
    if (ret != ESP_OK) {
        heap_caps_free(m->sizes); heap_caps_free(m->keys);
        m->sizes = m->keys = NULL;
        return ret;
    }
    m->open = true;
    m->t_start_us = esp_timer_get_time();
    return ESP_OK;
}

/* ---- frames ---- */

/* Next start code (00 00 01) at or after `from`; returns its position or `len`. */
static uint32_t find_start(const uint8_t *d, uint32_t from, uint32_t len)
{
    for (uint32_t i = from; i + 3 <= len; i++) {
        if (d[i + 2] > 1) { i += 2; continue; }            /* cannot be inside 00 00 01: skip ahead */
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    }
    return len;
}

esp_err_t mp4_write_frame(mp4_writer_t *m, uint8_t *d, uint32_t len)
{
    if (!m->open) return ESP_ERR_INVALID_STATE;
    if (m->frames >= m->cap) return ESP_ERR_NO_MEM;
    uint32_t stored = 0;
    bool key = false;

    uint32_t sc = find_start(d, 0, len);
    while (sc < len) {
        uint32_t nal = sc + 3;                              /* first byte of the NAL unit */
        uint32_t next = find_start(d, nal, len);
        uint32_t end = next;
        while (end > nal && d[end - 1] == 0) end--;         /* zero padding before the next start code */
        uint32_t n = end - nal;
        uint8_t type = n ? d[nal] & 0x1f : 0;
        if (type == 7) {                                    /* sequence parameter set */
            if (n <= sizeof(m->sps)) { memcpy(m->sps, d + nal, n); m->sps_len = (uint8_t)n; }
        } else if (type == 8) {                             /* picture parameter set */
            if (n <= sizeof(m->pps)) { memcpy(m->pps, d + nal, n); m->pps_len = (uint8_t)n; }
        } else if (type >= 1 && type <= 5) {                /* picture data */
            uint8_t hdr[4];
            be32(hdr, n);
            if (sdw_write(&m->sd, hdr, 4) != ESP_OK || sdw_write(&m->sd, d + nal, n) != ESP_OK) return ESP_FAIL;
            stored += 4 + n;
            if (type == 5) key = true;
        }
        sc = next;
    }
    if (!stored) return ESP_OK;                             /* nothing but parameter sets */
    m->sizes[m->frames++] = stored;
    if (key) m->keys[m->n_keys++] = m->frames;
    return ESP_OK;
}

/* ---- the index (moov) ---- */

typedef struct { uint8_t *b; size_t n; } buf_t;

static size_t box(buf_t *o, const char *type)               /* start a box, return where it began */
{
    size_t at = o->n;
    memcpy(o->b + at + 4, type, 4);
    o->n += 8;
    return at;
}
static void end(buf_t *o, size_t at)                        /* close it: fill in its size */
{
    be32(o->b + at, (uint32_t)(o->n - at));
}
static void u32(buf_t *o, uint32_t v) { be32(o->b + o->n, v); o->n += 4; }
static void u16(buf_t *o, uint32_t v) { be16(o->b + o->n, v); o->n += 2; }
static void zeros(buf_t *o, size_t n) { memset(o->b + o->n, 0, n); o->n += n; }
static void bytes(buf_t *o, const void *p, size_t n) { memcpy(o->b + o->n, p, n); o->n += n; }
static void matrix(buf_t *o)                                /* identity transform */
{
    static const uint32_t k[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++) u32(o, k[i]);
}

static size_t build_moov(const mp4_writer_t *m, uint8_t *b, uint32_t delta)
{
    buf_t o = { b, 0 };
    uint32_t duration = m->frames * delta;
    size_t moov = box(&o, "moov");

    size_t a = box(&o, "mvhd");
    u32(&o, 0); u32(&o, 0); u32(&o, 0); u32(&o, TIMESCALE); u32(&o, duration);
    u32(&o, 0x00010000); u16(&o, 0x0100); zeros(&o, 10); matrix(&o); zeros(&o, 24); u32(&o, 2);
    end(&o, a);

    size_t trak = box(&o, "trak");
    a = box(&o, "tkhd");
    u32(&o, 0x00000007); u32(&o, 0); u32(&o, 0); u32(&o, 1); u32(&o, 0); u32(&o, duration);
    zeros(&o, 8); u16(&o, 0); u16(&o, 0); u16(&o, 0); u16(&o, 0); matrix(&o);
    u32(&o, m->w << 16); u32(&o, m->h << 16);
    end(&o, a);

    size_t mdia = box(&o, "mdia");
    a = box(&o, "mdhd");
    u32(&o, 0); u32(&o, 0); u32(&o, 0); u32(&o, TIMESCALE); u32(&o, duration); u16(&o, 0x55C4); u16(&o, 0);
    end(&o, a);
    a = box(&o, "hdlr");
    u32(&o, 0); u32(&o, 0); bytes(&o, "vide", 4); zeros(&o, 12); bytes(&o, "VideoHandler", 13);
    end(&o, a);

    size_t minf = box(&o, "minf");
    a = box(&o, "vmhd"); u32(&o, 1); zeros(&o, 8); end(&o, a);
    size_t dinf = box(&o, "dinf");
    size_t dref = box(&o, "dref"); u32(&o, 0); u32(&o, 1);
    a = box(&o, "url "); u32(&o, 1); end(&o, a);
    end(&o, dref); end(&o, dinf);

    size_t stbl = box(&o, "stbl");
    size_t stsd = box(&o, "stsd"); u32(&o, 0); u32(&o, 1);
    size_t avc1 = box(&o, "avc1");
    zeros(&o, 6); u16(&o, 1); zeros(&o, 16); u16(&o, m->w); u16(&o, m->h);
    u32(&o, 0x00480000); u32(&o, 0x00480000); u32(&o, 0); u16(&o, 1); zeros(&o, 32); u16(&o, 0x0018); u16(&o, 0xFFFF);
    a = box(&o, "avcC");
    o.b[o.n++] = 1; o.b[o.n++] = m->sps[1]; o.b[o.n++] = m->sps[2]; o.b[o.n++] = m->sps[3];
    o.b[o.n++] = 0xFF;                                      /* 4-byte NAL lengths */
    o.b[o.n++] = 0xE1; u16(&o, m->sps_len); bytes(&o, m->sps, m->sps_len);
    o.b[o.n++] = 1;    u16(&o, m->pps_len); bytes(&o, m->pps, m->pps_len);
    end(&o, a); end(&o, avc1); end(&o, stsd);

    a = box(&o, "stts"); u32(&o, 0); u32(&o, 1); u32(&o, m->frames); u32(&o, delta); end(&o, a);
    a = box(&o, "stss"); u32(&o, 0); u32(&o, m->n_keys);
    for (uint32_t i = 0; i < m->n_keys; i++) u32(&o, m->keys[i]);
    end(&o, a);
    a = box(&o, "stsc"); u32(&o, 0); u32(&o, 1); u32(&o, 1); u32(&o, m->frames); u32(&o, 1); end(&o, a);
    a = box(&o, "stsz"); u32(&o, 0); u32(&o, 0); u32(&o, m->frames);
    for (uint32_t i = 0; i < m->frames; i++) u32(&o, m->sizes[i]);
    end(&o, a);
    a = box(&o, "stco"); u32(&o, 0); u32(&o, 1); u32(&o, FTYP_BYTES + MDAT_HDR_BYTES); end(&o, a);

    end(&o, stbl); end(&o, minf); end(&o, mdia); end(&o, trak); end(&o, moov);
    return o.n;
}

esp_err_t mp4_close(mp4_writer_t *m, uint32_t *duration_ms)
{
    if (!m->open) return ESP_ERR_INVALID_STATE;
    int64_t dur_us = esp_timer_get_time() - m->t_start_us;
    uint32_t fps_x1000 = dur_us > 0 ? (uint32_t)((uint64_t)m->frames * 1000000000ULL / (uint64_t)dur_us) : 15000;
    if (m->frames < 2 || fps_x1000 < 1000 || fps_x1000 > 60000) fps_x1000 = 15000;
    uint32_t delta = (uint32_t)((uint64_t)TIMESCALE * 1000 / fps_x1000);

    uint32_t mdat_size = sdw_tell(&m->sd) - FTYP_BYTES;
    esp_err_t ret = ESP_FAIL;
    size_t moov_max = 1024 + (size_t)(m->frames + m->n_keys) * 4;
    uint8_t *moov = heap_caps_malloc(moov_max, MALLOC_CAP_SPIRAM);
    if (moov && m->frames && m->sps_len >= 4 && m->pps_len) {
        size_t n = build_moov(m, moov, delta);
        ret = sdw_write(&m->sd, moov, n);
        if (ret == ESP_OK) {
            uint8_t start[FTYP_BYTES + MDAT_HDR_BYTES];
            ret = sdw_rewrite_start(&m->sd, start, build_start(start, mdat_size));
        }
    } else {
        ESP_LOGE(TAG, "nothing to index (%lu frames, sps %u, pps %u)", (unsigned long)m->frames, m->sps_len, m->pps_len);
    }
    heap_caps_free(moov);
    uint32_t end_pos = sdw_tell(&m->sd);
    esp_err_t cret = sdw_close(&m->sd);
    if (ret == ESP_OK) ret = cret;
    m->open = false;
    heap_caps_free(m->sizes); heap_caps_free(m->keys);
    m->sizes = m->keys = NULL;
    if (duration_ms) *duration_ms = (uint32_t)(dur_us / 1000);
    ESP_LOGI(TAG, "closed: %lu frames (%lu key), %.1f fps, %lu KB", (unsigned long)m->frames, (unsigned long)m->n_keys,
             fps_x1000 / 1000.0, (unsigned long)(end_pos / 1024));
    return ret;
}
