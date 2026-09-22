#include "Learning/LearnWizard.h"

#include "MockHAL.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

/*
 * The headless learn is the 2022 interaction (spec 7.5): hold the modifier
 * (AUX1), press the input being programmed, release the modifier. These tests
 * drive it purely through MockHal's ADC values and clock, with NO link.
 *
 * The wizard no longer counts AUX1 presses to pick a menu slot, so there is no
 * `PressAux`/`kSelectGapMs` machinery here. What the wizard does now is NAME the
 * input by which line left its idle, which is the whole of "no channel
 * selection".
 */
namespace {

// SWC1's ladder idle and a button well below it. A press pulls the input DOWN
// (spec 6.3), so a held button is a LOWER voltage than idle.
constexpr int kSwc1IdleMv   = 2835;
constexpr int kSwc1ButtonMv = 1430;
constexpr int kSwc2IdleMv   = 2810;
constexpr int kSwc2ButtonMv = 980;
constexpr int kAux2IdleMv   = 3300;
constexpr int kAux2PressedMv = 60;

struct Rig {
    MockHal        hal;
    BuzzerGrammar  buzzer{&hal.InterfaceRef(), 2};
    LedGrammar     leds{&hal.InterfaceRef(), 2};
    LearnWizard    wiz{&hal.InterfaceRef(), &buzzer, &leds};
    uint64_t       t = 1000;

    // A two-ladder + one-switch input list, which is what an "SWC1, SWC2, AUX2"
    // install hands the wizard. The caller rebuilds this every tick because it
    // carries the LIVE idle of each input.
    LearnInputs Inputs() const {
        LearnInputs in;
        LearnInput &swc1 = in.in[in.count++];
        swc1.wire_channel = 0;
        swc1.adc = ADC_CH_SWC1;
        swc1.idle_mv = kSwc1IdleMv;
        swc1.is_ladder = true;
        swc1.existing = nullptr;
        swc1.id = nullptr;

        LearnInput &swc2 = in.in[in.count++];
        swc2.wire_channel = 1;
        swc2.adc = ADC_CH_SWC2;
        swc2.idle_mv = kSwc2IdleMv;
        swc2.is_ladder = true;
        swc2.existing = nullptr;
        swc2.id = nullptr;

        LearnInput &aux2 = in.in[in.count++];
        aux2.wire_channel = 2 + 1;   // kAuxWireChannelBase(2) + index 1
        aux2.adc = ADC_CH_AUX2;
        aux2.idle_mv = kAux2IdleMv;
        aux2.is_ladder = false;
        aux2.existing = nullptr;
        aux2.id = "aux2";
        return in;
    }

    void Tick(uint32_t ms = 5) {
        buzzer.Update(t);
        leds.Update(t);
        wiz.Tick(Inputs(), t, 0);
        t += ms;
    }
    void WaitOut(uint32_t ms) {
        const uint64_t end = t + ms;
        while (t < end) Tick();
    }
    void Hold(AdcChannel ch, int mv, uint32_t ms) {
        hal.SetAdcMilliVolts(ch, mv);
        WaitOut(ms);
    }
    void AllIdle() {
        hal.SetAdcMilliVolts(ADC_CH_SWC1, kSwc1IdleMv);
        hal.SetAdcMilliVolts(ADC_CH_SWC2, kSwc2IdleMv);
        hal.SetAdcMilliVolts(ADC_CH_AUX2, kAux2IdleMv);
    }
};

}  // namespace

TEST(LearnWizard, AStrayHoldThatNamesNoInputCommitsNothing) {
    // A hold that never sees an input leave idle has nothing to store. Committing
    // a button fabricated from an idle line would be a phantom the user never
    // pressed -- the outcome the old menu's "zero presses selects nothing" guarded.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    ASSERT_TRUE(r.wiz.Active());
    r.WaitOut(2000);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kArmed)
        << "with nothing pressed the wizard must still be waiting for an input";
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone);
}

TEST(LearnWizard, ASwcPressNamesSwc1AndStoresTheButton) {
    // The input is named by which line moved off idle -- no menu. This is the
    // whole of "you just do the gesture on whichever swc_in switch".
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    // Press SWC1 and hold, then release the MODIFIER (which is what commits).
    r.Hold(ADC_CH_SWC1, kSwc1ButtonMv, 300);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kSampling);
    EXPECT_EQ(r.wiz.TargetWireChannel(), 0) << "the moving input is SWC1";
    EXPECT_TRUE(r.wiz.TargetIsLadder());

    r.wiz.Release(r.t);
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone);
    EXPECT_EQ(r.wiz.ButtonCount(), 1) << "the learned button must be stored";
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kSwc1ButtonMv);
    EXPECT_EQ(r.wiz.Profile().learned_idle_mv, kSwc1IdleMv);
    EXPECT_GT(std::strlen(r.wiz.Profile().buttons[0].id), 0u);
}

TEST(LearnWizard, PressingSwc2NamesSwc2NotTheFirstInput) {
    // The distinction the whole redesign exists for: pressing the SECOND channel
    // must program the second channel, not a hardcoded channel 0 (open item N-23).
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_SWC2, kSwc2ButtonMv, 300);
    EXPECT_EQ(r.wiz.TargetWireChannel(), 1) << "SWC2 moved, so SWC2 is the target";

    r.wiz.Release(r.t);
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone);
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kSwc2ButtonMv);
    EXPECT_EQ(r.wiz.Profile().learned_idle_mv, kSwc2IdleMv)
        << "and the denominator is SWC2's own idle, not SWC1's";
}

TEST(LearnWizard, AnAuxPressNamesTheAuxSwitchAndStoresItsWindow) {
    // An AUX switch is a different KIND of input: its "learn" records the switch
    // window (centre + a wide tolerance), not a ladder button.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_AUX2, kAux2PressedMv, 300);
    EXPECT_FALSE(r.wiz.TargetIsLadder()) << "AUX2 is a switch, not a ladder";
    EXPECT_EQ(r.wiz.TargetWireChannel(), 2 + 1);

    r.wiz.Release(r.t);
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone);
    ASSERT_EQ(r.wiz.ButtonCount(), 1);
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kAux2PressedMv);
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_tolerance, LearnWizard::kSwitchToleranceMv);
    EXPECT_STREQ(r.wiz.Profile().buttons[0].id, "aux2")
        << "an AUX input's id is its config entry's, supplied by the caller";
}

TEST(LearnWizard, AFailedAdcReadNeverNamesAnInput) {
    // `adc_read_mv` returns -1 on error and 0 mV is legal. An unchecked -1 against
    // a positive idle is the largest possible excursion, so a run of bad reads
    // would name an input nobody touched and store a phantom button.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, -1);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC2, -1);
    r.hal.SetAdcMilliVolts(ADC_CH_AUX2, -1);
    r.WaitOut(500);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kArmed)
        << "a failed conversion is not a press and must not name an input";
}

TEST(LearnWizard, ALightTouchInsideTheIdleBandIsRefusedNotStoredDead) {
    // A switch that only barely left idle would otherwise store a centre inside
    // the classifier's idle band, so its own press would classify as idle and the
    // input would be dead -- LEARN_OK over a switch that never fires. The at-idle
    // gate shares the classifier's own margin (`kIdleMarginPermille`) for exactly
    // this reason.
    //
    // A 50 mV excursion crosses the 40 mV detect threshold (so the input IS named)
    // but lands at ratio ~985, inside the 30 permille idle band.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_AUX2, kAux2IdleMv - 50, 300);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kSampling)
        << "the excursion crossed the detect threshold, so the input is named";

    r.wiz.Release(r.t);
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kAtIdle)
        << "a level inside the idle band must be refused, not stored as a dead switch";
    EXPECT_EQ(r.wiz.ButtonCount(), 0) << "and nothing may be stored for it";
}

TEST(LearnWizard, ATouchUnderTheDetectThresholdIsNotEvenAPress) {
    // Below the detect threshold there is no press at all: the wizard keeps
    // waiting rather than naming an input on noise.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_AUX2, kAux2IdleMv - 30, 300);
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kArmed)
        << "a touch under the detect threshold is not a press";
    EXPECT_FALSE(r.wiz.ConsumeCommitted());
}

TEST(LearnWizard, ReLearningAnExistingButtonCorrectsItRatherThanRefusing) {
    // A re-measure lands inside the existing button's own window by definition, so
    // leaving that entry in the neighbour set would refuse the button for being
    // too close to ITSELF -- making it impossible to correct.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    LadderProfile seed{};
    seed.learned_idle_mv = kSwc1IdleMv;
    seed.count = 1;
    std::strncpy(seed.buttons[0].id, "swc1_bt1", sizeof(seed.buttons[0].id) - 1);
    std::strncpy(seed.buttons[0].name, "Button 1", sizeof(seed.buttons[0].name) - 1);
    seed.buttons[0].mv_center = kSwc1ButtonMv;
    seed.buttons[0].mv_tolerance = 40;

    LearnInputs in = r.Inputs();
    in.in[0].existing = &seed;

    r.wiz.Arm(r.t);
    // Move the button slightly and re-measure it. The press is inside the old
    // window (1430 +/- 40), so a wizard that failed to exclude the seed would
    // return kTooCloseToExisting.
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kSwc1ButtonMv + 20);
    for (int i = 0; i < 60; ++i) {
        r.wiz.Tick(in, r.t, 0);
        r.t += 5;
    }
    r.wiz.Release(r.t);

    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone)
        << "a re-measure must correct the button, not be refused as too close to itself";
    ASSERT_EQ(r.wiz.ButtonCount(), 1) << "and it must replace, not duplicate";
    EXPECT_STREQ(r.wiz.Profile().buttons[0].id, "swc1_bt1")
        << "the re-measure keeps the existing id";
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, kSwc1ButtonMv + 20);
}

TEST(LearnWizard, ASecondNewButtonIsNamedTheNextSlot) {
    // A NEW button (one that matches no existing window) is named with the next
    // free `swc<ch>_bt<n>`, so a headless learn keeps the naming the old slot menu
    // produced.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    LadderProfile seed{};
    seed.learned_idle_mv = kSwc1IdleMv;
    seed.count = 1;
    std::strncpy(seed.buttons[0].id, "swc1_bt1", sizeof(seed.buttons[0].id) - 1);
    seed.buttons[0].mv_center = 600;
    seed.buttons[0].mv_tolerance = 40;

    LearnInputs in = r.Inputs();
    in.in[0].existing = &seed;

    r.wiz.Arm(r.t);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kSwc1ButtonMv);
    for (int i = 0; i < 60; ++i) {
        r.wiz.Tick(in, r.t, 0);
        r.t += 5;
    }
    r.wiz.Release(r.t);

    ASSERT_EQ(r.wiz.LastResult(), LearnReject::kNone);
    ASSERT_EQ(r.wiz.ButtonCount(), 2) << "the new button must ADD to the seed";
    EXPECT_EQ(r.wiz.Profile().buttons[1].mv_center, kSwc1ButtonMv);
    EXPECT_STREQ(r.wiz.Profile().buttons[1].id, "swc1_bt2")
        << "a new button takes the next free slot number";
    EXPECT_EQ(r.wiz.Profile().buttons[0].mv_center, 600)
        << "and the seeded button is preserved, not clobbered";
}

TEST(LearnWizard, AFullLadderIsRefusedRatherThanReportedAsLearned) {
    // A real measurement that cannot be STORED is a REJECTION, not a commit.
    // Reporting it as `kNone` plays LEARN_OK while the caller persists an
    // unchanged ladder -- the silent "success for a lost button" the app path
    // refuses with `no_space` (FR-29, spec 7.4 step 6). Reachable with no
    // contrivance: a ladder the app filled to 16 with app slugs, then a headless
    // learn of a genuinely new level.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    // A full ladder of app-named buttons, clustered low so the held level below is
    // unambiguous against every one of them.
    LadderProfile full{};
    full.learned_idle_mv = kSwc1IdleMv;
    full.count = kLadderMaxButtons;
    for (int i = 0; i < kLadderMaxButtons; ++i) {
        std::snprintf(full.buttons[i].id, sizeof(full.buttons[i].id), "app_%d", i);
        full.buttons[i].mv_center = static_cast<MilliVolt>(300 + i * 30);
        full.buttons[i].mv_tolerance = 10;
    }

    LearnInputs in = r.Inputs();
    in.in[0].existing = &full;

    r.wiz.Arm(r.t);
    r.hal.SetAdcMilliVolts(ADC_CH_SWC1, kSwc1ButtonMv);
    for (int i = 0; i < 60; ++i) {
        r.wiz.Tick(in, r.t, 0);
        r.t += 5;
    }
    r.wiz.Release(r.t);

    EXPECT_EQ(r.wiz.ButtonCount(), kLadderMaxButtons)
        << "a full ladder must not gain a seventeenth button";
    EXPECT_NE(r.wiz.LastResult(), LearnReject::kNone)
        << "a learn that stored nothing must not be reported as accepted -- the "
           "caller persists on kNone, so this is 'LEARN_OK for a lost button'";
    EXPECT_FALSE(r.wiz.ConsumeCommitted()) << "and nothing may be flagged to persist";
}

TEST(LearnWizard, AbandonCommitsNothing) {
    // The 3 s maintenance escalation abandons a running learn: a user who over-held
    // is reaching maintenance, not finishing a programming session.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_SWC1, kSwc1ButtonMv, 200);
    r.wiz.Abandon(r.t);
    EXPECT_FALSE(r.wiz.Active());
    EXPECT_EQ(r.wiz.LastResult(), LearnReject::kNone)
        << "abandon is not a commit, but it is also not a rejection";
    EXPECT_FALSE(r.wiz.ConsumeCommitted()) << "nothing was committed to persist";
}

TEST(LearnWizard, ArmAndAbandonLeaveTheLedInTheSpecStates) {
    // Spec 7.4: arm -> LED_STAT alternate; finish -> solid. A wizard that left the
    // LED alternating after finishing would look like it was still waiting.
    Rig r;
    r.AllIdle();
    r.wiz.Arm(r.t);
    EXPECT_TRUE(r.wiz.Active());
    r.wiz.Abandon(r.t);
    EXPECT_FALSE(r.wiz.Active());
    EXPECT_EQ(r.wiz.CurrentState(), LearnWizard::State::kExit);
}

TEST(LearnWizard, ACommitIsConsumedExactlyOnce) {
    // The caller persists on `ConsumeCommitted`, so it must report the fact once
    // and then clear -- otherwise the caller would save on every tick.
    Rig r;
    r.AllIdle();
    for (int i = 0; i < 5; ++i) r.Tick();

    r.wiz.Arm(r.t);
    r.Hold(ADC_CH_SWC1, kSwc1ButtonMv, 300);
    r.wiz.Release(r.t);
    EXPECT_TRUE(r.wiz.ConsumeCommitted());
    EXPECT_FALSE(r.wiz.ConsumeCommitted()) << "the commit is a one-tick fact";
}
