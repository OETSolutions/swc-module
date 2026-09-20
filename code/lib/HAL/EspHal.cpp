// EspHal — the only file in the firmware that talks to ESP-IDF drivers.
//
// C++, not C, and that is forced rather than preferred: this file consumes
// Task 4's AdcCalibration, whose header uses `constexpr` and `enum class`. A C
// translation unit cannot include it, and re-implementing the raw-to-millivolt
// conversion in C would be a second home for the fallback rule -- the defect
// class this project keeps hitting. The IDF driver headers are C but every one
// of them is `extern "C"`-guarded, so C++ is the language that can see both.
//
// Everything above this file is host-testable against MockHal; this file is
// tested on the bench (test/test_hw).

#include "HAL/EspHal.h"

#include <string.h>

#include "Analog/CalibrationCurve.h"
#include "HAL/PinMap.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "esp_hal";

// One place the driver state lives. Not exposed through IHAL: the interface is
// the seam the logic uses, and driver handles are this file's business.
struct EspHalState {
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t         cali;
    // The two-point curve the logic uses (Task 4). Derived from the eFuse
    // scheme when available, else the documented linear approximation.
    AdcCalibration            curve;
    bool                      cali_degraded;
    i2c_master_bus_handle_t   i2c_bus;
    i2c_master_dev_handle_t   dac;
    bool                      nvs_open;
};

static EspHalState g_state;

// ---------------------------------------------------------------------------
// ADC channel mapping
//
// IHAL's AdcChannel is the semantic name; the driver wants (unit, channel). All
// eight inputs are on ADC1 (spec 2.2), so the unit is fixed and only the channel
// varies. The mapping is written out rather than computed from the pin number:
// the S3's GPIO-to-ADC1-channel correspondence is not the identity (IO7 is
// channel 6, IO1 is channel 0), so arithmetic here would be a silent
// misread on one pin rather than a compile error.
// ---------------------------------------------------------------------------
struct AdcPin {
    adc_channel_t channel;
    bool          used;
};

static AdcPin AdcPinFor(AdcChannel ch)
{
    switch (ch) {
        case ADC_CH_SWC1:       return {ADC_CHANNEL_0, true};   // IO1
        case ADC_CH_SWC2:       return {ADC_CHANNEL_1, true};   // IO2
        case ADC_CH_TEMP:       return {ADC_CHANNEL_6, true};   // IO7
        case ADC_CH_KEY_SENSE1: return {ADC_CHANNEL_7, true};   // IO8
        case ADC_CH_KEY_SENSE2: return {ADC_CHANNEL_8, true};   // IO9
        case ADC_CH_AUX1:       return {ADC_CHANNEL_3, true};   // IO4
        case ADC_CH_AUX2:       return {ADC_CHANNEL_4, true};   // IO5
        case ADC_CH_AUX3:       return {ADC_CHANNEL_5, true};   // IO6
        case ADC_CH_COUNT:
        default:                return {ADC_CHANNEL_0, false};
    }
}

// ---------------------------------------------------------------------------
// GPIO
// ---------------------------------------------------------------------------
static int    GpioPinFor(GpioPin pin, bool *ok)
{
    *ok = true;
    switch (pin) {
        case GPIO_LED_STAT:   return SWC_PIN_LED_STAT;
        case GPIO_LED2:       return SWC_PIN_LED2;
        case GPIO_BOOT:       return SWC_PIN_BOOT;
        case GPIO_VBUS_VALID: return SWC_PIN_VBUS_VALID;
        case GPIO_COUNT:
        default:              *ok = false; return -1;
    }
}

// ---------------------------------------------------------------------------
// IHAL members
// ---------------------------------------------------------------------------
static int HalAdcReadMv(void *ctx, AdcChannel ch)
{
    (void)ctx;
    const AdcPin pin = AdcPinFor(ch);
    if (!pin.used) return -1;

    int raw = 0;
    if (adc_oneshot_read(g_state.adc, pin.channel, &raw) != ESP_OK) {
        // -1, never 0: zero millivolts is a legal reading (a button shorted to
        // the ladder's common), so returning it for an error would be
        // indistinguishable from a real measurement.
        return -1;
    }
    // The per-chip eFuse curve, applied as the spec's §2.3 requires: via
    // `adc_cali_raw_to_voltage()` on the handle created at init. This call was
    // MISSING -- the handle was created and then ignored, and every reading went
    // through `AdcRawToMilliVolts`, which `AdcCalibrationSelect` populates with
    // the SAME straight line whether or not the eFuse was available. So the
    // factory calibration was computed at boot and discarded, and the device
    // reported `cali_degraded == false` while using the degraded line.
    //
    // The fallback is the linear curve, and only when the eFuse is genuinely
    // absent (spec 3.2: `ESP_ERR_NOT_SUPPORTED` on blank-eFuse batches). The two
    // paths are now genuinely different, which is what `CalibrationSource`
    // promised and did not deliver.
    if (g_state.cali != NULL) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(g_state.cali, raw, &mv) == ESP_OK) {
            return mv;
        }
        // A conversion failure on a live handle falls back to the line rather
        // than returning -1: the reading is usable and dropping it would look
        // like a bus fault. Logged once per call site is too noisy, so it is not
        // logged here -- the init-time path already reports the degraded source.
    }
    return AdcRawToMilliVolts(g_state.curve, (uint16_t)raw);
}

static void HalDacSetCode(void *ctx, DacChannel ch, uint16_t code)
{
    (void)ctx;
    if (g_state.dac == NULL) return;
    if (code > 4095) code = 4095;

    // Which MCP4728 output: A=KEY1, B=ADJ1, C=KEY2, D=ADJ2 (DESIGN.md 4.3).
    uint8_t dac_sel;
    switch (ch) {
        case DAC_CH_KEY1: dac_sel = SWC_MCP4728_MW_DAC0; break;
        case DAC_CH_ADJ1: dac_sel = SWC_MCP4728_MW_DAC1; break;
        case DAC_CH_KEY2: dac_sel = SWC_MCP4728_MW_DAC2; break;
        case DAC_CH_ADJ2: dac_sel = SWC_MCP4728_MW_DAC3; break;
        default: return;
    }

    // Sequenced multi-write, one channel: [cmd] [DAC sel | UDAC] [hi] [lo]
    //
    // UDAC is CLEAR (latched immediately) rather than deferred to ~LDAC. The
    // spec's gain-mode switch depends on a power-down mode being in effect
    // *while* the signal code is written (spec 2.3): with a deferred latch, a
    // later ~LDAC pulse would apply both channels' new values at one instant and
    // the intermediate state is the one the servo sees. Writing through keeps
    // the two independent, which is what dac_power_mode's separate call implies.
    uint8_t frame[4];
    frame[0] = SWC_MCP4728_CMD_MULTI_WRITE;
    frame[1] = (uint8_t)(dac_sel & ~SWC_MCP4728_MW_UDAC);
    frame[2] = (uint8_t)((code >> 8) & 0x0F);
    frame[3] = (uint8_t)(code & 0xFF);

    // The driver retries internally on a bus fault and reports; IHAL's void
    // return is deliberate (spec 6.8), so a persistent failure is logged here
    // rather than pushed onto every call site.
    esp_err_t err = i2c_master_transmit(g_state.dac, frame, sizeof(frame), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac write failed: %s", esp_err_to_name(err));
    }
}

static void HalDacPowerMode(void *ctx, DacChannel ch, DacPowerMode mode)
{
    (void)ctx;
    if (g_state.dac == NULL) return;

    uint8_t dac_sel;
    switch (ch) {
        case DAC_CH_KEY1: dac_sel = SWC_MCP4728_MW_DAC0; break;
        case DAC_CH_ADJ1: dac_sel = SWC_MCP4728_MW_DAC1; break;
        case DAC_CH_KEY2: dac_sel = SWC_MCP4728_MW_DAC2; break;
        case DAC_CH_ADJ2: dac_sel = SWC_MCP4728_MW_DAC3; break;
        default: return;
    }

    // PD1:PD0 in bits 5:4 of the high data byte. The MCP4728 has NO
    // high-impedance state -- every mode is a pull-DOWN (spec 2.3), which is why
    // "release" is a high command rather than a disconnected output.
    uint8_t pd;
    switch (mode) {
        case DAC_POWER_NORMAL:  pd = 0x0; break;
        case DAC_POWER_GND_1K:  pd = 0x1; break;
        case DAC_POWER_GND_100K: pd = 0x2; break;
        case DAC_POWER_GND_500K: pd = 0x3; break;
        default:               pd = 0x0; break;
    }

    uint8_t frame[4];
    frame[0] = SWC_MCP4728_CMD_MULTI_WRITE;
    frame[1] = (uint8_t)(dac_sel & ~SWC_MCP4728_MW_UDAC);
    // Power-down replaces the top data bits, so the code field is 12 bits and
    // the PD bits sit above them in the high byte.
    frame[2] = (uint8_t)(pd << 4);
    frame[3] = 0x00;

    esp_err_t err = i2c_master_transmit(g_state.dac, frame, sizeof(frame), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac power-mode write failed: %s", esp_err_to_name(err));
    }
}

static void HalDacLdac(void *ctx, bool assert)
{
    (void)ctx;
    // ~LDAC is active LOW. The line has an external 10k pulldown (R13), so
    // "not asserted" must actively drive HIGH -- leaving it floating would let
    // the pulldown assert a latch at a random moment.
    gpio_set_level((gpio_num_t)SWC_PIN_DAC_LDAC_B, assert ? 0 : 1);
}

static void HalGpioWrite(void *ctx, GpioPin pin, bool level)
{
    (void)ctx;
    bool ok = false;
    const gpio_num_t gpio = (gpio_num_t)GpioPinFor(pin, &ok);
    if (!ok) return;
    gpio_set_level(gpio, level ? 1 : 0);
}

static bool HalGpioRead(void *ctx, GpioPin pin)
{
    (void)ctx;
    bool ok = false;
    const gpio_num_t gpio = (gpio_num_t)GpioPinFor(pin, &ok);
    if (!ok) return false;
    return gpio_get_level(gpio) != 0;
}

static void HalBuzzerOn(void *ctx, bool on)
{
    (void)ctx;
    // On/off only: BZ1 is an active self-driving buzzer at a fixed ~2.4 kHz, so
    // the entire vocabulary is gating the supply (spec 7.1).
    gpio_set_level((gpio_num_t)SWC_PIN_BUZZ, on ? 1 : 0);
}

static uint64_t HalNowMs(void *ctx)
{
    (void)ctx;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t HalNowUs(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

static int HalNvsGet(void *ctx, const char *key, void *out, size_t len)
{
    (void)ctx;
    if (!g_state.nvs_open) return -1;
    nvs_handle_t h;
    if (nvs_open(SWC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return -1;

    size_t sz = len;
    const esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);

    // -1 for "absent", which is the same signal MockHal produces for an empty
    // NVS, so ConfigStore's kNoConfig path is identical on host and device.
    if (err == ESP_ERR_NVS_NOT_FOUND) return -1;
    if (err != ESP_OK) return -1;
    return (int)sz;
}

static int HalNvsSet(void *ctx, const char *key, const void *in, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(SWC_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return -1;

    esp_err_t err = nvs_set_blob(h, key, in, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? (int)len : -1;
}

static void HalReboot(void *ctx)
{
    (void)ctx;
    esp_restart();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static esp_err_t InitAdc(void)
{
    // Every input on this board is ADC1 (spec 2.2): ADC2 is unusable while WiFi
    // is active on the S3, and normal mode keeps the radio off, but using ADC1
    // throughout means the constraint never has to be re-checked.
    // Zero-initialize and assign: partial designated initialization leaves the
    // other members uninitialized, which IDF builds as an ERROR (-Werror on
    // missing-field-initializers), not a warning.
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &g_state.adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 12 dB attenuation is the ONLY setting whose calibrated ceiling is 2.9 V
    // (spec 2.1). Configure every channel the HAL can be asked for.
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten    = (adc_atten_t)SWC_ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = (adc_bitwidth_t)SWC_ADC_BITWIDTH;
    for (int c = 0; c < (int)ADC_CH_COUNT; ++c) {
        const AdcPin pin = AdcPinFor((AdcChannel)c);
        if (!pin.used) continue;
        err = adc_oneshot_config_channel(g_state.adc, pin.channel, &chan_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc channel %d config failed: %s", (int)pin.channel,
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void InitCalibration(void)
{
    // The per-chip eFuse curve. It is NOT universally available: some module
    // batches ship with blank eFuses, and adc_cali_create_scheme_curve_fitting
    // then returns ESP_ERR_NOT_SUPPORTED (spec 3.2). Never compute
    // raw * 3300 / 4095 as a substitute -- it ignores both the eFuse correction
    // and the 2.9 V ceiling and is wrong by hundreds of millivolts at the top.
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = ADC_UNIT_1;
    // chan is left 0: the S3 has no per-channel calibration compensation
    // (SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED is unset), so the driver ignores it.
    cali_cfg.chan     = ADC_CHANNEL_0;
    cali_cfg.atten    = (adc_atten_t)SWC_ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = (adc_bitwidth_t)SWC_ADC_BITWIDTH;
    const esp_err_t err =
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_state.cali);

    const bool supported = (err == ESP_OK);
    g_state.curve = AdcCalibrationSelect(supported);
    g_state.cali_degraded = !supported;

    if (!supported) {
        // Reported, not silent (spec 3.2): a log at init and BOOT_DEGRADED from
        // the orchestrator, the same class of condition as a config fallback.
        ESP_LOGW(TAG, "eFuse ADC calibration unavailable (%s); using the linear "
                      "approximation -- readings are degraded",
                 esp_err_to_name(err));
    }
}

static esp_err_t InitI2c(void)
{
    // Internal pullups are enabled but are NOT sufficient for 400 kHz on their
    // own; the board has R5/R6 10k to +3V3 (DESIGN.md 4.3), so this only helps
    // during the brief window before the bus is driven.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = -1;   // auto-select a port
    bus_cfg.sda_io_num        = (gpio_num_t)SWC_PIN_I2C_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)SWC_PIN_I2C_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority     = 0;    // 0 = let the driver pick
    bus_cfg.trans_queue_depth = 0;    // unused: all transfers here are synchronous
    // Internal pullups help only during the brief window before the bus is
    // driven; at 400 kHz the external R5/R6 10k are what actually hold the line.
    bus_cfg.flags.enable_internal_pullup = 1;
    bus_cfg.flags.allow_pd               = 0;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_state.i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = SWC_MCP4728_ADDR;
    dev_cfg.scl_speed_hz    = SWC_I2C_FREQ_HZ;
    err = i2c_master_bus_add_device(g_state.i2c_bus, &dev_cfg, &g_state.dac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac device add failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void InitGpio(void)
{
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << (int)SWC_PIN_BUZZ) | (1ULL << (int)SWC_PIN_LED2) |
                           (1ULL << (int)SWC_PIN_LED_STAT) |
                           (1ULL << (int)SWC_PIN_DAC_LDAC_B);
    out_cfg.mode         = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&out_cfg);

    // Only two GPIO inputs exist (spec 2.2). SENSE1/SENSE2 are ADC channels, NOT
    // GPIO -- configuring them here would take the pins away from the ADC and
    // break the servo's feedback path. IO0 is a strapping pin, so it is input
    // only and must never be driven.
    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = (1ULL << (int)SWC_PIN_BOOT) |
                          (1ULL << (int)SWC_PIN_VBUS_VALID);
    in_cfg.mode         = GPIO_MODE_INPUT;
    in_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&in_cfg);

    // Start with the buzzer gated off and both LEDs dark, so nothing is
    // energized before the orchestrator says so.
    gpio_set_level((gpio_num_t)SWC_PIN_BUZZ, 0);
    gpio_set_level((gpio_num_t)SWC_PIN_LED2, 0);
    gpio_set_level((gpio_num_t)SWC_PIN_LED_STAT, 0);
    // ~LDAC HIGH (not asserted): the external pulldown would otherwise latch at
    // an arbitrary time.
    gpio_set_level((gpio_num_t)SWC_PIN_DAC_LDAC_B, 1);
}

IHAL *EspHalInit(void)
{
    memset(&g_state, 0, sizeof(g_state));

    if (InitAdc() != ESP_OK) return NULL;
    InitCalibration();
    InitGpio();

    // A missing DAC is fatal: the whole product is driving the KEY line, and
    // spec 6.8 says never to drive a guessed code. Failing init loudly is better
    // than an orchestrator that believes it is serving presses.
    if (InitI2c() != ESP_OK) return NULL;

    // NVS is NOT fatal. A device with an unreadable namespace is exactly FR-25's
    // pass-through case, so it must still serve. Opening here only proves the
    // partition is mounted; failures fall through to the absent-key signal.
    nvs_handle_t h;
    if (nvs_open(SWC_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        g_state.nvs_open = true;
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs namespace '%s' unavailable; running unconfigured (FR-25)",
                 SWC_NVS_NAMESPACE);
    }

    static IHAL iface;
    memset(&iface, 0, sizeof(iface));
    iface.adc_read_mv    = HalAdcReadMv;
    iface.dac_set_code   = HalDacSetCode;
    iface.dac_power_mode = HalDacPowerMode;
    iface.dac_ldac       = HalDacLdac;
    iface.gpio_write     = HalGpioWrite;
    iface.gpio_read      = HalGpioRead;
    iface.buzzer_on      = HalBuzzerOn;
    iface.now_ms         = HalNowMs;
    iface.now_us         = HalNowUs;
    iface.nvs_get        = HalNvsGet;
    iface.nvs_set        = HalNvsSet;
    iface.reboot         = HalReboot;
    iface.ctx            = &g_state;
    return &iface;
}

bool EspHalCalibrationIsDegraded(void)
{
    return g_state.cali_degraded;
}
