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
    esp_err_t ret = capture_trigger();
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
    void *cam_buf[FP_CAM_BUFS];
    for (int i = 0; i < FP_CAM_BUFS; i++) {
        cam_buf[i] = heap_caps_aligned_calloc(cache_line, 1, app_video_get_buf_size(), MALLOC_CAP_SPIRAM);
        if (!cam_buf[i]) {
            ESP_LOGE(TAG, "camera buffer alloc failed");
            return;
        }
    }
    ESP_ERROR_CHECK(app_video_set_bufs(cam_fd, FP_CAM_BUFS, (const void **)cam_buf));

    /*
     * Sensor timing + exposure, programmed before streaming starts (the driver only writes
     * its register table when the format is set). The mode table yields ~16 fps and its
     * built-in AEC does not adapt here, so: faster frame timing + our own AE in manual mode.
     */
    ESP_ERROR_CHECK(ov5647_ctl_init());
    ESP_ERROR_CHECK(ov5647_ctl_set_timing(OV5647_HTS_FAST, OV5647_VTS_FAST));
    ESP_ERROR_CHECK(auto_exposure_init(OV5647_EXPO_MAX(OV5647_VTS_FAST) / 2, 16, OV5647_EXPO_MAX(OV5647_VTS_FAST)));

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(frame_pipeline_on_camera_frame));
    ESP_ERROR_CHECK(settings_init());
    settings_apply();                                                 /* orientation, quality */
    ESP_ERROR_CHECK(app_video_stream_task_start(cam_fd, 1, NULL));   /* camera + pipeline on core 1 */

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
