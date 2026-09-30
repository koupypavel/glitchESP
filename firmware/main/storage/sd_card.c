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

esp_err_t sd_card_mount(void)
{
    if (s_card) return ESP_OK;
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHANNEL };
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr), TAG, "card power");
    }
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;
    const sdmmc_slot_config_t slot = {
        /* slot 0 uses dedicated pins: nothing to assign */
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
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
