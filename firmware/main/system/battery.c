#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "battery.h"

static const char *TAG = "battery";

#define BAT_GPIO        20
#define BAT_DIV_NUM     3           /* 200 k over 100 k: the pin sees a third of the cell voltage */
#define BAT_DIV_DEN     1
#define SAMPLE_MS       2000
#define SAMPLES         16          /* averaged per reading */

#define MV_CHARGING     4230        /* a cell alone does not stay above this */
#define MV_NO_CELL      4450        /* only the charger's output gets this high */
#define MV_CRITICAL     3400
#define TREND_SAMPLES   30          /* a minute of readings */
#define MV_RISE         12          /* a rise this large over that minute means a charger is on */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static battery_info_t s_info = { .state = BATTERY_UNKNOWN, .percent = -1 };

/* Open-circuit voltage -> charge for a typical LiPo cell, in 5 % steps from empty to full. */
static const uint16_t k_curve[21] = {
    3300, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820, 3840,
    3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200,
};

static int percent_of(uint32_t mv)
{
    if (mv <= k_curve[0]) return 0;
    if (mv >= k_curve[20]) return 100;
    int i = 0;
    while (mv > k_curve[i + 1]) i++;
    return i * 5 + (int)((mv - k_curve[i]) * 5 / (k_curve[i + 1] - k_curve[i]));
}

static bool read_pin_mv(uint32_t *mv)
{
    int sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw = 0, v = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) return false;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &v) != ESP_OK) return false;
        } else {
            v = raw * 3300 / 4095;                         /* uncalibrated estimate */
        }
        sum += v;
    }
    *mv = (uint32_t)(sum / SAMPLES);
    return true;
}

static void battery_task(void *arg)
{
    (void)arg;
    uint32_t smooth = 0;
    uint32_t hist[TREND_SAMPLES] = { 0 };
    int n = 0;
    for (;;) {
        uint32_t pin;
        if (read_pin_mv(&pin)) {
            uint32_t mv = pin * BAT_DIV_NUM / BAT_DIV_DEN;
            /* the camera's load makes the voltage dip with every frame: smooth it heavily,
             * but follow a plug or unplug at once */
            if (!smooth || (mv > smooth + 150) || (mv + 150 < smooth)) smooth = mv;
            else smooth = (smooth * 7 + mv) / 8;
            uint32_t old = hist[n % TREND_SAMPLES];             /* a minute ago, 0 at first */
            hist[n % TREND_SAMPLES] = smooth;
            n++;
            bool rising = old && smooth >= old + MV_RISE;
            battery_info_t b = { .mv = smooth, .pin_mv = pin };
            if (smooth >= MV_NO_CELL) b.state = BATTERY_NONE;
            else if (smooth >= MV_CHARGING || rising) b.state = BATTERY_CHARGING;
            else b.state = BATTERY_DISCHARGING;
            b.percent = b.state == BATTERY_NONE ? -1 : percent_of(smooth);
            b.low = b.state == BATTERY_DISCHARGING && b.percent < 10;
            b.critical = b.state == BATTERY_DISCHARGING && smooth < MV_CRITICAL;
            s_info = b;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

esp_err_t battery_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BAT_GPIO, &unit, &s_chan), TAG, "GPIO%d is no ADC pin", BAT_GPIO);
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "ADC unit");
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &ccfg), TAG, "ADC channel");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = { .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = { .unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    if (!s_cali) ESP_LOGW(TAG, "no ADC calibration: readings are approximate");
    ESP_LOGI(TAG, "battery on GPIO%d (ADC%d channel %d)", BAT_GPIO, (int)unit + 1, (int)s_chan);
    BaseType_t ok = xTaskCreatePinnedToCore(battery_task, "battery", 3072, NULL, 2, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void battery_get(battery_info_t *out)
{
    *out = s_info;
}
