#include "System/SystemOrchestrator.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Config/ConfigStore.h"
#include "MockHAL.h"

namespace {

/*
 * A CONFIGURED device: the fixture's config is PERSISTED before Boot.
 *
 * **This helper used to skip the save, and that made 27 tests hollow.** `Boot`
 * asks the NVS whether a config exists; an empty NVS answers `kNoConfig`, which
 * sets the pass-through flag (FR-25) and bypasses learned-window classification
 * for the whole run. So every test built on this helper was exercising the
 * unconfigured code path while its name and comments described the configured
 * one -- and it still passed, because a pass-through device also moves the DAC.
 *
 * `AConfiguredDeviceDoesNotUsePassThrough` had already found this ("an empty NVS
 * reports kNoConfig, and `MakeOrch` leaves it empty... (It did.)") but repaired
 * only itself. `AFreshDeviceTaughtHeadlesslyStopsPassingThrough` hit it too and
 * worked around it with its own save. Fixing the helper is what makes the rest
 * of the suite test what it says it tests.
 */
SystemOrchestrator MakeOrch(MockHal &hal) {
    MockHal::Defaults d;   // a valid config + the default timings
    ConfigStore store(&hal.InterfaceRef());
    // Not asserted: a helper cannot fail a test, and `MakeOrch` is called at
    // construction time in dozens of places. A save that fails leaves the device
    // unconfigured, which the tests that care about it detect directly.
    store.Save(d.config);
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

/*
 * Press `mv` on SWC1, release, and return the DAC code while the pulse is still
 * ON THE LINE -- or the idle code if none was driven.
 *
 * **Why a mid-pulse capture rather than reading the code after the window.** The
 * pulse is only `send_duration_ms` (200 ms) long (spec 6.6 rule 2), so a test
 * that polls a fixed 700 ms past the release and then reads the code sees the
 * RELEASE, not the key -- and would only pass if the pulse were wrongly
 * lengthened. This is the same sampling the headless-learn test already uses.
 *
 * It also makes the resolve timing explicit: a button binding SINGLE+LONG (no
 * DOUBLE) resolves at RELEASE (spec 6.6 rule 3), so the pulse begins there; a
 * button binding DOUBLE would begin it a double-press window later.
 */
int PressAndCaptureDrivenCode(SystemOrchestrator &o, MockHal &hal, int mv) {
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, mv);
    PollFor(o, hal, 100);                 // press + debounce
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    for (uint32_t t = 0; t < 900; t += 10) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(10);
        const int code = hal.LastDacCode(DAC_CH_KEY1);
        if (code != idle_code) return code;   // the pulse is on the line
    }
    return idle_code;                         // nothing was driven
}

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

TEST(SystemOrchestrator, BootAppliesTheGainPolicyWhenTheChannelDefers) {
    /*
     * FR-14's AUTO rule, END TO END rather than at the unit.
     *
     * This test is what found the real defect: the fixture's channel named a
     * CONCRETE gain mode, and a concrete mode overrides the policy, so
     * `settings.gain_policy` was never consulted from any config the codec would
     * accept -- spec 6.2's AUTO rule was unit-tested but unreachable. Fixing it
     * needed `GainMode::kAuto`, which the spec's own worked example already used
     * on a channel and the codec REJECTED.
     *
     * So a channel that defers (`kAuto`) plus the default device policy must
     * measure this channel's own head-unit idle and pick from it.
     *
     * The threshold is on the DOUBLED reading: `kGuardLowMv` is 2600, so tracking
     * needs a sense reading under 1300.
     */
    constexpr int kSenseBelowGuard = 1200;   // x2 = 2400 mV < 2600 -> a 3 V line
    // 2490 x2 = 4980 mV, above the guard -> a 5 V line (kSenseFor5vHeadUnit).

    {
        MockHal hal;
        MockHal::Defaults d;
        d.config.channels[0].output.gain_mode = GainMode::kAuto;   // defer to policy
        d.config.settings.gain_policy = GainPolicy::kAuto;
        ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(d.config));
        SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
        hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
        o.Boot();
        EXPECT_EQ(o.ChannelGainMode(0), GainMode::kAmplified)
            << "4980 mV is above the guard, so AUTO must amplify";
        EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K)
            << "gain 1.82 needs the 1k pulldown";
    }
    {
        MockHal hal;
        MockHal::Defaults d;
        d.config.channels[0].output.gain_mode = GainMode::kAuto;
        d.config.settings.gain_policy = GainPolicy::kAuto;
        ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(d.config));
        SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
        hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseBelowGuard);
        o.Boot();
        EXPECT_EQ(o.ChannelGainMode(0), GainMode::kTracking)
            << "2400 mV is below the guard, so AUTO must take gain 1.00";
        EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_NORMAL)
            << "gain 1.00 needs V_ADJ tracking, not the pulldown";
    }
    {
        // A CONCRETE channel mode still overrides the policy -- the other half of
        // the rule, and the reason the policy was unreachable before.
        MockHal hal;
        MockHal::Defaults d;
        d.config.channels[0].output.gain_mode = GainMode::kTracking;   // concrete
        d.config.settings.gain_policy = GainPolicy::kForceAmplified;
        ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(d.config));
        SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
        hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);   // would say 1.82
        o.Boot();
        EXPECT_EQ(o.ChannelGainMode(0), GainMode::kTracking)
            << "a channel that names a gain must not be overridden by the policy";
    }
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
    // vol_up binds SINGLE (a 2400 mV output) AND LONG (a release), but NOT
    // DOUBLE, so per spec 6.6 rule 3 the press resolves at RELEASE with no
    // double-press wait. The pulse is then `send_duration_ms` long, so the DAC
    // must be sampled WHILE it is on the line -- reading afterwards sees the
    // release, not the key.
    const int driven = PressAndCaptureDrivenCode(o, hal, 1430);
    EXPECT_NE(driven, idle_code) << "a resolved press must change the output";

    // Let the recognition pulse elapse; the line must return to idle.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
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
    EXPECT_NE(PressAndCaptureDrivenCode(o, hal, 1430), idle_code)
        << "a press must be served with no link present";
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

    // Drive a key and HOLD it open with a bench command whose hold outlasts the
    // test. A real press is no good here: vol_up binds SINGLE+LONG (no DOUBLE),
    // so its SINGLE is a 200 ms pulse that self-releases, and the assertion below
    // could not tell the fault-release from the normal timeout. Holding the line
    // open makes the fault the ONLY thing that can release it.
    ASSERT_TRUE(o.TestDriveKeyMv(0, 2400, 5000, hal.NowMs()));
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "the bench drive must hold a key";

    // The +3V3 rail sags: 500 mV is 18% of the learned 2835 mV idle, and the
    // KEY line collapses with it (2 x 250 = 500 mV, outside the 1.8-5.2 V
    // envelope), so the head unit is gone.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 500);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);
    PollFor(o, hal, 400);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a rail fault during a driven key must release it, not hold it";
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
    // Persisted like MakeOrch: FR-9 is about the two classifiers, and it must be
    // the classifiers that are compared rather than two pass-through mappings.
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
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

TEST(SystemOrchestrator, AnUnrecognizedPressIsReportedWithANullButton) {
    // FR-12, and the spec says it three times (§6.3, §7.2, and the FR table): a
    // press that matches no learned window is reported as `event{button: null}`
    // and beeps `KEY_UNKNOWN`. It is NEVER guessed at.
    //
    // **An earlier version of this test asserted the opposite** -- that nothing at
    // all is reported -- because the §4.3 note it was written from said so. That
    // note was wrong: it contradicted FR-12 and two other spec sections, and the
    // firmware matched the note rather than the requirement. A test written from a
    // bad reading of the spec enshrines the bug, which is exactly what happened
    // here.
    //
    // Reporting matters because it is the ONLY way a user can see, from the app,
    // that the device is receiving a press it does not recognise. Staying silent
    // leaves them with a live ladder that never moves while the device is in fact
    // seeing every press.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    // 2400 mV is inside the ladder's RANGE but inside no learned window, which is
    // the only thing FR-12 is about. At the fixture's 2835 mV learned idle the
    // ratio is 846 permille: above `next`'s window (718-794) and below the idle
    // band (which starts at 970), so it classifies as kUnknown.
    //
    // The value matters and an earlier version of this test got it wrong: 3000 mV
    // is ratio 1058, which is ABOVE the reference and therefore a kFault, not an
    // unknown. It took the fault path, reported nothing, and the test failed for a
    // reason that had nothing to do with FR-12. An out-of-range reading is a
    // different requirement (spec 6.8's "ladder out of range" row).
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    ASSERT_EQ(g_reported.size(), 1u)
        << "an unrecognized press must be reported once, not silently dropped";
    EXPECT_EQ(g_reported[0].button_id, nullptr)
        << "there is no button to name, and naming one would be the guess FR-12 forbids";
    EXPECT_EQ(g_reported[0].gesture, Gesture::kNone);
    EXPECT_GT(g_reported[0].level_mv, 0) << "the reading is still reported, so the app can see it";
}

TEST(SystemOrchestrator, AnUnrecognizedPressDoesNotDriveTheOutput) {
    // FR-12's other half: never guessed at. The report is diagnostic, not a key --
    // driving an output for an unrecognised level would reach the radio as a
    // command nobody asked for.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);   // ratio 846: in range, in no window
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "an unrecognized level must never drive the KEY line";
}

// --- the unbound-gesture pass-through: the no-app default --------------------
//
// The 2022 design's default was that a press PRESENTS the button: `lookup_single/
// double/long_press_val` always mapped the key's own value to an output region,
// and only the (then-stubbed) `program_alt_key` overrode it. The new firmware
// instead did NOTHING for a recognised-but-unbound gesture -- it released the line
// and played KEY_UNKNOWN. That is the silent no-app failure the user reported: a
// fresh device learned by AUX1 alone has windows but no bindings (`ConfigDefault`
// ships `binding_count = 0`, and every runtime binding otherwise comes from a
// config the app pushes), so the learned ladder could never drive a key even
// though the learn itself beeped LEARN_OK.
//
// The fixture's `vol_dn` (1785 mV) has a learned window but NO binding, which is
// exactly the no-app shape: recognised, and unbound.

TEST(SystemOrchestrator, AnUnboundGesturePresentsTheButtonRatherThanDoingNothing) {
    // The core no-app requirement: with no binding for a recognised button's
    // gesture, the device acts as a STOCK WHEEL and presents that button's own
    // level. "It would be passed through."
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // vol_dn is learned (1785 +/- 120) and UNBOUND, so it has neither DOUBLE nor
    // LONG of its own: per spec 6.6 rule 3 it resolves at release and presents
    // immediately. Sampled mid-pulse, because the present is 200 ms long.
    const int driven = PressAndCaptureDrivenCode(o, hal, 1785);
    EXPECT_NE(driven, idle_code)
        << "an unbound gesture on a RECOGNISED button must present the button, "
           "not silently do nothing -- this is the no-app product requirement";
}

TEST(SystemOrchestrator, TheUnboundGestureIsPresentedAsABoundedPulseThenReleased) {
    // Spec 6.6 rule 2: one key event, not a held line. The head unit is
    // gesture-blind, so a line held down is a different (and wrong) thing to it.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    ASSERT_NE(PressAndCaptureDrivenCode(o, hal, 1785), idle_code);
    // The line must have returned to idle well inside the poll window above: the
    // present is `send_duration_ms` (200 ms), not a line held for the press.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "the present must be a bounded pulse, not a held key";
}

TEST(SystemOrchestrator, AnUnboundButtonResolvesAtThePressWithNoSiblingLatency) {
    // Spec 6.6 rule 3: the double-press wait is a PER-BUTTON property. `vol_dn`
    // binds NOTHING, so it has no ambiguity at all and must resolve the instant
    // it is classified -- NOT wait out the 500 ms window that `next`'s DOUBLE
    // (a sibling button on the same channel) would impose.
    //
    // The earlier resolve read the CHANNEL's bindings, so any DOUBLE anywhere on
    // the channel added ~500 ms to EVERY button's SINGLE. That is the latency
    // this asserts against: the gesture must be reported within ~100 ms of the
    // press (the debounce), not ~600 ms.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    const uint64_t pressed_at = hal.NowMs();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1785);   // vol_dn: unbound
    PollFor(o, hal, 300);

    ASSERT_EQ(g_reported.size(), 1u) << "an unbound button must resolve promptly";
    EXPECT_EQ(std::string(g_reported[0].button_id), "vol_dn");
    EXPECT_EQ(g_reported[0].gesture, Gesture::kSingle);
    const uint64_t latency = g_reported[0].at_ms - pressed_at;
    EXPECT_LT(latency, 200u)
        << "an unbound button must not inherit a sibling's double-press window";
}

TEST(SystemOrchestrator, ARecognizedAndUnboundPressIsAcceptedNotBeepedUnknown) {
    // KEY_UNKNOWN means "a press matched no learned window" (FR-12). This press
    // DID match a window, so reporting it as unknown would tell the app a
    // different story than what happened -- and would play the wrong tone at the
    // driver.
    //
    // The two tones are BOTH single pulses, so counting buzzer drives cannot tell
    // them apart (an earlier version of this test did exactly that and passed
    // under a mutation that played the wrong one). They differ by DURATION:
    // KEY_ACCEPTED is 25 ms on, KEY_UNKNOWN is 120 ms on (spec 7.2 / the grammar
    // table). Measuring the on-time is what actually discriminates them.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    PollFor(o, hal, 400);          // let the boot announcement finish
    ASSERT_FALSE(hal.BuzzerIsOn());

    // vol_dn is unbound, so per spec 6.6 rule 3 it binds neither DOUBLE nor LONG
    // and has NO ambiguity to resolve: the press resolves as a SINGLE AT THE
    // PRESS (the fast path), so the acknowledging tone plays immediately and the
    // count must span the press as well as the release.
    int on_ticks = 0;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1785);
    for (uint32_t t = 0; t < 700; t += 5) {
        o.Tick(hal.NowMs());
        if (hal.BuzzerIsOn()) ++on_ticks;
        hal.AdvanceMs(5);
        if (t == 95) hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // release ~100 ms in
    }
    // 5 ms cadence: the 25 ms accepted pulse is ~5 ticks, the 120 ms unknown pulse
    // ~24. A threshold of 12 ticks (60 ms) sits between them with margin.
    EXPECT_LE(on_ticks, 12)
        << "the accepted tone (25 ms) must play, not the unknown tone (120 ms)";
    EXPECT_GT(on_ticks, 0) << "a recognised press must be acknowledged at all";
}

TEST(SystemOrchestrator, AnUnrecognizedLevelStillDrivesNothingEvenWithTheFallback) {
    // The boundary of the fallback: it applies to a button the ladder RECOGNISED.
    // A level in no window is still FR-12's unknown -- reported with a null id and
    // never guessed at -- so the fallback must not widen into "any off-idle press
    // is presented", which would reintroduce the guess FR-12 forbids.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);   // in range, in NO window
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "an UNRECOGNISED level must still drive nothing";
    ASSERT_EQ(g_reported.size(), 1u);
    EXPECT_EQ(g_reported[0].button_id, nullptr)
        << "an unrecognised level must still report a null button";
}

TEST(SystemOrchestrator, AnUnrecognizedPressIsNotReportedRepeatedlyWhileHeld) {
    // One report per press. A held unrecognised level would otherwise emit an event
    // on every poll tick -- 100/s -- which is both a flooded link and a live view
    // that cannot be read.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);   // ratio 846: in range, in no window
    PollFor(o, hal, 500);          // held for 50 ticks
    EXPECT_EQ(g_reported.size(), 1u) << "a held unrecognized level reports once, not per tick";
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
    // PERSISTED, so the device is CONFIGURED and the wizard is reached normally.
    // With an empty NVS this test took the kNoConfig path instead: pass-through
    // failed for want of a reference and the learn was then entered with
    // learn_idle_mv_ still 0 -- a different condition that happened to produce the
    // same "did not commit" answer, so the test could not tell the two apart.
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
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

// --- FR-33: maintenance from AUX1, the no-app fallback ------------------------
//
// Spec 8.2 nests the two holdings: 1.5 s is PROGRAMMING, 3 s is MAINTENANCE, the
// shorter a subset of the longer "so holding too long to program escalates
// cleanly into maintenance rather than into an undefined state". That nesting is
// the whole reason this can be tested at all: the escalation has to survive the
// programming hold having ALREADY fired at 1.5 s.

namespace {

// Hold AUX1 for `ms` from a clean release.
void HoldAuxFor(SystemOrchestrator &o, MockHal &hal, uint32_t ms) {
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, ms);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    PollFor(o, hal, 60);
}

}  // namespace

TEST(SystemOrchestrator, ASustainedAUX1HoldEntersMaintenanceWithNoApp) {
    // FR-33's no-app fallback. The user may not have the head unit out of the
    // dash, so reaching the setup page cannot require a phone.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_FALSE(o.MaintenanceActive());

    HoldAuxFor(o, hal, SystemOrchestrator::kMaintenanceHoldMs + 150);
    EXPECT_TRUE(o.MaintenanceActive()) << "a 3 s AUX1 hold must open the maintenance window";
    EXPECT_EQ(o.MaintenanceTriggeredBy(), MaintenanceTrigger::kAux1Hold);
}

TEST(SystemOrchestrator, AProgrammingHoldEscalatesIntoMaintenanceRatherThanUndefinedState) {
    // Spec 8.2's nesting, which is the part that is easy to get wrong: by 3 s the
    // 1.5 s programming hold has ALREADY toggled the wizard, so the maintenance
    // entry has to fire on its own latch and leave the wizard. Without that, a
    // long hold enters programming and then exits it again, and the user who
    // wanted maintenance gets neither.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxFor(o, hal, SystemOrchestrator::kMaintenanceHoldMs + 150);
    EXPECT_TRUE(o.MaintenanceActive());
    EXPECT_FALSE(o.LearnActive())
        << "escalating to maintenance must not leave a learn running behind it";
}

TEST(SystemOrchestrator, TheProgrammingHoldAloneDoesNotOpenMaintenance) {
    // The two gestures must stay distinguishable: a user programming a button
    // must not find themselves in the setup page.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxFor(o, hal, LearnWizard::kEnterHoldMs + 100);   // 1.5 s only
    EXPECT_FALSE(o.MaintenanceActive());
    EXPECT_TRUE(o.LearnActive()) << "1.5 s is the programming hold";
}

TEST(SystemOrchestrator, MaintenanceTimesOutAndDoesNotStrandTheDevice) {
    // FR-38: a device stuck unable to serve input because someone opened a web
    // page is unacceptable. The window is bounded at 5 minutes of inactivity.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    ASSERT_TRUE(o.MaintenanceActive());

    // Poll for just under the window, then past it.
    PollFor(o, hal, 299000);
    EXPECT_TRUE(o.MaintenanceActive()) << "the window must not close early";
    PollFor(o, hal, 2000);
    EXPECT_FALSE(o.MaintenanceActive()) << "5 minutes of inactivity must close the window";
}

TEST(SystemOrchestrator, PressesStillWorkWhileMaintenanceIsOpen) {
    // FR-38's stated reason for a bounded window: "a device unable to serve input
    // is unacceptable". That only means something if maintenance is NOT exclusive,
    // so this asserts the steering wheel keeps working while the radio is up.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    ASSERT_TRUE(o.MaintenanceActive());

    EXPECT_NE(PressAndCaptureDrivenCode(o, hal, 1430), idle_code)
        << "the steering wheel must keep working while maintenance is open";
}

TEST(SystemOrchestrator, NoteMaintenanceActivityKeepsAWorkingUserInTheWindow) {
    // A user typing a PoP or working in the web UI must not be kicked out
    // mid-task, which is what the activity clock is for.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());

    for (int i = 0; i < 5; ++i) {
        PollFor(o, hal, 200000);                       // 200 s, under the window
        o.NoteMaintenanceActivity(hal.NowMs());
    }
    EXPECT_TRUE(o.MaintenanceActive())
        << "repeated activity must keep the window open past the original 5 minutes";
}

TEST(SystemOrchestrator, AFreshDeviceTaughtHeadlesslyStopsPassingThrough) {
    // The requirement in the user's words: the AUX1 programming must work with no
    // Android device attached. A FRESH device is the case that matters -- there is
    // no app to write a config, so the whole config comes from the headless learn.
    //
    // **A fresh device boots into FR-25 pass-through, and pass-through RETURNS
    // EARLY from ServiceChannel.** So if a learn does not clear it, the learned
    // windows are never consulted: the user holds AUX1, gets LEARN_OK, and the
    // button they just taught is ignored -- with perfectly correct feedback.
    MockHal hal;
    MockHal::Defaults d;
    d.config.channel_count = 1;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 0;
    d.config.binding_count = 0;
    // Deliberately NOT stored: this is a factory-fresh device.
    hal.ClearNvs();
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "a device with no config passes through (FR-25)";

    HoldAuxToToggle(o, hal);
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 200);

    ASSERT_NE(o.LastLearnedProfile(0), nullptr) << "the learn must have committed";
    EXPECT_FALSE(o.PassThroughActive())
        << "after a learn the device HAS learned windows, so it must classify "
           "against them rather than passing through and ignoring them";
}

// --- FR-4: an out-of-range channel must be REPORTED, not merely survived -----
//
// The firmware detected the fault, released the KEY line (correct, and already
// tested) and then said nothing anywhere. Spec 7.3's entire fault indication --
// `LED_STAT` blink, "5 Hz: fault -- the buzzer's FAULT_* says which" -- was
// unreachable, because the only `SetStat` callers were the learn wizard and the
// identify flash. FR-4 says "detect and report"; only the detection existed.

namespace {
// Count rising edges on LED_STAT, which is what separates the three patterns:
// solid is one edge, 5 Hz blink is many, off is none.
int CountStatEdges(MockHal &hal, SystemOrchestrator &o, uint32_t total_ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        o.Tick(hal.NowMs());
        const bool now = hal.GpioRead(GPIO_LED_STAT);
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(SystemOrchestrator, AnOutOfRangeChannelBlinksTheFaultLamp) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    ASSERT_FALSE(o.Faulted()) << "a healthy boot is not a fault";

    // A ratio of 1058 is ABOVE the idle reference, which the classifier reports
    // as kFault -- a short to 12 V reads this way. 100 ms settles the debounce.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 100);

    EXPECT_TRUE(o.Faulted()) << "FR-4 requires the out-of-range channel to be reported";
    // Many edges over one second is the 5 Hz blink; the healthy LED is solid
    // (one edge) or breathing (a slow pair), so the count distinguishes them.
    EXPECT_GT(CountStatEdges(hal, o, 1000), 4)
        << "the fault indication must be a blink, not the normal solid/breathe";
}

TEST(SystemOrchestrator, AFaultDoesNotClearItselfWhenTheLevelReturnsToIdle) {
    // A wiring fault or a collapsed rail does not fix itself, so an indication
    // that faded as soon as the reading looked reasonable again would be a lie:
    // the user would see a healthy LED over a channel that is still broken.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 100);
    ASSERT_TRUE(o.Faulted());

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 2000);
    EXPECT_TRUE(o.Faulted()) << "the fault must latch until a reboot";
}

TEST(SystemOrchestrator, TheLedIsBreathingBeforeAnyHostAttaches) {
    // Spec 7.3: LED_STAT answers "is this thing OK?" at a glance, and its normal
    // states are solid WITH a host and breathing without. Both were unreachable:
    // LedGrammar starts at kOff and nothing set a normal pattern, so a correctly
    // running device showed a dark LED -- indistinguishable from no power.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    EXPECT_GT(CountStatEdges(hal, o, 2000), 0)
        << "a running device must not look switched off";
}

TEST(SystemOrchestrator, ConnectingAHostMakesTheLedSolidAndDisconnectingResumesBreathing) {
    // The two normal states must be distinguishable, or the LED says nothing
    // about the one thing it is meant to report.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    o.SetUsbConnected(true);
    // Solid: exactly one rising edge however long it is watched.
    EXPECT_EQ(CountStatEdges(hal, o, 2000), 1) << "solid is one edge, then held";

    o.SetUsbConnected(false);
    // Breathing: a repeating slow pattern, so more than one edge over the same
    // window. 1 Hz (spec 7.3) gives ~2 cycles in 2 s.
    EXPECT_GT(CountStatEdges(hal, o, 2000), 1) << "breathing must differ from solid";
}

TEST(SystemOrchestrator, AConnectedHostDoesNotRepaintOverAFault) {
    // "Is this thing OK?" has one right answer when the device is faulted, and it
    // is not green. Without the precedence, a link connect mid-fault silently
    // cleared the indication.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 100);
    ASSERT_TRUE(o.Faulted());

    o.SetUsbConnected(true);
    EXPECT_GT(CountStatEdges(hal, o, 1000), 4)
        << "a faulted device must not show the healthy solid pattern just because a host attached";
}

// --- FR-33/8.2: a maintenance window is visible on LED_STAT ------------------
//
// The constants (`kAuxPressedMv`/`kAuxReleasedMv`) and `HoldAuxToToggle` helpers
// are defined in the learn-wiring block above. A 3 s hold necessarily passes
// through the 1.5 s programming tier, so this reuses the same helper.

namespace {
// Rising edges on LED_STAT: the existing `CountStatEdges` above. A double-flash
// is a BURST (two rises then a long gap), so it and the 5 Hz fault blink are both
// "many edges" -- which is why counting alone does not identify either and the
// pairing assertion in the first test below is what pins the pattern.
}  // namespace

TEST(SystemOrchestrator, AMaintenanceWindowDoubleFlashesLedStat) {
    // Spec 8.2: "On exit ... `LED_STAT` stops the maintenance double-flash."
    // That sentence presumes something STARTED one, and nothing did: the
    // `kDoubleFlash` pattern existed and was tested in isolation, but no
    // maintenance code path ever selected it, so a user who opened the window by
    // any of its four triggers had no indication they were in it -- and the one
    // state that explains "why is my wheel not responding" looked identical to a
    // healthy device with no host.
    //
    // Entered by the 3 s AUX1 hold, which is the no-app trigger and therefore
    // the one that must be visible on the device alone.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    o.Boot();
    ASSERT_TRUE(o.SafeIdleEstablished());

    // 3.2 s, past spec 8.2's 3 s maintenance tier.
    HoldAuxToToggle(o, hal, kAuxPressedMv);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 3200);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    ASSERT_TRUE(o.MaintenanceActive())
        << "the 3 s hold is spec 8.2's maintenance trigger";

    // The window is open, so the indication is wired. Re-enter at a KNOWN tick
    // before measuring the cadence: `SetStat` restarts the pattern's phase, so the
    // burst's alignment otherwise depends on when the hold happened to cross 3 s,
    // and a test that counts rises from an arbitrary phase is a test that passes
    // or fails on its own setup. (It did: the first version of this test asserted
    // four rises and got five.)
    o.ExitMaintenance();
    o.Tick(hal.NowMs());
    hal.AdvanceMs(10);
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    o.Tick(hal.NowMs());
    hal.AdvanceMs(10);

    // Sample the pairs. The pattern is 100 on / 100 off / 100 on / 600 off, so
    // from an aligned start over 1.8 s (two periods) there are exactly four rises
    // and the gaps between them are 200, 700, 200.
    std::vector<int> rises_ms;
    bool prev = false;
    const uint64_t start = hal.NowMs();
    while (hal.NowMs() - start < 1800) {
        o.Tick(hal.NowMs());
        const bool now = hal.GpioRead(GPIO_LED_STAT);
        if (now && !prev) rises_ms.push_back(static_cast<int>(hal.NowMs() - start));
        prev = now;
        hal.AdvanceMs(5);
    }

    // The signature is the GAP CADENCE, not the rise count: the pattern's period
    // is 900 ms, so 1.8 s legitimately holds five rises and asserting an exact
    // count would just be re-deriving the sample boundary. What no other pattern
    // produces is alternating short and long gaps -- a pair of quick flashes, then
    // a pause. 5 Hz blink would give ~10 evenly spaced gaps of ~100 ms, and
    // breathing one gap of ~1000 ms.
    ASSERT_GE(rises_ms.size(), 4u) << "at least a pair, twice";
    for (size_t i = 0; i + 1 < rises_ms.size(); ++i) {
        const int gap = rises_ms[i + 1] - rises_ms[i];
        if (i % 2 == 0) {
            EXPECT_LT(gap, 400) << "gap " << i << " is within a pair, so it is short";
        } else {
            EXPECT_GT(gap, 500) << "gap " << i << " separates the pairs, so it is long";
        }
    }
}

TEST(SystemOrchestrator, LeavingMaintenanceStopsTheDoubleFlash) {
    // The other half of spec 8.2's sentence. Without this the lamp would keep
    // flashing after the window closed, which reads as a fault and would send a
    // user looking for a hardware problem that does not exist.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    o.Boot();
    o.SetUsbConnected(true);

    HoldAuxToToggle(o, hal, kAuxPressedMv);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 3200);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    ASSERT_TRUE(o.MaintenanceActive());

    // Establish what is being stopped: without this the test would pass on a
    // device that never flashed at all, which is the defect it exists to catch.
    EXPECT_GT(CountStatEdges(hal, o, 1000), 1)
        << "the window is open, so it must be flashing before we close it";

    o.ExitMaintenance();
    o.Tick(hal.NowMs());
    hal.AdvanceMs(10);
    // A connected host resumes SOLID: exactly one rise however long it is watched.
    EXPECT_EQ(CountStatEdges(hal, o, 2000), 1)
        << "the window is closed, so the healthy solid pattern must resume";
}

TEST(SystemOrchestrator, TheMaintenanceTimeoutClosesTheWindowAndItsIndication) {
    // FR-38's 5-minute timeout is the one exit with no caller to notify, so the
    // LED must be restated from the tick rather than from the entry/exit sites.
    // A device whose radio window expired while still double-flashing would tell
    // the user to look at a state it is no longer in.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    o.Boot();
    o.SetUsbConnected(true);

    HoldAuxToToggle(o, hal, kAuxPressedMv);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 3200);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    ASSERT_TRUE(o.MaintenanceActive());

    // Past the 5-minute window, in one jump: `ShouldTimeout` is a comparison on
    // elapsed time, so the intermediate ticks carry no information.
    hal.AdvanceMs(300001);
    o.Tick(hal.NowMs());
    hal.AdvanceMs(10);
    EXPECT_FALSE(o.MaintenanceActive()) << "the window must close on its own";
    EXPECT_EQ(CountStatEdges(hal, o, 2000), 1)
        << "and the indication must go with it";
}

TEST(SystemOrchestrator, AFaultOutranksAMaintenanceWindowOnLedStat) {
    // Precedence has to be stated rather than inherited: "is this thing OK?" has
    // one right answer, and a device that is both faulted and in maintenance is
    // not. Without the ordering in RestatLeds, whichever call landed last would
    // win, and the fault could be hidden by a radio window.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    o.Boot();
    ASSERT_TRUE(o.SafeIdleEstablished());

    HoldAuxToToggle(o, hal, kAuxPressedMv);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 3200);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    ASSERT_TRUE(o.MaintenanceActive());

    // Now collapse the ladder while the window is open.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 200);
    ASSERT_TRUE(o.Faulted());

    // Fault blink is 5 Hz: far more rises than the double-flash's two-per-second.
    EXPECT_GT(CountStatEdges(hal, o, 1000), 4)
        << "the fault must win over the maintenance double-flash";
}

TEST(SystemOrchestrator, AFaultArrivingDuringAMaintenanceWindowStillBlinks) {
    // The precedence has to hold in the direction nobody wires: a fault is
    // detected inside ServiceChannel, and the only other repaint is
    // SetUsbConnected, which a maintenance window never calls. So a ladder that
    // collapses while the window is open -- a real possibility, since maintenance
    // still serves presses (FR-38) -- would leave the lamp double-flashing and its
    // continuous fault indication unseen. (Measured: with the guard removed and
    // ReportFault left as a bare SetStat, the suite was green.)
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_TRUE(o.SafeIdleEstablished());

    HoldAuxToToggle(o, hal, kAuxPressedMv);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, 3200);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    ASSERT_TRUE(o.MaintenanceActive());

    // Collapse the ladder while the window is open. No SetUsbConnected, no exit:
    // the tick's restate is the only thing that can repaint here.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 200);
    ASSERT_TRUE(o.Faulted());
    ASSERT_TRUE(o.MaintenanceActive()) << "the window is still open";

    // Over 2 s the 5 Hz fault blink (100/100) gives ~10 rises and the
    // double-flash (a 900 ms period holding 2 rises) gives ~5 -- so >6 separates
    // them without depending on a single pattern boundary.
    EXPECT_GT(CountStatEdges(hal, o, 2000), 6)
        << "the fault must repaint even though the window is open";
}

TEST(SystemOrchestrator, MaintenanceDoesNotPaintOverALearnThatOwnsTheLeds) {
    // The wizard is the LED owner from Enter until ConsumeExited hands back, and a
    // maintenance window opened underneath it must not steal LED_STAT. The two
    // triggers collide on purpose here: a 3 s AUX1 hold on top of a learn leaves
    // the wizard (`ServiceLearn` exits it first), but the USB path does not, so an
    // app cannot know to avoid it. Painting over the wizard would leave a
    // half-programmed user staring at a double-flash with no prompt.
    //
    // The discriminator is the PAIR, not the rate: while LED_STAT is `kAlternate`
    // it owns LED2 and the two are complements, so exactly one is lit at every
    // instant. A double-flash on LED_STAT with LED2 left off (which is what
    // `Enter` sets, and `kSelectButton` never touches) breaks that invariant --
    // and unlike a rise count it does not depend on the two periods being far
    // apart, which at 500 ms and 900 ms they are not.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // 1.5 s: the programming hold. The wizard is in kSelectButton, owning LED_STAT
    // as `kAlternate`.
    HoldAuxFor(o, hal, LearnWizard::kEnterHoldMs + 100);
    ASSERT_TRUE(o.LearnActive());

    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    ASSERT_TRUE(o.MaintenanceActive());

    int both_same = 0;
    for (uint32_t t = 0; t < 2000; t += 5) {
        o.Tick(hal.NowMs());
        if (hal.GpioRead(GPIO_LED_STAT) == hal.GpioRead(GPIO_LED2)) ++both_same;
        hal.AdvanceMs(5);
    }
    EXPECT_EQ(both_same, 0)
        << "the wizard still owns the LEDs, so they must stay complementary";
}

// --- FR-18: an out-of-envelope key is clamped AND warned about ---------------

namespace {
struct LogCapture {
    std::vector<std::string> lines;
    static void Sink(void *ctx, const char *level, const char *msg) {
        static_cast<LogCapture *>(ctx)->lines.push_back(std::string(level) + ": " + msg);
    }
};
}  // namespace

TEST(SystemOrchestrator, AnOutOfEnvelopeKeyIsClampedAndTheClampIsWarnedAbout) {
    // FR-18: "validate any DAC code against the current gain mode's ceiling before
    // writing it, and clamp with a logged warning". The clamp existed; the warning
    // did not, because nothing in the firmware emitted a `log` frame at all. The
    // frame type was in the contract with no producer.
    //
    // A binding with an absurd key_mv is the trigger. ConfigValidate only rejects
    // key_mv == 0, so an out-of-envelope value reaches the drive path and must be
    // handled there.
    MockHal hal;
    MockHal::Defaults d;
    d.config.bindings[0].button[0] = '\0';   // no binding matches, so bind directly
    // vol_up SINGLE -> a key far above the 5200 mV ceiling.
    std::strncpy(d.config.bindings[0].button, "vol_up", sizeof(d.config.bindings[0].button) - 1);
    d.config.bindings[0].gesture = Gesture::kSingle;
    d.config.bindings[0].action_count = 1;
    d.config.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    d.config.bindings[0].actions[0].key_mv = 9000;   // above kOutputCeilingMv
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    LogCapture logs;
    o.SetLogSink(&LogCapture::Sink, &logs);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    ASSERT_EQ(logs.lines.size(), 1u) << "the clamp must be warned about, not silent";
    EXPECT_NE(logs.lines[0].find("WARN"), std::string::npos);
    EXPECT_NE(logs.lines[0].find("9000"), std::string::npos)
        << "the warning must name the value that was clamped";
}

TEST(SystemOrchestrator, AnInEnvelopeKeyProducesNoWarning) {
    // The inverse: a warning on every press would be noise, and noise is how a
    // real warning gets ignored. The fixture's own binding is inside the envelope.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    LogCapture logs;
    o.SetLogSink(&LogCapture::Sink, &logs);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);
    EXPECT_TRUE(logs.lines.empty()) << "an in-envelope key needs no warning";
}

// --- LED2, the activity channel (spec 7.3) ----------------------------------

namespace {
// LED2 while a key is driven: one rising edge then held, versus none at all.
int CountLed2Edges(MockHal &hal, SystemOrchestrator &o, uint32_t total_ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        o.Tick(hal.NowMs());
        const bool now = hal.GpioRead(GPIO_LED2);
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(SystemOrchestrator, LED2ShowsTheLineIsDrivenThenReturnsOff) {
    // Spec 7.3: "the user can see that the adapter is holding a key, which
    // distinguishes 'the adapter is doing something wrong' from 'the head unit is
    // ignoring it'." That diagnostic was unreachable -- the only Set2 callers
    // were the learn wizard, so LED2 sat dark through every press.
    //
    // The drive lands LATE: a SINGLE resolves on release, after the 500 ms double
    // window, and is then held for send_duration_ms (200). So the release poll is
    // what must be watched, not the press.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    // Idle first: the derivation must not light LED2 just because the device runs.
    EXPECT_EQ(CountLed2Edges(hal, o, 200), 0) << "an idle device must not show activity";

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);                       // press
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);    // release; SINGLE resolves inside
    EXPECT_GT(CountLed2Edges(hal, o, 700), 0)
        << "the driven KEY line must be visible on LED2";
}

TEST(SystemOrchestrator, LED2DoesNotLatchOnAfterThePulseEnds) {
    // The other half: a released key must not leave the activity LED lit, or the
    // diagnostic inverts -- the user would see "holding a key" on a device that is
    // not, and would go looking for a fault that is not there.
    //
    // The settle is LONGER than the pulse plus the double window on purpose. An
    // earlier version of this test started measuring 700 ms after release, which
    // is exactly when the pulse ends, so it counted the tail of the drive as a
    // rising edge and failed against correct code.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 1200);      // past the double window AND the drive pulse

    EXPECT_EQ(CountLed2Edges(hal, o, 500), 0)
        << "a released key must leave LED2 off, not latched on";
    EXPECT_FALSE(hal.GpioRead(GPIO_LED2)) << "and the line must be off, not merely steady";
}

TEST(SystemOrchestrator, AnUnrecognizedPressShowsNoActivity) {
    // FR-12 again, from the LED's side: an unrecognised press drives nothing, so
    // LED2 must stay dark. If it lit, the LED would report activity for a press
    // that never reached the radio -- the opposite of its purpose, and it would
    // send the user hunting for a radio problem that is really a learned-window
    // problem.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);   // in range, in no window
    PollFor(o, hal, 200);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    EXPECT_EQ(CountLed2Edges(hal, o, 700), 0)
        << "nothing was driven, so there is no activity to show";
}

TEST(SystemOrchestrator, ALearnLED2IsTheComplementOfLedStatNotActivity) {
    // What the user watching a learn actually sees, pinned so it cannot drift.
    //
    // Spec 7.3 makes "alternating with LED2" a property of the PAIR: while
    // LED_STAT alternates, LED2 is its complement and `Set2` is ignored outright.
    // That is why a learn looks like an alternating pair rather than a state LED
    // plus an activity LED, and it is worth a test because it is the one place
    // where the activity derivation is legitimately overridden.
    //
    // (I could not write a test that fails when the wizard guard in
    // `UpdateLed2ForDrivingState` is removed: during the prompt phase the
    // alternate override already swallows any `Set2`, so the guard is
    // belt-and-braces there. This test pins the behaviour that actually decides
    // what the LEDs show.)
    MockHal hal;
    MockHal::Defaults d;
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

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());

    // Sample both LEDs together. Three assertions, and the THIRD is the one that
    // matters: "never both on, never both off" is satisfied by a plain solid
    // LED_STAT too, so on its own it does not prove alternation. Requiring LED2 to
    // be ON for part of the window is what separates `kAlternate` (LED2 is the
    // complement, so it lights) from any non-alternating pattern (LED2 stays off).
    int both_on = 0, neither = 0, stat_on = 0, led2_on = 0, samples = 0;
    for (uint32_t t = 0; t < 1200; t += 5) {
        o.Tick(hal.NowMs());
        const bool stat = hal.GpioRead(GPIO_LED_STAT);
        const bool led2 = hal.GpioRead(GPIO_LED2);
        if (stat && led2) ++both_on;
        if (!stat && !led2) ++neither;
        if (stat) ++stat_on;
        if (led2) ++led2_on;
        ++samples;
        hal.AdvanceMs(5);
    }
    EXPECT_GT(samples, 0);
    EXPECT_EQ(both_on, 0) << "the pair must alternate, so both are never lit at once";
    EXPECT_EQ(neither, 0) << "and one of the two must always be lit during a learn";
    EXPECT_GT(stat_on, 0) << "LED_STAT must light for part of the cycle";
    EXPECT_GT(led2_on, 0)
        << "LED2 must light too -- that is what makes this an ALTERNATION rather "
           "than a solid state LED with a dark activity LED";
}

TEST(SystemOrchestrator, PROBE_LEARN_LED2) {
    MockHal hal;
    MockHal::Defaults d;
    d.config.channel_count = 1;
    d.config.binding_count = 0;
    hal.ClearNvs();
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    printf("PROBE pass_through=%d\n", (int)o.PassThroughActive());
    HoldAuxToToggle(o, hal);
    printf("PROBE learn=%d\n", (int)o.LearnActive());
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 30; ++i) {
        o.Tick(hal.NowMs()); hal.AdvanceMs(5);
        printf("  t=%3d LED2=%d writes=%d\n", i*5,
               (int)hal.GpioRead(GPIO_LED2), hal.GpioWriteCount(GPIO_LED2));
    }
}

/*
 * Spec 6.6 rule 2: "Every drive is a bounded pulse, held for `send_duration_ms`
 * and then released ... never a held line."
 *
 * The pass-through path set `key_released_at_ms` and then RETURNED above the
 * release check, so the line stayed driven for as long as the button was held --
 * and the release check it reached on the configured path was the only thing that
 * ever released a pass-through pulse. A held button therefore produced one long
 * key event, not one event.
 */
TEST(SystemOrchestrator, APassThroughPulseSelfReleasesWhileTheButtonIsStillHeld) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "precondition: the press must drive the line";

    // Hold well past send_duration_ms (200 ms) and sample every tick, so a
    // re-arm (a second press pulse) is visible as a second transition.
    int transitions = 0;
    int last = hal.LastDacCode(DAC_CH_KEY1);
    for (uint32_t t = 0; t < 1000; t += 10) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(10);
        const int now = hal.LastDacCode(DAC_CH_KEY1);
        if (now != last) {
            ++transitions;
            last = now;
        }
    }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a held pass-through press must self-release after send_duration_ms";
    EXPECT_EQ(transitions, 1)
        << "exactly one transition (pressed -> idle); a still-held button must not "
           "re-arm the pulse every send_duration_ms";
}

// FR-39 / spec 6.8: a lost head unit releases the line in the same tick. The
// pass-through path used to return above that check as well, so a head unit that
// went away mid-press left the KEY line driven against nothing.
TEST(SystemOrchestrator, APassThroughPressReleasesWhenTheHeadUnitGoesAway) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 20);   // driving, still inside send_duration_ms
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "precondition: the press must drive the line";

    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 0);   // head unit unplugged
    PollFor(o, hal, 400);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a collapsed KEY sense means the head unit is gone; holding the line "
           "is the phantom-key hazard FR-39 forbids";
}

/*
 * The pass-through reference is captured once at Boot. If the user is holding a
 * button at power-on, the captured "idle" is a PRESSED level, and every later
 * press then looks too close to idle to register -- pass-through would be dead
 * for the whole session with no recovery but a reboot, which is exactly the
 * "steering wheel does nothing" outcome FR-25 exists to prevent.
 *
 * Seeing the line ABOVE the captured reference proves the capture was a press (no
 * button pulls the ladder UP from true idle), so the reference self-heals.
 */
TEST(SystemOrchestrator, AHeldButtonAtBootDoesNotKillPassThroughForTheSession) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // button held at power-on
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // released -> the true idle
    PollFor(o, hal, 200);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // press the same button again
    PollFor(o, hal, 100);

    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "pass-through must recover after a boot-time press poisoned its reference";
}
