#include "Arduino.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
namespace {
adc_oneshot_unit_handle_t unit = nullptr;
adc_cali_handle_t cal[3] = {};
bool configured[3] = {};
} // namespace
void analogSetPinAttenuation(int pin, int) {
    if (pin < 1 || pin > 3)
        return;
    if (!unit) {
        adc_oneshot_unit_init_cfg_t c = {};
        c.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&c, &unit));
    }
    int i = pin - 1;
    if (configured[i])
        return;
    adc_oneshot_chan_cfg_t c = {};
    c.atten = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(unit, adc_channel_t(i), &c));
    adc_cali_curve_fitting_config_t a = {};
    a.unit_id = ADC_UNIT_1;
    a.chan = adc_channel_t(i);
    a.atten = c.atten;
    a.bitwidth = c.bitwidth;
    if (adc_cali_create_scheme_curve_fitting(&a, &cal[i]) != ESP_OK)
        ESP_LOGE("adc", "GPIO%d calibration unavailable; reporting invalid channel", pin);
    configured[i] = true;
}
uint32_t analogReadMilliVolts(int pin) {
    if (pin < 1 || pin > 3)
        return 0;
    int i = pin - 1, raw = 0, mv = 0;
    if (!unit || !cal[i] || adc_oneshot_read(unit, adc_channel_t(i), &raw) != ESP_OK)
        return 0;
    // Saturation is outside the calibrated ADC range; propagate a disconnected/overrange reading.
    if (raw >= 4090)
        return 0;
    return adc_cali_raw_to_voltage(cal[i], raw, &mv) == ESP_OK ? uint32_t(mv) : 0;
}
