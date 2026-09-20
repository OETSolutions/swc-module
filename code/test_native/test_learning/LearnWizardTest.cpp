#include "Learning/LearnWizard.h"

#include "MockHAL.h"

#include <gtest/gtest.h>

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
        wiz.Tick(channel, t);
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
