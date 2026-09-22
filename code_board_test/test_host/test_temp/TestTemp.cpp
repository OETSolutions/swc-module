// Host tests for the NTC thermistor math.
//
// RT1 is a 10k B3380 NTC against R29 10k (to +3V3), so the ADC reads the NTC's own
// drop. The reason this is tested at all is the denominator: the divider divides
// against the MEASURED 3V3, not an assumed 3.30, and the spec's bring-up step 3
// sweeps that rail 3.14-3.47 V. Hard-coding 3.30 is a several-degree error that
// nothing would report.

#include <unity.h>
#include <math.h>

#include "swc_logic/Temp.h"

void setUp(void) {}
void tearDown(void) {}

// At 25 C the NTC is exactly its nominal 10k, so it drops exactly half the rail:
// a divider of two equal resistors. That is the one point where the expected
// voltage is known with no arithmetic, which makes it the load-bearing assertion.
static void test_midpoint_is_25c(void)
{
    const float c = Temp::CelsiusFromMv(1650.0f, 3300.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 25.0f, c);
}

static void test_resistance_at_midpoint_is_nominal(void)
{
    const float r = Temp::ResistanceFromMv(1650.0f, 3300.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 10000.0f, r);
}

// A colder NTC has HIGHER resistance, so it drops MORE of the rail.
static void test_colder_reads_higher_voltage_and_higher_ohms(void)
{
    const float r0  = Temp::ResistanceFromMv(1900.0f, 3300.0f);
    const float c0  = Temp::CelsiusFromMv(1900.0f, 3300.0f);
    TEST_ASSERT_TRUE(r0 > 10000.0f);
    TEST_ASSERT_TRUE(c0 < 25.0f);
}

static void test_hotter_reads_lower_voltage_and_lower_ohms(void)
{
    const float r0 = Temp::ResistanceFromMv(1000.0f, 3300.0f);
    const float c0 = Temp::CelsiusFromMv(1000.0f, 3300.0f);
    TEST_ASSERT_TRUE(r0 < 10000.0f);
    TEST_ASSERT_TRUE(c0 > 25.0f);
}

// A known B3380 datasheet point: at 0 C a 10k B3380 is ~27.2 kOhm. Compute the
// voltage that resistance would produce and check the round trip returns ~0 C.
static void test_datasheet_point_0c(void)
{
    const float r_expected = 27219.0f;  // ~27.2k at 0 C for R0=10k, B=3380
    // V = 3300 * R / (R + R29)
    const float v = 3300.0f * r_expected / (r_expected + 10000.0f);
    const float c = Temp::CelsiusFromMv(v, 3300.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 0.0f, c);
}

static void test_monotonic_over_the_cabin_range(void)
{
    // Rising pin voltage -> falling temperature, all the way up. The first sample
    // seeds `prev` rather than comparing against it: a sentinel would make the
    // first assertion a comparison against nothing.
    bool seeded = false;
    float prev = 0.0f;
    int checked = 0;
    for (float v = 200.0f; v <= 3100.0f; v += 50.0f) {
        const float c = Temp::CelsiusFromMv(v, 3300.0f);
        TEST_ASSERT_FALSE(isnan(c));
        if (seeded) {
            TEST_ASSERT_TRUE(c < prev);
            checked++;
        }
        prev = c;
        seeded = true;
    }
    // Guard against the loop silently not running.
    TEST_ASSERT_TRUE(checked > 50);
}

// ---------------------------------------------------------------------------
// Non-physical readings must say so, not return a number.
// ---------------------------------------------------------------------------
static void test_zero_voltage_is_rejected(void)
{
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, Temp::ResistanceFromMv(0.0f, 3300.0f));
    TEST_ASSERT_TRUE(isnan(Temp::CelsiusFromMv(0.0f, 3300.0f)));
}

static void test_at_or_above_rail_is_rejected(void)
{
    // NTC shorted, or the pin is being driven. Both are real bring-up faults and
    // both must be reported rather than turned into an absurd resistance.
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, Temp::ResistanceFromMv(3300.0f, 3300.0f));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, Temp::ResistanceFromMv(3400.0f, 3300.0f));
    TEST_ASSERT_TRUE(isnan(Temp::CelsiusFromMv(3300.0f, 3300.0f)));
}

static void test_zero_rail_is_rejected(void)
{
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, Temp::ResistanceFromMv(1000.0f, 0.0f));
}

static void test_non_positive_resistance_is_nan(void)
{
    TEST_ASSERT_TRUE(isnan(Temp::CelsiusFromResistance(0.0f)));
    TEST_ASSERT_TRUE(isnan(Temp::CelsiusFromResistance(-5.0f)));
}

// ---------------------------------------------------------------------------
// The rail assumption. This is the whole reason the function takes the rail.
// ---------------------------------------------------------------------------
static void test_wrong_rail_assumption_produces_a_measurable_error(void)
{
    // Same pin voltage, but computed against 3.30 V instead of the real 3.14 V.
    // The error must be large enough to matter and must have a sign -- if this
    // were ~0 the extra parameter would be pointless and should be removed.
    const float err = Temp::ErrorFromAssumedRailC(1650.0f, 3140.0f, 3300.0f);
    TEST_ASSERT_FALSE(isnan(err));
    TEST_ASSERT_TRUE(fabsf(err) > 0.5f);
}

static void test_correct_rail_gives_no_error(void)
{
    const float err = Temp::ErrorFromAssumedRailC(1650.0f, 3300.0f, 3300.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, err);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_midpoint_is_25c);
    RUN_TEST(test_resistance_at_midpoint_is_nominal);
    RUN_TEST(test_colder_reads_higher_voltage_and_higher_ohms);
    RUN_TEST(test_hotter_reads_lower_voltage_and_lower_ohms);
    RUN_TEST(test_datasheet_point_0c);
    RUN_TEST(test_monotonic_over_the_cabin_range);
    RUN_TEST(test_zero_voltage_is_rejected);
    RUN_TEST(test_at_or_above_rail_is_rejected);
    RUN_TEST(test_zero_rail_is_rejected);
    RUN_TEST(test_non_positive_resistance_is_nan);
    RUN_TEST(test_wrong_rail_assumption_produces_a_measurable_error);
    RUN_TEST(test_correct_rail_gives_no_error);
    return UNITY_END();
}
