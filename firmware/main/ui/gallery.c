#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "gallery.h"
#include "ui_lvgl.h"
#include "ui_live.h"
#include "cam_ctrl.h"
#include "capture.h"
#include "frame_pipeline.h"
#include "fx.h"

static const char *TAG = "gallery";

/* How the borrowed camera block is used (all offsets are cache-line multiples). */
#define DEC_W_MAX       1088
#define DEC_H_MAX       1920
#define DEC_BYTES       ((size_t)DEC_W_MAX * DEC_H_MAX * 2)     /* decoded picture, RGB565 */
#define VIEW_BYTES      ((size_t)FP_OUT_BYTES)                  /* the picture scaled to the screen */
#define FILE_OFF        (DEC_BYTES + VIEW_BYTES)
#define FILE_MAX        ((size_t)3 * 1024 * 1024)               /* largest JPEG (or video frame) read */

#define MAX_ENTRIES     1024
#define NAME_LEN        20
#define REFRESH_MS      100         /* how often the still picture is redrawn so the buttons stay live */

#define COLOR_KEY_CLEAR   lv_color_hex(0x000008)   /* RGB565 0x0001: show the picture (see ui_lvgl.h) */
#define COLOR_KEY_DIM     lv_color_hex(0x000010)   /* RGB565 0x0002: darken the picture */

typedef enum { G_OPEN, G_CLOSE, G_NEXT, G_PREV, G_PLAY, G_LOOK, G_DELETE } gcmd_t;

typedef struct { char name[NAME_LEN]; } entry_t;

static struct {
    QueueHandle_t q;
    volatile bool active;
    jpeg_decoder_handle_t dec;

    uint8_t *block;                 /* borrowed from cam_ctrl while active */
    uint16_t *dec_buf, *view;
    uint8_t *file_buf;

    entry_t *entries;
    int count, index;

    /* what the idle refresh draws */
    const uint16_t *cur;
    uint32_t cur_stride, cur_w, cur_h;

    /* video being shown */
    int avi_fd;
    uint32_t avi_first, avi_pos, avi_end, frame_us;
    bool playing;
    int64_t next_due, play_t0;
    uint32_t play_frames;

    /* widgets */
    lv_obj_t *root, *info, *msg, *btn_play, *lbl_play, *btn_look, *btn_del, *lbl_del;
    lv_timer_t *del_timer;
    bool del_armed;
} s_g = { .avi_fd = -1 };

static void post(gcmd_t c)
{
    if (s_g.q) xQueueSend(s_g.q, &c, 0);
}

bool gallery_active(void)   { return s_g.active; }
void gallery_open(void)     { post(G_OPEN); }
void gallery_close(void)    { post(G_CLOSE); }
void gallery_next(void)     { post(G_NEXT); }
void gallery_prev(void)     { post(G_PREV); }
void gallery_play(void)     { post(G_PLAY); }
void gallery_use_look(void) { post(G_LOOK); }
void gallery_delete(void)   { post(G_DELETE); }

/* ---- files ---- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_video(const char *name) { return has_ext(name, ".avi"); }

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static void scan(void)
{
    s_g.count = 0;
    DIR *d = opendir(CAPTURE_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_g.count < MAX_ENTRIES) {
        const char *n = e->d_name;
        bool img = strncasecmp(n, "IMG_", 4) == 0 && has_ext(n, ".jpg");
        bool vid = strncasecmp(n, "VID_", 4) == 0 && has_ext(n, ".avi");
        if ((!img && !vid) || strlen(n) >= NAME_LEN) continue;
        strcpy(s_g.entries[s_g.count++].name, n);
    }
    closedir(d);
    qsort(s_g.entries, (size_t)s_g.count, sizeof(entry_t), by_name);     /* photos first, then videos */
}

static void path_of(int i, char *path, size_t len)
{
    snprintf(path, len, CAPTURE_DIR "/%s", s_g.entries[i].name);
}

static void sidecar_of(int i, char *path, size_t len)
{
    path_of(i, path, len);
    char *dot = strrchr(path, '.');
    if (dot && (size_t)(dot - path) + 6 <= len) strcpy(dot, ".json");
}

/* Read up to `max` bytes at `offset`; returns the number read, or -1. */
static long read_at(int fd, uint32_t offset, uint8_t *dst, size_t max)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return -1;
    size_t got = 0;
    while (got < max) {
        size_t want = max - got < 65536 ? max - got : 65536;
        ssize_t n = read(fd, dst + got, want);
        if (n < 0) return -1;
        if (n == 0) break;
        got += (size_t)n;
    }
    return (long)got;
}

/* ---- recipe from a sidecar ---- */

/* The sidecars are written by capture.c: flat JSON with known keys, so plain string search
 * is enough. Fills chain, amount and seed; `text` gets a short description. */
static bool sidecar_recipe(int i, fp_recipe_t *r, char *text, size_t text_len)
{
    char path[64], json[2048];
    sidecar_of(i, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = 0;

    memset(r, 0, sizeof(*r));
    r->zoom = 1.0f;
    const char *p = strstr(json, "\"amount\":");
    r->amount = p ? strtof(p + 9, NULL) : 0.5f;
    p = strstr(json, "\"seed\":");
    r->seed = p ? (uint32_t)strtoul(p + 7, NULL, 10) : 1;

    fx_chain_clear(&r->chain);
    if (text_len) text[0] = 0;
    p = strstr(json, "\"effects\"");
    while (p && (p = strstr(p, "\"id\": \"")) != NULL) {
        p += 7;
        char id[16] = { 0 };
        size_t k = 0;
        while (*p && *p != '"' && k < sizeof(id) - 1) id[k++] = *p++;
        const char *end = strstr(p, "\"id\": \"");       /* this effect's parameters stop here */
        const fx_desc_t *fx = fx_registry_find(id);
        if (!fx) continue;
        int slot = fx_chain_add(&r->chain, fx);
        if (slot < 0) break;
        for (int q = 0; q < fx->n_params; q++) {
            char key[24];
            snprintf(key, sizeof(key), "\"%s\":", fx->params[q].id);
            const char *v = strstr(p, key);
            if (v && (!end || v < end)) r->chain.slots[slot].params[q] = strtof(v + strlen(key), NULL);
        }
        if (text_len) {
            if (text[0]) strlcat(text, " + ", text_len);
            strlcat(text, fx->name, text_len);
        }
    }
    if (text_len) {
        char pct[12];
        if (!text[0]) strlcat(text, "no effects", text_len);
        snprintf(pct, sizeof(pct), "  %d%%", (int)(r->amount * 100.0f + 0.5f));
        strlcat(text, pct, text_len);
    }
    return true;
}

/* ---- UI (every function here takes the LVGL lock itself unless it is a callback) ---- */

static void ui_set(const char *info, const char *msg, bool video, bool has_look)
{
    if (!ui_lvgl_lock(300)) return;
    lv_label_set_text(s_g.info, info);
    if (msg) {
        lv_label_set_text(s_g.msg, msg);
        lv_obj_remove_flag(s_g.msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_g.msg, LV_OBJ_FLAG_HIDDEN);
    }
    if (video) lv_obj_remove_flag(s_g.btn_play, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(s_g.btn_play, LV_OBJ_FLAG_HIDDEN);
    if (has_look) lv_obj_remove_flag(s_g.btn_look, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(s_g.btn_look, LV_OBJ_FLAG_HIDDEN);
    if (s_g.count > 0) lv_obj_remove_flag(s_g.btn_del, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(s_g.btn_del, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_g.lbl_play, LV_SYMBOL_PLAY);
    ui_lvgl_unlock();
}

static void ui_play_icon(bool playing)
{
    if (!ui_lvgl_lock(100)) return;
    lv_label_set_text(s_g.lbl_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    ui_lvgl_unlock();
}

/* ---- decoding and showing ---- */

static void show_black(void)
{
    memset(s_g.view, 0, VIEW_BYTES);
    s_g.cur = s_g.view;
    s_g.cur_stride = s_g.cur_w = FP_OUT_W;
    s_g.cur_h = FP_OUT_H;
}

/* JPEG in file_buf -> dec_buf. Returns NULL on success, or a message for the screen. */
static const char *decode(size_t len, uint32_t *w, uint32_t *h, uint32_t *stride)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(s_g.file_buf, (uint32_t)len, &info) != ESP_OK) return "not a JPEG this camera can read";
    uint32_t w16 = (info.width + 15) & ~15u, h16 = (info.height + 15) & ~15u;
    if ((size_t)w16 * h16 * 2 > DEC_BYTES || w16 < 32 || h16 < 32) return "picture size not supported";
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out = 0;
    /* the decoder reads the file from memory, not from this core's cache */
    esp_cache_msync(s_g.file_buf, (len + 127) & ~(size_t)127, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_err_t ret = jpeg_decoder_process(s_g.dec, &cfg, s_g.file_buf, (uint32_t)len, (uint8_t *)s_g.dec_buf,
                                         (uint32_t)DEC_BYTES, &out);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "decode failed: %s", esp_err_to_name(ret));
        return "cannot decode this picture";
    }
    *w = info.width;
    *h = info.height;
    *stride = w16;
    return NULL;
}

static void close_video(void)
{
    if (s_g.avi_fd >= 0) close(s_g.avi_fd);
    s_g.avi_fd = -1;
    s_g.playing = false;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Read and decode the video frame at avi_pos, advance. False at the end or on an error. */
static bool video_frame(uint32_t *w, uint32_t *h, uint32_t *stride)
{
    uint8_t hdr[8];
    while (s_g.avi_pos + 8 <= s_g.avi_end) {
        if (read_at(s_g.avi_fd, s_g.avi_pos, hdr, 8) != 8) return false;
        uint32_t size = le32(hdr + 4);
        uint32_t data = s_g.avi_pos + 8;
        s_g.avi_pos = data + size + (size & 1);
        if (memcmp(hdr + 2, "dc", 2) != 0 && memcmp(hdr + 2, "db", 2) != 0) continue;    /* not a video chunk */
        if (size == 0 || size > FILE_MAX) return false;
        if (read_at(s_g.avi_fd, data, s_g.file_buf, size) != (long)size) return false;
        return decode(size, w, h, stride) == NULL;
    }
    return false;
}

/* Open the AVI and find the frames. Returns NULL on success. */
static const char *open_video(const char *path)
{
    s_g.avi_fd = open(path, O_RDONLY);
    if (s_g.avi_fd < 0) return "cannot open the file";
    uint8_t *h = s_g.file_buf;
    long n = read_at(s_g.avi_fd, 0, h, 4096);
    if (n < 64 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) return "not an AVI file";
    s_g.frame_us = 100000;
    s_g.avi_first = 0;
    for (long i = 12; i + 12 <= n; i++) {
        if (memcmp(h + i, "avih", 4) == 0) s_g.frame_us = le32(h + i + 8);
        if (memcmp(h + i, "movi", 4) == 0 && memcmp(h + i - 8, "LIST", 4) == 0) {
            s_g.avi_first = (uint32_t)i + 4;
            s_g.avi_end = (uint32_t)i + le32(h + i - 4);
            break;
        }
    }
    if (!s_g.avi_first) return "no video data found";
    if (s_g.frame_us < 20000 || s_g.frame_us > 1000000) s_g.frame_us = 100000;
    s_g.avi_pos = s_g.avi_first;
    return NULL;
}

static void show(int i)
{
    close_video();
    if (s_g.count == 0) {
        show_black();
        ui_set("", capture_sd_available() ? "No photos yet" : "No SD card", false, false);
        return;
    }
    if (i < 0) i = s_g.count - 1;
    if (i >= s_g.count) i = 0;
    s_g.index = i;

    char path[64], look[96] = "", info[200];
    path_of(i, path, sizeof(path));
    struct stat st;
    long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    bool video = is_video(s_g.entries[i].name);
    fp_recipe_t recipe;
    bool has_look = sidecar_recipe(i, &recipe, look, sizeof(look));

    uint32_t w = 0, h = 0, stride = 0;
    const char *err = NULL;
    int64_t t0 = esp_timer_get_time();
    if (video) {
        err = open_video(path);
        if (!err && !video_frame(&w, &h, &stride)) err = "cannot read the first frame";
    } else if (size <= 0 || (size_t)size > FILE_MAX) {
        err = "file too large";
    } else {
        int fd = open(path, O_RDONLY);
        long n = fd >= 0 ? read_at(fd, 0, s_g.file_buf, (size_t)size) : -1;
        if (fd >= 0) close(fd);
        err = n == size ? decode((size_t)size, &w, &h, &stride) : "cannot read the file";
    }

    if (err) {
        show_black();
        snprintf(info, sizeof(info), "%d / %d   %s", i + 1, s_g.count, s_g.entries[i].name);
    } else {
        if (video) {                    /* frames are drawn straight from the decoder's buffer */
            s_g.cur = s_g.dec_buf;
            s_g.cur_stride = stride; s_g.cur_w = w; s_g.cur_h = h;
        } else {                        /* stills are scaled once and then only copied */
            frame_pipeline_fit(s_g.dec_buf, stride, w, h, s_g.view);
            s_g.cur = s_g.view;
            s_g.cur_stride = s_g.cur_w = FP_OUT_W;
            s_g.cur_h = FP_OUT_H;
        }
        snprintf(info, sizeof(info), "%d / %d   %s   %lux%lu   %ld KB\n%s", i + 1, s_g.count, s_g.entries[i].name,
                 (unsigned long)w, (unsigned long)h, size / 1024, look);
        ESP_LOGI(TAG, "%s: %lux%lu, %ld KB, shown in %lld ms", s_g.entries[i].name, (unsigned long)w, (unsigned long)h,
                 size / 1024, (long long)((esp_timer_get_time() - t0) / 1000));
    }
    ui_set(info, err, video && !err, has_look);
}

static void redraw(void)
{
    if (s_g.cur) frame_pipeline_present(s_g.cur, s_g.cur_stride, s_g.cur_w, s_g.cur_h);
}

/* ---- commands ---- */

static void do_open(void)
{
    if (s_g.active) return;
    uint8_t *block;
    size_t len;
    esp_err_t ret = cam_ctrl_pause(&block, &len);
    if (ret != ESP_OK || len < FILE_OFF + FILE_MAX) {
        ESP_LOGW(TAG, "cannot open now: %s", esp_err_to_name(ret));
        if (ret == ESP_OK) cam_ctrl_resume();
        return;
    }
    if (!s_g.dec) {
        jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 2000 };
        if (jpeg_new_decoder_engine(&eng, &s_g.dec) != ESP_OK) s_g.dec = NULL;
    }
    s_g.entries = heap_caps_malloc(MAX_ENTRIES * sizeof(entry_t), MALLOC_CAP_SPIRAM);
    if (!s_g.dec || !s_g.entries) {
        ESP_LOGE(TAG, "no decoder or no memory");
        heap_caps_free(s_g.entries);
        s_g.entries = NULL;
        cam_ctrl_resume();
        return;
    }
    s_g.block = block;
    s_g.dec_buf = (uint16_t *)block;
    s_g.view = (uint16_t *)(block + DEC_BYTES);
    s_g.file_buf = block + FILE_OFF;
    s_g.active = true;

    if (ui_lvgl_lock(500)) {
        ui_live_set_visible(false);
        lv_obj_remove_flag(s_g.root, LV_OBJ_FLAG_HIDDEN);
        ui_lvgl_unlock();
    }
    capture_sd_ensure();                                /* a card put in after boot */
    scan();
    /* start at the newest photo (videos sort after the photos) */
    int start = s_g.count - 1;
    while (start > 0 && is_video(s_g.entries[start].name)) start--;
    show(start);
    redraw();
    ESP_LOGI(TAG, "opened: %d file(s)", s_g.count);
}

static void do_close(const fp_recipe_t *look)
{
    if (!s_g.active) return;
    close_video();
    s_g.cur = NULL;
    heap_caps_free(s_g.entries);
    s_g.entries = NULL;
    s_g.count = 0;
    if (ui_lvgl_lock(500)) {
        lv_obj_add_flag(s_g.root, LV_OBJ_FLAG_HIDDEN);
        ui_live_set_visible(true);
        if (look) {
            ui_live_apply_recipe(look);
            ui_live_toast("look applied", 1500);
        }
        ui_lvgl_unlock();
    }
    s_g.active = false;
    cam_ctrl_resume();
    ESP_LOGI(TAG, "closed");
}

static void do_delete(void)
{
    if (s_g.count == 0) return;
    char path[64];
    close_video();
    path_of(s_g.index, path, sizeof(path));
    int rc = unlink(path);
    ESP_LOGI(TAG, "delete %s: %s", path, rc == 0 ? "ok" : "failed");
    if (rc != 0) return;
    sidecar_of(s_g.index, path, sizeof(path));
    unlink(path);
    memmove(&s_g.entries[s_g.index], &s_g.entries[s_g.index + 1],
            (size_t)(s_g.count - s_g.index - 1) * sizeof(entry_t));
    s_g.count--;
    show(s_g.index >= s_g.count ? s_g.count - 1 : s_g.index);
}

static void do_play(void)
{
    if (s_g.avi_fd < 0) return;
    if (!s_g.playing && s_g.avi_pos + 8 > s_g.avi_end) s_g.avi_pos = s_g.avi_first;      /* at the end: start over */
    s_g.playing = !s_g.playing;
    s_g.next_due = s_g.play_t0 = esp_timer_get_time();
    s_g.play_frames = 0;
    ui_play_icon(s_g.playing);
}

static void play_step(void)
{
    uint32_t w, h, stride;
    if (!video_frame(&w, &h, &stride)) {
        int64_t ms = (esp_timer_get_time() - s_g.play_t0) / 1000;
        ESP_LOGI(TAG, "played %lu frames in %lld ms (recorded at %.1f fps)", (unsigned long)s_g.play_frames,
                 (long long)ms, 1e6 / (double)s_g.frame_us);
        s_g.playing = false;
        ui_play_icon(false);
        return;
    }
    s_g.play_frames++;
    s_g.cur = s_g.dec_buf;
    s_g.cur_stride = stride; s_g.cur_w = w; s_g.cur_h = h;
    redraw();
    s_g.next_due += s_g.frame_us;
    int64_t now = esp_timer_get_time();
    if (s_g.next_due < now) s_g.next_due = now;          /* cannot keep up: play as fast as it goes */
}

static void gallery_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_g.active) {
            if (s_g.playing) {
                int64_t us = s_g.next_due - esp_timer_get_time();
                wait = us > 0 ? pdMS_TO_TICKS(us / 1000) : 0;
            } else {
                wait = pdMS_TO_TICKS(REFRESH_MS);
            }
        }
        gcmd_t c;
        if (xQueueReceive(s_g.q, &c, wait) == pdTRUE) {
            if (c == G_OPEN) {
                do_open();
            } else if (s_g.active) {
                switch (c) {
                case G_CLOSE:  do_close(NULL); break;
                case G_NEXT:   show(s_g.index + 1); break;
                case G_PREV:   show(s_g.index - 1); break;
                case G_PLAY:   do_play(); break;
                case G_DELETE: do_delete(); break;
                case G_LOOK: {
                    fp_recipe_t r;
                    if (s_g.count && sidecar_recipe(s_g.index, &r, NULL, 0)) do_close(&r);
                    break;
                }
                default: break;
                }
            }
            if (s_g.active && !s_g.playing) redraw();
        } else if (s_g.active) {
            if (s_g.playing) play_step();
            else             redraw();
        }
    }
}

esp_err_t gallery_init(void)
{
    s_g.q = xQueueCreate(8, sizeof(gcmd_t));
    ESP_RETURN_ON_FALSE(s_g.q, ESP_ERR_NO_MEM, TAG, "queue");
    /* core 1, like the camera task it stands in for: the scaler splits its work with core 0 */
    BaseType_t ok = xTaskCreatePinnedToCore(gallery_task, "gallery", 16 * 1024, NULL, 4, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

/* ---- widgets ---- */

static void nav_cb(lv_event_t *e)
{
    post((gcmd_t)(intptr_t)lv_event_get_user_data(e));
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT)  post(G_NEXT);
    if (dir == LV_DIR_RIGHT) post(G_PREV);
}

static void del_disarm(void)
{
    s_g.del_armed = false;
    if (s_g.del_timer) { lv_timer_delete(s_g.del_timer); s_g.del_timer = NULL; }
    lv_label_set_text(s_g.lbl_del, LV_SYMBOL_TRASH);
    lv_obj_set_style_bg_color(s_g.btn_del, lv_color_hex(0x505050), 0);
}

static void del_timer_cb(lv_timer_t *t) { (void)t; s_g.del_timer = NULL; del_disarm(); }

/* Deleting takes two taps within three seconds. */
static void del_cb(lv_event_t *e)
{
    (void)e;
    if (s_g.del_armed) {
        del_disarm();
        post(G_DELETE);
        return;
    }
    s_g.del_armed = true;
    lv_label_set_text(s_g.lbl_del, "Delete?");
    lv_obj_set_style_bg_color(s_g.btn_del, lv_color_hex(0xC02020), 0);
    s_g.del_timer = lv_timer_create(del_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_g.del_timer, 1);
}

static lv_obj_t *bar_button(lv_obj_t *bar, const char *text, uint32_t color, lv_event_cb_t cb, gcmd_t cmd, lv_obj_t **label)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_size(b, 106, 76);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    if (label) *label = l;
    return b;
}

void gallery_create_ui(lv_obj_t *scr)
{
    /* full-screen, see-through: the picture is drawn underneath by the gallery task */
    lv_obj_t *root = s_g.root = lv_obj_create(scr);
    lv_obj_set_size(root, FP_OUT_W, FP_OUT_H);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(root, COLOR_KEY_CLEAR, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, NULL);      /* swipe left / right */
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_g.info = lv_label_create(root);
    lv_obj_set_style_text_font(s_g.info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_g.info, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_g.info, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(s_g.info, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_g.info, 8, 0);
    lv_obj_set_width(s_g.info, FP_OUT_W);
    lv_obj_align(s_g.info, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_g.info, "");

    s_g.msg = lv_label_create(root);
    lv_obj_set_style_text_font(s_g.msg, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_g.msg, lv_color_white(), 0);
    lv_obj_center(s_g.msg);
    lv_label_set_text(s_g.msg, "");
    lv_obj_add_flag(s_g.msg, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, FP_OUT_W, 100);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_KEY_DIM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 12, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    bar_button(bar, LV_SYMBOL_LEFT, 0x303030, nav_cb, G_PREV, NULL);
    bar_button(bar, LV_SYMBOL_RIGHT, 0x303030, nav_cb, G_NEXT, NULL);
    s_g.btn_play = bar_button(bar, LV_SYMBOL_PLAY, 0x2060C0, nav_cb, G_PLAY, &s_g.lbl_play);
    s_g.btn_look = bar_button(bar, "Use look", 0x2060C0, nav_cb, G_LOOK, NULL);
    s_g.btn_del = bar_button(bar, LV_SYMBOL_TRASH, 0x505050, del_cb, G_DELETE, &s_g.lbl_del);
    bar_button(bar, LV_SYMBOL_CLOSE, 0xE0007A, nav_cb, G_CLOSE, NULL);

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}
