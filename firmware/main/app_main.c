/*
 * glitchESP camera app.
 * Camera (OV5647 / MIPI-CSI) -> ISP -> RGB565 -> [effects] -> panel frame buffer (+UI layer) on the 720x1280 LCD.
 * BOOT button = shutter: hardware JPEG encode of the displayed frame -> /sdcard/GLITCH.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_video_init.h"

#include "app_video.h"
#include "frame_pipeline.h"
#include "capture.h"
#include "ui_live.h"
#include "ui_lvgl.h"
#include "display.h"
#include "buttons.h"
#include "ov5647_ctl.h"
#include "auto_exposure.h"
#include "fx_parallel.h"
#include "settings.h"
#include "remote.h"
#include "cam_ctrl.h"
#include "gallery.h"

#define GLITCH_BENCH 0          /* 1 = run main/bench.c after boot (effect timings, test shot) */
void bench_start(void);
void bench_microbench_on_core1(void);

static const char *TAG = "app";

static void on_capture_done(const capture_result_t *res, void *user)
{
    (void)user;
    ui_live_on_capture_done(res);
}

static void on_video_done(const video_result_t *res, void *user)
{
    (void)user;
    ui_live_on_video_done(res);
}

static void on_video(void *user)
{
    (void)user;
    if (gallery_active()) return;
    if (capture_video_active()) {
        capture_video_stop();
        return;
    }
    esp_err_t ret = capture_video_start(on_video_done, NULL);
    if (ret == ESP_OK) {
        ui_live_on_video_started();
    } else {
        ESP_LOGW(TAG, "video start failed: %s", esp_err_to_name(ret));
        ui_live_on_video_error(ret == ESP_ERR_NOT_FOUND ? "no SD card" : "cannot start video");
    }
}

static void on_shutter(void *user)
{
    (void)user;
    if (gallery_active()) {                 /* the shutter leads back to the camera */
        gallery_close();
        return;
    }
    /* At zoom 1 the sensor can be re-read at full resolution for the photo (about a second);
     * otherwise the frame on screen is saved. */
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (settings_get()->photo_hires && cam_ctrl_still_available()) ret = cam_ctrl_take_still(false);
    if (ret != ESP_OK) ret = capture_trigger();
    if (ret == ESP_OK) {
        ui_live_on_capture_started();
    } else {
        ESP_LOGW(TAG, "shutter ignored: %s", esp_err_to_name(ret));
    }
}

void app_main(void)
{
    /* NVS for the shot counter */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Panel frame buffers (video path) and LVGL (widgets only), see display/ and ui/ui_lvgl.c */
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_lvgl_init());

    /* microSD (optional) */
    bool sd_ok = (bsp_sdcard_mount() == ESP_OK);
    if (sd_ok) {
        ESP_LOGI(TAG, "SD card mounted at %s (%s, %llu MB)", BSP_SD_MOUNT_POINT, bsp_sdcard->cid.name,
                 ((uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size) / (1024 * 1024));
        sdmmc_card_print_info(stdout, bsp_sdcard);
    } else {
        ESP_LOGW(TAG, "no SD card: shots will not be saved");
    }

#if GLITCH_BENCH
    /* bench_microbench_on_core1(); */   /* one-off measurement, see docs/EFFECTS.md */
#endif

    /* Pipeline buffers + PPA, capture engine */
    ESP_ERROR_CHECK(frame_pipeline_init());
    ESP_ERROR_CHECK(fx_parallel_init());
    ESP_ERROR_CHECK(capture_init(sd_ok, on_capture_done, NULL));

    /* UI */
    ESP_ERROR_CHECK(gallery_init());
    ui_lvgl_lock(0);
    ui_live_create();
    ui_lvgl_unlock();

    /* Camera: shares the board I2C bus with touch/codec */
    ESP_ERROR_CHECK(app_video_main(bsp_i2c_get_handle()));
    int cam_fd = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (cam_fd < 0) {
        ESP_LOGE(TAG, "camera open failed (is the OV5647 connected?)");
        return;
    }
    size_t cache_line = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line));
    /* One region for all camera buffers: five preview frames of the largest preview mode
     * (800x1280, what the driver starts in), or two 1088x1920 frames for a still. */
    size_t cam_buf_len = (app_video_get_buf_size() + cache_line - 1) & ~(cache_line - 1);
    void *cam_block = heap_caps_aligned_calloc(cache_line, 1, cam_buf_len * FP_CAM_BUFS, MALLOC_CAP_SPIRAM);
    if (!cam_block) {
        ESP_LOGE(TAG, "camera buffer alloc failed");
        return;
    }

    /*
     * Sensor modes, frame timing and exposure (camera/cam_ctrl.c): the driver's built-in
     * auto exposure does not adapt in this configuration, so the sensor runs in manual mode
     * under our own AE. Starts in the wide (binned) mode; orientation comes from settings.
     */
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(frame_pipeline_on_camera_frame));
    ESP_ERROR_CHECK(cam_ctrl_init(cam_fd, cam_block, cam_buf_len, FP_CAM_BUFS));
    ESP_ERROR_CHECK(cam_ctrl_start());                                /* camera + pipeline on core 1 */

    /* Shutter */
    ESP_ERROR_CHECK(buttons_init(on_shutter, on_video, NULL));
    ESP_ERROR_CHECK(remote_init(on_shutter, on_video, NULL));   /* same actions over the serial port */


#if GLITCH_BENCH
    bench_start();
#endif
    ESP_LOGI(TAG, "running. free heap: internal %u KB, psram %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
