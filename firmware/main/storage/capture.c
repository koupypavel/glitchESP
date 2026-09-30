#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include "capture.h"
#include "avi_writer.h"
#include "sd_writer.h"
#include "frame_pipeline.h"
#include "display.h"
#include "settings.h"
#include "cam_ctrl.h"
#include "sd_card.h"

static const char *TAG = "capture";

#define NVS_NS          "glitch"
#define NVS_KEY_COUNT   "shot_count"
#define NVS_KEY_VCOUNT  "vid_count"
#define JPEG_OUT_MAX    (2 * 1024 * 1024)

typedef enum { JOB_STILL, JOB_VIDEO_FRAME, JOB_VIDEO_STOP, JOB_SD_CHECK } job_type_t;
typedef struct {
    job_type_t type;
    int fb_idx;
    uint32_t seq;
} job_t;

typedef struct {
    bool sd_ok;
    capture_done_cb_t done_cb;
    void *user;

    jpeg_encoder_handle_t enc;
    uint8_t *raw_buf;   size_t raw_len;    /* RGB565 snapshot, encoder-aligned */
    uint8_t *jpg_buf;   size_t jpg_len;

    QueueHandle_t jobs;
    volatile bool busy;                    /* a still is in flight */
    bool serial_dump;
    volatile bool dump_once;               /* next still goes to the serial port, not the card */
    uint32_t count;
    uint32_t seq;
    fp_recipe_t recipe;

    /* video */
    volatile bool rec_active;
    volatile bool rec_encoding;            /* a frame job is queued or being encoded */
    bool rec_failed;                       /* a video write failed: the card is probably gone */
    bool ejected;                          /* unmounted on request: do not re-mount by ourselves */

    /* burst: several stills from one press, each with a new random seed */
    int burst_left;                        /* shots still to take after the current one */
    int burst_saved;
    bool burst_rerolled;
    uint32_t burst_seed0;                  /* seed to go back to afterwards */
    capture_shot_cb_t shot_cb;
    avi_writer_t avi;
    video_done_cb_t vdone_cb;
    void *vuser;
    uint32_t vcount;
    uint32_t vframes, vdropped;
    int64_t v_t0;
    char vpath[64];
} capture_t;

static capture_t s_c;

static uint32_t nvs_load_u32(const char *key)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_store_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void write_recipe_json(FILE *f, const char *file, uint32_t w, uint32_t h, uint32_t jpeg_bytes, int quality,
                              uint32_t seq, const fp_recipe_t *r, const char *extra)
{
    fprintf(f,
            "{\n"
            "  \"file\": \"%s\",\n"
            "  \"width\": %lu,\n"
            "  \"height\": %lu,\n"
            "  \"jpeg_bytes\": %lu,\n"
            "  \"jpeg_quality\": %d,\n"
            "  \"frame_seq\": %lu,\n"
            "  \"uptime_ms\": %lld,\n"
            "  \"camera\": { \"sensor\": \"OV5647\", \"mode\": \"%s\" },\n"
            "%s"
            "  \"zoom\": %.2f,\n"
            "  \"amount\": %.3f,\n"
            "  \"seed\": %lu,\n"
            "  \"frame_no\": %lu,\n"
            "  \"effects\": [",
            file, (unsigned long)w, (unsigned long)h, (unsigned long)jpeg_bytes, quality, (unsigned long)seq,
            (long long)(esp_timer_get_time() / 1000), cam_ctrl_mode_name(), extra ? extra : "", (double)r->zoom,
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
}

static void write_sidecar(const char *media_path, uint32_t w, uint32_t h, uint32_t bytes, int quality, uint32_t seq,
                          const char *extra)
{
    char path[64];
    strlcpy(path, media_path, sizeof(path));
    char *dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".json");
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "sidecar open failed: %s", strerror(errno));
        return;
    }
    const char *name = strrchr(media_path, '/') ? strrchr(media_path, '/') + 1 : media_path;
    write_recipe_json(f, name, w, h, bytes, quality, seq, &s_c.recipe, extra);
    fclose(f);
}

static esp_err_t encode_wh(const uint8_t *rgb565, uint32_t w, uint32_t h, int quality, uint32_t *out_size)
{
    jpeg_encode_cfg_t cfg = {
        .width = w,
        .height = h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = quality,
        .pixel_reverse = false,
    };
    return jpeg_encoder_process(s_c.enc, &cfg, rgb565, w * h * 2, s_c.jpg_buf, s_c.jpg_len, out_size);
}

static esp_err_t encode(const uint8_t *rgb565, int quality, uint32_t *out_size)
{
    return encode_wh(rgb565, FP_OUT_W, FP_OUT_H, quality, out_size);
}

/* The card may have been put in after boot: try to mount it when something wants to save
 * (at most every few seconds, a failed attempt takes a while). */
static bool sd_ensure(void)
{
    static int64_t s_last_try;
    if (s_c.sd_ok) return true;
    int64_t now = esp_timer_get_time();
    if (s_last_try && now - s_last_try < 3000000) return false;
    s_last_try = now;
    if (sd_card_mount() != ESP_OK) return false;
    struct stat st;
    if (stat(CAPTURE_DIR, &st) != 0 && mkdir(CAPTURE_DIR, 0775) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", CAPTURE_DIR, strerror(errno));
        sd_card_unmount();
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted after boot");
    s_c.sd_ok = true;
    s_c.ejected = false;
    return true;
}

/* A write failed: most likely the card was pulled. Let go of it so it can be mounted again. */
static void sd_lost(void)
{
    if (!s_c.sd_ok) return;
    ESP_LOGW(TAG, "SD card write failed: unmounting");
    s_c.sd_ok = false;
    sd_card_unmount();
}

bool capture_sd_ensure(void)
{
    return sd_ensure();
}

void capture_sd_poll(void)
{
    if (s_c.sd_ok || s_c.ejected || s_c.busy || s_c.rec_active || !s_c.jobs) return;
    job_t j = { .type = JOB_SD_CHECK };
    xQueueSend(s_c.jobs, &j, 0);
}

void capture_sd_eject(void)
{
    if (s_c.busy || s_c.rec_active) return;
    s_c.sd_ok = false;
    s_c.ejected = true;
    sd_card_unmount();
    ESP_LOGI(TAG, "SD card released");
}

/* Write the encoded JPEG in jpg_buf as the next IMG_nnnn.jpg plus its sidecar. */
static void store_still(capture_result_t *res, uint32_t out_size, uint32_t w, uint32_t h, int quality)
{
    uint32_t n = s_c.count + 1;
    snprintf(res->path, sizeof(res->path), CAPTURE_DIR "/IMG_%04lu.jpg", (unsigned long)n);
    sd_writer_t wr;
    if (sdw_open(&wr, res->path) != ESP_OK) {
        res->error = "file open failed";
        sd_lost();
        return;
    }
    esp_err_t e1 = sdw_write(&wr, s_c.jpg_buf, out_size);
    esp_err_t e2 = sdw_close(&wr);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        res->error = "write failed";
        ESP_LOGE(TAG, "%s: %s", res->error, res->path);
        sd_lost();
        return;
    }
    write_sidecar(res->path, w, h, out_size, quality, s_c.seq, NULL);
    s_c.count = n;
    nvs_store_u32(NVS_KEY_COUNT, n);
    res->ok = true;
    res->jpeg_bytes = out_size;
}

/* Runs in the camera task: the snapshot is complete, queue the still job. */
static void on_snapshot(const fp_snapshot_t *snap, void *user)
{
    (void)user;
    s_c.seq = snap->seq;
    s_c.recipe = snap->recipe;
    job_t j = { .type = JOB_STILL };
    xQueueSend(s_c.jobs, &j, 0);
}

/* Print a JPEG as base64 lines ("J:" prefix so log lines in between can be told apart). */
static void dump_jpeg_serial(uint32_t out_size)
{
    unsigned char line[80];
    printf("JPEG_B64_BEGIN %lu\n", (unsigned long)out_size);
    for (uint32_t off = 0; off < out_size; off += 57) {
        size_t n = out_size - off < 57 ? out_size - off : 57, olen = 0;
        mbedtls_base64_encode(line, sizeof(line), &olen, s_c.jpg_buf + off, n);
        line[olen] = 0;
        printf("J:%s\n", line);
        if ((off / 57) % 64 == 0) vTaskDelay(1);
    }
    printf("JPEG_B64_END\n");
}

static void do_still(void)
{
    capture_result_t res = { 0 };
    int64_t t0 = esp_timer_get_time();
    uint32_t out_size = 0;
    bool dump = s_c.dump_once;
    s_c.dump_once = false;
    int quality = dump ? CAPTURE_DUMP_QUALITY : settings_get()->jpeg_quality;
    esp_err_t ret = encode(s_c.raw_buf, quality, &out_size);
    int64_t t1 = esp_timer_get_time();
    res.encode_ms = (uint32_t)((t1 - t0) / 1000);

    if (ret != ESP_OK) {
        res.error = "JPEG encode failed";
        ESP_LOGE(TAG, "%s: %s", res.error, esp_err_to_name(ret));
    } else if (dump) {
        res.error = "sent over serial";
        dump_jpeg_serial(out_size);
    } else if (!sd_ensure()) {
        res.error = "no SD card";
        ESP_LOGW(TAG, "encoded %lu bytes but no SD card mounted", (unsigned long)out_size);
        if (s_c.serial_dump) dump_jpeg_serial(out_size);
    } else {
        store_still(&res, out_size, FP_OUT_W, FP_OUT_H, quality);
    }
    res.write_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
    if (res.ok) {
        ESP_LOGI(TAG, "saved %s (%lu bytes, encode %lu ms, write %lu ms)", res.path,
                 (unsigned long)res.jpeg_bytes, (unsigned long)res.encode_ms, (unsigned long)res.write_ms);
        s_c.burst_saved++;
    }

    /* burst: new seed, let a frame rendered with it come through, take the next shot */
    if (res.ok && s_c.burst_left > 0) {
        s_c.burst_left--;
        frame_pipeline_reroll();
        s_c.burst_rerolled = true;
        if (s_c.shot_cb) s_c.shot_cb();
        vTaskDelay(pdMS_TO_TICKS(120));
        if (frame_pipeline_request_capture(s_c.raw_buf, s_c.raw_len, on_snapshot, NULL) == ESP_OK) return;
    }
    if (s_c.burst_rerolled) frame_pipeline_set_seed(s_c.burst_seed0);     /* the preview goes back to its look */
    s_c.burst_rerolled = false;
    s_c.burst_left = 0;
    res.burst = (uint32_t)s_c.burst_saved;
    s_c.busy = false;
    if (s_c.done_cb) s_c.done_cb(&res, s_c.user);
}

static void do_video_frame(const job_t *j)
{
    if (s_c.rec_active && s_c.avi.open) {
        uint32_t out_size = 0;
        const uint8_t *fb = (const uint8_t *)display_fb(j->fb_idx);
        esp_err_t ret = encode(fb, VIDEO_JPEG_QUALITY, &out_size);
        if (ret == ESP_OK) {
            if (avi_write_frame(&s_c.avi, s_c.jpg_buf, out_size) == ESP_OK) {
                s_c.vframes++;
            } else {
                ESP_LOGE(TAG, "avi write failed, stopping");
                s_c.rec_active = false;
                s_c.rec_failed = true;
                job_t stop = { .type = JOB_VIDEO_STOP };     /* close the file and report */
                xQueueSend(s_c.jobs, &stop, 0);
            }
        } else {
            ESP_LOGE(TAG, "video encode failed: %s", esp_err_to_name(ret));
        }
    }
    display_release_fb(j->fb_idx);
    s_c.rec_encoding = false;
}

static void do_video_stop(void)
{
    video_result_t res = { 0 };
    strlcpy(res.path, s_c.vpath, sizeof(res.path));
    res.frames = s_c.vframes;
    res.dropped = s_c.vdropped;
    if (s_c.avi.open) {
        uint32_t dur = 0;
        esp_err_t ret = avi_close(&s_c.avi, &dur);
        res.duration_ms = dur;
        res.ok = ret == ESP_OK && s_c.vframes > 0;
        if (!res.ok) res.error = "avi finalize failed";
        if (res.ok) {
            char extra[64];
            snprintf(extra, sizeof(extra), "  \"frames\": %lu,\n  \"duration_ms\": %lu,\n",
                     (unsigned long)res.frames, (unsigned long)res.duration_ms);
            write_sidecar(s_c.vpath, FP_OUT_W, FP_OUT_H, 0, VIDEO_JPEG_QUALITY, s_c.seq, extra);
        }
    } else {
        res.error = "no file";
    }
    ESP_LOGI(TAG, "video %s: %lu frames, %lu ms, %lu dropped", res.ok ? "saved" : "failed",
             (unsigned long)res.frames, (unsigned long)res.duration_ms, (unsigned long)res.dropped);
    if (s_c.rec_failed) {
        s_c.rec_failed = false;
        sd_lost();
    }
    if (s_c.vdone_cb) s_c.vdone_cb(&res, s_c.vuser);
}

static void capture_task(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        xQueueReceive(s_c.jobs, &j, portMAX_DELAY);
        switch (j.type) {
        case JOB_STILL:       do_still(); break;
        case JOB_VIDEO_FRAME: do_video_frame(&j); break;
        case JOB_VIDEO_STOP:  do_video_stop(); break;
        case JOB_SD_CHECK:    sd_ensure(); break;
        }
    }
}

esp_err_t capture_init(bool sd_mounted, capture_done_cb_t done_cb, void *user)
{
    s_c.sd_ok = sd_mounted;
    s_c.done_cb = done_cb;
    s_c.user = user;
    s_c.count = nvs_load_u32(NVS_KEY_COUNT);
    s_c.vcount = nvs_load_u32(NVS_KEY_VCOUNT);

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

    s_c.jobs = xQueueCreate(4, sizeof(job_t));
    ESP_RETURN_ON_FALSE(s_c.jobs, ESP_ERR_NO_MEM, TAG, "queue");

    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "capture", 8 * 1024, NULL, 3, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");

    /* self-test: the video path encodes straight from a panel frame buffer */
    {
        uint32_t n = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t r = encode((const uint8_t *)display_fb(0), VIDEO_JPEG_QUALITY, &n);
        ESP_LOGI(TAG, "video encode self-test from frame buffer: %s, %lu bytes, %lld ms",
                 esp_err_to_name(r), (unsigned long)n, (long long)((esp_timer_get_time() - t0) / 1000));
    }

    ESP_LOGI(TAG, "ready (sd=%d, shots %lu, videos %lu, raw %u B, jpg %u B)", s_c.sd_ok,
             (unsigned long)s_c.count, (unsigned long)s_c.vcount, (unsigned)s_c.raw_len, (unsigned)s_c.jpg_len);
    return ESP_OK;
}

esp_err_t capture_trigger_burst(int shots)
{
    if (s_c.busy || s_c.rec_active) return ESP_ERR_INVALID_STATE;
    s_c.busy = true;
    fp_recipe_t r;
    frame_pipeline_get_recipe(&r);
    s_c.burst_seed0 = r.seed;
    s_c.burst_left = shots > 1 ? shots - 1 : 0;
    s_c.burst_saved = 0;
    s_c.burst_rerolled = false;
    esp_err_t ret = frame_pipeline_request_capture(s_c.raw_buf, s_c.raw_len, on_snapshot, NULL);
    if (ret != ESP_OK) s_c.busy = false;
    return ret;
}

esp_err_t capture_trigger(void)
{
    return capture_trigger_burst(1);
}

void capture_set_shot_cb(capture_shot_cb_t cb)
{
    s_c.shot_cb = cb;
}

void capture_notify_shot(void)
{
    if (s_c.shot_cb) s_c.shot_cb();
}

esp_err_t capture_begin_external(void)
{
    if (s_c.busy || s_c.rec_active) return ESP_ERR_INVALID_STATE;
    s_c.busy = true;
    s_c.burst_saved = 0;
    return ESP_OK;
}

bool capture_finish_external(const uint8_t *rgb565, uint32_t w, uint32_t h, const fp_recipe_t *recipe, bool dump,
                             bool more)
{
    capture_result_t res = { 0 };
    if (!rgb565) {
        res.error = "camera did not deliver";
    } else {
        int64_t t0 = esp_timer_get_time();
        uint32_t out_size = 0;
        int quality = dump ? CAPTURE_DUMP_QUALITY : settings_get()->jpeg_quality;
        s_c.recipe = *recipe;
        s_c.seq = recipe->frame_no;
        esp_err_t ret = encode_wh(rgb565, w, h, quality, &out_size);
        if (ret != ESP_OK && quality > 70) {                 /* most likely the output buffer: try smaller */
            quality = 70;
            ret = encode_wh(rgb565, w, h, quality, &out_size);
        }
        int64_t t1 = esp_timer_get_time();
        res.encode_ms = (uint32_t)((t1 - t0) / 1000);
        if (ret != ESP_OK) {
            res.error = "JPEG encode failed";
            ESP_LOGE(TAG, "%s: %s", res.error, esp_err_to_name(ret));
        } else if (dump) {
            res.error = "sent over serial";
            dump_jpeg_serial(out_size);
        } else if (!sd_ensure()) {
            res.error = "no SD card";
        } else {
            store_still(&res, out_size, w, h, quality);
        }
        res.write_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
        if (res.ok) {
            ESP_LOGI(TAG, "saved %s (%lux%lu, %lu bytes, encode %lu ms, write %lu ms)", res.path,
                     (unsigned long)w, (unsigned long)h, (unsigned long)res.jpeg_bytes,
                     (unsigned long)res.encode_ms, (unsigned long)res.write_ms);
            s_c.burst_saved++;
        }
    }
    if (more && res.ok) return true;            /* the caller has another shot coming */
    res.burst = (uint32_t)s_c.burst_saved;
    s_c.busy = false;
    if (s_c.done_cb) s_c.done_cb(&res, s_c.user);
    return false;
}

esp_err_t capture_trigger_dump(void)
{
    if (s_c.busy || s_c.rec_active) return ESP_ERR_INVALID_STATE;
    s_c.dump_once = true;
    esp_err_t ret = capture_trigger();
    if (ret != ESP_OK) s_c.dump_once = false;
    return ret;
}

esp_err_t capture_trigger_screenshot(void)
{
    if (s_c.busy || s_c.rec_active) return ESP_ERR_INVALID_STATE;
    frame_pipeline_capture_with_ui(true);
    esp_err_t ret = capture_trigger_dump();
    if (ret != ESP_OK) frame_pipeline_capture_with_ui(false);
    return ret;
}

/* ---- video ---- */

esp_err_t capture_video_start(video_done_cb_t done_cb, void *user)
{
    if (s_c.rec_active || s_c.busy) return ESP_ERR_INVALID_STATE;
    if (!sd_ensure()) return ESP_ERR_NOT_FOUND;
    uint32_t n = s_c.vcount + 1;
    snprintf(s_c.vpath, sizeof(s_c.vpath), CAPTURE_DIR "/VID_%04lu.avi", (unsigned long)n);
    ESP_RETURN_ON_ERROR(avi_open(&s_c.avi, s_c.vpath, FP_OUT_W, FP_OUT_H, VIDEO_MAX_FRAMES), TAG, "avi open");
    frame_pipeline_get_recipe(&s_c.recipe);
    s_c.vcount = n;
    nvs_store_u32(NVS_KEY_VCOUNT, n);
    s_c.vdone_cb = done_cb;
    s_c.vuser = user;
    s_c.vframes = 0;
    s_c.vdropped = 0;
    s_c.v_t0 = esp_timer_get_time();
    s_c.rec_encoding = false;
    s_c.rec_active = true;
    ESP_LOGI(TAG, "recording %s", s_c.vpath);
    return ESP_OK;
}

esp_err_t capture_video_stop(void)
{
    if (!s_c.rec_active) return ESP_ERR_INVALID_STATE;
    s_c.rec_active = false;
    job_t j = { .type = JOB_VIDEO_STOP };
    return xQueueSend(s_c.jobs, &j, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_FAIL;
}

bool capture_video_active(void)
{
    return s_c.rec_active;
}

uint32_t capture_video_elapsed_ms(void)
{
    return s_c.rec_active ? (uint32_t)((esp_timer_get_time() - s_c.v_t0) / 1000) : 0;
}

void capture_video_on_frame(int fb_idx, uint32_t seq)
{
    if (!s_c.rec_active) return;
    if (s_c.rec_encoding) { s_c.vdropped++; return; }        /* encoder still busy: skip */
    if (!display_hold_fb(fb_idx)) { s_c.vdropped++; return; }
    job_t j = { .type = JOB_VIDEO_FRAME, .fb_idx = fb_idx, .seq = seq };
    s_c.rec_encoding = true;
    if (xQueueSend(s_c.jobs, &j, 0) != pdTRUE) {
        display_release_fb(fb_idx);
        s_c.rec_encoding = false;
        s_c.vdropped++;
    }
}

void capture_set_serial_dump(bool enable)
{
    s_c.serial_dump = enable;
}

bool capture_busy(void)
{
    return s_c.busy;
}

bool capture_sd_available(void)
{
    return s_c.sd_ok;
}

uint32_t capture_get_count(void)
{
    return s_c.count;
}
