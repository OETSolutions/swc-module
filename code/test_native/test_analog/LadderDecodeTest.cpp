#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {

constexpr int kIdleMv = 2835;

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
 *
 * The fields are MILLIVOLTS (spec 3.4), and they are scaled to `learned_idle_mv`.
 * That scaling is the whole point of storing mv against a recorded rail: the
 * levels above are what the ladder produced at the 2835 mV nominal rail, so a
 * profile "learned at" a different rail must carry levels measured at THAT rail
 * (V_button = ratio x V_rail). Without it the fixture would claim a rail it did
 * not use, and the derived window would drift with the sweep instead of staying
 * fixed -- passing for the wrong reason.
 *
 * The permille figures above are what the classifier derives, and are noted so
 * the expectations below stay readable. Storing the permille instead of the mv
 * was 105% of the NVS partition (spec 3.5).
 */
LadderProfile MakeProfile(int learned_idle_mv) {
    const auto at_rail = [learned_idle_mv](int nominal_mv) {
        return static_cast<uint16_t>(
            (static_cast<long long>(nominal_mv) * learned_idle_mv + kIdleMv / 2) / kIdleMv);
    };
    LadderProfile p{};
    p.learned_idle_mv = learned_idle_mv;
    p.count = 3;
    p.buttons[0] = {"VOL_UP",   "Volume Up",   at_rail(1430), at_rail(120), 3300, 235, 200, 98};
    p.buttons[1] = {"VOL_DOWN", "Volume Down", at_rail(1785), at_rail(120), 3300, 235, 200, 97};
    p.buttons[2] = {"NEXT",     "Next Track",  at_rail(2145), at_rail(110), 3300, 235, 200, 99};
    return p;
}

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
    //
    // MakeProfile scales its levels to the rail it is learned at, so at 3006 the
    // VOL_UP level is 3006 x 0.504 = 1515 mV -- still below the ceiling, which is
    // why the press survives while the reference does not.
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
    // Two deliberately overlapping windows, in millivolts at the 2835 idle:
    // A centre 1418 (500 permille) half 227 (80 permille) -> [420,580]
    // B centre 1531 (540 permille) half 227 (80 permille) -> [460,620]
    p.count = 2;
    p.buttons[0] = {"A", "Button A", 1418, 227, 3300, 235, 200, 98};
    p.buttons[1] = {"B", "Button B", 1531, 227, 3300, 235, 200, 98};
    EXPECT_EQ(LadderClassify(p, 1418, kIdleMv).index, 0);  // 500 -> centre 0
    EXPECT_EQ(LadderClassify(p, 1531, kIdleMv).index, 1);  // 540 -> centre 1
}

TEST(LadderProfileRebase, ScalesCentresAndTolerancesAndPreservesEveryPermilleWindow) {
    // A profile has ONE denominator, so a re-learn on a moved rail must put every
    // button in the frame the profile is about to be stamped with. Scaling the
    // centre and the tolerance TOGETHER is what keeps the permille window the
    // same: both scale with the rail, so `LadderRatioPermille` of a scaled button
    // against the scaled denominator is the same number to within the one
    // permille that rounding to whole millivolts costs.
    LadderProfile p = MakeProfile(kIdleMv);   // learned at 2835
    const int before_center = LadderRatioPermille(p.buttons[0].mv_center, kIdleMv);
    const int before_tol    = LadderRatioPermille(p.buttons[0].mv_tolerance, kIdleMv);

    const int kMoved = 2693;                  // ~-5 %, inside the documented band
    LadderProfileRebase(p, kIdleMv, kMoved);

    EXPECT_EQ(p.learned_idle_mv, kMoved);
    EXPECT_EQ(LadderRatioPermille(p.buttons[0].mv_center, kMoved), before_center)
        << "the centre's permille window must be UNCHANGED by the rebase";
    EXPECT_EQ(LadderRatioPermille(p.buttons[0].mv_tolerance, kMoved), before_tol);
    // And the absolute value moved with the rail, which is the point.
    EXPECT_LT(p.buttons[0].mv_center, 1430);
    EXPECT_GT(p.buttons[0].mv_center, 1330);
}

TEST(LadderProfileRebase, AProfileWithNoSourceFrameIsLeftUntouched) {
    // A fresh learn passes an empty profile (learned_idle_mv 0): there is no frame
    // to convert FROM, and the measured button is already in the target frame.
    LadderProfile p = MakeProfile(kIdleMv);
    const LadderButton first = p.buttons[0];
    LadderProfileRebase(p, 0, 2693);
    EXPECT_EQ(p.learned_idle_mv, 2835) << "no source frame means no conversion";
    EXPECT_EQ(p.buttons[0].mv_center, first.mv_center);
    EXPECT_EQ(p.buttons[0].mv_tolerance, first.mv_tolerance);
}

TEST(LadderProfileRebase, AnImpossibleScaleClampsRatherThanWrappingTheType) {
    // MilliVolt is a uint16_t, and the validator bounds a centre only by the ADC
    // ceiling -- NOT relative to `learned_idle_mv`. So a profile it ACCEPTS (a
    // near-zero learned idle with centres at the ceiling) scales past the type,
    // and an unchecked write wraps to a small, plausible-looking, WRONG value.
    // Clamping keeps the profile self-consistent and bounded instead.
    LadderProfile p{};
    p.learned_idle_mv = 1;                 // accepted by LadderProfileIsValid today
    p.count = 1;
    p.buttons[0] = {"x", "X", 2900, 100, 3300, 235, 200, 98};
    LadderProfileRebase(p, 1, 2900);
    EXPECT_EQ(p.learned_idle_mv, 2900);
    EXPECT_LE(p.buttons[0].mv_center, 2900) << "the centre must not wrap";
    EXPECT_LE(p.buttons[0].mv_tolerance, 2900) << "nor the tolerance";
    EXPECT_GT(p.buttons[0].mv_center, 2900 / 2) << "and clamping is at the top, not zero";
}
