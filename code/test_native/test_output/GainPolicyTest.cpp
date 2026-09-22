#include "Output/GainPolicy.h"
#include <gtest/gtest.h>

TEST(GainConstants, AreExactlyTheDividerValuesAndNotTheRoundedDecimal) {
    // 1 + 82/100 = 1.82 exactly. A tempting "1.812" comes from misreading the
    // resistor pair; the code must use the ratio, not a decimal approximation.
    EXPECT_EQ(kGainR58, 82);
    EXPECT_EQ(kGainR61, 100);
}

TEST(GainPolicy, AutoPicksAmplifiedWhenTheHeadUnitIdleSitsInTheGuardBand) {
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 3000), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 2600), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 3400), GainMode::kAmplified);
}

TEST(GainPolicy, AutoPicksTrackingWhenTheHeadUnitIdleIsLow) {
    // A 1.9V idle is well under the guard band's floor: the head unit's own
    // pull-up is set up for a low-impedance source, so 1.00 is the safe choice.
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 1900), GainMode::kTracking);
}

TEST(GainPolicy, AutoPicksAmplifiedForAHeadUnitIdlingAboveTheGuardBand) {
    // Spec 6.2 step 4: V_KEY_idle >= 3.4V is the 5V range, gain 1.82. The
    // asymmetry is the safety argument -- the only dangerous error is
    // OVER-ranging a 3V head unit, so gain 1.00 is taken only on positive
    // evidence of a 3V line, and everything above the guard floor is amplified.
    //
    // An earlier revision of GainPolicySelect returned Tracking for everything
    // outside the band, which put this side backwards: a 5V head unit idling at
    // 4.98V was driven at 1.00. These two cases are why that could not be
    // caught -- the old test asserted only at and below the band's ceiling, so
    // the entire high side was unasserted.
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 3480), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 4980), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 5200), GainMode::kAmplified);
}

TEST(GainPolicy, AutoPicksAmplifiedWhenThereIsNoMeasurementAtAll) {
    // The default-to-amplified rule (spec 6.2): 0 means nothing was measured,
    // and the safe answer is still 1.82 rather than 1.00.
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 0), GainMode::kAmplified);
}

TEST(GainPolicy, ForcedModesOverrideAuto) {
    EXPECT_EQ(GainPolicySelect(GainPolicy::kForceTracking, 3000), GainMode::kTracking);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kForceAmplified, 1900), GainMode::kAmplified);
}

TEST(GainPolicy, AmplifiedModeProducesTheDocumentedEnvelope) {
    // Full-scale DAC (0..3.3V) at gain 1.82 with V_ADJ at the 1k pulldown.
    const int at_full = GainPolicyKeyMvForCode(GainMode::kAmplified, 4095);
    const int at_zero = GainPolicyKeyMvForCode(GainMode::kAmplified, 0);
    EXPECT_GT(at_full, 5200);
    EXPECT_LT(at_zero, 1800);
    // The useful span must be inside 1.80-5.20V, which is what makes this gain
    // selected at all.
    EXPECT_GT(at_full - at_zero, 3400);
}

TEST(GainPolicy, TargetAboveTheCeilingClampsAndSaysSo) {
    const GainDecision d = GainPolicyCodeForTarget(GainMode::kAmplified, 6000);
    EXPECT_TRUE(d.clamped);
    EXPECT_LE(GainPolicyKeyMvForCode(d.mode, d.dac_code), kOutputCeilingMv);
}

TEST(GainPolicy, TargetBelowTheFloorClampsAndSaysSo) {
    const GainDecision d = GainPolicyCodeForTarget(GainMode::kAmplified, 500);
    EXPECT_TRUE(d.clamped);
    EXPECT_GE(GainPolicyKeyMvForCode(d.mode, d.dac_code), kOutputFloorMv);
}

TEST(GainPolicy, NoLegalTargetEverEscapesTheEnvelope) {
    for (int target = 0; target <= 6000; target += 25) {
        for (GainMode m : {GainMode::kTracking, GainMode::kAmplified}) {
            const GainDecision d = GainPolicyCodeForTarget(m, target);
            const int actual = GainPolicyKeyMvForCode(m, d.dac_code);
            ASSERT_GE(actual, kOutputFloorMv) << "mode=" << (int)m << " target=" << target;
            ASSERT_LE(actual, kOutputCeilingMv) << "mode=" << (int)m << " target=" << target;
        }
    }
}

TEST(GainPolicy, CodeForTargetIsMonotonic) {
    int prev = -1;
    for (uint16_t code = 0; code <= 4095; ++code) {
        const int mv = GainPolicyKeyMvForCode(GainMode::kAmplified, code);
        ASSERT_GE(mv, prev) << "non-monotonic at code=" << code;
        prev = mv;
    }
}

TEST(GainPolicy, AnExtremeRatioSaturatesInsteadOfWrappingIntoAValidTarget) {
    // The ratio is `head_unit_idle * level / wheel_idle`. With a small
    // `wheel_idle` the product exceeds 65535, and narrowing it to `MilliVolt`
    // (uint16_t) reduces it MODULO 65536 -- and about half of the wrapped values
    // land back inside [1800, 5200], where the value is indistinguishable from a
    // real target. The clamp in `GainPolicyCodeForTarget` then sees nothing wrong
    // and the device drives a key voltage nothing defined.
    //
    // A CONCRETE reachable case: `mv_center` 2896 with `learned_idle_mv` 1. Both
    // are permitted by the validators (each bounds its field to a plausible ADC
    // reading, independently -- and a button "learned" against an unreadable input
    // legitimately reads near full scale). 4980 * 2896 / 1 = 14,422,080, which
    // wraps to 4160 -- a perfectly plausible 5 V target.
    constexpr int kHeadUnitIdle = 4980;   // a 5 V head unit
    const long exact = (long)kHeadUnitIdle * 2896 / 1;
    ASSERT_GT(exact, 65535) << "the premise: this ratio must exceed the uint16 range";
    const unsigned wrapped = (unsigned)(uint16_t)exact;
    ASSERT_GE(wrapped, (unsigned)kOutputFloorMv);
    ASSERT_LE(wrapped, (unsigned)kOutputCeilingMv);

    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(kHeadUnitIdle, 2896, 1), kOutputCeilingMv)
        << "an extreme ratio must SATURATE at the output ceiling, not wrap mod 65536 "
           "into a valid-looking " << wrapped << " mV";
}

TEST(GainPolicy, TheRatioMappingStillMatchesTheOrdinaryCase) {
    // The saturation must not change the normal arithmetic: the existing
    // ratio-mapped press test depends on 1430/2835 of a 4980 mV head-unit idle, i.e. 2511 mV.
    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(4980, 1430, 2835), 2511);
    // And a level above the wheel's idle saturates rather than exceeding the range.
    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(4980, 3300, 100), kOutputCeilingMv);
    // No denominator (or no numerator): nothing is asked for.
    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(4980, 1430, 0), 0);
    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(4980, 0, 2835), 0);
    EXPECT_EQ(GainPolicyMapWheelLevelToHeadUnit(0, 1430, 2835), 0);
}

// --- Spec 6.2's command band (N-32) -----------------------------------------
//
// "Command targets must stay inside `[V_OUT_floor, V_KEY_idle - 0.20 V]` so the
// sink FET is never asked to drive above the line's own resting level -- above
// that point the servo can only turn `Q4` off, which is the release behavior, not
// a command." The envelope clamp alone is NOT that bound: it permits any target
// from 1800 to 5200 mV, so a target between the head unit's own idle and the
// ceiling reached the DAC and drove nothing.

TEST(GainPolicy, ACommandAboveTheHeadUnitsOwnIdleIsClampedBelowIt) {
    // A 5 V head unit idling at 4980 mV rests well inside the 1800..5200 envelope,
    // so FR-18's clamp sees nothing wrong with a 5100 mV command -- and the FET can
    // only turn OFF there, so the radio receives no key at all.
    bool clamped = false;
    EXPECT_EQ(GainPolicyClampCommand(5100, 4980, &clamped), 4980 - kCommandHeadroomMv)
        << "a command above the line's rest must come down to V_KEY_idle - 0.20 V";
    EXPECT_TRUE(clamped);
}

TEST(GainPolicy, ACommandInsideTheBandIsUntouched) {
    // No warning for a legal value: a warning on every press is noise, and noise
    // is how a real warning gets ignored.
    bool clamped = true;   // must be reset even when nothing moves
    EXPECT_EQ(GainPolicyClampCommand(2400, 4980, &clamped), 2400);
    EXPECT_FALSE(clamped);
}

TEST(GainPolicy, ACommandBelowTheFloorComesUpToTheFloor) {
    // The floor is the same value `GainPolicyCodeForTarget` refuses to go below, so
    // the bottom of the commandable range has one definition.
    bool clamped = false;
    EXPECT_EQ(GainPolicyClampCommand(500, 4980, &clamped), kOutputFloorMv);
    EXPECT_TRUE(clamped);
}

TEST(GainPolicy, AnEmptyBandReportsAbsentRatherThanClampingToTheFloor) {
    // A head unit idling at 1900 mV leaves `1900 - 200 = 1700`, below the servo's
    // 1800 mV floor: there is no level that is BOTH reachable and below the line's
    // rest. Reporting the floor here would drive a level the spec forbids, so the
    // band is empty and 0 is returned -- the same "absent" the rest of the output
    // code uses, so the caller releases instead of guessing.
    bool clamped = false;
    EXPECT_EQ(GainPolicyClampCommand(1500, 1900, &clamped), 0)
        << "an empty band has no command to make";
    EXPECT_TRUE(clamped) << "the request could not be honoured as written";
}

TEST(GainPolicy, WithNoMeasuredHeadUnitTheEnvelopeIsTheOnlyBound) {
    // `head_unit_idle_mv <= 0` is "no head unit measured" (spec 6.2 step 2), not a
    // resting level of zero: there is nothing to stay below, so the envelope's own
    // ceiling applies, and a legal command is untouched.
    bool clamped = false;
    EXPECT_EQ(GainPolicyClampCommand(2400, 0, &clamped), 2400);
    EXPECT_FALSE(clamped);

    bool clamped_high = false;
    EXPECT_EQ(GainPolicyClampCommand(9000, 0, &clamped_high), kOutputCeilingMv);
    EXPECT_TRUE(clamped_high);
}

TEST(GainPolicy, TheClampIsNullSafeAndTheBandBoundaryIsExact) {
    // The `clamped` out-param is optional, and the boundary itself is IN the band:
    // `V_KEY_idle - 200` is the lowest level the spec still permits.
    EXPECT_EQ(GainPolicyClampCommand(4780, 4980, nullptr), 4780)
        << "exactly V_KEY_idle - 0.20 V is inside the band";
    EXPECT_EQ(GainPolicyClampCommand(4781, 4980, nullptr), 4780);
}
