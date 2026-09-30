#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "sound.h"
#include "settings.h"

static const char *TAG = "sound";

#define RATE        22050
#define MAX_MS      260
#define MAX_SAMPLES (RATE * MAX_MS / 1000)
#define VOLUME      75          /* percent */
#define IDLE_OFF_MS 30000       /* amplifier off this long after the last sound: opening the
                                   codec takes 0.1 to 0.3 s, which a shutter click should not wait for
                                   in the middle of a session */

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_spk;
static bool s_open;
static int16_t *s_buf;

/* ---- synthesis: everything is written into s_buf, returns the number of samples ---- */

static uint32_t s_noise = 0x1234567;
static inline int noise(void)
{
    s_noise = s_noise * 1664525u + 1013904223u;
    return (int)(s_noise >> 16) - 32768;
}

/* A burst of noise with a fast exponential decay, low-passed a little: one "click". */
static int add_click(int at, int len, float amp, float decay, float smooth)
{
    float env = amp, lp = 0;
    for (int i = 0; i < len && at + i < MAX_SAMPLES; i++) {
        lp += ((float)noise() - lp) * smooth;
        s_buf[at + i] += (int16_t)(lp * env);
        env *= decay;
    }
    return at + len;
}

static int add_tone(int at, int ms, float hz, float amp)
{
    int len = RATE * ms / 1000;
    for (int i = 0; i < len && at + i < MAX_SAMPLES; i++) {
        float fade = i < 60 ? i / 60.0f : (len - i < 200 ? (len - i) / 200.0f : 1.0f);   /* no clicks at the ends */
        s_buf[at + i] += (int16_t)(sinf(6.2832f * hz * (float)i / RATE) * amp * fade * 32767.0f);
    }
    return at + len;
}

static int synth(sound_id_t id)
{
    memset(s_buf, 0, MAX_SAMPLES * sizeof(int16_t));
    int end = 0;
    switch (id) {
    case SOUND_SHUTTER:                     /* two clicks: shutter opens, shutter closes */
        add_click(0, RATE * 25 / 1000, 0.55f, 0.990f, 0.45f);
        end = add_click(RATE * 70 / 1000, RATE * 40 / 1000, 0.40f, 0.994f, 0.25f);
        break;
    case SOUND_REC_START:                   /* rising pair */
        end = add_tone(0, 70, 880.0f, 0.30f);
        end = add_tone(end + RATE * 30 / 1000, 90, 1320.0f, 0.30f);
        break;
    case SOUND_REC_STOP:                    /* falling pair */
        end = add_tone(0, 70, 1320.0f, 0.30f);
        end = add_tone(end + RATE * 30 / 1000, 90, 880.0f, 0.30f);
        break;
    }
    return end > MAX_SAMPLES ? MAX_SAMPLES : end;
}

/* ---- output ---- */

static bool open_speaker(void)
{
    if (s_open) return true;
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
        if (!s_spk) {
            ESP_LOGE(TAG, "codec init failed");
            return false;
        }
    }
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 1, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk, VOLUME);
    s_open = true;
    return true;
}

static void sound_task(void *arg)
{
    (void)arg;
    for (;;) {
        sound_id_t id;
        if (xQueueReceive(s_q, &id, s_open ? pdMS_TO_TICKS(IDLE_OFF_MS) : portMAX_DELAY) != pdTRUE) {
            esp_codec_dev_close(s_spk);     /* quiet for a while: amplifier off */
            s_open = false;
            continue;
        }
        if (!open_speaker()) continue;
        int n = synth(id);
        int ret = esp_codec_dev_write(s_spk, s_buf, n * (int)sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "write failed: %d", ret);
    }
}

esp_err_t sound_init(void)
{
    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);   /* keep on-chip RAM free */
    s_q = xQueueCreate(4, sizeof(sound_id_t));
    ESP_RETURN_ON_FALSE(s_buf && s_q, ESP_ERR_NO_MEM, TAG, "buffer/queue");
    BaseType_t ok = xTaskCreatePinnedToCore(sound_task, "sound", 4 * 1024, NULL, 4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void sound_play(sound_id_t id)
{
    if (!s_q || !settings_get()->sound) return;
    xQueueSend(s_q, &id, 0);
}
