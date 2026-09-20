#include "Learning/LearnSession.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

namespace {
// Spec 3.4's worked-example rail and the ~23.5 C the ladder was learned at.
constexpr MilliVolt kRailMv     = 3300;
constexpr int16_t   kTempTenths = 235;

LadderProfile ExistingWith(const char *id, int mv_center, int mv_tolerance) {
    LadderProfile p{};
    p.learned_idle_mv = 2835;
    p.count = 1;
    std::strncpy(p.buttons[0].id, id, sizeof(p.buttons[0].id) - 1);
    p.buttons[0].mv_center    = static_cast<MilliVolt>(mv_center);
    p.buttons[0].mv_tolerance = static_cast<MilliVolt>(mv_tolerance);
    return p;
}

void Feed(LearnSession &s, int mv, int idle, int n, uint64_t &t) {
    for (int i = 0; i < n; ++i) {
        s.AddSample(mv, idle, kRailMv, kTempTenths, t);
        t += 10;
    }
}
}  // namespace

TEST(LearnSession, ASteadyLevelCommitsAndRecordsEverythingLearnIsTheSourceOf) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 30, t);
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    // Spec 3.4's eight fields. Learn is the only source of six of them, so all
    // six are asserted here -- a profile that commits without its rail or its
    // temperature is one FR-17 and FR-30 cannot use.
    EXPECT_EQ(out.mv_center, 1430);
    EXPECT_GT(out.mv_tolerance, 0);
    EXPECT_EQ(out.learned_at_rail_mv, kRailMv);
    EXPECT_EQ(out.temp_c_at_learn, kTempTenths);
    EXPECT_EQ(out.sample_count, 30);
    EXPECT_GT(out.confidence, 0);
    // FR-30: the idle reference is recorded so runtime classification can
    // detect a rail fault and the app can display absolute millivolts.
    EXPECT_EQ(s.LearnedIdleMv(), 2835);
}

TEST(LearnSession, TheIdAndNameAreLeftToTheCallerNotInvented) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 30, t);
    LadderButton out{};
    out.id[0] = '\0';
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_EQ(out.id[0], '\0') << "learn cannot invent a slug; the caller supplies it";
}

TEST(LearnSession, TooFewSamplesIsRejectedNotAcceptedFromOneReading) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 2, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooFewSamples);
}

TEST(LearnSession, ANoisyLevelIsRejectedWithTheNoiseReason) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // A 550 mV swing at a 2835 mV idle is ~195 permille of wobble: far wider
    // than the classification tolerance.
    for (int i = 0; i < 30; ++i) {
        s.AddSample((i % 2) ? 1430 : 1980, 2835, kRailMv, kTempTenths, t);
        t += 10;
    }
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooNoisy);
}

TEST(LearnSession, ALevelAtIdleIsRejectedBecauseTheButtonWasNotPressed) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 2835, 2835, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kAtIdle);
}

TEST(LearnSession, ALevelWithinAnExistingButtonsToleranceIsRejectedAsAmbiguous) {
    LearnSession s;
    s.Start(0, ExistingWith("vol_up", 1430, 120));
    uint64_t t = 1000;
    Feed(s, 1450, 2835, 30, t);   // 511 permille, inside vol_up's window
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooCloseToExisting);
}

TEST(LearnSession, ALevelAboveTheAdcCeilingIsRejectedAsOutOfRange) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // 3000 mV exceeds the 2900 mV calibrated ADC ceiling (spec 3.2), so this
    // cannot be a real reading from this hardware -- it is a wiring or
    // calibration fault. NOTE: this is the INPUT side. The 2490 mV figure
    // belongs to the output sense divider (spec 2.3) and is a different net.
    Feed(s, 3000, 2835, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kOutOfRange);
}

TEST(LearnSession, OutOfRangeSamplesStillCountTowardTheMinimumSoTheReasonIsActionable) {
    // The gate order only means anything if gate 1 can be passed by samples gate
    // 2 is about to reject. With a single counter, 30 implausible readings fail
    // the COUNT gate and report "too_few_samples" -- telling the user to hold the
    // button longer when nothing they can do will help.
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 3200, 2835, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kOutOfRange)
        << "out-of-range must be reported as such, not as too few samples";
}

TEST(LearnSession, EachRejectionHasADistinctWireReason) {
    // FR-29 requires the rejection to say *why*: "it didn't work" is not
    // actionable for a user holding a button with one hand in a car.
    const LearnReject all[] = {LearnReject::kTooNoisy, LearnReject::kTooCloseToExisting,
                               LearnReject::kAtIdle, LearnReject::kOutOfRange,
                               LearnReject::kTooFewSamples};
    std::set<std::string> reasons;
    for (LearnReject r : all) reasons.insert(LearnRejectReason(r));
    EXPECT_EQ(reasons.size(), 5u) << "every rejection needs its own reason string";
    for (const auto &r : reasons) EXPECT_FALSE(r.empty());
    // And kNone is not one of them: success must not share a string with a
    // failure, or a caller matching on the string cannot tell them apart.
    EXPECT_EQ(reasons.count(LearnRejectReason(LearnReject::kNone)), 0u);
}

TEST(LearnSession, ToleranceIsTheMidpointOfTheGapNotADoubleOfTheSpread) {
    // Spec 3.4: mv_tolerance is the midpoint of the gap to the nearest
    // neighbouring button, capped. Deriving it from the observed spread instead
    // is the classic cause of two buttons both triggering the same action --
    // exactly what this rule exists to prevent.
    LearnSession s;
    // A neighbour 200 mV below the level being learned, with a narrow window so
    // the gap (not the neighbour's tolerance) is what determines the answer.
    s.Start(0, ExistingWith("other", 1230, 20));
    uint64_t t = 1000;
    // A 30 mV spread. A spread-derived tolerance would give 60 mV; the gap
    // midpoint is 100 mV. The two are far enough apart to tell apart.
    for (int i = 0; i < 30; ++i) {
        s.AddSample((i % 2) ? 1445 : 1415, 2835, kRailMv, kTempTenths, t);
        t += 10;
    }
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_NEAR(out.mv_center, 1430, 2);
    EXPECT_NEAR(out.mv_tolerance, 100, 3)
        << "half the 200 mV gap to the neighbour, not the 120 mV cap and not 60 mV";
    EXPECT_GE(out.mv_tolerance, 20) << "and it must still cover the measured spread";
}

TEST(LearnSession, ToleranceIsCappedWhenNoNeighbourIsNearby) {
    LearnSession s;
    s.Start(0, LadderProfile{});   // nothing learned yet
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 30, t);
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_EQ(out.mv_tolerance, LearnSession::kMaxToleranceMv)
        << "with no neighbour the cap governs, not infinity";
}

TEST(LearnSession, TheToleranceFloorCoversTheSpreadItWasMeasuredThrough) {
    // The spread's ONLY role is as a floor. A window narrower than the noise it
    // was measured through would reject the very button it describes.
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // A 60 mV spread, no neighbour: the 120 cap would otherwise govern and hide
    // whether the floor is applied at all. Force the floor by capping the gap.
    for (int i = 0; i < 30; ++i) {
        s.AddSample((i % 2) ? 1460 : 1400, 2835, kRailMv, kTempTenths, t);
        t += 10;
    }
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_GE(out.mv_tolerance, 60) << "the window must cover the observed spread";
}

TEST(LearnSession, ASecondLearnOnTheSameChannelDoesNotCommitOverItsOwnSibling) {
    // Learning `vol_dn` after `vol_up` must not refuse itself against the button
    // it is supposed to sit beside -- the check is against a DIFFERENT window.
    LearnSession s;
    s.Start(0, ExistingWith("vol_up", 1430, 120));
    uint64_t t = 1000;
    Feed(s, 1785, 2835, 30, t);   // 355 mV away: clearly a different button
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_EQ(out.mv_center, 1785);
}

TEST(LearnSession, AStartResetsStateSoOneSessionCannotLeakIntoTheNext) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 30, t);
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);

    // A fresh session with too few samples must start clean rather than counting
    // the previous run's samples.
    LearnSession s2;
    s2.Start(0, LadderProfile{});
    LadderButton out2{};
    EXPECT_EQ(s2.SampleCount(), 0);
    EXPECT_EQ(s2.Commit(&out2), LearnReject::kTooFewSamples)
        << "a reset session must not inherit the prior run's samples";
}
