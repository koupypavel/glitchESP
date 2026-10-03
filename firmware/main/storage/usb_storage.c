#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "tusb_tasks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "capture.h"
#include "usb_storage.h"

static const char *TAG = "usb";

static bool s_active;
static bool s_installed;        /* the driver, once installed, stays: see usb_storage_stop() */

esp_err_t usb_storage_start(void)
{
    if (s_active) return ESP_OK;
    if (capture_busy() || capture_video_active()) return ESP_ERR_INVALID_STATE;

    capture_sd_eject();                         /* the camera lets go of the file system */
    sdmmc_card_t *card = NULL;
    esp_err_t ret = sd_card_open_raw(&card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(ret));
        capture_sd_reclaim();
        return ESP_ERR_NOT_FOUND;
    }

    /* The library never gets the card mounted for the application here, so the computer owns
     * it from the first moment. If the computer ejects the drive, the library mounts it for
     * itself (CONFIG_TINYUSB_MSC_MOUNT_PATH) and reports "no medium" from then on. */
    const tinyusb_msc_sdmmc_config_t msc = {
        .card = card,
        .mount_config = { .max_files = 2 },
    };
    ret = tinyusb_msc_storage_init_sdmmc(&msc);
    if (ret == ESP_OK) {
        if (!s_installed) {
            const tinyusb_config_t tusb = { 0 };    /* descriptors from Kconfig */
            ret = tinyusb_driver_install(&tusb);
            s_installed = (ret == ESP_OK);
        } else {
            ret = tusb_run_task();
            if (ret == ESP_OK) tud_connect();
        }
        if (ret != ESP_OK) tinyusb_msc_storage_deinit();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(ret));
        sd_card_close_raw();
        capture_sd_reclaim();
        return ret;
    }
    s_active = true;
    ESP_LOGI(TAG, "card offered on the OTG port: %llu MB",
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

void usb_storage_stop(void)
{
    if (!s_active) return;
    s_active = false;
    /* tinyusb_driver_uninstall() cannot be used: esp_tinyusb 1.7 tears down root port 0, the
     * P4's high-speed port is port 1, so it fails half way and the PHY stays taken. Instead
     * the device leaves the bus and its task is stopped, so that no callback from the host
     * can reach the card after this; start() brings both back. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));                 /* a transfer in flight ends */
    tusb_stop_task();
    tinyusb_msc_storage_unmount();                  /* the library's own mount after an eject */
    tinyusb_msc_storage_deinit();
    sd_card_close_raw();
    capture_sd_reclaim();
    ESP_LOGI(TAG, "card back with the camera: %s", capture_sd_available() ? "mounted" : "not mounted");
}

bool usb_storage_active(void)
{
    return s_active;
}

usb_storage_state_t usb_storage_state(void)
{
    if (!s_active) return USB_STORAGE_OFF;
    if (!tud_mounted()) return USB_STORAGE_WAITING;
    return tinyusb_msc_storage_in_use_by_usb_host() ? USB_STORAGE_CONNECTED : USB_STORAGE_EJECTED;
}
