#include "esp_log.h"
#include "esp_check.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "buttons.h"

static const char *TAG = "buttons";

static buttons_shutter_cb_t s_cb;
static void *s_user;

static void on_shutter_press(void *handle, void *usr)
{
    (void)handle; (void)usr;
    if (s_cb) {
        s_cb(s_user);
    }
}

esp_err_t buttons_init(buttons_shutter_cb_t on_shutter, void *user)
{
    s_cb = on_shutter;
    s_user = user;

    button_config_t btn_cfg = {
        .long_press_time = 1500,
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
    /* Fire on press-down so the shot feels instant; long-press is reserved for later. */
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, on_shutter_press, NULL), TAG, "cb");
    ESP_LOGI(TAG, "shutter on GPIO%d", BTN_SHUTTER_GPIO);
    return ESP_OK;
}
