// On-device tests for EspHal. Unity, run with `pio test -e esp32s3 -f test_hw`.
//
// These assert what only real silicon can answer -- bus behaviour, calibration
// availability, pin direction. Everything with logic is covered by the host
// suite against MockHal, so nothing here re-tests that.
//
// NOTE 1: an earlier revision of the plan used ADC_CH_SENSE1/ADC_CH_SENSE2,
// which do not exist. The IHAL enumerators are ADC_CH_KEY_SENSE1/2 (spec 10.2);
// this file uses the real names so it compiles.
//
// NOTE 2: the test-definition macro takes an UNQUOTED identifier, and the name
// is stringified inside the macro. Unity 2.6.1 defines no `TEST` at all, and
// `TEST_CASE` is the PARAMETERIZED-test decorator (empty unless
// UNITY_SUPPORT_TEST_CASES is set), so the plan's version of this file --
// which used quoted `TEST_CASE("name", "[tag]")` throughout -- could never have
// compiled against this Unity. The macro now lives in test/unity_config.h.

#include "unity.h"

#include "HAL/EspHal.h"
#include "HAL/PinMap.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static IHAL *hal = NULL;

static void swc_setup(void)
{
    if (hal == NULL) hal = EspHalInit();
    TEST_ASSERT_NOT_NULL_MESSAGE(hal, "EspHalInit returned NULL on real hardware");
}

static void swc_teardown(void) {}

TEST(esp_hal_init_succeeds_on_this_board, "[hw]")
{
    TEST_ASSERT_NOT_NULL(hal);
    TEST_ASSERT_NOT_NULL(hal->ctx);
}

TEST(esp_hal_adc_is_stable_and_in_range, "[hw]")
{
    // With nothing connected, both ladder channels sit at their pull-up level.
    // The assertion is only that the value is stable and plausible, so a wiring
    // fault shows up as an implausible reading rather than a silently working
    // test. 2900 is the calibrated 12 dB attenuation limit (spec 2.1).
    const int a = hal->adc_read_mv(hal->ctx, ADC_CH_SWC1);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int b = hal->adc_read_mv(hal->ctx, ADC_CH_SWC1);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, a);
    TEST_ASSERT_LESS_OR_EQUAL_INT(2900, a);
    TEST_ASSERT_INT_WITHIN(80, a, b);   // not flapping
}

TEST(esp_hal_adc_reports_error_as_negative_not_zero, "[hw]")
{
    // Zero millivolts is a LEGAL reading (a button shorted to the ladder's
    // common), so a driver error must not be reported as 0 -- the two would be
    // indistinguishable to every caller (spec 6.3 consequence 2).
    const int v = hal->adc_read_mv(hal->ctx, ADC_CH_COUNT);
    TEST_ASSERT_EQUAL_INT(-1, v);
}

TEST(esp_hal_dac_write_reaches_the_sense_divider, "[hw]")
{
    // Drive the KEY line high, then read it back through the exact /2 divider
    // (spec 2.3). This is the one test that exercises the whole analog path --
    // I2C, DAC, gain channel, servo, sense buffer -- at once.
    hal->dac_power_mode(hal->ctx, DAC_CH_ADJ1, DAC_POWER_GND_1K);
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int low = hal->adc_read_mv(hal->ctx, ADC_CH_KEY_SENSE1);

    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 4095);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int high = hal->adc_read_mv(hal->ctx, ADC_CH_KEY_SENSE1);

    // NOTE the argument order: `TEST_ASSERT_GREATER_THAN_INT_MESSAGE(threshold,
    // actual, msg)` asserts `actual > threshold`. Written the other way round
    // (the reading first) these three were comparing the CONSTANT against the
    // reading, so a valid measurement was reported as `Expected <constant> to be
    // greater than <reading>` -- e.g. "Expected -1 to be greater than 505", with
    // 505 mV a perfectly good driven level. Every assertion here was backwards
    // and the test could only ever fail.
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, low, "sense read failed");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(-1, high, "sense read failed");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(low + 200, high,
        "a full-scale code must move the KEY line measurably");
}

TEST(esp_hal_ldac_idles_high_so_the_pulldown_cannot_latch, "[hw]")
{
    // R13 is a 10k pulldown on ~LDAC, which is active LOW. Leaving the pin
    // floating would let that pulldown assert a latch at an arbitrary moment,
    // so "not asserted" must actively drive HIGH.
    hal->dac_ldac(hal->ctx, false);
    TEST_ASSERT_EQUAL_INT(1, gpio_get_level((gpio_num_t)SWC_PIN_DAC_LDAC_B));
    hal->dac_ldac(hal->ctx, true);
    TEST_ASSERT_EQUAL_INT(0, gpio_get_level((gpio_num_t)SWC_PIN_DAC_LDAC_B));
    hal->dac_ldac(hal->ctx, false);
}

TEST(esp_hal_clock_advances, "[hw]")
{
    // Compare as UINT32, not UINT64. `now_ms` returns uint64_t, but Unity's
    // 64-bit assertions are a STUB on this target: UNITY_SUPPORT_64 turns
    // itself on only when UNITY_LONG_WIDTH or UNITY_POINTER_WIDTH is 64, and
    // both are 32 on xtensa. The UINT64 macro therefore expands to
    // UNITY_TEST_FAIL("not supported") -- so the test could never pass AND
    // asserted nothing, which is why `t0` was reported unused. The low 32 bits
    // suffice for a 50 ms interval (a wrap needs ~49 days of uptime).
    const uint32_t t0 = (uint32_t)hal->now_ms(hal->ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    const uint32_t t1 = (uint32_t)hal->now_ms(hal->ctx);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(t0 + 40, t1);
}

TEST(esp_hal_nvs_round_trips, "[hw]")
{
    const char payload[] = "swc-nvs-probe";
    // 0 on success, NOT the byte count. An earlier revision returned `len`, and
    // every consumer treats nonzero as failure (ConfigStore's `!= 0`), so this
    // assertion is what keeps a device save from silently never persisting.
    TEST_ASSERT_EQUAL_INT(0, hal->nvs_set(hal->ctx, "probe", payload, sizeof(payload)));
    char out[sizeof(payload)] = {0};
    TEST_ASSERT_GREATER_THAN_INT(0, hal->nvs_get(hal->ctx, "probe", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(payload, out);
}

TEST(esp_hal_nvs_get_rejects_an_undersized_buffer, "[hw]")
{
    // nvs_get_blob returns ESP_ERR_NVS_INVALID_LENGTH (mapped to -1) when the
    // caller's buffer is smaller than the stored value -- it does NOT truncate.
    // ConfigStore relies on this to detect a short read, and reading a 2048-byte
    // chunk through a 16-byte buffer is exactly the bug that made every config
    // load fail on the device while the (truncating) mock passed on the host.
    const char payload[] = "swc-nvs-width-probe";
    TEST_ASSERT_EQUAL_INT(0, hal->nvs_set(hal->ctx, "width", payload, sizeof(payload)));
    char small[4] = {0};
    TEST_ASSERT_EQUAL_INT(-1, hal->nvs_get(hal->ctx, "width", small, sizeof(small)));
    char full[sizeof(payload)] = {0};
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload),
                          hal->nvs_get(hal->ctx, "width", full, sizeof(full)));
}

TEST(esp_hal_nvs_is_mounted_so_a_write_succeeds, "[hw]")
{
    // The mount itself. If `nvs_flash_init()` were never called, EVERY nvs_open
    // returns ESP_ERR_NVS_NOT_INITIALIZED and this write fails -- which is exactly
    // the state the firmware shipped in until this test existed: config and learn
    // persistence silently did nothing on hardware while the host suite passed
    // (MockHal has no mount step). EspHalInit is what mounts it, and setUp has
    // already called that.
    const char payload[] = "swc-nvs-mounted";
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, hal->nvs_set(hal->ctx, "mounted", payload, sizeof(payload)),
        "nvs_set failed -- is NVS mounted (nvs_flash_init in EspHalInit)?");
    char out[sizeof(payload)] = {0};
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload),
                          hal->nvs_get(hal->ctx, "mounted", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(payload, out);
}

TEST(esp_hal_nvs_read_back_sees_a_save_in_the_same_session, "[hw]")
{
    // The fresh-board ordering that a cached availability flag broke: on a
    // factory-fresh device the `swc` namespace does not exist, so a boot-time
    // READONLY open fails (ESP_ERR_NVS_NOT_FOUND). The FIRST save creates the
    // namespace via a READWRITE open -- and a read in that SAME power cycle must
    // then see it. A cached boot flag left every get reporting "absent" until the
    // next reset, so a config saved over USB looked lost until the device was
    // rebooted.
    const char payload[] = "swc-nvs-fresh-read";
    TEST_ASSERT_EQUAL_INT(0, hal->nvs_set(hal->ctx, "fresh", payload, sizeof(payload)));
    char out[sizeof(payload)] = {0};
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload),
                          hal->nvs_get(hal->ctx, "fresh", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(payload, out);
}

TEST(esp_hal_nvs_reports_an_absent_key_as_negative, "[hw]")
{
    // ConfigStore distinguishes "absent" from "present but short" by this
    // signal, and on the device it must come from ESP_ERR_NVS_NOT_FOUND.
    char out[32] = {0};
    TEST_ASSERT_EQUAL_INT(-1, hal->nvs_get(hal->ctx, "no-such-key-xyz", out, sizeof(out)));
}

TEST(esp_hal_sense_pins_read_as_analog_not_driven, "[hw]")
{
    // Spec 2.2 marks /SENSE1 and /SENSE2 A-in, and the servo trim loop samples
    // them every tick. If a later change configured them as GPIO OUTPUTS, the
    // ADC would read our own driven level instead of the KEY line and the trim
    // loop would chase its own output.
    //
    // This does not (and cannot) read back the pad's direction: IDF exposes no
    // public direction query, and for an ADC pin the GPIO matrix's view is not
    // the pad's. What it CAN assert is the consequence -- an ADC channel read
    // returns a plausible value rather than a hard 0/2900 rail, which is what a
    // wrongly-driven output would produce. A tautological direction check lived
    // here first; it asserted nothing, which is worse than no test.
    hal->dac_power_mode(hal->ctx, DAC_CH_ADJ1, DAC_POWER_GND_1K);
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 2048);   // mid-scale
    vTaskDelay(pdMS_TO_TICKS(20));
    const int mid = hal->adc_read_mv(hal->ctx, ADC_CH_KEY_SENSE1);
    // `(threshold, actual)` -- see the note in `esp_hal_dac_write_reaches_the_
    // sense_divider`. These two were ALSO reversed, so the lower bound was
    // compared as `100 > mid` and failed for every real reading above 100 mV.
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(100, mid, "sense pin reads as a driven rail");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(2900, mid, "sense pin reads as a driven rail");
}

// Spec 10.5's free-heap gate: ">= 20% free at worst-case steady state".
//
// **This gate did not exist until 2026-09-25.** 10.5 listed it with the command
// "runtime assertion in the device test", and no device test asserted it -- the
// N-33 shape (a gate reported as covered by nothing that runs it). It is
// implemented here because that is where the spec put it, and the RATIO is
// computed at runtime from the device's OWN figures rather than a magic constant:
// a hardcoded threshold would pass a future build whose heap grew.
//
// **What this does and does not establish.** It measures the DEVICE-TEST image,
// which does not boot the orchestrator, the TinyUSB link or the radio -- so its
// free heap is HIGHER than the production image's, and the test is therefore a
// LOWER BOUND, not the production figure. The production image's real number is
// peer-visible in `status.heap_free` (the IHAL::heap_free producer), and the
// maintenance window's depth is measured in FR-32's row. Stated here rather than
// left implicit, because a gate that silently measures the wrong image is exactly
// the size-gate defect (10.5's own note) one resource over.
TEST(esp_hal_free_heap_headroom_meets_the_gate, "[hw]")
{
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, total,
        "no default heap region was registered -- the ratio would be meaningless");
    // >= 20% free (spec 10.5). Integer math, no float: 5*free >= 1*total.
    TEST_ASSERT_TRUE_MESSAGE(freeb * 5 >= total,
        "free heap is below 20% of the default heap region (spec 10.5 gate)");
}
