#include "esp_log.h"
#include "esp_check.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "buttons.h"

static const char *TAG = "buttons";

static buttons_shutter_cb_t s_shutter_cb;
static buttons_shutter_cb_t s_video_cb;
static void *s_user;

static void on_click(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_shutter_cb) s_shutter_cb(s_user);
}

static void on_long(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_video_cb) s_video_cb(s_user);
}

esp_err_t buttons_init(buttons_shutter_cb_t on_shutter, buttons_shutter_cb_t on_video, void *user)
{
    s_shutter_cb = on_shutter;
    s_video_cb = on_video;
    s_user = user;

    button_config_t btn_cfg = {
        .long_press_time = 700,
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BTN_SHUTTER_GPIO,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t btn = NULL;
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn), TAG, "shutter button");
    /* short press (on release) = photo, long press (700 ms) = start/stop video */
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_click, NULL), TAG, "click cb");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, on_long, NULL), TAG, "long cb");
    ESP_LOGI(TAG, "shutter on GPIO%d (click = photo, hold = video)", BTN_SHUTTER_GPIO);
    return ESP_OK;
}
