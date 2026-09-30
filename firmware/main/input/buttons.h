/*
 * Physical controls.
 *
 * On the board:   BOOT button (GPIO35)     click = photo, hold 0.7 s = start/stop video
 * On the 40-pin header (optional; all inputs have pull-ups and are active low, so nothing
 * happens while they are not wired):
 *
 *   function        GPIO   header pin   other side of the switch
 *   shutter          21       15        GND (pin 13)        same as BOOT
 *   re-roll          22       17        GND (pin 19)        new random seed
 *   encoder A        29       20        encoder common to GND (pin 26)
 *   encoder B        30       22
 *   encoder push     31       24        click = next preset, hold = hide/show the bar
 *
 * Turning the encoder moves the amount knob (in the gallery: previous / next picture).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BTN_BOOT_GPIO      35
#define HDR_SHUTTER_GPIO   21
#define HDR_REROLL_GPIO    22
#define HDR_ENC_A_GPIO     29
#define HDR_ENC_B_GPIO     30
#define HDR_ENC_SW_GPIO    31

typedef void (*buttons_cb_t)(void *user);

typedef struct {
    buttons_cb_t shutter;                       /* BOOT or header shutter: click */
    buttons_cb_t video;                         /* BOOT or header shutter: hold */
    buttons_cb_t reroll;
    buttons_cb_t knob_click;
    buttons_cb_t knob_hold;
    void (*knob_turn)(int steps, void *user);   /* +1 clockwise, -1 counter-clockwise */
    void *user;
} buttons_handlers_t;

/* Handlers are called from the button driver's timer task: keep them short. */
esp_err_t buttons_init(const buttons_handlers_t *handlers);

#ifdef __cplusplus
}
#endif
