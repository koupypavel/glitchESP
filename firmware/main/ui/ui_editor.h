/*
 * Parameter editor: a sheet at the bottom of the live screen with one slider (or switch)
 * per parameter of an active effect, a tab per active effect, and a reset button. The
 * video stays visible above it and changes as the sliders move.
 *
 * Values set here stay until the amount slider is moved (which maps every parameter from
 * the one knob again) or "Reset" is pressed. They are part of the recipe, so presets and
 * photo sidecars carry them.
 *
 * All functions must be called with the LVGL lock held.
 */
#pragma once

#include <stdbool.h>
#include "fx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open for `fx` (NULL: the first active effect). False if no effect is active. */
bool ui_editor_open(const fx_desc_t *fx);
void ui_editor_close(void);
bool ui_editor_is_open(void);

/* The recipe changed somewhere else (amount slider, effect chips, a preset): show the new
 * values, or close if the edited effect is no longer active. */
void ui_editor_sync(void);

#ifdef __cplusplus
}
#endif
