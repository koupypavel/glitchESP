#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_card.h"

static const char *TAG = "sd";

#define SD_LDO_CHANNEL  4       /* on-chip LDO that powers the card slot on this board */

static sd_pwr_ctrl_handle_t s_pwr;      /* created once, kept across mounts */
static sdmmc_card_t *s_card;
static sdmmc_card_t *s_raw;             /* the card without a file system (USB storage mode) */

static esp_err_t host_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHANNEL };
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr), TAG, "card power");
    }
    sdmmc_host_t h = SDMMC_HOST_DEFAULT();
    h.slot = SDMMC_HOST_SLOT_0;
    h.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    h.pwr_ctrl_handle = s_pwr;
    *host = h;
    const sdmmc_slot_config_t s = {
        /* slot 0 uses dedicated pins: nothing to assign */
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
    *slot = s;
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    if (s_card) return ESP_OK;
    if (s_raw) return ESP_ERR_INVALID_STATE;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    ESP_RETURN_ON_ERROR(host_config(&host, &slot), TAG, "host");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (ret != ESP_OK) s_card = NULL;
    return ret;
}

void sd_card_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

bool sd_card_mounted(void)
{
    return s_card != NULL;
}

const sdmmc_card_t *sd_card_info(void)
{
    return s_card;
}

esp_err_t sd_card_open_raw(sdmmc_card_t **card)
{
    if (s_card || s_raw) return ESP_ERR_INVALID_STATE;
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    ESP_RETURN_ON_ERROR(host_config(&host, &slot), TAG, "host");
    sdmmc_card_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        free(c);
        return ret;
    }
    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret == ESP_OK) ret = sdmmc_card_init(&host, c);
    if (ret != ESP_OK) {
        sdmmc_host_deinit();
        free(c);
        return ret;
    }
    s_raw = c;
    *card = c;
    return ESP_OK;
}

void sd_card_close_raw(void)
{
    if (!s_raw) return;
    sdmmc_host_deinit();
    free(s_raw);
    s_raw = NULL;
}
