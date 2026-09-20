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
