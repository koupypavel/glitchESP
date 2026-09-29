#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "fx.h"
#include "fx_parallel.h"

static const char *TAG = "fx_par";

typedef struct {
    /* effect job */
    const fx_desc_t *fx;
    const fx_frame_t *in;
    fx_frame_t *out;
    const float *params;
    fx_ctx_t ctx;            /* with y0/y1 of the worker's share */
    /* generic row job */
    fx_row_fn row_fn;
    void *row_arg;
    int row_y0, row_y1;
    fx_frame_t row_out;
} job_t;

static struct {
    TaskHandle_t worker;
    SemaphoreHandle_t start;
    SemaphoreHandle_t done;
    job_t job;
} s_w;

/* Cache housekeeping for rows written by another core (write-back there, invalidate here). */
static void rows_writeback(const fx_frame_t *f, int y0, int y1)
{
    uint8_t *p = (uint8_t *)(f->px + (size_t)y0 * f->stride_px);
    size_t len = (size_t)(y1 - y0) * f->stride_px * 2;
    esp_cache_msync(p, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void rows_invalidate(const fx_frame_t *f, int y0, int y1)
{
    /* M2C requires cache-line alignment: shrink the range inward to whole lines (the row
     * boundary at y1 is line-aligned for all frame sizes we use, so nothing is lost). */
    uintptr_t start = (uintptr_t)(f->px + (size_t)y0 * f->stride_px);
    uintptr_t end = (uintptr_t)(f->px + (size_t)y1 * f->stride_px);
    start = (start + 127) & ~(uintptr_t)127;
    end &= ~(uintptr_t)127;
    if (end > start) {
        esp_cache_msync((void *)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_w.start, portMAX_DELAY);
        job_t *j = &s_w.job;
        if (j->row_fn) {
            j->row_fn(j->row_arg, j->row_y0, j->row_y1);
            rows_writeback(&j->row_out, j->row_y0, j->row_y1);
        } else {
            j->fx->apply(j->in, j->out, j->params, &j->ctx);
            rows_writeback(j->out, j->ctx.y0, j->ctx.y1);
        }
        xSemaphoreGive(s_w.done);
    }
}

/* Engine hook: split rows, run the top half on the worker, the bottom half here, join. */
static void run_split(const fx_desc_t *fx, const fx_frame_t *in, fx_frame_t *out,
                      const float *params, const fx_ctx_t *ctx)
{
    int y0 = ctx->y0, y1 = ctx->y1;
    int mid = y0 + (y1 - y0) / 2;

    /* the worker may read input rows this core wrote (chains): make sure they are in memory */
    rows_writeback(in, 0, in->h);

    s_w.job.row_fn = NULL;
    s_w.job.fx = fx;
    s_w.job.in = in;
    s_w.job.out = out;
    s_w.job.params = params;
    s_w.job.ctx = *ctx;
    s_w.job.ctx.y0 = (uint16_t)y0;
    s_w.job.ctx.y1 = (uint16_t)mid;
    xSemaphoreGive(s_w.start);

    fx_ctx_t mine = *ctx;
    mine.y0 = (uint16_t)mid;
    mine.y1 = (uint16_t)y1;
    fx->apply(in, out, params, &mine);

    xSemaphoreTake(s_w.done, portMAX_DELAY);
    /* drop any stale copy this core holds of the rows the worker produced */
    rows_invalidate(out, y0, mid);
}

void fx_parallel_rows(fx_row_fn fn, void *arg, int y0, int y1, uint16_t *out, uint32_t out_stride_px)
{
    if (!s_w.worker) { fn(arg, y0, y1); return; }
    int mid = y0 + (y1 - y0) / 2;
    s_w.job.row_fn = fn;
    s_w.job.row_arg = arg;
    s_w.job.row_y0 = y0;
    s_w.job.row_y1 = mid;
    s_w.job.row_out.px = out;
    s_w.job.row_out.stride_px = out_stride_px;
    xSemaphoreGive(s_w.start);
    fn(arg, mid, y1);
    xSemaphoreTake(s_w.done, portMAX_DELAY);
    fx_frame_t o = { out, 0, 0, out_stride_px };
    rows_invalidate(&o, y0, mid);
}

esp_err_t fx_parallel_init(void)
{
    s_w.start = xSemaphoreCreateBinary();
    s_w.done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_w.start && s_w.done, ESP_ERR_NO_MEM, TAG, "semaphores");
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "fx_worker", 16 * 1024, NULL, 12, &s_w.worker, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "worker task");
    fx_set_parallel_runner(run_split);
    ESP_LOGI(TAG, "row-parallel effects: worker on core 0");
    return ESP_OK;
}
