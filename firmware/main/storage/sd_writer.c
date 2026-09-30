#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sd_writer.h"

static const char *TAG = "sdw";

#define SDW_BUF_BYTES   (32 * 1024)

/* One buffer, allocated once and kept. Allocating it per file worked until the on-chip heap
 * became fragmented: plenty of memory free, but no 32 KB block, and saving failed. */
static uint8_t *s_buf;
static bool s_buf_busy;

esp_err_t sdw_prealloc(void)
{
    if (!s_buf) s_buf = heap_caps_aligned_alloc(64, SDW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return s_buf ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sdw_open(sd_writer_t *w, const char *path)
{
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    if (s_buf_busy || sdw_prealloc() != ESP_OK) return ESP_ERR_NO_MEM;     /* one file at a time */
    w->buf = s_buf;
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) {
        ESP_LOGE(TAG, "open %s: %s", path, strerror(errno));
        w->buf = NULL;
        return ESP_FAIL;
    }
    s_buf_busy = true;
    return ESP_OK;
}

static esp_err_t flush(sd_writer_t *w)
{
    if (w->fill == 0) return ESP_OK;
    ssize_t n = write(w->fd, w->buf, w->fill);
    if (n != (ssize_t)w->fill) {
        ESP_LOGE(TAG, "short write %d/%u: %s", (int)n, (unsigned)w->fill, strerror(errno));
        w->failed = true;
        return ESP_FAIL;
    }
    w->fill = 0;
    return ESP_OK;
}

esp_err_t sdw_write(sd_writer_t *w, const void *data, size_t len)
{
    if (w->fd < 0 || w->failed) return ESP_ERR_INVALID_STATE;
    const uint8_t *p = data;
    while (len) {
        size_t n = SDW_BUF_BYTES - w->fill;
        if (n > len) n = len;
        memcpy(w->buf + w->fill, p, n);
        w->fill += n;
        w->pos += n;
        p += n;
        len -= n;
        if (w->fill == SDW_BUF_BYTES && flush(w) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sdw_rewrite_start(sd_writer_t *w, const void *data, size_t len)
{
    if (w->fd < 0 || w->failed || len > SDW_BUF_BYTES) return ESP_ERR_INVALID_STATE;
    if (flush(w) != ESP_OK) return ESP_FAIL;
    if (lseek(w->fd, 0, SEEK_SET) != 0) return ESP_FAIL;
    memcpy(w->buf, data, len);
    if (write(w->fd, w->buf, len) != (ssize_t)len) {
        w->failed = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sdw_close(sd_writer_t *w)
{
    if (w->fd < 0) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = w->failed ? ESP_FAIL : flush(w);
    if (close(w->fd) != 0) ret = ESP_FAIL;
    w->fd = -1;
    w->buf = NULL;
    s_buf_busy = false;
    return ret;
}
