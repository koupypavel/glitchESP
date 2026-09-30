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

/* White balance carried over a sensor mode change: the sensor's auto white balance starts
 * again from neutral gains and needs about a second, during which the picture is green.
 * The gains it had before are applied as manual gains for that time, then auto resumes. */
#define WB_HOLD_FRAMES  40
static uint8_t s_wb[6];
static bool s_wb_known, s_wb_hold;

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
    s_frame_counter++;
    if (!s_applied) {
        if (s_frame_counter < 2) return;          /* frames are arriving: streaming for sure */
        ov5647_ctl_set_exposure_lines(s_ae.exposure_lines);
        ov5647_ctl_set_gain_x16(s_ae.gain_x16);
        if (s_wb_hold) {
            for (int i = 0; i < 6; i++) ov5647_ctl_write(OV5647_REG_WB_MANUAL + i, s_wb[i]);
            ov5647_ctl_write(OV5647_REG_WB_CTRL, OV5647_WB_MANUAL_EN);
        }
        s_applied = true;
        return;
    }
    if (s_wb_hold && s_frame_counter >= WB_HOLD_FRAMES) {
        ov5647_ctl_write(OV5647_REG_WB_CTRL, 0);  /* back to the sensor's auto white balance */
        s_wb_hold = false;
    }
    if (s_frame_counter % 3 != 0) return;         /* let the sensor settle between steps */

    uint32_t sum = 0, n = 0, sr = 0, sg = 0, sb = 0;
    for (int y = 8; y < h; y += 16) {
        const uint16_t *row = px + (size_t)y * stride_px;
        for (int x = 8; x < w; x += 16) {
            uint16_t p = row[x];
            sum += fx_luma(p);
            sr += fx_r5(p); sg += fx_g6(p); sb += fx_b5(p);
            n++;
        }
    }
    if (!n) return;
    uint32_t luma = sum / n;
    s_ae.measured_luma = (uint8_t)luma;
    s_ae.mean_r = (uint8_t)((sr << 3) / n);
    s_ae.mean_g = (uint8_t)((sg << 2) / n);
    s_ae.mean_b = (uint8_t)((sb << 3) / n);
    s_ae.frame = (uint32_t)s_frame_counter;
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

bool auto_exposure_read_wb(uint8_t gains[6])
{
    if (s_wb_hold || !s_applied) {                /* not the sensor's own result right now */
        if (!s_wb_known) return false;
        for (int i = 0; i < 6; i++) gains[i] = s_wb[i];
        return true;
    }
    uint8_t g[6];
    for (int i = 0; i < 6; i++) {
        if (ov5647_ctl_read(OV5647_REG_WB_CURRENT + i, &g[i]) != ESP_OK) return false;
    }
    bool neutral = g[0] == 4 && g[2] == 4 && g[4] == 4 && !g[1] && !g[3] && !g[5];
    if (neutral && s_frame_counter < 60) return false;   /* has not had time to settle */
    for (int i = 0; i < 6; i++) gains[i] = s_wb[i] = g[i];
    s_wb_known = true;
    return true;
}

void auto_exposure_restart(uint32_t max_exposure_lines, uint32_t num, uint32_t den, uint32_t gain_num, uint32_t gain_den)
{
    if (gain_den) {
        uint32_t gain = (uint32_t)((uint64_t)s_ae.gain_x16 * gain_num / gain_den);
        s_ae.gain_x16 = gain < 16 ? 16 : (gain > s_ae.max_gain_x16 ? s_ae.max_gain_x16 : gain);
    }
    s_wb_hold = s_wb_known;                       /* gains saved by auto_exposure_read_wb() */
    uint32_t expo = den ? (uint32_t)((uint64_t)s_ae.exposure_lines * num / den) : s_ae.exposure_lines;
    if (expo < 8) expo = 8;
    if (expo > max_exposure_lines) expo = max_exposure_lines;
    s_ae.exposure_lines = expo;
    s_ae.max_exposure_lines = max_exposure_lines;
    ov5647_ctl_set_manual(true, true);
    s_applied = false;               /* write exposure/gain again once frames are flowing */
    s_frame_counter = 0;
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
