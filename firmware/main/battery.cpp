// battery.cpp — read the LiPo pack voltage through the on-board divider.
//
// The Waveshare ESP32-S3-ePaper-1.54 routes the battery to GPIO4 (ADC1 channel
// 3) through a ~1/2 resistor divider. We sample with 12 dB attenuation (full
// ~3.3 V range — the divided 4.2 V pack is ~2.08 V, comfortably in range),
// average a few reads, apply the on-chip calibration, then scale by the divider.
#include "battery.h"
#include "glane_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BAT_ADC_UNIT   ADC_UNIT_1
#define BAT_ADC_CHAN   ADC_CHANNEL_3      // GPIO4 on ESP32-S3
#define BAT_ATTEN      ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc     = nullptr;
static adc_cali_handle_t         s_cali    = nullptr;
static bool                      s_cali_ok = false;

void battery_init(void)
{
    if (s_adc) return;

    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = BAT_ADC_UNIT;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed");
        s_adc = nullptr;
        return;
    }

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = BAT_ATTEN;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &ccfg);

    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = BAT_ADC_UNIT;
    cal.chan     = BAT_ADC_CHAN;
    cal.atten    = BAT_ATTEN;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);

    ESP_LOGI(TAG, "battery adc ready (cali=%s)", s_cali_ok ? "yes" : "no");
}

int battery_read_mv(void)
{
    if (!s_adc) return 0;

    int acc = 0, got = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) == ESP_OK) {
            acc += raw;
            got++;
        }
    }
    if (got == 0) return 0;
    int raw = acc / got;

    int mv = 0;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    } else {
        mv = raw * 3300 / 4095;   // rough fallback (12 dB ≈ 0..3.3 V)
    }
    return (int)(mv * BAT_DIVIDER);
}

int battery_percent(void)
{
    int mv = battery_read_mv();
    if (mv <= 0) return -1;
    int pct = (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
