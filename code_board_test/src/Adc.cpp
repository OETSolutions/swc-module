#include "Adc.h"

#include <Arduino.h>
#include <math.h>

#include "BoardPins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

namespace Adc {

static adc_oneshot_unit_handle_t s_unit = nullptr;
static adc_cali_handle_t         s_cali = nullptr;
static bool                      s_degraded = true;  // until proven otherwise
static bool                      s_begun = false;

// The pin -> ADC1 channel map, written out rather than computed. The S3's
// GPIO-to-ADC1-channel correspondence is not the identity (IO7 is channel 6, IO1
// is channel 0), so arithmetic here would silently misread one pin. The values
// match the production firmware's AdcPinFor() and the spec's 2.2 table.
struct PinCh { uint8_t pin; adc_channel_t ch; const char *name; };

static const PinCh kMap[kCount] = {
    {PIN_SWC1_ADC, ADC_CHANNEL_0, "SWC1 ladder"},
    {PIN_SWC2_ADC, ADC_CHANNEL_1, "SWC2 ladder"},
    {PIN_TEMP_ADC, ADC_CHANNEL_6, "NTC RT1"},
    {PIN_SENSE1,   ADC_CHANNEL_7, "KEY1 sense /2"},
    {PIN_SENSE2,   ADC_CHANNEL_8, "KEY2 sense /2"},
    {PIN_AUX1,     ADC_CHANNEL_3, "AUX1"},
    {PIN_AUX2,     ADC_CHANNEL_4, "AUX2"},
    {PIN_AUX3,     ADC_CHANNEL_5, "AUX3"},
};

const char *Name(Ch ch) { return (ch >= 0 && ch < kCount) ? kMap[ch].name : "?"; }
uint8_t     Pin(Ch ch)  { return (ch >= 0 && ch < kCount) ? kMap[ch].pin : 0xFF; }

bool Begin()
{
    if (s_begun) return !s_degraded;

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &s_unit) != ESP_OK) {
        s_unit = nullptr;
        return false;
    }

    // 12 dB is the only attenuation whose calibrated ceiling is 2.9 V (spec 2.1).
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    for (int i = 0; i < (int)kCount; ++i) {
        if (adc_oneshot_config_channel(s_unit, kMap[i].ch, &chan_cfg) != ESP_OK) {
            return false;
        }
    }

    // The per-chip eFuse curve. NOT universally available: blank-eFuse module
    // batches return ESP_ERR_NOT_SUPPORTED. The S3 has no per-channel compensation
    // (SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED is unset), so `chan` is ignored.
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = ADC_UNIT_1;
    cali_cfg.chan     = ADC_CHANNEL_0;
    cali_cfg.atten    = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    const esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);

    s_degraded = (err != ESP_OK);
    if (s_degraded) s_cali = nullptr;

    s_begun = true;
    return !s_degraded;
}

bool CalibrationDegraded() { return s_degraded; }

const char *CalibrationSourceName()
{
    return s_degraded ? "LINEAR FALLBACK (eFuse calibration unavailable)"
                      : "eFuse curve-fitting (per-chip)";
}

bool ReadRaw(Ch ch, uint16_t *out_raw)
{
    if (!s_unit) return false;
    int raw = 0;
    if (adc_oneshot_read(s_unit, kMap[ch].ch, &raw) != ESP_OK) return false;
    if (out_raw) *out_raw = (uint16_t)raw;
    return true;
}

// The fallback line. It is a DOCUMENTED approximation, not a claim of accuracy:
// 12-bit counts mapped onto the 0..2.9 V calibrated ceiling (not 3.3 V, and not
// 4095 -- the raw ceiling at 12 dB is ~4095 counts for 2.9 V at the pin).
static uint32_t LinearFallbackMv(uint16_t raw)
{
    return (uint32_t)(((uint32_t)raw * ADC_CEILING_MV_12DB) / ADC_RAW_MAX);
}

bool ReadMv(Ch ch, uint32_t *out_mv, uint16_t *out_raw)
{
    uint16_t raw = 0;
    if (!ReadRaw(ch, &raw)) return false;
    if (out_raw) *out_raw = raw;

    uint32_t mv = 0;
    if (s_cali != nullptr) {
        int v = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &v) == ESP_OK && v >= 0) {
            mv = (uint32_t)v;
        } else {
            mv = LinearFallbackMv(raw);
        }
    } else {
        mv = LinearFallbackMv(raw);
    }
    if (out_mv) *out_mv = mv;
    return true;
}

bool ReadAvgMv(Ch ch, int samples, uint32_t *out_mv, uint16_t *out_raw)
{
    if (samples < 1) samples = 1;
    uint32_t sum = 0;
    uint32_t rawsum = 0;
    int got = 0;
    for (int i = 0; i < samples; ++i) {
        uint32_t mv = 0;
        uint16_t raw = 0;
        if (!ReadMv(ch, &mv, &raw)) continue;
        sum += mv;
        rawsum += raw;
        ++got;
    }
    if (got == 0) return false;
    if (out_mv) *out_mv = sum / (uint32_t)got;
    if (out_raw) *out_raw = (uint16_t)(rawsum / (uint32_t)got);
    return true;
}

}  // namespace Adc
