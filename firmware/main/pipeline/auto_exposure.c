#include "esp_log.h"
#include "esp_check.h"
#include "auto_exposure.h"
#include "ov5647_ctl.h"
#include "fx.h"

static const char *TAG = "ae";

static ae_state_t s_ae = {
    .target_luma = 105,
    .max_gain_x16 = 16 * 16,     /* 16x: the driver's own default; beyond it noise dominates */
};
static int s_frame_counter;
static bool s_applied;           /* gain/exposure must be written while streaming (see EFFECTS.md) */

esp_err_t auto_exposure_init(uint32_t start_exposure_lines, uint32_t start_gain_x16, uint32_t max_exposure_lines)
{
    s_ae.max_exposure_lines = max_exposure_lines;
    s_ae.exposure_lines = start_exposure_lines > max_exposure_lines ? max_exposure_lines : start_exposure_lines;
    s_ae.gain_x16 = start_gain_x16;
    /* Manual exposure only. The gain register's encoding on this sensor is not what the
     * datasheet suggests (writes gave black or saturated frames), so gain stays as the
     * driver configured it and only exposure is steered. */
    s_ae.gain_x16 = start_gain_x16 < 16 ? 16 : start_gain_x16;
    /* Only the mode bits now; exposure and gain are written after the first frames arrive,
     * because values written before streaming starts are latched wrongly by this sensor
     * (black or saturated frames were observed). */
    ESP_RETURN_ON_ERROR(ov5647_ctl_set_manual(true, true), TAG, "manual exposure/gain mode");
    ESP_LOGI(TAG, "manual AE: start %lu lines, gain %lu/16, max exposure %lu lines",
             (unsigned long)s_ae.exposure_lines, (unsigned long)s_ae.gain_x16, (unsigned long)max_exposure_lines);
    return ESP_OK;
}

void auto_exposure_feed(const uint16_t *px, int w, int h, int stride_px)
{
    if (++s_frame_counter % 3 != 0) return;      /* let the sensor settle between steps */
    if (!s_applied) {
        if (s_frame_counter < 6) return;          /* streaming for sure now */
        ov5647_ctl_set_exposure_lines(s_ae.exposure_lines);
        ov5647_ctl_set_gain_x16(s_ae.gain_x16);
        s_applied = true;
        return;
    }

    uint32_t sum = 0, n = 0;
    for (int y = 8; y < h; y += 16) {
        const uint16_t *row = px + (size_t)y * stride_px;
        for (int x = 8; x < w; x += 16) {
            sum += fx_luma(row[x]);
            n++;
        }
    }
    if (!n) return;
    uint32_t luma = sum / n;
    s_ae.measured_luma = (uint8_t)luma;
    if (s_ae.locked) return;

    uint32_t target = s_ae.target_luma;
    if (luma == 0) luma = 1;
    /* dead band: within 8% of target, do nothing */
    if (luma * 100 > target * 92 && luma * 100 < target * 108) return;

    /* desired change factor, damped so we don't oscillate */
    float ratio = (float)target / (float)luma;
    if (ratio > 1.6f) ratio = 1.6f;
    if (ratio < 0.6f) ratio = 0.6f;
    ratio = 1.0f + (ratio - 1.0f) * 0.6f;

    uint32_t expo = s_ae.exposure_lines, gain = s_ae.gain_x16;
    if (ratio > 1.0f) {
        /* brighten: exposure first (no noise), then gain */
        if (expo < s_ae.max_exposure_lines) {
            expo = (uint32_t)(expo * ratio);
            if (expo > s_ae.max_exposure_lines) expo = s_ae.max_exposure_lines;
        } else if (gain < s_ae.max_gain_x16) {
            gain = (uint32_t)(gain * ratio);
            if (gain > s_ae.max_gain_x16) gain = s_ae.max_gain_x16;
        }
    } else {
        /* darken: gain first, then exposure */
        if (gain > 16) {
            gain = (uint32_t)(gain * ratio);
            if (gain < 16) gain = 16;
        } else {
            expo = (uint32_t)(expo * ratio);
            if (expo < 8) expo = 8;
        }
    }
    if (expo != s_ae.exposure_lines) {
        s_ae.exposure_lines = expo;
        ov5647_ctl_set_exposure_lines(expo);
    }
    if (gain != s_ae.gain_x16) {
        s_ae.gain_x16 = gain;
        ov5647_ctl_set_gain_x16(gain);
    }
}

void auto_exposure_set_max_exposure(uint32_t lines)
{
    s_ae.max_exposure_lines = lines;
    if (s_ae.exposure_lines > lines) {
        s_ae.exposure_lines = lines;
        ov5647_ctl_set_exposure_lines(lines);
    }
}

void auto_exposure_set_locked(bool locked) { s_ae.locked = locked; }
void auto_exposure_get(ae_state_t *out)     { *out = s_ae; }
