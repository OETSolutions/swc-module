#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {

/*
 * The spec 3.7 default ladder, in the units the decoder actually works in.
 *
 * The decoder normalizes against the IDLE READING, not against a rail
 * (spec 6.3: n = V_ADC / V_ADC_idle). A press pulls the input DOWN from idle,
 * so every button's ratio is BELOW 1000 and idle is 1000 by construction.
 *
 *   2835 mV idle -> VOL_UP 1430, VOL_DOWN 1785, NEXT 2145
 *   in permille of idle: 1430/2835 = 504, 1785/2835 = 630, 2145/2835 = 757
 *   tolerance 120 mV = 42 permille; 110 mV = 39 permille
 */
LadderProfile MakeProfile(int learned_idle_mv) {
    LadderProfile p{};
    p.learned_idle_mv = learned_idle_mv;
    p.count = 3;
    p.buttons[0] = {"VOL_UP",   504, 42, 1};
    p.buttons[1] = {"VOL_DOWN", 630, 42, 2};
    p.buttons[2] = {"NEXT",     757, 39, 3};
    return p;
}

constexpr int kIdleMv = 2835;

}  // namespace

TEST(LadderRatio, IsLevelOverIdleInPermille) {
    EXPECT_EQ(LadderRatioPermille(2500, 5000), 500);
    EXPECT_EQ(LadderRatioPermille(5000, 5000), 1000);
    EXPECT_EQ(LadderRatioPermille(0, 5000), 0);
    EXPECT_EQ(LadderRatioPermille(1150, 5750), 200);
}

TEST(LadderClassify, IdleReturnsIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    EXPECT_EQ(LadderClassify(p, kIdleMv, kIdleMv).result, ClassifyResult::kIdle);
    EXPECT_EQ(LadderClassify(p, kIdleMv - 20, kIdleMv).result, ClassifyResult::kIdle);
}

TEST(LadderClassify, EachLearnedButtonClassifiesToItsOwnIndex) {
    LadderProfile p = MakeProfile(kIdleMv);
    EXPECT_EQ(LadderClassify(p, 1430, kIdleMv).index, 0);  // 504 permille
    EXPECT_EQ(LadderClassify(p, 1785, kIdleMv).index, 1);  // 630 permille
    EXPECT_EQ(LadderClassify(p, 2145, kIdleMv).index, 2);  // 757 permille
}

/*
 * THE test. The +3V3 rail moves across its regulator tolerance band (spec 11
 * FR-30). The same physical button must classify identically across the whole
 * band. This is why the decode normalizes: an absolute millivolt window would
 * classify correctly at exactly one rail voltage. Both the numerator and the
 * idle reference come off the same ADC with the same reference, so the ratio
 * is invariant to the rail.
 *
 * NOTE 1: this is a +3V3 sweep, NOT a vehicle-rail sweep. An earlier revision
 * swept 11.0-14.8 V against a 5 V ladder; no such term exists in the transfer
 * function (spec 6.3).
 *
 * NOTE 2: the idle reference has only **2.29 % of headroom** before it reaches
 * the 2900 mV ADC ceiling — with the spec 3.7 nominal idle of 2835 mV, that is
 * ~3375 mV of rail. Above that the idle reading clips while the button reading
 * does not, so the ratio is distorted. It survives anyway (the distortion is
 * ~3.5 % at +6 % overvoltage, well inside the +/-8.3 % window), which the
 * clipping test below asserts. But the margin is thin, and it is another reason
 * the bring-up measurement (spec 10.6) gates the R15/R16 decision.
 */
TEST(LadderClassify, SameButtonClassifiesIdenticallyAcrossTheThreeVoltThreeSweep) {
    for (int rail_mv = 3140; rail_mv <= 3470; rail_mv += 10) {
        // Idle scales with the rail; so does the button level. The ratio does not.
        const int idle_mv  = (kIdleMv * rail_mv) / 3300;
        const int level_mv = (idle_mv * 504) / 1000;   // VOL_UP at 504 permille
        LadderProfile p = MakeProfile(idle_mv);
        const ClassifyOutcome out = LadderClassify(p, level_mv, idle_mv);
        ASSERT_EQ(out.result, ClassifyResult::kButton)
            << "rail=" << rail_mv << " idle=" << idle_mv << " level=" << level_mv;
        ASSERT_EQ(out.index, 0) << "rail=" << rail_mv;
    }
}

TEST(LadderClassify, StillClassifiesWhenTheIdleReferenceClipsAtTheAdcCeiling) {
    // At +6% of rail the true idle (3006 mV) is above the 2900 mV ceiling, so
    // the measured idle reference saturates while the pressed reading does not.
    // The ratio shifts but must stay inside the button's window: classification
    // degrades gracefully rather than dropping the press.
    LadderProfile p = MakeProfile(3006);
    const int clipped_idle_mv = 2900;
    const int pressed_mv = (3006 * 504) / 1000;   // 1515 mV, still below the ceiling
    const ClassifyOutcome out = LadderClassify(p, pressed_mv, clipped_idle_mv);
    EXPECT_EQ(out.result, ClassifyResult::kButton)
        << "a saturated idle reference must not lose the press";
    EXPECT_EQ(out.index, 0);
}

TEST(LadderClassify, UnlearnedLevelIsUnknownAndNeverGuessed) {
    LadderProfile p = MakeProfile(kIdleMv);
    // 2400 mV is 847 permille: between NEXT (757 +/- 39) and idle (1000 - 30).
    const ClassifyOutcome out = LadderClassify(p, 2400, kIdleMv);
    EXPECT_EQ(out.result, ClassifyResult::kUnknown);
}

TEST(LadderClassify, LevelAboveTheReferenceIsAFaultNotAButtonOrIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    // A short to a supply above the idle reference. Reporting this as IDLE
    // would be the worst outcome -- the user's button would do nothing and
    // nothing would say why.
    EXPECT_EQ(LadderClassify(p, 3000, kIdleMv).result, ClassifyResult::kFault);
}

TEST(LadderClassify, CollapsedRailIsAFaultNotAnIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    // FR-30: an idle reading at or below 20% of the learned value is a rail
    // fault, not idle. Checked against the LEARNED idle, because ratio
    // normalization deliberately cancels rail movement out of the ratios.
    EXPECT_EQ(LadderClassify(p, 500, 500).result, ClassifyResult::kFault);
}

TEST(LadderClassify, ToleranceBoundaryIsInclusiveAtTheEdgeAndExclusiveBeyond) {
    LadderProfile p = MakeProfile(kIdleMv);
    // window 504 +/- 42 permille -> [462, 546]
    EXPECT_EQ(LadderClassify(p, 1310, kIdleMv).result, ClassifyResult::kButton);  // 462 incl
    EXPECT_EQ(LadderClassify(p, 1548, kIdleMv).result, ClassifyResult::kButton);  // 546 incl
    EXPECT_EQ(LadderClassify(p, 1307, kIdleMv).result, ClassifyResult::kUnknown); // 461
    EXPECT_EQ(LadderClassify(p, 1551, kIdleMv).result, ClassifyResult::kUnknown); // 547
}

TEST(LadderClassify, OverlappingWindowsResolveToTheNearestCentreNotTheFirstMatch) {
    LadderProfile p = MakeProfile(kIdleMv);
    // Two deliberately overlapping windows.
    p.count = 2;
    p.buttons[0] = {"A", 500, 80, 1};  // [420,580]
    p.buttons[1] = {"B", 540, 80, 2};  // [460,620]
    EXPECT_EQ(LadderClassify(p, 1418, kIdleMv).index, 0);  // 500 -> centre 0
    EXPECT_EQ(LadderClassify(p, 1531, kIdleMv).index, 1);  // 540 -> centre 1
}
