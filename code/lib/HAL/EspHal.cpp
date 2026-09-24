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
#include "HAL/DacFrame.h"
#include "HAL/DacRetry.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
// `RTC_CNTL_OPTION1_REG` / `RTC_CNTL_FORCE_DOWNLOAD_BOOT` for the software
// download-boot request (spec 4.3's `boot_target: "bootloader"`). The register
// header is per-target (`soc/esp32s3/register/soc/rtc_cntl_reg.h`, reached
// through `soc/rtc_cntl_reg.h`), and it pulls in `soc/soc.h` for `REG_SET_BIT`.
#include "soc/rtc_cntl_reg.h"

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
    // Latched by any failed DAC write. See HalDacSetCode and IHAL::dac_faulted.
    bool                      dac_failed = false;
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
    if (!DacFrame::SelectForChannel(static_cast<uint8_t>(ch), &dac_sel)) return;

    // Multi-write, one channel: THREE bytes, with the command type, the channel
    // select and UDAC packed into byte 0 (DS22187E Figure 5-8). DacFrame.h owns
    // the field layout AND the reason it is host-testable rather than inline
    // here -- EspHal is the one file the host build excludes, so a byte layout
    // written inline is checked by nothing.
    //
    // UDAC is CLEAR (0), so the addressed channel's output updates on the final
    // ACK with no ~LDAC pulse. That matters for the gain switch: spec 2.3 wants
    // a power-down mode in effect *while* the signal code is written, and a
    // deferred latch would apply both channels' new values at one instant.
    uint8_t frame[DacFrame::kSize];
    DacFrame::EncodeSet(frame, dac_sel, 0 /* VREF = VDD */, 0 /* normal power */,
                        0 /* gain x1 */, code);

    // Spec 6.8's "retry with backoff". The policy (3 attempts, 1 ms then 2 ms) is
    // in DacRetry.h rather than inline here, because EspHal is excluded from the
    // host build and a loop written here would be executed by no test. What this
    // comment used to claim -- that the driver retries internally -- was false:
    // `i2c_master_transmit` is one synchronous transaction that delegates straight
    // to `i2c_multi_buffer_transmit` (no loop), so a NACK or timeout was logged
    // once and the write was lost.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < DacRetry::kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(DacRetry::BackoffMsBefore(attempt)));
        }
        err = i2c_master_transmit(g_state.dac, frame, sizeof(frame), 100);
        if (err == ESP_OK) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac write failed after %d attempts: %s", DacRetry::kMaxAttempts,
                 esp_err_to_name(err));
        // "Never drive a guessed code" (spec 6.8) is already honoured: the write
        // was refused, so the channel keeps whatever code it held. What the row
        // also asks for -- "release the line and report a fault" -- is the
        // LATCH below. It is never cleared, because spec 7.3's reboot-only rule
        // covers a hardware condition that does not fix itself. FR-37's health
        // gate reads it through `dac_faulted`; without it the gate had no signal
        // that could say NO, so it was constant-true and would confirm a bricked
        // image.
        g_state.dac_failed = true;
    }
}

static bool HalDacFaulted(void *ctx)
{
    (void)ctx;
    return g_state.dac_failed;
}

static void HalDacPowerMode(void *ctx, DacChannel ch, DacPowerMode mode)
{
    (void)ctx;
    if (g_state.dac == NULL) return;

    uint8_t dac_sel;
    if (!DacFrame::SelectForChannel(static_cast<uint8_t>(ch), &dac_sel)) return;

    // Power-down replaces the top data bits, so the code field is 12 bits and
    // PD1:PD0 sits above them in byte 1 (DacFrame::EncodeSet). The MCP4728 has
    // NO high-impedance state -- every mode is a pull-DOWN (spec 2.3), which is
    // why "release" is a high command rather than a disconnected output.
    //
    // The 12-bit code is written as 0: a power-mode write is a mode change, not
    // a code change, and the channel's stored code is re-driven on the next
    // code write. Keeping the two independent is exactly why they are separate
    // IHAL members.
    uint8_t frame[DacFrame::kSize];
    DacFrame::EncodeSet(frame, dac_sel, 0 /* VREF = VDD */,
                        DacFrame::PowerDownCode(mode), 0 /* gain x1 */,
                        0 /* code field */);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < DacRetry::kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(DacRetry::BackoffMsBefore(attempt)));
        }
        err = i2c_master_transmit(g_state.dac, frame, sizeof(frame), 100);
        if (err == ESP_OK) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac power-mode write failed after %d attempts: %s",
                 DacRetry::kMaxAttempts, esp_err_to_name(err));
        // Same latch as the code write: a gain-mode write that did not land
        // leaves the output on the wrong gain, which is precisely "cannot do its
        // job" for FR-37's gate.
        g_state.dac_failed = true;
    }
}

/*
 * FR-13's "VERIFY the DAC is in the safe state" / spec 6.1 step 3b.
 *
 * The MCP4728 Read Command (DS22187E Fig 5-15, §5.6.5) is the address byte with
 * R/W = 1 and then 24 sequential bytes: 3 of input register + 3 of EEPROM, per
 * channel A to D. `DacFrame::DecodeReadCode` owns the byte layout, here only the
 * transfer and the fault accounting.
 *
 * **This is a CHECK, not the write path's arbiter.** A read that fails latches
 * the same fault as a failed write (the bus is not answering, which is exactly
 * what §6.8's "persistent" fault is), but a read that succeeds never CLEARS the
 * latch: spec 7.3's rule is reboot-only, and a transient that recovered is still
 * a bus that failed. It also deliberately does not retry -- the retry policy
 * exists to get a code ONTO the part, and a failed verification is a reportable
 * fact rather than something to paper over.
 */
static bool HalDacReadCode(void *ctx, DacChannel ch, uint16_t *out)
{
    (void)ctx;
    if (g_state.dac == NULL || out == nullptr) return false;
    uint8_t dac_sel;
    if (!DacFrame::SelectForChannel(static_cast<uint8_t>(ch), &dac_sel)) return false;

    uint8_t buf[DacFrame::kReadBytes] = {};
    esp_err_t err = i2c_master_receive(g_state.dac, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac read failed: %s", esp_err_to_name(err));
        g_state.dac_failed = true;
        return false;
    }
    return DacFrame::DecodeReadCode(buf, dac_sel, out);
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

static uint32_t HalHeapFree(void *ctx)
{
    (void)ctx;
    // The default-capability malloc's free total, which is the number a
    // fragmentation or leak question is actually about. `esp_get_free_heap_size`
    // reports the internal-region figure; the minimum-ever value is a different
    // question (a low-water mark, not the current headroom) and is deliberately
    // not what a periodic `status` reports.
    return static_cast<uint32_t>(esp_get_free_heap_size());
}

static int HalResetReason(void *ctx)
{
    (void)ctx;
    // IDF's reset-reason enum, exposed so the app can show WHY the device last
    // started (spec 4.3's `status.reset_reason`). Notably, FR-40's watchdog
    // recovery is observable this way from the host: the console is on the ROM
    // USB-Serial-JTAG, which the firmware stops emitting to once TinyUSB takes the
    // PHY, so the reset reason is the only reset source a peer can confirm.
    return static_cast<int>(esp_reset_reason());
}

static bool HalCalibrationDegraded(void *ctx)
{
    (void)ctx;
    // Spec 3.2's "fall back AND report that it did" (spec 4.3's
    // `status.calibration_degraded`). False means the eFuse curve is in use.
    return g_state.cali_degraded;
}

static int HalNvsGet(void *ctx, const char *key, void *out, size_t len)
{
    (void)ctx;
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
    // 0 on success, nonzero on failure -- the SAME contract MockHal::NvsSet
    // keeps. It used to return `len` on success, and every consumer treats
    // nonzero as a write failure (`ConfigStore::WriteSlot`'s `!= 0`), so on a
    // real device EVERY NVS write read as failed: Save() always returned false,
    // cfg_seq never advanced, and a config or a headless learn could never
    // persist -- while passing on the host, where the mock returns 0. The
    // on-device suite asserts `== 0` (test/test_hw/TestEspHal.c), so this was a
    // contract the two HALs disagreed on with no host test able to see it.
    return (err == ESP_OK) ? 0 : -1;
}

static void HalReboot(void *ctx)
{
    (void)ctx;
    esp_restart();
}

// Restart into the ROM USB download bootloader (spec 4.3's `boot_target:
// "bootloader"`). The ESP32-S3 ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT`
// (`RTC_CNTL_OPTION1_REG` bit 0) on EVERY reset, and that register is in the RTC
// domain, so it survives `esp_restart()` (a software reset) but not a power
// cycle -- which is exactly the lifetime a developer affordance wants: ask, and
// the next boot is the download stub; unplug, and the device is normal again.
//
// **This is IDF's own idiom, not a trick.** `esp_usb_console_before_restart`
// (components/esp_system/port/usb_console.c) writes this same register for its
// `REBOOT_BOOTLOADER` path, and esptool's `ESP32S3ROM.hard_reset` CLEARS the same
// bit before it resets -- so a flashing tool already knows to clear it, and a
// device asked to enter download mode is not trapped there afterwards.
//
// Written with the struct-typed register rather than a raw address so the field
// and the bit position come from the SOC headers for this exact target; a
// hard-coded address is the kind of second source of truth that goes stale
// silently on a part change.
static void HalRebootToDownload(void *ctx)
{
    (void)ctx;
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
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
        // Reported, not silent (spec 3.2): a log at init here, and BOOT_DEGRADED
        // from the orchestrator, the same class of condition as a config fallback.
        // The orchestrator gets the flag via `SystemOrchestratorCreate`'s argument,
        // read from `EspHalCalibrationIsDegraded` in `src/main.cpp` -- this file is
        // the one the host build excludes, so the orchestrator cannot call into it.
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

    // NVS MUST BE MOUNTED before any nvs_open, and this call was MISSING: nothing
    // in the firmware ever called `nvs_flash_init()`, so on a real device every
    // nvs_open returned ESP_ERR_NVS_NOT_INITIALIZED. The namespace probe below,
    // every HalNvsGet, and every HalNvsSet all failed silently -- so a config or a
    // headless learn could never be saved or loaded on hardware, while the host
    // suite passed throughout against MockHal (which has no mount step). This is
    // the same device-only blind spot as the HalNvsSet/HalNvsGet return-contract
    // bugs, one layer down.
    //
    // Not fatal, and the same reasoning as the namespace probe below: a device
    // whose NVS cannot be mounted is FR-25's pass-through case, so it must still
    // serve presses. The difference is that a mount failure with NO free pages or
    // a version mismatch is recoverable by erasing, which is the standard IDF
    // idiom -- and worth doing, because the alternative is a device that appears
    // to save a config and forgets it at reboot.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs a format (%s); erasing and retrying", esp_err_to_name(nvs_err));
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init failed: %s; running unconfigured (FR-25)",
                 esp_err_to_name(nvs_err));
    }

    // NVS is NOT fatal. A device with an unreadable namespace is exactly FR-25's
    // pass-through case, so it must still serve. The open is NOT cached as a
    // boot-time gate: nvs_open(READONLY) fails with ESP_ERR_NVS_NOT_FOUND on a
    // factory-fresh board, where the namespace does not exist until the first
    // Save creates it -- so a cached "unavailable" flag stayed false for the
    // whole first power cycle and made every HAL nvs_get report "absent" even
    // right after a successful save. Each get/set opens for itself (cheap, and
    // it is what makes a same-session read-back see the write).
    nvs_handle_t h;
    if (nvs_open(SWC_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "nvs namespace '%s' not present yet; it is created on first "
                      "save (FR-25 pass-through until then)",
                 SWC_NVS_NAMESPACE);
    }

    static IHAL iface;
    memset(&iface, 0, sizeof(iface));
    iface.adc_read_mv    = HalAdcReadMv;
    iface.dac_set_code   = HalDacSetCode;
    iface.dac_power_mode = HalDacPowerMode;
    iface.dac_ldac       = HalDacLdac;
    iface.dac_read_code  = HalDacReadCode;
    iface.dac_faulted    = HalDacFaulted;
    iface.gpio_write     = HalGpioWrite;
    iface.gpio_read      = HalGpioRead;
    iface.buzzer_on      = HalBuzzerOn;
    iface.now_ms         = HalNowMs;
    iface.now_us         = HalNowUs;
    iface.heap_free      = HalHeapFree;
    iface.reset_reason   = HalResetReason;
    iface.calibration_degraded = HalCalibrationDegraded;
    iface.nvs_get        = HalNvsGet;
    iface.nvs_set        = HalNvsSet;
    iface.reboot         = HalReboot;
    iface.reboot_to_download = HalRebootToDownload;
    iface.ctx            = &g_state;
    return &iface;
}

bool EspHalCalibrationIsDegraded(void)
{
    return g_state.cali_degraded;
}
