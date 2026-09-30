/*
 * Presets: effect recipes (which effects, their parameters, the amount knob and the seed)
 * kept in NVS so a look can be recalled later. Zoom and camera settings are not part of it.
 *
 * Effects are stored by their id string, not by position in the registry, so presets
 * survive firmware updates that add or reorder effects; an effect that no longer exists is
 * simply left out when the preset is loaded.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "frame_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRESET_SLOTS 8

bool presets_exists(int slot);
esp_err_t presets_save(int slot, const fp_recipe_t *recipe);
/* Fills chain, amount and seed of `out` (other fields untouched). ESP_ERR_NOT_FOUND if empty. */
esp_err_t presets_load(int slot, fp_recipe_t *out);
esp_err_t presets_clear(int slot);
/* Short text for a list row: "Wave + Kaleido  60%", "no effects  50%", or "empty". */
void presets_describe(int slot, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
