/*
 * Temporary on-device benchmark (enabled with GLITCH_BENCH in app_main.c):
 *  1. cycles through every registered effect and one 3-effect chain, 5 s each, so the
 *     pipeline stats line reports fx time and fps per effect;
 *  2. sweeps the OV5647 gain register with exposure locked and logs brightness per value.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "frame_pipeline.h"
#include "auto_exposure.h"
#include "ov5647_ctl.h"
#include "fx.h"
#include "capture.h"

static const char *TAG = "bench";

#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#define MB_W 720
#define MB_H 1280
#define MB_PX (MB_W * MB_H)

static void IRAM_ATTR loop16(const uint16_t *s, uint16_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void IRAM_ATTR loop16_op(const uint16_t *s, uint16_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) { uint16_t p = s[i]; d[i] = (uint16_t)((p & 0xF800) | ((p >> 1) & 0x03E0) | (p & 0x001F)); }
}
static void IRAM_ATTR loop32(const uint32_t *s, uint32_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void IRAM_ATTR loop32_op(const uint32_t *s, uint32_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) { uint32_t p = s[i]; d[i] = (p & 0xF800F800u) | ((p >> 1) & 0x03E003E0u) | (p & 0x001F001Fu); }
}
static void IRAM_ATTR loop32_op_unroll(const uint32_t *s, uint32_t *d, size_t n)
{
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t p0 = s[i], p1 = s[i + 1], p2 = s[i + 2], p3 = s[i + 3];
        d[i]     = (p0 & 0xF800F800u) | ((p0 >> 1) & 0x03E003E0u) | (p0 & 0x001F001Fu);
        d[i + 1] = (p1 & 0xF800F800u) | ((p1 >> 1) & 0x03E003E0u) | (p1 & 0x001F001Fu);
        d[i + 2] = (p2 & 0xF800F800u) | ((p2 >> 1) & 0x03E003E0u) | (p2 & 0x001F001Fu);
        d[i + 3] = (p3 & 0xF800F800u) | ((p3 >> 1) & 0x03E003E0u) | (p3 & 0x001F001Fu);
    }
    for (; i < n; i++) d[i] = s[i];
}

#define TIME(label, stmt) do { int64_t t0 = esp_timer_get_time(); stmt; int64_t t1 = esp_timer_get_time();     ESP_LOGI(TAG, "%-34s %6lld us", label, (long long)(t1 - t0)); } while (0)

void memory_microbench(void)
{
    uint16_t *a = heap_caps_aligned_calloc(128, 1, MB_PX * 2, MALLOC_CAP_SPIRAM);
    uint16_t *b = heap_caps_aligned_calloc(128, 1, MB_PX * 2, MALLOC_CAP_SPIRAM);
    size_t in_px = 32 * 1024;   /* 64 KB internal buffers */
    uint16_t *ia = heap_caps_aligned_calloc(128, 1, in_px * 2, MALLOC_CAP_INTERNAL);
    uint16_t *ib = heap_caps_aligned_calloc(128, 1, in_px * 2, MALLOC_CAP_INTERNAL);
    if (!a || !b || !ia || !ib) { ESP_LOGE(TAG, "microbench alloc failed"); return; }
    for (size_t i = 0; i < MB_PX; i++) a[i] = (uint16_t)i;
    ESP_LOGI(TAG, "--- memory microbench: %dx%d RGB565 (%u bytes) ---", MB_W, MB_H, (unsigned)(MB_PX * 2));
    for (int rep = 0; rep < 2; rep++) {
        TIME("psram memcpy",                  memcpy(b, a, MB_PX * 2));
        TIME("psram 16-bit copy loop",        loop16(a, b, MB_PX));
        TIME("psram 16-bit op loop",          loop16_op(a, b, MB_PX));
        TIME("psram 32-bit copy loop",        loop32((uint32_t *)a, (uint32_t *)b, MB_PX / 2));
        TIME("psram 32-bit op loop",          loop32_op((uint32_t *)a, (uint32_t *)b, MB_PX / 2));
        TIME("psram 32-bit op loop unrolled", loop32_op_unroll((uint32_t *)a, (uint32_t *)b, MB_PX / 2));
        TIME("psram read-only 32-bit sum",    { volatile uint32_t acc = 0; const uint32_t *s = (const uint32_t *)a; for (size_t i = 0; i < MB_PX / 2; i++) acc += s[i]; });
        TIME("psram write-only 32-bit fill",  { uint32_t *d = (uint32_t *)b; for (size_t i = 0; i < MB_PX / 2; i++) d[i] = i; });
        TIME("internal 16-bit op loop x14",   for (int k = 0; k < 14; k++) loop16_op(ia, ib, in_px));
        TIME("internal 32-bit op loop x14",   for (int k = 0; k < 14; k++) loop32_op((uint32_t *)ia, (uint32_t *)ib, in_px / 2));
    }
    ESP_LOGI(TAG, "(internal x14 = same pixel count as one frame)");
    heap_caps_free(a); heap_caps_free(b); heap_caps_free(ia); heap_caps_free(ib);
}

static void microbench_task(void *arg)
{
    memory_microbench();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

/* Run the memory micro-benchmark on core 1 and wait for it (call before the camera starts). */
void bench_microbench_on_core1(void)
{
    xTaskCreatePinnedToCore(microbench_task, "microbench", 4096, xTaskGetCurrentTaskHandle(), 5, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void bench_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));

    fx_chain_t chain;
    for (int i = 0; i < fx_registry_count(); i++) {
        fx_chain_clear(&chain);
        fx_chain_add(&chain, fx_registry_get(i));
        frame_pipeline_set_chain(&chain);
        frame_pipeline_set_amount(0.6f);
        frame_pipeline_set_quality(FP_QUALITY_FULL);
        ESP_LOGI(TAG, "=== effect: %s (full) ===", fx_registry_get(i)->id);
        vTaskDelay(pdMS_TO_TICKS(2500));
        frame_pipeline_set_quality(FP_QUALITY_HALF);
        ESP_LOGI(TAG, "=== effect: %s (half) ===", fx_registry_get(i)->id);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    fx_chain_clear(&chain);
    fx_chain_add(&chain, fx_registry_find("chanshift"));
    fx_chain_add(&chain, fx_registry_find("scanline"));
    fx_chain_add(&chain, fx_registry_find("bitcrush"));
    frame_pipeline_set_chain(&chain);
    frame_pipeline_set_quality(FP_QUALITY_FULL);
    ESP_LOGI(TAG, "=== chain: chanshift scanline bitcrush (full) ===");
    vTaskDelay(pdMS_TO_TICKS(2500));
    frame_pipeline_set_quality(FP_QUALITY_HALF);
    ESP_LOGI(TAG, "=== chain (half) ===");
    vTaskDelay(pdMS_TO_TICKS(2500));
    frame_pipeline_set_quality(FP_QUALITY_AUTO);

    /* capture test: one shot with channel shift active; without an SD card the JPEG is
     * printed as base64 so it can be decoded on the PC */
    fx_chain_clear(&chain);
    fx_chain_add(&chain, fx_registry_find("chanshift"));
    frame_pipeline_set_chain(&chain);
    frame_pipeline_set_amount(0.5f);
    vTaskDelay(pdMS_TO_TICKS(1500));
    capture_set_serial_dump(true);
    ESP_LOGI(TAG, "=== capture test ===");
    capture_trigger();
    vTaskDelay(pdMS_TO_TICKS(30000));
    capture_set_serial_dump(false);

    fx_chain_clear(&chain);
    frame_pipeline_set_chain(&chain);
    ESP_LOGI(TAG, "=== effects off ===");
    if (0) {   /* gain sweep done once: register is linear in 1/16 steps, 10 bits */
    auto_exposure_set_locked(true);
    ae_state_t ae; auto_exposure_get(&ae);
    ESP_LOGI(TAG, "exposure locked at %lu lines", (unsigned long)ae.exposure_lines);
    static const uint16_t gains[] = { 0x0010, 0x0018, 0x0020, 0x0030, 0x0040, 0x0080, 0x00f0, 0x00ff, 0x0100, 0x0107, 0x0200, 0x0507 };
    for (size_t i = 0; i < sizeof(gains) / sizeof(gains[0]); i++) {
        ov5647_ctl_set_manual(true, true);
        ov5647_ctl_write(0x350a, gains[i] >> 8);
        ov5647_ctl_write(0x350b, gains[i] & 0xff);
        vTaskDelay(pdMS_TO_TICKS(1500));
        auto_exposure_get(&ae);
        ESP_LOGI(TAG, "gain reg 0x%04x -> luma %u", gains[i], ae.measured_luma);
    }
    /* restore: AGC back to the driver's setting, keep manual exposure */
    ov5647_ctl_write(0x350a, 0x05);
    ov5647_ctl_write(0x350b, 0x07);
    ov5647_ctl_set_manual(true, false);
    auto_exposure_set_locked(false);
    }
    ESP_LOGI(TAG, "=== bench done ===");
    vTaskDelete(NULL);
}

void bench_start(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 4096, NULL, 2, NULL, 0);
}
