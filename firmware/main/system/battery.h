/*
 * Battery level. The board feeds the LiPo voltage to GPIO20 through a resistor divider; it is
 * sampled every couple of seconds, smoothed, and turned into a percentage with a typical
 * LiPo discharge curve.
 *
 * The board has no signal that tells whether USB power is present or the cell is charging,
 * so both are inferred from the voltage: it rises while charging, and a charger can hold it
 * above what a cell gives on its own. A full cell on USB looks like a full cell.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATTERY_UNKNOWN,        /* not measured yet */
    BATTERY_DISCHARGING,
    BATTERY_CHARGING,       /* the voltage is rising, or held above what a cell gives on its own */
    BATTERY_NONE,           /* no cell: the reading is what the charger puts on the pin */
} battery_state_t;

typedef struct {
    battery_state_t state;
    uint32_t mv;            /* cell voltage, smoothed */
    uint32_t pin_mv;        /* what the ADC sees at GPIO20, last sample */
    int percent;            /* 0..100, -1 while unknown */
    bool low;               /* below 10 %: worth a warning */
    bool critical;          /* below about 3.4 V: stop recording before the board browns out */
} battery_info_t;

esp_err_t battery_init(void);
void battery_get(battery_info_t *out);

#ifdef __cplusplus
}
#endif
