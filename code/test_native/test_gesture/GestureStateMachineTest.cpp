#include "Gesture/GestureStateMachine.h"
#include <gtest/gtest.h>

namespace {
GestureEvent Feed(GestureStateMachine &sm, ChannelLevel level, uint8_t button,
                  uint64_t &now, uint32_t hold_ms, uint32_t step_ms = 10) {
    GestureEvent fired{};
    GestureEvent last{};
    for (uint32_t elapsed = 0; elapsed < hold_ms; elapsed += step_ms) {
        if (sm.Update(level, button, now, &fired)) last = fired;
        now += step_ms;
    }
    return last;
}

// The same, for a button whose bindings constrain the resolve (spec 6.6).
GestureEvent Feed(GestureStateMachine &sm, ChannelLevel level, uint8_t button,
                  uint64_t &now, uint32_t hold_ms, const GestureBindings &bindings,
                  uint32_t step_ms = 10) {
    GestureEvent fired{};
    GestureEvent last{};
    for (uint32_t elapsed = 0; elapsed < hold_ms; elapsed += step_ms) {
        if (sm.Update(level, button, now, &fired, bindings)) last = fired;
        now += step_ms;
    }
    return last;
}
}  // namespace

TEST(Gesture, ShortPressEmitsSingleOnlyAfterTheDoubleWindowCloses) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);   // press
    GestureEvent ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);  // release + wait
    EXPECT_EQ(ev.gesture, Gesture::kSingle);
    EXPECT_EQ(ev.button_index, 0);
}

TEST(Gesture, TwoPressesInsideTheWindowEmitOneDoubleAndNoSingles) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 200);   // gap < 500ms
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "must not emit SINGLE on the first press";
    // DOUBLE fires at the START of the second press -- the implementation's
    // comment says so, and DoubleWindowBoundaryIsInclusiveAt499AndExclusiveAt500
    // relies on it (its 499ms case reads kDouble from a kPressed Update). So it
    // is THIS Feed that carries the DOUBLE; the release Feed below must be
    // silent, which is FR-11's "the second press of a DOUBLE must not emit its
    // own SINGLE". The plan read the DOUBLE from the release Feed, which
    // correctly returns kNone, so that form could never pass.
    ev = Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    EXPECT_EQ(ev.gesture, Gesture::kDouble);
    EXPECT_EQ(ev.button_index, 0);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "the second press must not also emit a SINGLE";
}

TEST(Gesture, DoubleWindowBoundaryIsInclusiveAt499AndExclusiveAt500) {
    // Spec 10.4 requires the 500ms boundary at exactly 499/500/501. Feed()
    // CANNOT express it: its loop steps 10ms and runs `elapsed < hold_ms`, so
    // Feed(idle,499) and Feed(idle,500) advance the clock identically (both to
    // gap 490) and the boundary is invisible. Direct Update calls instead.
    // The window closes when `now - released_at_ >= double_press_off_ms`, so a
    // 499ms gap is still a double and a 500ms gap is already a single.
    struct Case { uint32_t gap_ms; Gesture expect; };
    const Case cases[] = {{499, Gesture::kDouble},
                          {500, Gesture::kSingle},
                          {501, Gesture::kSingle}};
    for (const Case &c : cases) {
        GestureStateMachine sm(GestureTimingsDefault());
        GestureEvent ev{};
        sm.Update(ChannelLevel::kPressed, 0, 1000, &ev);   // press begins
        sm.Update(ChannelLevel::kIdle, 0, 1100, &ev);      // release; gap starts here
        const bool closed = sm.Update(ChannelLevel::kIdle, 0, 1100 + c.gap_ms, &ev);
        if (c.expect == Gesture::kSingle) {
            EXPECT_TRUE(closed) << "gap=" << c.gap_ms << "ms must have closed the window";
            EXPECT_EQ(ev.gesture, Gesture::kSingle) << "gap=" << c.gap_ms;
        } else {
            EXPECT_FALSE(closed) << "gap=" << c.gap_ms << "ms must not have closed yet";
            const bool dbl = sm.Update(ChannelLevel::kPressed, 0, 1100 + c.gap_ms + 10, &ev);
            EXPECT_TRUE(dbl) << "gap=" << c.gap_ms;
            EXPECT_EQ(ev.gesture, Gesture::kDouble) << "gap=" << c.gap_ms;
        }
    }
}

TEST(Gesture, LongPressFiresAtTheThresholdBeforeRelease) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev = Feed(sm, ChannelLevel::kPressed, 0, now, 800);
    EXPECT_EQ(ev.gesture, Gesture::kLong);
    // The `u` suffixes are load-bearing: ev.at_ms is uint64_t, so the
    // subtraction yields unsigned long long and comparing it against a signed
    // literal trips -Wsign-compare, which -Werror promotes. The host build is
    // where this bites, because it is the one that compiles the tests; the
    // device build never does (test_native is in test_ignore). Both compilers
    // reject the signed form -- verified against device GCC 14.2.0 as well --
    // so this is a correctness fix, not a host-only workaround.
    EXPECT_GE(ev.at_ms - 1000, 750u);
    EXPECT_LT(ev.at_ms - 1000, 800u) << "must fire at the threshold, not on release";
}

TEST(Gesture, LongPressBoundaryIsSilentAt749InclusiveAt750AndAt751) {
    // Spec 10.4 requires the 750ms boundary at 749/750/751, the LONG counterpart
    // to the 500ms double window's 499/500/501. Driven by direct Update calls
    // rather than Feed(): Feed's loop is `elapsed < hold_ms` with a 10ms step,
    // so it can only land on a multiple of 10 and cannot express 749 or 751.
    // The threshold is `>=`, so 749 is silent and both 750 and 751 fire.
    const uint32_t offsets[] = {749, 750, 751};
    const bool expect_long[]  = {false, true, true};
    for (int i = 0; i < 3; ++i) {
        GestureStateMachine sm(GestureTimingsDefault());
        GestureEvent ev{};
        sm.Update(ChannelLevel::kPressed, 0, 1000, &ev);          // press begins
        const bool fired = sm.Update(ChannelLevel::kPressed, 0, 1000 + offsets[i], &ev);
        EXPECT_EQ(fired, expect_long[i]) << "offset=" << offsets[i] << "ms";
        if (expect_long[i]) {
            EXPECT_EQ(ev.gesture, Gesture::kLong) << "offset=" << offsets[i];
        }
    }
}

TEST(Gesture, ALongPressNeverAlsoEmitsASingle) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    Feed(sm, ChannelLevel::kPressed, 0, now, 800);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 800);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "LONG must not be followed by SINGLE";
}

TEST(Gesture, PressingAButtonThatIsNeverReleasedDoesNotHangTheMachine) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    for (int i = 0; i < 1000; ++i) {  // 10 seconds held
        sm.Update(ChannelLevel::kPressed, 0, now, &ev);
        now += 10;
    }
    // Exactly one LONG, and the machine still responds afterwards.
    ev = GestureEvent{};
    Feed(sm, ChannelLevel::kIdle, 0, now, 600);
    sm.Update(ChannelLevel::kIdle, 0, now, &ev);
    now += 600;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    GestureEvent out{};
    const bool fired = sm.Update(ChannelLevel::kIdle, 0, now, &out);
    EXPECT_EQ(fired, false);  // nothing spurious
}

TEST(Gesture, TwoChannelsDoNotInterfere) {
    GestureStateMachine a(GestureTimingsDefault());
    GestureStateMachine b(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent eva{};
    GestureEvent evb{};
    for (uint32_t e = 0; e < 900; e += 10) {
        a.Update(ChannelLevel::kPressed, 0, now, &eva);
        b.Update(ChannelLevel::kPressed, 1, now, &evb);
        now += 10;
    }
    EXPECT_EQ(eva.gesture, Gesture::kLong);
    EXPECT_EQ(evb.gesture, Gesture::kLong);
    EXPECT_EQ(eva.button_index, 0);
    EXPECT_EQ(evb.button_index, 1);
}

TEST(Gesture, FaultResetsAnyInFlightGesture) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    sm.Update(ChannelLevel::kFault, 0xFF, now, nullptr);
    now += 600;
    GestureEvent ev{};
    const bool fired = sm.Update(ChannelLevel::kIdle, 0, now, &ev);
    EXPECT_FALSE(fired) << "a pending single must be dropped when the channel faults";
}

// --- The adaptive resolve (spec 6.6) -----------------------------------------
//
// The head unit is gesture-blind, so a button that binds only SINGLE has no
// ambiguity to resolve and must not inherit the double-press window's latency.

TEST(Gesture, AButtonBindingNeitherDoubleNorLongResolvesAtThePress) {
    GestureStateMachine sm(GestureTimingsDefault());
    const GestureBindings single_only{/*has_double=*/false, /*has_long=*/false};
    uint64_t now = 1000;
    GestureEvent ev{};
    // The very first pressed sample must emit -- no window, no release.
    for (int i = 0; i < 5 && ev.gesture == Gesture::kNone; ++i) {
        sm.Update(ChannelLevel::kPressed, 0, now, &ev, single_only);
        now += 10;
    }
    EXPECT_EQ(ev.gesture, Gesture::kSingle) << "no ambiguity means no wait";
}

TEST(Gesture, AButtonBindingOnlyLongResolvesOnReleaseWithoutTheDoubleWindow) {
    GestureStateMachine sm(GestureTimingsDefault());
    const GestureBindings long_only{/*has_double=*/false, /*has_long=*/true};
    uint64_t now = 1000;
    GestureEvent ev{};
    for (int i = 0; i < 10; ++i) { sm.Update(ChannelLevel::kPressed, 0, now, &ev, long_only); now += 10; }
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "held under the threshold: nothing yet";

    // Release. LONG was the only alternative and the threshold was never
    // reached, so the release itself is the resolution -- it must not wait out
    // the 500 ms double-press window, which nothing on this button can use.
    ASSERT_TRUE(sm.Update(ChannelLevel::kIdle, 0, now, &ev, long_only));
    EXPECT_EQ(ev.gesture, Gesture::kSingle);
}

TEST(Gesture, AButtonBindingOnlyLongStillFiresLongAtTheThreshold) {
    GestureStateMachine sm(GestureTimingsDefault());
    const GestureBindings long_only{/*has_double=*/false, /*has_long=*/true};
    uint64_t now = 1000;
    GestureEvent ev{};
    for (uint32_t elapsed = 0; elapsed < 760 && ev.gesture == Gesture::kNone; elapsed += 10) {
        sm.Update(ChannelLevel::kPressed, 0, now, &ev, long_only);
        now += 10;
    }
    EXPECT_EQ(ev.gesture, Gesture::kLong);
}

TEST(Gesture, AButtonBindingDoubleWaitsOutTheWindowEvenWithNoLong) {
    GestureStateMachine sm(GestureTimingsDefault());
    const GestureBindings double_only{/*has_double=*/true, /*has_long=*/false};
    uint64_t now = 1000;
    GestureEvent ev = Feed(sm, ChannelLevel::kPressed, 0, now, 100, double_only);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "the double is still possible";
    // Within the window: still nothing, because the second press could come.
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 400, double_only);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "inside the window the answer is not known";
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 200, double_only);
    EXPECT_EQ(ev.gesture, Gesture::kSingle);
}

TEST(Gesture, ANoLongButtonDoesNotFireLongEvenAfterTheThreshold) {
    // has_long=false means this button's LONG slot is unbound, so a hold must not
    // produce a LONG -- there would be nothing to run. The press simply continues
    // until release, where it resolves as a SINGLE.
    GestureStateMachine sm(GestureTimingsDefault());
    const GestureBindings no_long{/*has_double=*/false, /*has_long=*/false};
    uint64_t now = 1000;
    GestureEvent ev{};
    for (uint32_t elapsed = 0; elapsed < 2000; elapsed += 10) {
        sm.Update(ChannelLevel::kPressed, 0, now, &ev, no_long);
        now += 10;
    }
    EXPECT_EQ(ev.gesture, Gesture::kSingle) << "already resolved at the press";
    EXPECT_NE(ev.gesture, Gesture::kLong) << "an unbound LONG must never fire";
}
