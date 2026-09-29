#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "display.h"

static const char *TAG = "display";

typedef enum { FB_FREE, FB_SUBMITTED, FB_SHOWN } fb_state_t;

static struct {
    esp_lcd_panel_handle_t panel;
    uint16_t *fb[DISP_FB_NUM];
    volatile fb_state_t state[DISP_FB_NUM];
    volatile int shown;        /* index currently scanned out, -1 = none */
    volatile int submitted;    /* index queued for the next vsync, -1 = none */
    SemaphoreHandle_t free_sem;
    portMUX_TYPE lock;
} s_d = { .shown = -1, .submitted = -1, .lock = portMUX_INITIALIZER_UNLOCKED };

/* The driver has switched to the submitted buffer: the one shown before is free again. */
static bool IRAM_ATTR on_frame_buf_complete(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_d.lock);
    int sub = s_d.submitted;
    if (sub >= 0) {
        int old = s_d.shown;
        s_d.shown = sub;
        s_d.state[sub] = FB_SHOWN;
        s_d.submitted = -1;
        if (old >= 0 && old != sub) {
            s_d.state[old] = FB_FREE;
            xSemaphoreGiveFromISR(s_d.free_sem, &woken);
        }
    }
    portEXIT_CRITICAL_ISR(&s_d.lock);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    bsp_lcd_handles_t h = { 0 };
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(NULL, &h), TAG, "panel");
    s_d.panel = h.panel;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_d.panel, DISP_FB_NUM,
                        (void **)&s_d.fb[0], (void **)&s_d.fb[1], (void **)&s_d.fb[2]), TAG, "frame buffers");

    s_d.free_sem = xSemaphoreCreateCounting(DISP_FB_NUM, 0);
    ESP_RETURN_ON_FALSE(s_d.free_sem, ESP_ERR_NO_MEM, TAG, "sem");
    for (int i = 0; i < DISP_FB_NUM; i++) {
        s_d.state[i] = FB_FREE;
        xSemaphoreGive(s_d.free_sem);
    }

    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame_buf_complete };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_d.panel, &cbs, NULL), TAG, "callbacks");

    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight");
    ESP_LOGI(TAG, "%dx%d RGB565, %d frame buffers at %p %p %p", DISP_W, DISP_H, DISP_FB_NUM,
             s_d.fb[0], s_d.fb[1], s_d.fb[2]);
    return ESP_OK;
}

int display_acquire_fb(void)
{
    for (;;) {
        xSemaphoreTake(s_d.free_sem, portMAX_DELAY);
        portENTER_CRITICAL(&s_d.lock);
        int found = -1;
        for (int i = 0; i < DISP_FB_NUM; i++) {
            if (s_d.state[i] == FB_FREE) {
                s_d.state[i] = FB_SUBMITTED;   /* reserved: not free, not yet queued */
                found = i;
                break;
            }
        }
        portEXIT_CRITICAL(&s_d.lock);
        if (found >= 0) return found;
    }
}

uint16_t *display_fb(int idx)
{
    return (idx >= 0 && idx < DISP_FB_NUM) ? s_d.fb[idx] : NULL;
}

esp_err_t display_submit_fb(int idx)
{
    portENTER_CRITICAL(&s_d.lock);
    int prev = s_d.submitted;
    if (prev >= 0 && prev != idx && prev != s_d.shown) {
        /* superseded before it was ever shown: it is free again */
        s_d.state[prev] = FB_FREE;
        xSemaphoreGive(s_d.free_sem);
    }
    s_d.submitted = idx;
    portEXIT_CRITICAL(&s_d.lock);
    /* the buffer is one of the panel's own: cache write-back + DMA switch, no copy */
    return esp_lcd_panel_draw_bitmap(s_d.panel, 0, 0, DISP_W, DISP_H, s_d.fb[idx]);
}

esp_lcd_panel_handle_t display_panel(void)
{
    return s_d.panel;
}
