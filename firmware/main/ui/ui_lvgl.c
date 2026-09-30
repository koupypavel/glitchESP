#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "display.h"
#include "ui_lvgl.h"

static const char *TAG = "ui_lvgl";

#define DRAW_ROWS   40                     /* partial render buffer height */
#define MAX_RECTS   12                     /* UI regions to stamp per frame */

typedef struct { int16_t x1, y1, x2, y2; } rect_t;

static struct {
    lv_display_t *disp;
    lv_indev_t *indev;
    esp_lcd_touch_handle_t touch;
    uint16_t *layer;                      /* DISP_W x DISP_H RGB565 in PSRAM */
    uint16_t *draw_buf;                   /* DISP_W x DRAW_ROWS in internal RAM */
    SemaphoreHandle_t mutex;
    portMUX_TYPE lock;
    rect_t rects[MAX_RECTS];              /* dirty-ever regions of the layer */
    int n_rects;
    volatile int flash_frames;
} s_u = { .lock = portMUX_INITIALIZER_UNLOCKED };

/* ---- region bookkeeping: union of every area LVGL ever flushed, as a few rects ---- */

static void rects_add(const lv_area_t *a)
{
    rect_t r = { a->x1, a->y1, a->x2, a->y2 };
    portENTER_CRITICAL(&s_u.lock);
    /* merge into an existing rect if they overlap or touch vertically on the same columns */
    for (int i = 0; i < s_u.n_rects; i++) {
        rect_t *e = &s_u.rects[i];
        bool x_ok = r.x1 >= e->x1 - 8 && r.x2 <= e->x2 + 8;
        bool y_touch = r.y1 <= e->y2 + 4 && r.y2 >= e->y1 - 4;
        if ((x_ok && y_touch) || (r.x1 >= e->x1 && r.x2 <= e->x2 && r.y1 >= e->y1 && r.y2 <= e->y2)) {
            if (r.x1 < e->x1) e->x1 = r.x1;
            if (r.y1 < e->y1) e->y1 = r.y1;
            if (r.x2 > e->x2) e->x2 = r.x2;
            if (r.y2 > e->y2) e->y2 = r.y2;
            portEXIT_CRITICAL(&s_u.lock);
            return;
        }
    }
    if (s_u.n_rects < MAX_RECTS) {
        s_u.rects[s_u.n_rects++] = r;
    } else {
        /* out of slots: grow the last one to cover (rare) */
        rect_t *e = &s_u.rects[MAX_RECTS - 1];
        if (r.x1 < e->x1) e->x1 = r.x1;
        if (r.y1 < e->y1) e->y1 = r.y1;
        if (r.x2 > e->x2) e->x2 = r.x2;
        if (r.y2 > e->y2) e->y2 = r.y2;
    }
    portEXIT_CRITICAL(&s_u.lock);
}

/* ---- LVGL callbacks ---- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    const uint16_t *src = (const uint16_t *)px_map;
    /* copy into the layer and find the bounding box of pixels that are not "clear" */
    lv_area_t bb = { .x1 = INT16_MAX, .y1 = INT16_MAX, .x2 = -1, .y2 = -1 };
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(s_u.layer + (size_t)y * DISP_W + area->x1, src, (size_t)w * 2);
        for (int x = 0; x < w; x++) {
            if (src[x] != UI_KEY_CLEAR) {
                if (x + area->x1 < bb.x1) bb.x1 = x + area->x1;
                if (x + area->x1 > bb.x2) bb.x2 = x + area->x1;
                if (y < bb.y1) bb.y1 = y;
                if (y > bb.y2) bb.y2 = y;
            }
        }
        src += w;
    }
    if (bb.x2 >= 0) rects_add(&bb);
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x[1], y[1], strength[1];
    uint8_t n = 0;
    esp_lcd_touch_read_data(s_u.touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_u.touch, x, y, strength, &n, 1);
    if (pressed && n > 0) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay_ms = 5;
        if (ui_lvgl_lock(50)) {
            delay_ms = lv_timer_handler();
            ui_lvgl_unlock();
        }
        if (delay_ms < 5) delay_ms = 5;
        if (delay_ms > 30) delay_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t ui_lvgl_init(void)
{
    s_u.mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_u.mutex, ESP_ERR_NO_MEM, TAG, "mutex");

    s_u.layer = heap_caps_malloc(DISP_W * DISP_H * 2, MALLOC_CAP_SPIRAM);
    s_u.draw_buf = heap_caps_malloc(DISP_W * DRAW_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_u.layer && s_u.draw_buf, ESP_ERR_NO_MEM, TAG, "buffers");
    for (size_t i = 0; i < (size_t)DISP_W * DISP_H; i++) s_u.layer[i] = UI_KEY_CLEAR;

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_u.disp = lv_display_create(DISP_W, DISP_H);
    ESP_RETURN_ON_FALSE(s_u.disp, ESP_FAIL, TAG, "lv display");
    lv_display_set_color_format(s_u.disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_u.disp, s_u.draw_buf, NULL, DISP_W * DRAW_ROWS * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_u.disp, flush_cb);

    /* the screen background is the "clear" key: video shows through everywhere no widget is */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000008), 0);   /* -> RGB565 0x0001 */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* touch */
    bsp_display_cfg_t tcfg = { .touch_flags = { 0 } };
    if (bsp_touch_new(&tcfg, &s_u.touch) == ESP_OK) {
        s_u.indev = lv_indev_create();
        lv_indev_set_type(s_u.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_u.indev, touch_read_cb);
        lv_indev_set_display(s_u.indev, s_u.disp);
    } else {
        ESP_LOGW(TAG, "touch not available");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8 * 1024, NULL, 13, NULL, 0);   /* above the effect worker (12): the controls stay responsive under load */
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");
    ESP_LOGI(TAG, "LVGL %d.%d on core 0, partial buffer %d rows", lv_version_major(), lv_version_minor(), DRAW_ROWS);
    return ESP_OK;
}

bool ui_lvgl_lock(uint32_t timeout_ms)
{
    return xSemaphoreTakeRecursive(s_u.mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ui_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_u.mutex);
}

void ui_lvgl_flash(int frames)
{
    s_u.flash_frames = frames;
}

/* Composite the UI layer onto a frame buffer: called once per video frame on core 1. */
void IRAM_ATTR ui_lvgl_stamp(uint16_t *fb)
{
    if (s_u.flash_frames > 0) {
        s_u.flash_frames--;
        memset(fb, 0xff, (size_t)DISP_W * DISP_H * 2);
        return;
    }
    rect_t rects[MAX_RECTS];
    int n;
    portENTER_CRITICAL(&s_u.lock);
    n = s_u.n_rects;
    memcpy(rects, s_u.rects, sizeof(rect_t) * n);
    portEXIT_CRITICAL(&s_u.lock);

    for (int i = 0; i < n; i++) {
        const rect_t *r = &rects[i];
        for (int y = r->y1; y <= r->y2; y++) {
            const uint16_t *src = s_u.layer + (size_t)y * DISP_W + r->x1;
            uint16_t *dst = fb + (size_t)y * DISP_W + r->x1;
            int w = r->x2 - r->x1 + 1;
            for (int x = 0; x < w; x++) {
                uint16_t p = src[x];
                if (p == UI_KEY_CLEAR) continue;
                if (p == UI_KEY_DIM) { dst[x] = (dst[x] >> 1) & 0x7BEF; continue; }
                dst[x] = p;
            }
        }
    }
}
