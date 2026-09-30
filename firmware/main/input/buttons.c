#include "esp_log.h"
#include "esp_check.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "iot_knob.h"
#include "buttons.h"

static const char *TAG = "buttons";

static buttons_handlers_t s_h;

static void on_shutter(void *handle, void *usr)    { (void)handle; (void)usr; if (s_h.shutter) s_h.shutter(s_h.user); }
static void on_video(void *handle, void *usr)      { (void)handle; (void)usr; if (s_h.video) s_h.video(s_h.user); }
static void on_reroll(void *handle, void *usr)     { (void)handle; (void)usr; if (s_h.reroll) s_h.reroll(s_h.user); }
static void on_knob_click(void *handle, void *usr) { (void)handle; (void)usr; if (s_h.knob_click) s_h.knob_click(s_h.user); }
static void on_knob_hold(void *handle, void *usr)  { (void)handle; (void)usr; if (s_h.knob_hold) s_h.knob_hold(s_h.user); }
static void on_knob_left(void *handle, void *usr)  { (void)handle; (void)usr; if (s_h.knob_turn) s_h.knob_turn(-1, s_h.user); }
static void on_knob_right(void *handle, void *usr) { (void)handle; (void)usr; if (s_h.knob_turn) s_h.knob_turn(+1, s_h.user); }

/* An active-low push button with pull-up: click and (optionally) hold. */
static esp_err_t add_button(int gpio, button_cb_t click, button_cb_t hold)
{
    button_config_t btn_cfg = {
        .long_press_time = 700,
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t btn = NULL;
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn), TAG, "button on GPIO%d", gpio);
    if (click) ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, click, NULL), TAG, "click cb");
    if (hold)  ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, hold, NULL), TAG, "hold cb");
    return ESP_OK;
}

esp_err_t buttons_init(const buttons_handlers_t *handlers)
{
    s_h = *handlers;

    /* on the board: short press (on release) = photo, long press = start/stop video */
    ESP_RETURN_ON_ERROR(add_button(BTN_BOOT_GPIO, on_shutter, on_video), TAG, "BOOT");

    /* on the header: the same shutter, a re-roll button and a rotary encoder with push */
    ESP_RETURN_ON_ERROR(add_button(HDR_SHUTTER_GPIO, on_shutter, on_video), TAG, "shutter");
    ESP_RETURN_ON_ERROR(add_button(HDR_REROLL_GPIO, on_reroll, NULL), TAG, "re-roll");
    ESP_RETURN_ON_ERROR(add_button(HDR_ENC_SW_GPIO, on_knob_click, on_knob_hold), TAG, "encoder push");

    knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = HDR_ENC_A_GPIO,
        .gpio_encoder_b = HDR_ENC_B_GPIO,
    };
    knob_handle_t knob = iot_knob_create(&knob_cfg);
    ESP_RETURN_ON_FALSE(knob, ESP_FAIL, TAG, "encoder");
    ESP_RETURN_ON_ERROR(iot_knob_register_cb(knob, KNOB_LEFT, on_knob_left, NULL), TAG, "knob cb");
    ESP_RETURN_ON_ERROR(iot_knob_register_cb(knob, KNOB_RIGHT, on_knob_right, NULL), TAG, "knob cb");

    ESP_LOGI(TAG, "BOOT on GPIO%d; header: shutter %d, re-roll %d, encoder %d/%d, push %d", BTN_BOOT_GPIO,
             HDR_SHUTTER_GPIO, HDR_REROLL_GPIO, HDR_ENC_A_GPIO, HDR_ENC_B_GPIO, HDR_ENC_SW_GPIO);
    return ESP_OK;
}
