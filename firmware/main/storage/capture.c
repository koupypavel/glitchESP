#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include "capture.h"
#include "frame_pipeline.h"
#include "settings.h"

static const char *TAG = "capture";

#define NVS_NS          "glitch"
#define NVS_KEY_COUNT   "shot_count"
#define JPEG_OUT_MAX    (2 * 1024 * 1024)

typedef struct {
    bool sd_ok;
    capture_done_cb_t done_cb;
    void *user;

    jpeg_encoder_handle_t enc;
    uint8_t *raw_buf;   size_t raw_len;    /* RGB565 snapshot, encoder-aligned */
    uint8_t *jpg_buf;   size_t jpg_len;

    SemaphoreHandle_t frame_ready;
    volatile bool busy;
    bool serial_dump;      /* no SD: print the JPEG as base64 so it can be checked on a PC */
    uint32_t count;
    uint32_t seq;
    fp_recipe_t recipe;    /* recipe of the frame being saved */
} capture_t;

static capture_t s_c;

static uint32_t nvs_load_count(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_COUNT, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_store_count(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_COUNT, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Runs in the camera task: the snapshot is complete, wake the encoder task. */
static void on_snapshot(const fp_snapshot_t *snap, void *user)
{
    (void)user;
    s_c.seq = snap->seq;
    s_c.recipe = snap->recipe;
    xSemaphoreGive(s_c.frame_ready);
}

static void write_sidecar(const char *jpg_path, uint32_t jpeg_bytes, uint32_t seq)
{
    char path[64];
    strlcpy(path, jpg_path, sizeof(path));
    char *dot = strrchr(path, '.');
    if (dot) {
        strcpy(dot, ".json");
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "sidecar open failed: %s", strerror(errno));
        return;
    }
    const fp_recipe_t *r = &s_c.recipe;
    fprintf(f,
            "{\n"
            "  \"file\": \"%s\",\n"
            "  \"width\": %d,\n"
            "  \"height\": %d,\n"
            "  \"jpeg_bytes\": %lu,\n"
            "  \"jpeg_quality\": %d,\n"
            "  \"frame_seq\": %lu,\n"
            "  \"uptime_ms\": %lld,\n"
            "  \"camera\": { \"sensor\": \"OV5647\", \"mode\": \"RAW8_800x1280_50fps\", \"crop\": \"center 720x1280\" },\n"
            "  \"amount\": %.3f,\n"
            "  \"seed\": %lu,\n"
            "  \"frame_no\": %lu,\n"
            "  \"effects\": [",
            strrchr(jpg_path, '/') ? strrchr(jpg_path, '/') + 1 : jpg_path,
            FP_OUT_W, FP_OUT_H, (unsigned long)jpeg_bytes, settings_get()->jpeg_quality,
            (unsigned long)seq, (long long)(esp_timer_get_time() / 1000),
            (double)r->amount, (unsigned long)r->seed, (unsigned long)r->frame_no);
    int written = 0;
    for (int i = 0; i < r->chain.count; i++) {
        const fx_slot_t *slot = &r->chain.slots[i];
        if (!slot->enabled || !slot->fx) continue;
        fprintf(f, "%s\n    { \"id\": \"%s\", \"params\": {", written ? "," : "", slot->fx->id);
        for (int k = 0; k < slot->fx->n_params; k++) {
            fprintf(f, "%s \"%s\": %.3f", k ? "," : "", slot->fx->params[k].id, (double)slot->params[k]);
        }
        fprintf(f, " } }");
        written++;
    }
    fprintf(f, "%s]\n}\n", written ? "\n  " : "");
    fclose(f);
}

static void capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_c.frame_ready, portMAX_DELAY);

        capture_result_t res = { 0 };
        int64_t t0 = esp_timer_get_time();

        jpeg_encode_cfg_t cfg = {
            .width = FP_OUT_W,
            .height = FP_OUT_H,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = settings_get()->jpeg_quality,
            .pixel_reverse = false,
        };
        uint32_t out_size = 0;
        esp_err_t ret = jpeg_encoder_process(s_c.enc, &cfg, s_c.raw_buf, FP_OUT_BYTES,
                                             s_c.jpg_buf, s_c.jpg_len, &out_size);
        int64_t t1 = esp_timer_get_time();
        res.encode_ms = (uint32_t)((t1 - t0) / 1000);

        if (ret != ESP_OK) {
            res.error = "JPEG encode failed";
            ESP_LOGE(TAG, "%s: %s", res.error, esp_err_to_name(ret));
        } else if (!s_c.sd_ok) {
            res.error = "no SD card";
            ESP_LOGW(TAG, "encoded %lu bytes but no SD card mounted", (unsigned long)out_size);
            if (s_c.serial_dump) {
                /* 57 raw bytes -> 76 base64 chars per line */
                unsigned char line[80];
                printf("JPEG_B64_BEGIN %lu\n", (unsigned long)out_size);
                for (uint32_t off = 0; off < out_size; off += 57) {
                    size_t n = out_size - off < 57 ? out_size - off : 57, olen = 0;
                    mbedtls_base64_encode(line, sizeof(line), &olen, s_c.jpg_buf + off, n);
                    line[olen] = 0;
                    printf("%s\n", line);
                    if ((off / 57) % 64 == 0) vTaskDelay(1);   /* let the UART drain */
                }
                printf("JPEG_B64_END\n");
            }
        } else {
            uint32_t n = s_c.count + 1;
            snprintf(res.path, sizeof(res.path), CAPTURE_DIR "/IMG_%04lu.jpg", (unsigned long)n);
            FILE *f = fopen(res.path, "wb");
            if (!f) {
                res.error = "file open failed";
                ESP_LOGE(TAG, "%s (%s): %s", res.error, res.path, strerror(errno));
            } else {
                size_t w = fwrite(s_c.jpg_buf, 1, out_size, f);
                fclose(f);
                if (w != out_size) {
                    res.error = "short write";
                    ESP_LOGE(TAG, "%s: %u/%lu", res.error, (unsigned)w, (unsigned long)out_size);
                } else {
                    write_sidecar(res.path, out_size, s_c.seq);
                    s_c.count = n;
                    nvs_store_count(n);
                    res.ok = true;
                    res.jpeg_bytes = out_size;
                }
            }
        }
        res.write_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);

        if (res.ok) {
            ESP_LOGI(TAG, "saved %s (%lu bytes, encode %lu ms, write %lu ms)", res.path,
                     (unsigned long)res.jpeg_bytes, (unsigned long)res.encode_ms, (unsigned long)res.write_ms);
        }
        s_c.busy = false;
        if (s_c.done_cb) {
            s_c.done_cb(&res, s_c.user);
        }
    }
}

esp_err_t capture_init(bool sd_mounted, capture_done_cb_t done_cb, void *user)
{
    s_c.sd_ok = sd_mounted;
    s_c.done_cb = done_cb;
    s_c.user = user;
    s_c.count = nvs_load_count();

    if (s_c.sd_ok) {
        struct stat st;
        if (stat(CAPTURE_DIR, &st) != 0) {
            if (mkdir(CAPTURE_DIR, 0775) != 0) {
                ESP_LOGE(TAG, "mkdir %s failed: %s", CAPTURE_DIR, strerror(errno));
                s_c.sd_ok = false;
            }
        }
    }

    jpeg_encode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 2000 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_c.enc), TAG, "encoder engine");

    jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_c.raw_buf = jpeg_alloc_encoder_mem(FP_OUT_BYTES, &in_cfg, &s_c.raw_len);
    s_c.jpg_buf = jpeg_alloc_encoder_mem(JPEG_OUT_MAX, &out_cfg, &s_c.jpg_len);
    ESP_RETURN_ON_FALSE(s_c.raw_buf && s_c.jpg_buf, ESP_ERR_NO_MEM, TAG, "encoder buffers");

    s_c.frame_ready = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_c.frame_ready, ESP_ERR_NO_MEM, TAG, "semaphore");

    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "capture", 6 * 1024, NULL, 3, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");

    ESP_LOGI(TAG, "ready (sd=%d, shots so far=%lu, raw %u B, jpg %u B)", s_c.sd_ok,
             (unsigned long)s_c.count, (unsigned)s_c.raw_len, (unsigned)s_c.jpg_len);
    return ESP_OK;
}

esp_err_t capture_trigger(void)
{
    if (s_c.busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_c.busy = true;
    esp_err_t ret = frame_pipeline_request_capture(s_c.raw_buf, s_c.raw_len, on_snapshot, NULL);
    if (ret != ESP_OK) {
        s_c.busy = false;
    }
    return ret;
}

void capture_set_serial_dump(bool enable)
{
    s_c.serial_dump = enable;
}

bool capture_sd_available(void)
{
    return s_c.sd_ok;
}

uint32_t capture_get_count(void)
{
    return s_c.count;
}
