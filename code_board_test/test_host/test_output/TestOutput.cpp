// Host tests for the output-stage arithmetic.
//
// These are the numbers that decide what voltage lands on a car's steering-wheel
// input line. Two shipped defects lived exactly here (spec 2.3, 6.2):
//
//   1. The gain was written as 1.812 -- a truncated decimal -- instead of the
//      resistor ratio 82/100. The spec calls that a misreading.
//   2. The ADJ channel was put in tracking mode without its code ever being
//      written, so a 3 V head unit was driven at 1.82x. That is the OVER-RANGE
//      direction, the one the spec calls the only dangerous mistake.
//
// (2) cannot happen in this codebase because the transport has no API that could
// express it (see include/Dac.h). (1) is pinned below.

#include <unity.h>

#include "swc_logic/Output.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// The transfer function. V_KEY = (1 + R58/R61)*V_DAC - (R58/R61)*V_ADJ
// ---------------------------------------------------------------------------
static void test_gain_is_the_ratio_not_a_rounded_decimal(void)
{
    // 1 + 82/100 = 1.82 exactly. The numerator/denominator pair is what callers
    // must use; there is deliberately no float "1.82" anywhere in the library.
    TEST_ASSERT_EQUAL_INT(182, Output::kGainNum);
    TEST_ASSERT_EQUAL_INT(100, Output::kGainDen);
    TEST_ASSERT_EQUAL_INT(82, Output::kR58);
    TEST_ASSERT_EQUAL_INT(100, Output::kR61);
}

static void test_amplified_mode_gain_is_1p82(void)
{
    // V_DAC = 1000 mV, V_ADJ = 0 -> 1.82 * 1000 = 1820 mV.
    TEST_ASSERT_EQUAL_INT(1820, Output::KeyMvForDacMv(Output::Mode::kAmplified, 1000));
    TEST_ASSERT_EQUAL_INT(3640, Output::KeyMvForDacMv(Output::Mode::kAmplified, 2000));
}

static void test_tracking_mode_gain_is_1p00(void)
{
    // V_ADJ = V_DAC -> the R58 and R61 terms cancel -> V_KEY = V_DAC.
    TEST_ASSERT_EQUAL_INT(1000, Output::KeyMvForDacMv(Output::Mode::kTracking, 1000));
    TEST_ASSERT_EQUAL_INT(2000, Output::KeyMvForDacMv(Output::Mode::kTracking, 2000));
}

static void test_tracking_mode_adj_tracks_the_signal(void)
{
    // The relationship the production firmware broke. In tracking mode the ADJ
    // voltage is NOT zero and NOT stale -- it equals the signal DAC voltage on
    // every write.
    TEST_ASSERT_EQUAL_INT(1234, Output::AdjMvFor(Output::Mode::kTracking, 1234));
    TEST_ASSERT_EQUAL_INT(2000, Output::AdjMvFor(Output::Mode::kTracking, 2000));
    // In amplified mode the ADJ pin is the defined 1k path, so it is 0.
    TEST_ASSERT_EQUAL_INT(0, Output::AdjMvFor(Output::Mode::kAmplified, 1234));
}

static void test_a_stale_adj_is_the_defect_the_spec_names(void)
{
    // Demonstrates the failure mode in arithmetic, so the regression is
    // documented rather than remembered: a 3 V unit that should be driven at
    // gain 1.00 driven at gain 1.82 instead.
    const int target_key_mv = 2500;
    const int correct = Output::KeyMvForDacMv(Output::Mode::kTracking, target_key_mv);
    const int wrong   = Output::KeyMvForDacMv(Output::Mode::kAmplified, target_key_mv);
    TEST_ASSERT_EQUAL_INT(2500, correct);
    TEST_ASSERT_EQUAL_INT(4550, wrong);
    // 2050 mV of over-range on a 3 V system: the dangerous direction.
    TEST_ASSERT_TRUE(wrong > correct);
}

static void test_dac_scaling_uses_4096_not_4095(void)
{
    // Full scale is 4095/4096 of VREF, one LSB below the reference. Using 4095 in
    // the divisor puts a consistent -0.8 mV bias on every commanded level.
    TEST_ASSERT_EQUAL_INT(0, Output::DacMvForCode(0));
    TEST_ASSERT_EQUAL_INT(3299, Output::DacMvForCode(4095));  // 4095*3300/4096 = 3299.2
    TEST_ASSERT_EQUAL_INT(1650, Output::DacMvForCode(2048));
}

static void test_code_for_dac_mv_roundtrips(void)
{
    for (int mv = 0; mv <= 3300; mv += 7) {
        const uint16_t code = Output::CodeForDacMv(mv);
        const int back = Output::DacMvForCode(code);
        // One LSB is ~0.8 mV; the round trip must be within one LSB.
        TEST_ASSERT_INT_WITHIN(1, mv, back);
    }
}

static void test_code_for_dac_mv_clamps(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, Output::CodeForDacMv(-100));
    TEST_ASSERT_EQUAL_UINT16(4095, Output::CodeForDacMv(3300));
    TEST_ASSERT_EQUAL_UINT16(4095, Output::CodeForDacMv(9000));
}

static void test_code_for_target_rejects_unreachable_targets(void)
{
    // Amplified mode from a 3.3 V DAC tops out at 1.82 * 3.3 = 6.006 V. A target
    // above that is unreachable and must be REJECTED (-1), not saturated to 4095 --
    // a saturated code would be a silent guess.
    TEST_ASSERT_EQUAL_INT(-1, Output::CodeForTargetKeyMv(Output::Mode::kAmplified, 6100));
    TEST_ASSERT_EQUAL_INT(-1, Output::CodeForTargetKeyMv(Output::Mode::kTracking, 3400));
    // Reachable ones return a code.
    TEST_ASSERT_TRUE(Output::CodeForTargetKeyMv(Output::Mode::kAmplified, 5000) > 0);
    TEST_ASSERT_TRUE(Output::CodeForTargetKeyMv(Output::Mode::kTracking, 2500) > 0);
}

static void test_code_for_target_amplified_matches_the_relation(void)
{
    // target 3000 mV at gain 1.82 -> V_DAC = 3000*100/182 = 1648 mV -> code ~2046
    const int code = Output::CodeForTargetKeyMv(Output::Mode::kAmplified, 3000);
    TEST_ASSERT_EQUAL_INT(2046, code);
    // And it reproduces the target through the forward relation.
    TEST_ASSERT_INT_WITHIN(3, 3000, Output::KeyMvForCode(Output::Mode::kAmplified, (uint16_t)code));
}

static void test_code_for_target_tracking_passes_through(void)
{
    const int code = Output::CodeForTargetKeyMv(Output::Mode::kTracking, 2000);
    TEST_ASSERT_EQUAL_INT(Output::CodeForDacMv(2000), code);
    TEST_ASSERT_INT_WITHIN(2, 2000, Output::KeyMvForCode(Output::Mode::kTracking, (uint16_t)code));
}

// ---------------------------------------------------------------------------
// Gain selection (spec 6.2). The asymmetry is the safety argument.
// ---------------------------------------------------------------------------
static void test_select_5v_range_above_the_guard_band(void)
{
    Output::Mode m = Output::Mode::kTracking;
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged5V,
                          (int)Output::SelectFromIdleKeyMv(4200, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Mode::kAmplified, (int)m);
    // Exactly at the boundary: >= 3.4 V is the 5 V range, per the half-open guard
    // band (see Output.cpp -- the two spec steps overlap at 3.4 V and the tie is
    // resolved toward kAmplified, which is the safe direction).
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged5V,
                          (int)Output::SelectFromIdleKeyMv(3400, &m));
}

static void test_select_3v_range_only_on_positive_evidence(void)
{
    Output::Mode m = Output::Mode::kAmplified;
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged3V,
                          (int)Output::SelectFromIdleKeyMv(2200, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Mode::kTracking, (int)m);
    // Just below the guard band.
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged3V,
                          (int)Output::SelectFromIdleKeyMv(2599, &m));
}

static void test_guard_band_does_not_change_the_mode(void)
{
    // The guard band is [2.6, 3.4): indistinguishable, so the spec says do NOT
    // guess -- hold the current mode and re-measure. 3.4 V itself is the 5 V
    // range (see the half-open note in Output.cpp), so it is excluded here.
    for (int mv = 2600; mv < 3400; mv += 100) {
        Output::Mode m = Output::Mode::kAmplified;
        TEST_ASSERT_EQUAL_INT((int)Output::Decision::kGuardBand,
                              (int)Output::SelectFromIdleKeyMv(mv, &m));
        // Unchanged.
        TEST_ASSERT_EQUAL_INT((int)Output::Mode::kAmplified, (int)m);
    }
}

static void test_outside_envelope_is_no_head_unit(void)
{
    Output::Mode m = Output::Mode::kAmplified;
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(0, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(1799, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(5201, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(7000, &m));
}

static void test_envelope_boundaries_are_inclusive(void)
{
    // The envelope (spec 6.2 step 2) is inclusive at both ends: outside it means
    // no head unit. 1800 mV is the low edge and takes the 3 V branch; 5200 mV is
    // the high edge and takes the 5 V branch.
    Output::Mode m = Output::Mode::kAmplified;
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged3V,
                          (int)Output::SelectFromIdleKeyMv(1800, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kRanged5V,
                          (int)Output::SelectFromIdleKeyMv(5200, &m));
    // One millivolt outside either end is no head unit.
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(1799, &m));
    TEST_ASSERT_EQUAL_INT((int)Output::Decision::kNoHeadUnit,
                          (int)Output::SelectFromIdleKeyMv(5201, &m));
}

// ---------------------------------------------------------------------------
// Command band (spec 6.2).
// ---------------------------------------------------------------------------
static void test_clamp_holds_the_upper_bound_below_idle(void)
{
    bool clamped = false;
    // Idle 4.2 V -> the highest reachable command is 4.0 V.
    const int v = Output::ClampCommand(4500, 4200, &clamped);
    TEST_ASSERT_EQUAL_INT(4000, v);
    TEST_ASSERT_TRUE(clamped);
}

static void test_clamp_holds_the_lower_bound_at_the_servo_floor(void)
{
    bool clamped = false;
    const int v = Output::ClampCommand(500, 4200, &clamped);
    TEST_ASSERT_EQUAL_INT(Output::kEnvelopeLowMv, v);
    TEST_ASSERT_TRUE(clamped);
}

static void test_clamp_passes_a_target_inside_the_band(void)
{
    bool clamped = true;
    const int v = Output::ClampCommand(3000, 4200, &clamped);
    TEST_ASSERT_EQUAL_INT(3000, v);
    TEST_ASSERT_FALSE(clamped);
}

static void test_clamp_returns_zero_when_the_band_is_empty(void)
{
    // A head unit idling at 1.9 V leaves no level below its own rest that the
    // servo can still reach (floor is 1.8 V, headroom is 0.2 V). 0 is the shared
    // "nothing to command" value, which the caller turns into a RELEASE rather
    // than driving a guess.
    bool clamped = false;
    TEST_ASSERT_EQUAL_INT(0, Output::ClampCommand(1700, 1900, &clamped));
    // Exactly at the emptiness boundary: 1800 + 200 = 2000 needed.
    TEST_ASSERT_EQUAL_INT(0, Output::ClampCommand(1850, 1950, &clamped));
}

static void test_clamp_treats_no_measurement_as_no_upper_bound(void)
{
    // idle <= 0 means no head unit was measured, so the envelope is the only
    // bound that applies.
    bool clamped = false;
    TEST_ASSERT_EQUAL_INT(5000, Output::ClampCommand(5000, 0, &clamped));
    TEST_ASSERT_EQUAL_INT(Output::kEnvelopeHighMv, Output::ClampCommand(9000, 0, &clamped));
}

// ---------------------------------------------------------------------------
// The sense divider is an exact /2.
// ---------------------------------------------------------------------------
static void test_sense_divider_is_exactly_half(void)
{
    TEST_ASSERT_EQUAL_INT(1000, Output::SenseMvFromKeyMv(2000));
    TEST_ASSERT_EQUAL_INT(2000, Output::KeyMvFromSenseMv(1000));
    // And the ADC can never saturate: the op-amp rail binds first.
    const int max_sense = Output::SenseMvFromKeyMv(4980);
    TEST_ASSERT_TRUE(max_sense < 2900);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_gain_is_the_ratio_not_a_rounded_decimal);
    RUN_TEST(test_amplified_mode_gain_is_1p82);
    RUN_TEST(test_tracking_mode_gain_is_1p00);
    RUN_TEST(test_tracking_mode_adj_tracks_the_signal);
    RUN_TEST(test_a_stale_adj_is_the_defect_the_spec_names);
    RUN_TEST(test_dac_scaling_uses_4096_not_4095);
    RUN_TEST(test_code_for_dac_mv_roundtrips);
    RUN_TEST(test_code_for_dac_mv_clamps);
    RUN_TEST(test_code_for_target_rejects_unreachable_targets);
    RUN_TEST(test_code_for_target_amplified_matches_the_relation);
    RUN_TEST(test_code_for_target_tracking_passes_through);
    RUN_TEST(test_select_5v_range_above_the_guard_band);
    RUN_TEST(test_select_3v_range_only_on_positive_evidence);
    RUN_TEST(test_guard_band_does_not_change_the_mode);
    RUN_TEST(test_outside_envelope_is_no_head_unit);
    RUN_TEST(test_envelope_boundaries_are_inclusive);
    RUN_TEST(test_clamp_holds_the_upper_bound_below_idle);
    RUN_TEST(test_clamp_holds_the_lower_bound_at_the_servo_floor);
    RUN_TEST(test_clamp_passes_a_target_inside_the_band);
    RUN_TEST(test_clamp_returns_zero_when_the_band_is_empty);
    RUN_TEST(test_clamp_treats_no_measurement_as_no_upper_bound);
    RUN_TEST(test_sense_divider_is_exactly_half);
    return UNITY_END();
}
