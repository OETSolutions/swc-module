#include "Feedback/BuzzerGrammar.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

// Counts on-transitions of the buzzer line over a pattern's duration. The
// buzzer is the semantic member buzzer_on (spec 10.2), not a GpioPin.
namespace {
int CountBeeps(MockHal &hal, BuzzerGrammar &b, uint32_t total_ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        b.Update(hal.NowMs());
        const bool now = hal.BuzzerIsOn();
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(BuzzerGrammar, KeyAcceptedIsASingleShortBeep) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), /*level=*/3);
    b.Play(BuzzerPattern::kKeyAccepted);
    EXPECT_EQ(CountBeeps(hal, b, 400), 1);
}

// Spec 7.2's PROGRAM_ENTER is 40/40 x2 -- NOT the "3 short + 1 long"
// shave-and-a-haircut an earlier revision specified, which is not in the table
// and is not reproducible on a fixed-tone gated buzzer anyway.
TEST(BuzzerGrammar, ProgramEnterIsTwoPulsesPerTheSpecTable) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);
    EXPECT_EQ(CountBeeps(hal, b, 2000), 2);
}

// PROGRAM_STEP is ONE 40/40 pulse (spec 7.2); the caller repeats it n times.
// This is the arity rule: `reps` is a property of the named pattern, not an
// argument, so the n-beep menu is built by looping over Play().
TEST(BuzzerGrammar, ProgramStepIsOnePulseAndTheCallerRepeatsIt) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    int beeps = 0;
    for (int n = 0; n < 4; ++n) {
        b.Play(BuzzerPattern::kProgramStep);
        beeps += CountBeeps(hal, b, 200);   // one 40/40 cycle, plus margin
        // Drain the pattern so the next Play starts clean.
        for (int i = 0; i < 20; ++i) b.Update(hal.NowMs()), hal.AdvanceMs(5);
    }
    EXPECT_EQ(beeps, 4) << "four Play calls, one beep each";
}

TEST(BuzzerGrammar, LearnRejectIsDistinctFromLearnOk) {
    MockHal hal_a, hal_b;
    BuzzerGrammar ok(&hal_a.InterfaceRef(), 3);
    BuzzerGrammar reject(&hal_b.InterfaceRef(), 3);
    ok.Play(BuzzerPattern::kLearnOk);
    reject.Play(BuzzerPattern::kLearnReject);
    // A user doing this blind must be able to tell success from failure. Spec
    // 7.2 gives LEARN_OK 40/30 x2 and LEARN_REJECT 300/80 x2 -- the same COUNT
    // and very different rhythm, so counting alone cannot be the distinction.
    // Measure how long the line is actually on instead.
    EXPECT_EQ(CountBeeps(hal_a, ok, 2000), 2);
    EXPECT_EQ(CountBeeps(hal_b, reject, 2000), 2);

    MockHal ha2, hb2;
    BuzzerGrammar ok2(&ha2.InterfaceRef(), 3);
    BuzzerGrammar rej2(&hb2.InterfaceRef(), 3);
    ok2.Play(BuzzerPattern::kLearnOk);
    rej2.Play(BuzzerPattern::kLearnReject);
    int on_a = 0, on_b = 0;
    for (int i = 0; i < 400; ++i) {
        ok2.Update(ha2.NowMs());
        rej2.Update(hb2.NowMs());
        if (ha2.BuzzerIsOn()) ++on_a;
        if (hb2.BuzzerIsOn()) ++on_b;
        ha2.AdvanceMs(5);
        hb2.AdvanceMs(5);
    }
    EXPECT_GT(on_b, on_a * 2) << "reject must be audibly longer than ok";
}

TEST(BuzzerGrammar, LevelZeroSilencesEverythingExceptFatalPatterns) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), /*level=*/0);
    b.Play(BuzzerPattern::kKeyAccepted);
    EXPECT_EQ(CountBeeps(hal, b, 600), 0);
    b.Play(BuzzerPattern::kBootOk);
    EXPECT_EQ(CountBeeps(hal, b, 600), 0);

    // BOOT_ERROR and FAULT_* are the documented exceptions (spec 7.2): a device
    // that cannot serve output must still say so.
    b.Play(BuzzerPattern::kBootError);
    EXPECT_GT(CountBeeps(hal, b, 2000), 0);
    b.Play(BuzzerPattern::kFaultConfig);
    EXPECT_GT(CountBeeps(hal, b, 4000), 0);
}

TEST(BuzzerGrammar, PlayingWhileBusyReplacesRatherThanQueues) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);   // long pattern
    b.Update(hal.NowMs());
    ASSERT_TRUE(b.Busy());
    b.Play(BuzzerPattern::kKeyAccepted);    // a key press during programming feedback
    EXPECT_TRUE(b.Busy()) << "the new pattern is running, having replaced the old one";
    // The replacement is observable: the short pattern finishes in ~25 ms, where
    // PROGRAM_ENTER would have run for 120.
    for (int i = 0; i < 20; ++i) b.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_FALSE(b.Busy()) << "the newer, shorter pattern took over immediately";
}

TEST(BuzzerGrammar, NeverLeavesTheBuzzerStuckOnAfterAPatternCompletes) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);
    for (int i = 0; i < 2000; ++i) b.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_FALSE(hal.BuzzerIsOn()) << "a stuck buzzer is a stuck-on hardware fault";
}

// Every pattern in spec 7.2 must be playable and must finish. A pattern that
// never completes would hold the buzzer line forever, which the previous test
// only checks for one pattern.
TEST(BuzzerGrammar, EverySpecPatternCompletesAndReleasesTheLine) {
    const BuzzerPattern all[] = {
        BuzzerPattern::kBootOk, BuzzerPattern::kBootDegraded, BuzzerPattern::kBootError,
        BuzzerPattern::kKeyAccepted, BuzzerPattern::kKeyUnknown,
        BuzzerPattern::kProgramEnter, BuzzerPattern::kProgramStep,
        BuzzerPattern::kProgramSaved, BuzzerPattern::kProgramExit,
        BuzzerPattern::kProgramCancel,
        BuzzerPattern::kLearnPrompt, BuzzerPattern::kLearnOk, BuzzerPattern::kLearnReject,
        BuzzerPattern::kFaultDac, BuzzerPattern::kFaultConfig, BuzzerPattern::kFaultInput,
        BuzzerPattern::kFactoryReset,
        BuzzerPattern::kOtaStart, BuzzerPattern::kOtaOk, BuzzerPattern::kOtaFail,
    };
    for (BuzzerPattern p : all) {
        MockHal hal;
        BuzzerGrammar b(&hal.InterfaceRef(), 3);
        b.Play(p);
        // Spec 7.2's bound: routine patterns finish inside ~2 s, the three fatal
        // ones inside ~3 s. 3.5 s covers every row in the table with margin, and
        // is short enough that a pattern which never ends still fails here.
        for (int i = 0; i < 700; ++i) b.Update(hal.NowMs()), hal.AdvanceMs(5);
        EXPECT_FALSE(b.Busy()) << "pattern " << static_cast<int>(p) << " never finished";
        EXPECT_FALSE(hal.BuzzerIsOn()) << "pattern " << static_cast<int>(p) << " left the line on";
    }
}
