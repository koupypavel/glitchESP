#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "settings.h"
#include "app_video.h"
#include "frame_pipeline.h"

static const char *TAG = "settings";
#define NS "glitch"

static settings_t s_cfg = { .flip_h = false, .flip_v = false, .quality = 0, .jpeg_quality = 90, .photo_hires = true, .idle_dim = true, .sound = true, .burst = 1 };

esp_err_t settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "flip_h", &v) == ESP_OK) s_cfg.flip_h = v;
        if (nvs_get_u8(h, "flip_v", &v) == ESP_OK) s_cfg.flip_v = v;
        if (nvs_get_u8(h, "quality", &v) == ESP_OK) s_cfg.quality = v;
        if (nvs_get_u8(h, "jpeg_q", &v) == ESP_OK) s_cfg.jpeg_quality = v;
        if (nvs_get_u8(h, "hires", &v) == ESP_OK) s_cfg.photo_hires = v;
        if (nvs_get_u8(h, "bar_hide", &v) == ESP_OK) s_cfg.bar_hidden = v;
        if (nvs_get_u8(h, "dim", &v) == ESP_OK) s_cfg.idle_dim = v;
        if (nvs_get_u8(h, "sound", &v) == ESP_OK) s_cfg.sound = v;
        if (nvs_get_u8(h, "burst", &v) == ESP_OK && v >= 1 && v <= 10) s_cfg.burst = v;
        if (nvs_get_u8(h, "h264", &v) == ESP_OK) s_cfg.video_h264 = v;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "flip_h=%d flip_v=%d quality=%u jpeg=%u", s_cfg.flip_h, s_cfg.flip_v, s_cfg.quality, s_cfg.jpeg_quality);
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_apply(void)
{
    app_video_set_flip(s_cfg.flip_v, s_cfg.flip_h);
    frame_pipeline_set_quality((fp_quality_t)s_cfg.quality);
}

void settings_set(const settings_t *s)
{
    s_cfg = *s;
    settings_apply();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "flip_h", s_cfg.flip_h);
        nvs_set_u8(h, "flip_v", s_cfg.flip_v);
        nvs_set_u8(h, "quality", s_cfg.quality);
        nvs_set_u8(h, "jpeg_q", s_cfg.jpeg_quality);
        nvs_set_u8(h, "hires", s_cfg.photo_hires);
        nvs_set_u8(h, "bar_hide", s_cfg.bar_hidden);
        nvs_set_u8(h, "dim", s_cfg.idle_dim);
        nvs_set_u8(h, "sound", s_cfg.sound);
        nvs_set_u8(h, "burst", s_cfg.burst);
        nvs_set_u8(h, "h264", s_cfg.video_h264);
        nvs_commit(h);
        nvs_close(h);
    }
}
