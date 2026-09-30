/*
 * microSD card mount that can be repeated.
 *
 * The board support package's own mount function creates a new power-control handle for
 * the card's supply on every call and never frees it, so after one failed attempt (no card
 * at boot) every later attempt fails too. This one keeps the handle, which makes it
 * possible to put the card in later, or take it out and back in.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

esp_err_t sd_card_mount(void);              /* ESP_OK if it is (now) mounted */
void sd_card_unmount(void);
bool sd_card_mounted(void);
const sdmmc_card_t *sd_card_info(void);     /* NULL when not mounted */

#ifdef __cplusplus
}
#endif
