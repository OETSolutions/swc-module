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
