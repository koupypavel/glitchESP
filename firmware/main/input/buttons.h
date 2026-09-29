/* Physical buttons. M1: the on-board BOOT button (GPIO35) acts as the shutter. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BTN_SHUTTER_GPIO   35   /* on-board BOOT button; M5 moves this to the 40-pin header */

typedef void (*buttons_shutter_cb_t)(void *user);

esp_err_t buttons_init(buttons_shutter_cb_t on_shutter, buttons_shutter_cb_t on_video, void *user);

#ifdef __cplusplus
}
#endif
