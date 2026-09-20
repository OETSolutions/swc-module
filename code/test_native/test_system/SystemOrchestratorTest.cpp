#include "System/SystemOrchestrator.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Config/ConfigStore.h"
#include "MockHAL.h"

namespace {

SystemOrchestrator MakeOrch(MockHal &hal) {
    MockHal::Defaults d;   // a valid config + the default timings
    return SystemOrchestrator(&hal.InterfaceRef(), d.config, d.timings);
}

// Poll the loop at the cadence the plan's tests use (10 ms), for `ms`.
void PollFor(SystemOrchestrator &o, MockHal &hal, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(10);
    }
}

// The sense reading that yields a 4980 mV KEY idle -- a 5 V head unit, which
// selects gain 1.82 (spec 6.2 step 4).
constexpr int kSenseFor5vHeadUnit = 2490;

}  // namespace

TEST(SystemOrchestrator, SafeIdleIsEstablishedBeforeAnythingElse) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    EXPECT_TRUE(o.SafeIdleEstablished());
    // The DAC must have been written during Boot, before any USB work.
    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), 0);
}

TEST(SystemOrchestrator, BootDrivesTheAdjustChannelIntoTheOneKiloOhmPulldown) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    // Gain 1.82 requires V_ADJ at 0V, which is the 1k pulldown power-down mode.
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K);
}

TEST(SystemOrchestrator, APressProducesTheBoundOutputLevelAndThenReleases) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // vol_up at its learned 1430 mV. The ladder pulls DOWN from an idle of
    // 2835 mV, so a press is a LOWER reading -- never above idle.
    //
    // vol_up binds SINGLE (a 2400 mV output) AND LONG (a release), so the press
    // must stay UNDECIDED until it resolves (spec 6.6): driving at press time
    // would let the head unit act on a level the gesture had not settled on yet.
    // The press is therefore held past the debounce, released, and allowed the
    // double-press window to close.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);                              // press + debounce
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);                              // release + resolve the SINGLE

    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "a resolved press must change the output";

    // Let the recognition pulse elapse; the line must return to idle.
    PollFor(o, hal, 400);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "must return to idle";
}

TEST(SystemOrchestrator, ServesPressesWithNoUsbAndNoApp) {
    // FR-42: this is the normal in-car case. Nothing in the orchestrator may
    // depend on a link being present.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);
}

TEST(SystemOrchestrator, TheOutputIsNotDrivenWhileTheGestureIsUndecided) {
    // Spec 6.6 rule 1. vol_up binds LONG, so a press could still become a LONG
    // and mean something else entirely. Driving the SINGLE level at press time
    // would make the head unit act twice -- the phantom press of spec 6.7.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // press, and hold well under 750 ms
    PollFor(o, hal, 200);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "an undecided gesture must not drive the output";
}

TEST(SystemOrchestrator, ARailSagDuringAPressReleasesTheKey) {
    // FR-39: never leave a phantom key driven. FR-30: a 3V3 sag to <=20% of the
    // learned idle is a RAIL FAULT, not an idle reading and not a button.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // Drive a key first: press and let the SINGLE resolve.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);

    // The +3V3 rail sags: 500 mV is 18% of the learned 2835 mV idle, and the
    // KEY line collapses with it (2 x 250 = 500 mV, outside the 1.8-5.2 V
    // envelope), so the head unit is gone.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 500);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);
    PollFor(o, hal, 400);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a rail fault during a press must release the key, not hold it";
}

TEST(SystemOrchestrator, AnUnlearnedLevelNeverChangesTheOutput) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // 2400 mV: between the highest button (next, 2145 +/- 110) and idle (2835),
    // so it matches no learned button and is far above the rail-fault floor.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);
    PollFor(o, hal, 1500);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "UNKNOWN must do nothing -- the failure mode of a guess is worse";
}

TEST(SystemOrchestrator, TheTwoChannelsAreServedIndependently) {
    MockHal hal;
    MockHal::Defaults d;
    d.config.channel_count = 2;
    d.config.channels[1] = d.config.channels[0];
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);
    o.Boot();

    const int writes_after_boot = hal.DacWriteCount(DAC_CH_KEY2);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // press only channel 1
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);   // channel 2 idle
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), writes_after_boot)
        << "channel 1's press must drive channel 1";
    // Channel 2 stays idle, so its only writes are the boot idle write.
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY2), 1)
        << "channel 2 must not be disturbed by channel 1's press";
}

TEST(SystemOrchestrator, TickIsCheapEnoughToRunAtThePollCadence) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    // Let the boot announcement finish before measuring. BOOT_OK is 60/60 x1
    // (spec 7.2), so 300 ms is comfortably past it -- and the count must be taken
    // AFTER it, or this test would be measuring the boot feedback rather than
    // the idle ticks it is named for.
    PollFor(o, hal, 300);
    const int before = hal.BuzzerOnCount();

    // Idle ticks must not drive the buzzer at all: feedback is scheduled rather
    // than synchronous (FR-21), and an idle tick that touches the line is the
    // first symptom of feedback being able to delay a press.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // idle
    PollFor(o, hal, 500);
    EXPECT_EQ(hal.BuzzerOnCount(), before) << "idle ticks must be silent";
    EXPECT_FALSE(hal.BuzzerIsOn()) << "and must leave the line off";
}

// --- FR-25: the transparent pass-through (spec 6.9) -------------------------
//
// With no stored config there are no learned windows, so classification against
// learned buttons cannot work -- but the wheel must still do something. These
// tests drive the no-config path by leaving MockHal's NVS EMPTY, which is what
// makes ConfigStore report kNoConfig (it is not the same as a corrupt config).

namespace {

// An orchestrator whose ConfigStore is empty, so Boot() selects pass-through.
// The config is still supplied because it carries the timings and the channel
// count; what makes this the pass-through case is the empty NVS.
SystemOrchestrator MakeUnconfigured(MockHal &hal) {
    MockHal::Defaults d;
    d.config.channel_count = 1;
    d.config.binding_count = 0;
    hal.ClearNvs();
    return SystemOrchestrator(&hal.InterfaceRef(), d.config, d.timings);
}

}  // namespace

TEST(SystemOrchestrator, WithNoConfigTheDeviceEntersPassThrough) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);          // the wheel at idle
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    EXPECT_TRUE(o.PassThroughActive())
        << "an unconfigured device must serve the wheel (FR-25), not sit inert";
    // Safe idle still comes first (FR-13): pass-through changes WHAT is served,
    // never the ordering that makes the output safe before anything acts on it.
    EXPECT_TRUE(o.SafeIdleEstablished());
}

TEST(SystemOrchestrator, APressWithNoConfigDrivesAKeyMappedByRatio) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);          // idle, the reference
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // The wheel's button at 1430 mV: 1430/2835 = 504 permille. Mapped onto the
    // head unit's own idle (4980 mV) that is ~2510 mV -- NOT 1430, and that is
    // the point: the wheel's ladder and the head unit's need not match, so
    // copying the millivolts would land on the wrong key.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);

    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a press with no config must still drive the KEY line";
    // The driven level must be ABOVE the output floor: a literal voltage copy
    // would ask for 1430 mV and be clamped to 1800 mV, which is a different key
    // on the head unit. Comparing codes is exact here because the gain mode is
    // known (kSenseFor5vHeadUnit selects 1.82).
    EXPECT_GT(hal.LastDacCode(DAC_CH_KEY1),
              GainPolicyCodeForTarget(o.ChannelGainMode(0), kOutputFloorMv).dac_code)
        << "the mapped level must be above the output floor, not clamped to it";
}

TEST(SystemOrchestrator, ReleasingThePressWithNoConfigReturnsToIdle) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // released
    PollFor(o, hal, 400);

    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "releasing must return to idle; a held key is the phantom-key hazard";
}

TEST(SystemOrchestrator, WithNoLadderReferencePassThroughIsDisabledRatherThanGuessed) {
    // A dead or unreadable ladder gives no denominator, and a guessed one would
    // map every press to a voltage nothing defined. Serving nothing is the safe
    // direction.
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, -1);     // the HAL's error code
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    EXPECT_FALSE(o.PassThroughActive())
        << "no reference means no pass-through, not a fabricated one";
    EXPECT_TRUE(o.SafeIdleEstablished()) << "and the safe idle must still hold";
}

TEST(SystemOrchestrator, AConfiguredDeviceDoesNotUsePassThrough) {
    // The inverse, and the more dangerous direction: a CONFIGURED device must
    // classify against its learned windows. If pass-through were ever left on, a
    // configured device would silently ignore every binding it has -- the wheel
    // would "work" while doing the wrong thing.
    MockHal hal;
    MockHal::Defaults d;   // carries spec 3.7's worked ladder
    // ACTUALLY STORE a config: an empty NVS reports kNoConfig, and `MakeOrch`
    // leaves it empty, so a test written with MakeOrch would assert the opposite
    // of what it intends. (It did.)
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config)) << "the fixture must persist for this test to mean anything";

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    EXPECT_FALSE(o.PassThroughActive())
        << "a stored config means learned-window classification, not pass-through";
}

TEST(SystemOrchestrator, NoConfigAtAllStillMarksSafeIdleBeforePassThrough) {
    // FR-13 vs FR-25: pass-through changes WHAT is served, never WHEN the output
    // becomes safe. A watchdog reset mid-transfer must not leave a phantom key
    // driven, and that guarantee cannot depend on whether a config was stored.
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    EXPECT_TRUE(o.SafeIdleEstablished());
    EXPECT_TRUE(o.PassThroughActive());
}

// --- spec 4.3's `event`: one recognized gesture, reported to the link ---------

namespace {

// The sink signature is `void(*)(void*, const GestureEventRecord&)`, so a
// capturing lambda cannot be used directly. Recording into a file-scope vector
// keeps the test free of a global function per assertion, and the vector is
// cleared at the start of each test that uses it.
std::vector<SystemOrchestrator::GestureEventRecord> g_reported;

void RecordGesture(void *, const SystemOrchestrator::GestureEventRecord &ev) {
    g_reported.push_back(ev);
}

}  // namespace

TEST(SystemOrchestrator, ARecognizedGestureIsReportedAsAnEventWithTheLearnedButtonId) {
    // Spec 4.3 calls `event` "the core event", but the router emitted no such
    // frame anywhere: the app's live view had no source for "which button the
    // device thinks is pressed" outside a learn run. This asserts the frame's
    // payload now exists, and that `button` is the learned ID -- the app's
    // bindings grid and ladder view are keyed by the id, so an index here would
    // force every consumer to re-derive a mapping the device already has.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    // vol_up binds SINGLE, so the press stays undecided until it resolves.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    ASSERT_EQ(g_reported.size(), 1u) << "one resolved press must report exactly one event";
    EXPECT_EQ(std::string(g_reported[0].button_id), "vol_up");
    EXPECT_EQ(g_reported[0].gesture, Gesture::kSingle);
    EXPECT_EQ(g_reported[0].channel_index, 0);
    // The FILTERED level the decision was made on (FR-3), not a fresh conversion
    // and not an unfiltered one labelled "raw".
    EXPECT_GT(g_reported[0].level_mv, 0);
    EXPECT_FALSE(g_reported[0].button_id == nullptr);
}

TEST(SystemOrchestrator, NoReportedEventWithoutASink) {
    // FR-42/spec 6.6: the device serves every press with no app attached. A sink
    // is optional, and a null one must not be called through.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    // Deliberately no SetGestureSink.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);
    // The output must still have been driven -- the press is served regardless.
    EXPECT_TRUE(o.SafeIdleEstablished());
}

TEST(SystemOrchestrator, AnUnrecognizedPressReportsNothing) {
    // Spec 4.3: an `event` is only emitted for a press that matched a learned
    // window, because FR-12 forbids guessing. A level between windows is the
    // device saying it does not know, and there is then no button to name and no
    // gesture to report -- so the link stays silent rather than naming a button
    // the device did not actually classify.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    // 3000 mV is above every learned window (the lowest centre is 1430 with a
    // 120 mV half-width) and off idle (2835), so it classifies as kUnknown.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    EXPECT_TRUE(g_reported.empty())
        << "an unrecognized level must not be reported as a gesture on a button";
}

// --- FR-31: the headless learn, with no app and no host -----------------------
//
// This is the requirement that the adapter is USABLE and PROGRAMMABLE with
// nothing plugged into USB: a user in a car may not have the head unit out of the
// dash, so the double/long-press bindings must be teachable from AUX1 + buzzes
// alone. The wizard class was fully implemented and TESTED, and nothing in the
// firmware ever constructed or ticked it -- the same defect shape as ActionRunner
// having no caller. So these tests assert the WIRING, not the wizard's internals
// (test_native/test_learning already covers those).

namespace {

constexpr int kAuxReleasedMv = 3300;
constexpr int kAuxPressedMv  = 100;

// Hold AUX1 long enough to toggle the wizard, then release.
void HoldAuxToToggle(SystemOrchestrator &o, MockHal &hal, int mv = kAuxPressedMv) {
    hal.SetAdcMilliVolts(ADC_CH_AUX1, mv);
    PollFor(o, hal, LearnWizard::kEnterHoldMs + 100);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    PollFor(o, hal, 50);
}

// Press AUX1 n times, with pauses long enough to end the selection.
void PressAux(SystemOrchestrator &o, MockHal &hal, int times) {
    for (int i = 0; i < times; ++i) {
        hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
        PollFor(o, hal, 60);
        hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
        PollFor(o, hal, 60);
    }
    // The gap that ends selection and moves to the prompt.
    PollFor(o, hal, LearnWizard::kSelectGapMs + 100);
}

}  // namespace

TEST(SystemOrchestrator, ANullStoreStillRunsAHeadlessLearnButReportsItAsUnsaved) {
    // A bench build with no NVS: the wizard's feedback is real, and the fact that
    // nothing was persisted is REPORTED rather than assumed. Claiming "saved" here
    // would be a lie the user discovers on the next boot.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    // Deliberately no SetStore.
    EXPECT_FALSE(o.LastLearnPersisted());

    HoldAuxToToggle(o, hal);
    EXPECT_TRUE(o.LearnActive()) << "an AUX1 hold must enter the headless learn (FR-31)";
}

TEST(SystemOrchestrator, AUX1HoldTogglesTheHeadlessLearnBothWays) {
    // The SAME gesture enters and leaves (spec 7.4 step 5), which is what lets a
    // user finish programming and get back to driving without a phone.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    HoldAuxToToggle(o, hal);
    EXPECT_FALSE(o.LearnActive()) << "a second AUX1 hold must exit the learn";
}

TEST(SystemOrchestrator, AShortAUX1TouchDoesNotEnterTheLearn) {
    // Spec 7.5 chose a deliberate 1.5 s hold precisely so a stray touch cannot
    // start a learn in traffic. A tap must do nothing.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 200);          // well under kEnterHoldMs
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    PollFor(o, hal, 200);
    EXPECT_FALSE(o.LearnActive());
}

TEST(SystemOrchestrator, AHeadlessLearnStoresAButtonAndItClassifiesImmediately) {
    // The whole point: no app, no host, and the taught button WORKS afterwards.
    // The apply step is load-bearing -- without rebuilding the classifier the
    // device would keep using the old profile until the next boot, and the button
    // the user just taught would do nothing with correct-looking feedback.
    MockHal hal;
    MockHal::Defaults d;
    // An EMPTY ladder, so the learned button is the only one and cannot be
    // rejected as too close to an existing window.
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Enter, select slot 1, then hold the wheel button at 1430 mV.
    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 200);

    const LadderProfile *learned = o.LastLearnedProfile(0);
    ASSERT_NE(learned, nullptr) << "the learn must have committed a button";
    ASSERT_EQ(learned->count, 1);
    // The centre is the mean of what was held, within the ladder's noise.
    EXPECT_NEAR(learned->buttons[0].mv_center, 1430, 30);
    EXPECT_TRUE(o.LastLearnPersisted()) << "a store was attached, so the learn must persist";

    // Exit the wizard, then verify the taught button is CLASSIFIED now -- which
    // is what "learned" means. It does NOT drive the output, and that is correct:
    // a learned profile is a set of WINDOWS, and what a window DOES is a Binding.
    // Asserting the output here would have been asserting that learn also binds,
    // which it does not (spec 7.4's headless loop stores LadderButtons; the
    // gesture-to-action table is the config's).
    HoldAuxToToggle(o, hal);
    g_reported.clear();
    o.SetGestureSink(&RecordGesture, nullptr);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    ASSERT_EQ(g_reported.size(), 1u)
        << "the button taught headlessly must classify without a reboot";
    // The id is GENERATED, because a headless learn has no app to name it.
    EXPECT_EQ(std::string(g_reported[0].button_id), "swc1_bt1");
    EXPECT_EQ(g_reported[0].gesture, Gesture::kSingle);
}

TEST(SystemOrchestrator, AHeadlessLearnedButtonDrivesTheOutputOnceABindingNamesIt) {
    // The other half of the composition: the generated id is DETERMINISTIC
    // ("swc1_bt1"), so a config can bind it in advance and a headless learn fills
    // in the level. This is the shape that lets a user program the adapter with no
    // app at all -- the levels are learned on the device, the actions come from
    // the config.
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    // Pre-bind the slot the wizard will generate, to SINGLE -> 2400 mV.
    d.config.binding_count = 1;
    std::strncpy(d.config.bindings[0].id, "b1", sizeof(d.config.bindings[0].id) - 1);
    d.config.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(d.config.bindings[0].button, "swc1_bt1",
                 sizeof(d.config.bindings[0].button) - 1);
    d.config.bindings[0].gesture = Gesture::kSingle;
    d.config.bindings[0].enabled = true;
    d.config.bindings[0].action_count = 1;
    d.config.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    d.config.bindings[0].actions[0].key_mv = 2400;

    // STORE the config, so Boot LOADS it and leaves pass-through mode. With no
    // stored config the device runs FR-25's transparent pass-through, which
    // bypasses the classifier and the gesture resolver entirely -- so the binding
    // could never fire, and the test would be measuring the wrong mode.
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxToToggle(o, hal);
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 200);
    ASSERT_NE(o.LastLearnedProfile(0), nullptr);
    HoldAuxToToggle(o, hal);   // leave the wizard

    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    // Sample WHILE the pulse is on the line. This binding is SINGLE with no
    // DOUBLE and no LONG, so it resolves the moment the debounce elapses and the
    // 200 ms pulse is over well before the press is released -- reading the DAC
    // afterwards would see the release, not the key. (The neighbouring
    // APressProduces... test can poll longer only because its button also binds
    // LONG, so its SINGLE is delayed and the pulse is still on the line at the
    // end of the window.)
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 120);
    const int driven_code = hal.LastDacCode(DAC_CH_KEY1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_NE(driven_code, idle_code)
        << "a bound button taught headlessly must drive the output without a reboot";
}

TEST(SystemOrchestrator, AHeadlessLearnWithNoUsableIdleReferenceDoesNotCommit) {
    // With no idle reference the press detector would compare a reading against
    // zero, call every level "pressed", and commit a window computed from a
    // fabricated denominator. Refusing is the direction FR-12 takes for an
    // unrecognised level: serve nothing rather than guess.
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 0;   // no reference
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 0);              // and nothing live
    o.Boot();

    HoldAuxToToggle(o, hal);
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 0);
    PollFor(o, hal, 200);

    EXPECT_EQ(o.LastLearnedProfile(0), nullptr)
        << "with no idle reference, nothing may be committed";
}
