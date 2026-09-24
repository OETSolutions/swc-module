#include "Gesture/PressClassifier.h"
#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {
LadderProfile Profile() {
    LadderProfile p{};
    p.learned_idle_mv = 2835;
    p.count = 1;
    p.buttons[0] = {"VOL_UP", "Volume Up", 1430, 120, 3300, 235, 200, 98};
    return p;
}
}  // namespace

TEST(PressClassifier, A_SingleSettlingTickIsAbsorbedByTheDebounce) {
    // FR-3/N-18: the classifier reads `Value()` unconditionally rather than
    // gating on `AdcReader::Settled()`. That is safe because the ONLY effect of an
    // unsettled burst is one intermediate median (the mixed window's upper-middle
    // sample), and the classifier's `debounce_ms` (25 ms) requires the candidate to
    // PERSIST before it latches -- so a single such tick cannot fire a button.
    //
    // This pins that reasoning so the claim is not merely "expected to be
    // harmless": one settling tick between two real-level ticks must not produce a
    // press, and the press must still latch once the level holds.
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    // A mixed first window's median would land ~mid-swing, e.g. 2130 mV (751‰) --
    // between VOL_UP's window and idle, so it classifies kUnknown, not kButton.
    EXPECT_EQ(c.Update(2130, 2835, t), ChannelLevel::kIdle) << "one settling tick is not a press";
    t += 10;
    // The next window is fully the pressed level; the press latches after debounce.
    ChannelLevel level = ChannelLevel::kIdle;
    for (int i = 0; i < 5; ++i) { level = c.Update(1430, 2835, t); t += 10; }
    EXPECT_EQ(level, ChannelLevel::kPressed);
    EXPECT_EQ(c.ButtonIndex(), 0) << "the settling tick must not shift which button latches";
}

TEST(PressClassifier, ByteNoiseBelowTheDebounceWindowIsNotAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    // One 10ms sample dips into the window: not a press.
    EXPECT_EQ(c.Update(1430, 2835, 1000), ChannelLevel::kIdle);
    EXPECT_EQ(c.Update(2835, 2835, 1010), ChannelLevel::kIdle);
}

TEST(PressClassifier, ASustainedLevelBecomesPressedAfterDebounce) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    ChannelLevel level = ChannelLevel::kIdle;
    for (int i = 0; i < 5; ++i) {  // 50ms of sustained press > 25ms debounce
        level = c.Update(1430, 2835, t);
        t += 10;
    }
    EXPECT_EQ(level, ChannelLevel::kPressed);
    EXPECT_EQ(c.ButtonIndex(), 0);
}

TEST(PressClassifier, HysteresisKeepsAPressLatchedThroughASmallDip) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // 1605 mV is 566 permille: in the GAP between VOL_UP's window (504+/-42,
    // so [462,546]) and VOL_DOWN's ([588,672]). It classifies kUnknown, which
    // is the ONLY branch hysteresis acts on -- a value still inside the window
    // would classify kButton and never reach it. Hysteresis must hold the press
    // rather than let a drifting finger release it.
    for (int i = 0; i < 5; ++i) { c.Update(1605, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
}

TEST(PressClassifier, ReleaseRequiresReturningToIdleNotMerelyLeavingTheWindow) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // 2400 mV is 846 permille -- between the window and idle: still held.
    c.Update(2400, 2835, t); t += 10;
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
    for (int i = 0; i < 5; ++i) { c.Update(2835, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kIdle);
}

TEST(PressClassifier, FaultPropagatesAndNeverReadsAsAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(500, 500, t); t += 10; }  // collapsed rail
    EXPECT_EQ(c.Level(), ChannelLevel::kFault);
}

TEST(PressClassifier, UnlearnedLevelIsUnknownNotPressed) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(2400, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kUnknown);
    EXPECT_EQ(c.ButtonIndex(), 0xFF);
}

TEST(PressClassifier, SwitchingButtonsMidPressReportsTheNewButtonAfterDebounce) {
    LadderProfile p = Profile();
    p.count = 2;
    p.buttons[1] = {"VOL_DOWN", "Volume Down", 1785, 120, 3300, 235, 200, 97};
    PressClassifier c(p, GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.ButtonIndex(), 0);
    for (int i = 0; i < 8; ++i) { c.Update(1785, 2835, t); t += 10; }
    EXPECT_EQ(c.ButtonIndex(), 1);
}
