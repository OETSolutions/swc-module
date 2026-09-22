#include "Learning/LearnWizard.h"

#include "MockHAL.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {
// The AUX1 threshold the default profile uses; keep in step with
// Aux1ProfileDefault(). A press is well below the rail, a release at it.
constexpr int kAuxPressedMv  = 400;
constexpr int kAuxReleasedMv = 3300;
constexpr int kLevelHeldMv   = 1430;
constexpr int kLevelIdleMv   = 2835;

struct Rig {
    MockHal        hal;
    BuzzerGrammar  buzzer{&hal.InterfaceRef(), 2};
    LedGrammar     leds{&hal.InterfaceRef(), 2};
    LearnWizard    wiz{&hal.InterfaceRef(), &buzzer, &leds, Aux1ProfileDefault(),
                       GestureTimingsDefault()};
    uint64_t       t = 1000;

    void Tick(int channel = 0) {
        buzzer.Update(t);
        leds.Update(t);
        wiz.Tick(channel, t, kLevelIdleMv, 0);
        t += 5;
    }
    // Hold AUX1 released so the classifier sees a stable idle, then press it.
    void PressAux(int times) {
        for (int i = 0; i < times; ++i) {
            hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
            for (int k = 0; k < 8; ++k) Tick();
            hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
            for (int k = 0; k < 8; ++k) Tick();
        }
    }
    void WaitOut(uint32_t ms) {
        const uint64_t end = t + ms;
        while (t < end) Tick();
    }
    void HoldLevel(int mv, uint32_t ms) {
        hal.SetAdcMilliVolts(ADC_CH_SWC1, mv);
        const uint64_t end = t + ms;
        while (t < end) Tick();
    }
    // Return the level to idle and settle, which is what commits a prompt.
    void ReleaseLevel() {
        hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
        for (int i = 0; i < 20; ++i) Tick();
    }
};
}  // namespace

TEST(LearnWizard, AUX1PressesAreCountedWithTheSameClassifierTheChannelsUse) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    ASSERT_TRUE(r.wiz.Active());
    r.PressAux(2);
    EXPECT_EQ(r.wiz.PressCount(), 2);
}

TEST(LearnWizard, AStrayHoldWithNoPressesSelectsNothing) {
    // Zero presses is not a selection: entering a learn on a phantom would
    // create a button the user never pressed.
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    for (int i = 0; i < 10; ++i) r.Tick();
    r.wiz.Enter(r.t);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    EXPECT_EQ(r.wiz.PressCount(), 0);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kSelectButton)
        << "with nothing pressed there is no slot to learn";
}

TEST(LearnWizard, APauseAfterThePressesMovesToThePrompt) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();
    r.wiz.Enter(r.t);
    r.PressAux(2);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);
    EXPECT_EQ(r.wiz.SelectedSlot(), 2);
}

TEST(LearnWizard, AFullLearnCompletesWithNoLinkPresent) {
    // FR-31's whole point: this must work with no app and no USB.
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    ASSERT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);

    // Hold the physical button, then let go. The commit happens on RELEASE: the
    // release reading is the level travelling back to idle, so it must not be
    // folded into the spread (doing so rejected every learn as too noisy).
    r.HoldLevel(kLevelHeldMv, 400);
    r.ReleaseLevel();
    EXPECT_EQ(r.wiz.ButtonCount(), 1) << "the learned button must be stored on the profile";
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kLevelHeldMv);
    // And the id was generated, since a headless learn has no app to name it.
    EXPECT_GT(std::strlen(r.wiz.Profile().buttons[0].id), 0u);
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone);
}

TEST(LearnWizard, AnAtIdleHoldIsRejectedAndDoesNotBecomeAButton) {
    // A hold that never leaves the idle band must be refused, not stored as a
    // button at the idle voltage -- that button would swallow every release.
    //
    // A press is detected by the level leaving the idle band, so a hold AT idle
    // is not a "press" at all: the wizard keeps waiting and selects nothing. That
    // is the correct outcome (nothing is learned), and the assertion is on what
    // the user is left with rather than on a rejection code, because the wizard
    // never got far enough to have one.
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    ASSERT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);

    // Hold AT idle: the button was never pressed.
    r.HoldLevel(kLevelIdleMv, 400);
    EXPECT_EQ(r.wiz.ButtonCount(), 0)
        << "an at-idle hold must never become a button";
}

TEST(LearnWizard, TwoButtonsLearnInSequenceWithoutTheSecondClobberingTheFirst) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    r.HoldLevel(kLevelHeldMv, 400);
    r.ReleaseLevel();
    ASSERT_EQ(r.wiz.ButtonCount(), 1);

    // Second button: 2 presses, a different level.
    r.WaitOut(200);
    r.PressAux(2);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    r.HoldLevel(1785, 400);
    r.ReleaseLevel();
    EXPECT_EQ(r.wiz.ButtonCount(), 2) << "the second learn must add, not replace";
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kLevelHeldMv)
        << "and it must not disturb the first";
    EXPECT_EQ(r.wiz.Profile().buttons[1].mv_center, 1785);
}

TEST(LearnWizard, EnteringAndExitingLeavesTheLedInTheSpecStates) {
    // Spec 7.4: enter -> LED_STAT alternate; exit -> solid. A wizard that left
    // the LED alternating after finishing would look like it was still waiting.
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.wiz.Enter(r.t);
    EXPECT_TRUE(r.wiz.Active());
    r.wiz.Exit(r.t);
    EXPECT_FALSE(r.wiz.Active());
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kExit);
}

TEST(LearnWizard, EnteringStartsAFreshProfileRatherThanEditingTheOldOne) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.wiz.Enter(r.t);
    EXPECT_EQ(r.wiz.ButtonCount(), 0) << "a new learn starts empty";
    EXPECT_EQ(r.wiz.PressCount(), 0);
}

TEST(LearnWizard, ReEnteringAfterAnAbandonedPromptDoesNotCommitAStaleIdleWindow) {
    // The abandonment path is a real one: the AUX1 programming hold is the SAME
    // gesture at 1.5 s and 3 s (spec 8.2), so a user who holds a little too long
    // to start programming lands in maintenance and the running learn is exited
    // from inside `ServicePrompt`'s state. `prompt_pressed_` says "the user is
    // already holding the button" and is cleared only in ServicePrompt, so it
    // survives that exit and the next learn inherits it.
    //
    // Inheriting it is wrong in the direction that hurts: the next learn skips the
    // wait-for-press gate and samples the moment it is entered -- while the user
    // is still holding AUX1 and their hand is not on the wheel button at all. The
    // readings are the idle line, which commits into a button centred on idle with
    // a window wide enough to swallow the real button's level.
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    // First learn: reach the prompt and start sampling, then abandon it.
    r.wiz.Enter(r.t, /*aux_held=*/false);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    r.HoldLevel(kLevelHeldMv, 200);        // the user presses and holds
    ASSERT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);
    r.wiz.Exit(r.t);

    // Second learn: select a slot, then do NOT touch the wheel button. The level
    // sits at idle the whole time, so a prompt that behaves must simply WAIT.
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    r.wiz.Enter(r.t, /*aux_held=*/false);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);

    // The stale flag shows up as an instant commit-and-reject: the prompt samples
    // on its very first tick instead of waiting for a press, finds the idle line
    // "at_idle", and drops straight back to selection. The user sees the
    // LEARN_REJECT tone the moment the prompt starts, without having touched the
    // button -- and the wizard is back at the menu before they can press it.
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt)
        << "the prompt committed before the user pressed anything (stale prompt_pressed_)";
    r.WaitOut(400);
    EXPECT_EQ(r.wiz.ButtonCount(), 0)
        << "a learn with no press committed a button from the stale prompt state";
}

TEST(LearnWizard, AFullLadderIsRefusedRatherThanReportedAsLearned) {
    // The two learn write paths must agree about a full ladder. `HandleLearnCommit`
    // (the app path) refuses with `no_space`; the wizard used to BEEP LEARN_OK,
    // set `committed_`, and store NOTHING -- so `ApplyLearnedProfile` persisted an
    // unchanged ladder, `LastLearnPersisted()` returned true, and the app was told
    // the learn was durable. Spec 7.4 step 6 makes "accept" mean "BEEP LEARN_OK,
    // store LadderButton", and FR-29 requires a rejection to say WHY; a learn that
    // reports success and stores no button is the silent failure this project
    // keeps re-finding.
    //
    // Reachable without any contrivance: a ladder the app filled to 16 buttons
    // carries APP slugs, while the headless wizard generates its own `swc1_bt<n>`
    // -- so a selection matches no existing id and takes the APPEND path, which
    // has no room. (Holding AUX1 seventeen times reaches the same branch.)
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    // A full ladder of app-named buttons, clustered at the low end so the held
    // level below is unambiguous against every one of them.
    LadderProfile full{};
    full.learned_idle_mv = kLevelIdleMv;
    full.count = kLadderMaxButtons;
    for (int i = 0; i < kLadderMaxButtons; ++i) {
        std::snprintf(full.buttons[i].id, sizeof(full.buttons[i].id), "app_%d", i);
        full.buttons[i].mv_center = static_cast<MilliVolt>(300 + i * 30);
        full.buttons[i].mv_tolerance = 10;
    }

    r.wiz.Enter(r.t, /*aux_held=*/false, &full);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    ASSERT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);

    r.HoldLevel(kLevelHeldMv, 400);
    r.ReleaseLevel();

    EXPECT_EQ(r.wiz.ButtonCount(), kLadderMaxButtons)
        << "a full ladder must not gain a seventeenth button";
    EXPECT_NE(r.wiz.LastResult(), LearnReject::kNone)
        << "a learn that stored nothing must not be reported as accepted -- "
           "the caller persists on kNone, so this is 'LEARN_OK for a lost button'";
    for (int i = 0; i < kLadderMaxButtons; ++i) {
        EXPECT_NE(r.wiz.Profile().buttons[i].mv_center, kLevelHeldMv)
            << "the measured level must not have been written to slot " << i;
    }
}

/*
 * A FAILED AUX1 conversion must not count as a selection press.
 *
 * `IHAL::adc_read_mv` returns -1 on error, and 0 mV is a LEGAL reading here --
 * AUX1 is a switch to ground, so 0 is the FULLY PRESSED level. Fed to the
 * classifier, -1 gives a ratio of ~0 permille against the profile's 3300 mV
 * idle, which lands inside the AUX window and classifies as **kPressed**: one
 * bad conversion therefore counted a press the user never made, and a run of
 * them walked the slot menu with nobody touching the button. `SystemOrchestrator`
 * guards its own AUX1 read (ServiceLearn); this one did not.
 */
TEST(LearnWizard, AFailedAuxConversionDoesNotCountAsASelectionPress) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    ASSERT_TRUE(r.wiz.Active());

    // The HAL's error code, held for the whole selection window -- long past
    // kSelectGapMs, so a phantom press would have advanced the wizard to the
    // prompt on its own.
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, -1);
    r.WaitOut(LearnWizard::kSelectGapMs + 500);

    EXPECT_EQ(r.wiz.PressCount(), 0)
        << "a failed ADC read is not an AUX1 press; counting one here walks the "
           "slot menu with nobody touching the button";
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kSelectButton)
        << "a phantom press must not have been able to end selection by itself";
}

/*
 * A failed ladder conversion during the prompt must not be sampled as a press.
 *
 * `off_idle = level_mv - idle_mv` turns the HAL's -1 into a large NEGATIVE
 * excursion, so `(off_idle < -kPromptPressDetectMv)` is true and the wizard read
 * a bad conversion as the user pressing -- then `AddSample(-1, ...)` latched
 * `out_of_range_seen_`, which `Commit` reports as `out_of_range`. So one ADC
 * glitch anywhere in the prompt blamed the user's wiring for a learn that never
 * got a usable measurement, and the rejection named the wrong cause.
 */
TEST(LearnWizard, AFailedLevelConversionDoesNotStartOrPoisonThePrompt) {
    Rig r;
    r.hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kLevelIdleMv);
    for (int i = 0; i < 10; ++i) r.Tick();

    r.wiz.Enter(r.t);
    r.PressAux(1);
    r.WaitOut(LearnWizard::kSelectGapMs + 200);
    ASSERT_EQ(r.wiz.CurrentState(), LearnWizard::State::kPrompt);

    // A failed read while the user has not pressed. It must not even look like a
    // press: with the guard, the prompt stays waiting and no sample is taken.
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, -1);
    for (int i = 0; i < 20; ++i) r.Tick();
    EXPECT_EQ(r.wiz.ButtonCount(), 0)
        << "a failed read is not a press, so it must not start sampling";

    // Now a real, clean press and release: the one glitch above must not have
    // poisoned the session into an `out_of_range` rejection.
    r.HoldLevel(kLevelHeldMv, 400);
    r.ReleaseLevel();
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone)
        << "a failed read must not latch out_of_range for a learn that then got "
           "a clean measurement -- that blames the wiring for an ADC glitch";
    EXPECT_EQ(r.wiz.ButtonCount(), 1);
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kLevelHeldMv);
}
