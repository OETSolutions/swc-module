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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static IHAL *hal = NULL;

void setUp(void)
{
    if (hal == NULL) hal = EspHalInit();
    TEST_ASSERT_NOT_NULL_MESSAGE(hal, "EspHalInit returned NULL on real hardware");
}

void tearDown(void) {}

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

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(low, -1, "sense read failed");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(high, -1, "sense read failed");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(high, low + 200,
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
    const uint64_t t0 = hal->now_ms(hal->ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(t0 + 40, hal->now_ms(hal->ctx));
}

TEST(esp_hal_nvs_round_trips, "[hw]")
{
    const char payload[] = "swc-nvs-probe";
    TEST_ASSERT_EQUAL_INT(0, hal->nvs_set(hal->ctx, "probe", payload, sizeof(payload)));
    char out[sizeof(payload)] = {0};
    TEST_ASSERT_GREATER_THAN_INT(0, hal->nvs_get(hal->ctx, "probe", out, sizeof(out)));
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
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(mid, 100, "sense pin reads as a driven rail");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(mid, 2900, "sense pin reads as a driven rail");
}
