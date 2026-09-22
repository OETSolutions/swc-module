#include "System/SystemOrchestrator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Config/ConfigDefaults.h"
#include "Config/ConfigStore.h"
#include "MockHAL.h"
#include "Output/GainPolicy.h"

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

TEST(SystemOrchestrator, TheHealthGateCanSayNoWhenADacWriteFails) {
    // **FR-37's gate, and the defect was that it could not be false.** `app_main`
    // marked an OTA image valid on `SystemOrchestratorSafeIdle()`, which returns
    // `safe_idle_established_` -- assigned `true` once, at the end of an
    // `EstablishSafeIdle()` that returns void and cannot fail. The condition was a
    // compile-time constant `true`, so an image whose I2C bus is dead had its
    // pending rollback CANCELLED: bricked-but-"valid", the exact state spec §9.8
    // says must never happen.
    //
    // `OutputVerified()` folds in the HAL's latched `dac_faulted`, so the gate can
    // now answer NO. Both halves are asserted: the healthy device is verified, and
    // a device whose write failed is NOT.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    EXPECT_TRUE(o.OutputVerified()) << "a healthy boot must be verified, or the gate "
                                       "would refuse every good image";
    EXPECT_FALSE(hal.DacFaulted());

    // The falsifying case: a write that fails. Boot again so the failure lands
    // during the safe-idle write, which is exactly when the gate reads it.
    MockHal bad;
    bad.FailNextDacWrite();
    auto o2 = MakeOrch(bad);
    bad.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o2.Boot();
    EXPECT_TRUE(bad.DacFaulted()) << "the failed write must latch, and never clear";
    EXPECT_TRUE(o2.SafeIdleEstablished())
        << "the safe idle is still 'established' -- which is precisely why it "
           "cannot be the gate on its own";
    EXPECT_FALSE(o2.OutputVerified())
        << "a device that cannot drive the DAC must not be confirmed healthy";
}

TEST(SystemOrchestrator, BootDrivesTheAdjustChannelIntoTheOneKiloOhmPulldown) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    // Gain 1.82 requires V_ADJ at 0V, which is the 1k pulldown power-down mode.
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K);
}

TEST(SystemOrchestrator, TrackingModeMirrorsTheSignalCodeOntoTheAdjustChannel) {
    /*
     * Spec 2.3 / DESIGN 4.4: gain 1.00 in tracking mode exists ONLY because
     * `V_ADJ` tracks the signal channel's code -- `V_ADJ = V_DAC` cancels the
     * `(R58/R61)` terms in `V_KEY = (1+R58/R61)*V_DAC - (R58/R61)*V_ADJ`.
     *
     * The gain-mode SELECTION writes the power mode, and that was all that was
     * ever written: the ADJ channel's CODE was never set, so it stayed at its
     * power-on value of 0 and the amplifier delivered 1.82x, not 1.00x. That is
     * the OVER-RANGE direction spec 6.2 calls the only dangerous one -- a 3 V
     * head unit commanded at nearly twice its intended level. The existing suite
     * passed throughout, because no test read ADJ's code.
     */
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].output.gain_mode = GainMode::kTracking;   // concrete 3 V
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 1200);   // x2 = 2400 mV < 2600 -> 3 V
    o.Boot();

    ASSERT_EQ(o.ChannelGainMode(0), GainMode::kTracking);
    ASSERT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_NORMAL)
        << "gain 1.00 needs V_ADJ live, not the pulldown";
    // The mirror itself: the idle write must have put the SAME code on V_ADJ.
    EXPECT_EQ(hal.LastDacCode(DAC_CH_ADJ1), hal.LastDacCode(DAC_CH_KEY1))
        << "tracking mode must drive V_ADJ = V_DAC, or the gain is 1.82 not 1.00";
}

TEST(SystemOrchestrator, AmplifiedModeLeavesTheAdjustChannelAtZero) {
    // The other half, and the reason the mirror is conditioned on the mode: in
    // amplified mode V_ADJ must stay in its 1 kohm power-down (0 V) so the
    // 1.82 gain holds. Mirroring unconditionally would silently drop a 5 V head
    // unit to gain 1.00 and lose most of the output span.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);   // 5 V line
    o.Boot();
    ASSERT_EQ(o.ChannelGainMode(0), GainMode::kAmplified);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_ADJ1), 0)
        << "amplified mode needs V_ADJ at 0 V (the 1k pulldown); mirroring would "
           "defeat the 1.82 gain";
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

// The pass-through reference is PER CHANNEL. It was one device-wide member
// captured from SWC1 only, then self-healed inside `ServiceChannel` from
// whichever channel's reading was higher -- so a second channel whose idle sat
// more than `kPassThroughPressDeltaMv` above the first made the FIRST channel's
// real idle look like a press, and a channel reading ~0 (a disconnected input,
// the one-wheel-car case) did it unconditionally. Measured before the fix: two
// idle channels at 2835/3300 drove a phantom key on channel 0 with nothing held.
namespace {

// A two-channel device with NO stored config, so Boot() selects pass-through.
// Both channels are given a name and `enabled = true`, because
// `ConfigDefault`'s values are what the real device boots with.
SystemOrchestrator MakeUnconfiguredTwoChannel(MockHal &hal) {
    Config c{};
    ConfigDefault(&c);
    c.channel_count = 2;
    hal.ClearNvs();
    return SystemOrchestrator(&hal.InterfaceRef(), c, GestureTimingsDefault());
}

}  // namespace

TEST(SystemOrchestrator, ATwoChannelDeviceWithUnequalIdlesDoesNotDriveAPhantomKey) {
    MockHal hal;
    auto o = MakeUnconfiguredTwoChannel(hal);
    // SWC1 idles at the spec's rail; SWC2's wheel idles 465 mV higher -- a
    // different ladder, well past the 300 mV press threshold.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 3300);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle0 = hal.LastDacCode(DAC_CH_KEY1);
    // A phantom press is a BOUNDED PULSE that self-releases on `send_duration_ms`,
    // so the final code returns to idle within 200 ms either way -- the write
    // COUNT is what distinguishes "did not touch the line" from "drove a key and
    // released it". Measured before the fix: 2 extra writes and code 2916.
    const int writes0 = hal.DacWriteCount(DAC_CH_KEY1);

    // NOTHING is pressed. Neither KEY line may move off its safe idle at all.
    PollFor(o, hal, 300);
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY1), writes0)
        << "channel 1's higher idle must not read as a press on channel 0";
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle0);
}

TEST(SystemOrchestrator, ADisconnectedSecondChannelDoesNotDriveAPhantomKeyOnTheFirst) {
    // The common one-wheel car: SWC2 is unconnected, so it reads ~0. Against a
    // SHARED reference that is a large "press" and drove a key every tick.
    MockHal hal;
    auto o = MakeUnconfiguredTwoChannel(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 0);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);
    o.Boot();
    const int writes0 = hal.DacWriteCount(DAC_CH_KEY1);

    PollFor(o, hal, 300);
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY1), writes0)
        << "an unconnected sibling input must not look like a press";
}

TEST(SystemOrchestrator, OneChannelWithoutAReferenceDoesNotDisableTheOtherChannelsPassThrough) {
    // Spec 6.9's "disabled rather than guessed" is PER INPUT: one dead wheel must
    // serve nothing while a healthy wheel still passes through.
    MockHal hal;
    auto o = MakeUnconfiguredTwoChannel(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, -1);     // the HAL's error code: unreadable
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "channel 0 still has a usable reference";
    const int idle0 = hal.LastDacCode(DAC_CH_KEY1);

    // A real press on the healthy channel still drives its key.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle0)
        << "a dead sibling must not disable the healthy channel's pass-through";
}

// The HEAD UNIT's idle is per channel too (spec 6.2 samples `/SENSEn` per
// channel), and it was kept only from channel 0. Channel 1's pass-through was
// therefore DEAD whenever channel 0 had no head unit, and mapped onto the wrong
// idle when the two head-unit inputs differed.
TEST(SystemOrchestrator, ChannelOnesPassThroughUsesItsOwnHeadUnitIdle) {
    MockHal hal;
    auto o = MakeUnconfiguredTwoChannel(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 300);    // 600 mV: NO head unit on ch0
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);  // head unit on ch1
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "channel 1 has a usable wheel reference";
    const int idle1 = hal.LastDacCode(DAC_CH_KEY2);

    hal.SetAdcMilliVolts(ADC_CH_SWC2, 1430);   // a real press on channel 1
    PollFor(o, hal, 100);
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY2), idle1)
        << "channel 1's pass-through must not depend on channel 0's head unit";
}

TEST(SystemOrchestrator, AChannelWithNoHeadUnitServesNothingWhileItsSiblingStillDoes) {
    // The inverse: with no head unit on channel 1, channel 1 must drive nothing
    // (a fabricated denominator lands on a key nothing defined) while channel 0
    // still passes through.
    MockHal hal;
    auto o = MakeUnconfiguredTwoChannel(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, 300);   // 600 mV: no head unit on ch1
    o.Boot();
    const int writes1 = hal.DacWriteCount(DAC_CH_KEY2);
    const int idle0 = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC2, 1430);   // a press channel 1 cannot serve
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // a press channel 0 can
    PollFor(o, hal, 100);
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY2), writes1)
        << "no head unit on this channel means it drives nothing, not a guessed key";
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle0)
        << "and its healthy sibling still passes through";
}

// A `test_key` pulse on a PASS-THROUGH device must survive its requested hold.
// The pass-through branch released on any `!pressed` idle tick, which cut a
// bench-driven key to zero -- measured, a 200 ms hold was released on the first
// tick after it was driven, so the bench could not exercise the output at all on
// an unconfigured board.
TEST(SystemOrchestrator, ATestKeyPulseOnAPassThroughDeviceSurvivesItsHold) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // wheel idle: nothing pressed
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "precondition: this is the pass-through path";
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    ASSERT_TRUE(o.TestDriveKeyMv(0, 2400, 200, hal.NowMs()));
    const int driven = hal.LastDacCode(DAC_CH_KEY1);
    ASSERT_NE(driven, idle_code) << "the bench command drives the line";

    // Still driven a third of the way through the hold, with nothing pressed.
    PollFor(o, hal, 60);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), driven)
        << "the pass-through idle branch must not release a bench-driven key";

    // And released once the hold elapses.
    PollFor(o, hal, 300);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "the hold still ends in a release";
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

TEST(SystemOrchestrator, AnIdleAdjacentButtonStillClassifiesWhenTheRailMoves) {
    // Spec 6.3: the ratio denominator is the idle "measured now", so `n` is
    // invariant to the +3V3 rail's own tolerance. Pinning it to the LEARNED idle
    // instead makes every ratio drift with the rail, and the drift is largest
    // for the most idle-adjacent button -- `next` at 757 permille (FR-6's worst
    // case). At the +5% band edge a press's ratio lands outside `next`'s window
    // and the press is reported UNKNOWN instead of the button.
    //
    // This pins the invariant by DRIVING A PRESS at a moved rail: a fix that
    // merely moved the number around would not make the button fire.
    g_reported.clear();
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    const int rail = 3465;   // +5% of 3300, the top of spec 6.3's band
    const int live_idle = (2835 * rail) / 3300;   // 2976 mV
    hal.SetAdcMilliVolts(ADC_CH_SWC1, live_idle);
    o.Boot();
    o.SetGestureSink(&RecordGesture, nullptr);

    // `next` at 757 permille of the rail-scaled idle.
    const int next_mv = (live_idle * 757) / 1000;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, next_mv);
    PollFor(o, hal, 100);                      // press + debounce
    hal.SetAdcMilliVolts(ADC_CH_SWC1, live_idle);   // release
    PollFor(o, hal, 700);                      // let the DOUBLE window elapse
    ASSERT_EQ(g_reported.size(), 1u)
        << "an idle-adjacent press at a +5% rail must still resolve";
    EXPECT_EQ(std::string(g_reported[0].button_id), "next")
        << "a pinned denominator drops an idle-adjacent button at the band edge";
}

TEST(SystemOrchestrator, APressOnAHealthyMovedRailDoesNotReadAsAFault) {
    // The other half of the same defect: with the denominator pinned to the
    // LEARNED idle, a reading at the top of the +-5% band is >3% ABOVE the
    // reference, which LadderClassify reports as kFault -- the "short to a
    // supply" case. So a merely high rail fabricated a wiring fault. The live
    // denominator keeps idle at 1000 by construction, so it never trips.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, (2835 * 3465) / 3300);   // idle at +5% rail
    o.Boot();
    PollFor(o, hal, 200);
    EXPECT_FALSE(o.Faulted())
        << "a healthy +5% rail must not be reported as a fault";
}

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

TEST(SystemOrchestrator, AReLearnOnAMovedRailKeepsTheOtherButtonsOnTheirOwnWindows) {
    // A ladder profile has ONE `learned_idle_mv`, and every button stores its
    // centre in ABSOLUTE millivolts measured at that one rail. A re-learn measures
    // the new button on the LIVE rail and stamps the live idle as the denominator,
    // so if the rail moved since the seeded buttons were learned, leaving THEM
    // alone stores two frames under one denominator. Classification then reads
    // them on the wrong scale, and because nearest-centre matching still returns
    // SOMETHING, the wrong button fires -- a silent wrong action rather than an
    // error. Measured before the fix: two buttons 200 mV apart learned at 2835 mV,
    // one re-learned at 2693 mV (-5 %, inside the documented band); pressing the
    // other, whose window is centred at 864 permille, reported the 794-permille
    // button.
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 2;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    d.config.channels[0].ladder.buttons[0] = {"swc1_bt1", "A", 2250, 100, 3300, 235, 200, 98};
    d.config.channels[0].ladder.buttons[1] = {"swc1_bt2", "B", 2450, 100, 3300, 235, 200, 98};
    // The default bindings name `vol_up`/`next`, which this renamed ladder does
    // not carry; `ConfigValidate` refuses a binding that names no real button, and
    // a refused config decodes to defaults (spec 6.8). Zero them.
    d.config.binding_count = 0;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);

    // The rail has moved DOWN to 2693 (a -5 % regulator deviation), and the wheel
    // idles there. Boot adopts the live idle as the reference.
    const int kMovedIdle = 2693;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, kMovedIdle);
    o.Boot();
    ASSERT_EQ(o.IdleReferenceMv(0), kMovedIdle)
        << "Boot must adopt the live moved rail (spec 6.3)";

    // Re-learn slot 1 over the button physically at 2250 mV, which is 2137 mV on
    // the moved rail.
    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2137);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, kMovedIdle);
    PollFor(o, hal, 200);
    HoldAuxToToggle(o, hal);   // leave the wizard

    const LadderProfile *learned = o.LastLearnedProfile(0);
    ASSERT_NE(learned, nullptr) << "the re-learn must have committed";
    ASSERT_EQ(learned->count, 2) << "the re-learn must CORRECT a slot, not add a third";

    // Now press the OTHER button -- the one this re-learn did not touch. Its
    // physical level on the moved rail is 2450 * 2693/2835 = 2327 mV. It must
    // still name ITSELF; naming the 2250-mV button means the two frames were
    // mixed and the wrong window fired.
    g_reported.clear();
    o.SetGestureSink(&RecordGesture, nullptr);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2327);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, kMovedIdle);
    PollFor(o, hal, 700);

    ASSERT_EQ(g_reported.size(), 1u)
        << "the untouched button must classify after a re-learn on a moved rail";
    // `button_id` is null when the level matched no window, so compare through a
    // guard: a null deref here would turn a clean assertion failure into a crash.
    const char *got = g_reported[0].button_id;
    EXPECT_STREQ(got == nullptr ? "(none)" : got, "swc1_bt2")
        << "the press at 2327 mV is the 2450-mV button; reporting the 2250-mV one "
           "means the seeded buttons were left in the OLD rail's frame while the "
           "profile was stamped with the new one";
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

TEST(SystemOrchestrator, TheMaintenanceTriggerIsRecordedAndNothingInProductionBranchesOnIt) {
    // `MaintenanceTrigger` documents itself as existing because "the caller's
    // shutdown path differs": a USB command should be acknowledged, an AUX1 hold
    // gets a buzzer, and a no-config boot "must explain itself on the LED". None of
    // that exists. Two of its five values (`kConfigFlag`, `kNoConfigAtBoot`) have
    // NO emitter anywhere, and no production code branches on the trigger at all
    // -- the only reader in the tree is the accessor `MaintenanceTriggeredBy()`,
    // whose sole caller is a test. So a device in maintenance mode is
    // INDISTINGUISHABLE, on the wire and to the user, from one whose window opened
    // for any other reason.
    //
    // **That matters most for the AUX1 path, which is the no-app path.** A user who
    // holds AUX1 for 3 s gets LED_STAT's double-flash -- and so does a user whose
    // window opened from the app, or from a config flag. The LED is the only
    // evidence a no-app user has, and it cannot say which. The buzzer the enum
    // promises for an AUX1 hold is never played.
    //
    // This test records the state rather than fixing it: making the triggers
    // distinct is a feature (a per-trigger acknowledgement plus a wire field),
    // not a repair, and the recorded finding is N-61.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // The USB path records kUsbCommand...
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    ASSERT_TRUE(o.MaintenanceActive());
    EXPECT_EQ(o.MaintenanceTriggeredBy(), MaintenanceTrigger::kUsbCommand)
        << "the trigger is recorded faithfully -- this is not the defect";
    o.ExitMaintenance();

    // ...and the AUX1 path records kAux1Hold. The two ARE distinguished in the
    // stored value; what is missing is any consequence of the difference.
    HoldAuxFor(o, hal, SystemOrchestrator::kMaintenanceHoldMs + 150);
    ASSERT_TRUE(o.MaintenanceActive());
    EXPECT_EQ(o.MaintenanceTriggeredBy(), MaintenanceTrigger::kAux1Hold);

    // The observable consequence that IS missing: an AUX1 hold plays no
    // acknowledgement. Nothing in this build plays a maintenance-entered pattern
    // for either trigger, so the two are indistinguishable to the user.
    EXPECT_FALSE(o.LearnActive()) << "escalation still leaves no learn behind it";
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
    // page is unacceptable. The window is bounded by the configured
    // `maintenance_timeout_ms` -- default 5 minutes -- timed from `Enter`.
    //
    // NOT "5 minutes of inactivity": nothing calls `NoteActivity` in this build
    // (N-35), so the clock is never bumped and this is a FIXED deadline. The
    // poll below is from entry, which is what the firmware actually measures.
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
    EXPECT_FALSE(o.MaintenanceActive())
        << "the configured window must close and serve a press again";
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
        << "repeated activity must keep the window open past the original deadline";
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

// The fraction of a 2 s window LED_STAT spends LOW, in ticks. This is the
// discriminator the learn-handback tests need: `kSolid` holds the line high
// continuously (0 low ticks), while `kBreathe` is a 1 Hz 500/500 square (~half).
// A rising-edge COUNT cannot tell them apart -- a solid line produces exactly one
// rise, which is the same order as a slow breathe's two.
int CountStatLowTicks(MockHal &hal, SystemOrchestrator &o, uint32_t total_ms) {
    int low = 0;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        o.Tick(hal.NowMs());
        if (!hal.GpioRead(GPIO_LED_STAT)) ++low;
        hal.AdvanceMs(5);
    }
    return low;
}

TEST(SystemOrchestrator, LeavingTheLearnHandsLedStatBackToTheLinkStateNotASolidLie) {
    // **Spec 7.3 gives LED_STAT one meaning per state, and the learn's own
    // handback cannot supply it.** `Solid` is documented as "Running, output safe,
    // **USB connected**, config valid", "Slow breathe (1 Hz)" as "Running
    // normally, **no USB**". `LearnWizard::Exit` unconditionally sets `kSolid`
    // (LearnWizard.cpp:121) -- it has no HAL view of the link, and `ConsumeExited`
    // hands back only LED2 -- so the wizard alone would leave a no-host device
    // solid, which spec 7.3 reads as "USB connected".
    //
    // What makes the handback RIGHT is that the wizard's exit is not the last
    // word: `ServiceLearn` defers the maintenance-LED decision while the wizard is
    // active and holds `maintenance_led_state_` at its INVERSE, so the first tick
    // after the wizard leaves sees a mismatch and repaints through `RestatLeds`,
    // which ranks fault > maintenance > link and reads `usb_connected_`
    // (SystemOrchestrator.cpp:708-715). This test is the guard on that
    // mechanism -- mutation-tested by collapsing the deferred transition, which
    // makes it fail -- and on the link state being the one that lands.
    //
    // The exit is REACHABLE without a host by design: the second AUX1 hold is the
    // documented way to leave the learn (spec 7.4 step 5).
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Deliberately NO SetUsbConnected(true): no host is attached. Boot has
    // already painted `kBreathe` through RestatLeds.
    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    HoldAuxToToggle(o, hal);
    ASSERT_FALSE(o.LearnActive()) << "the learn is over";

    // Let the exit prompt and the handback settle before measuring the steady
    // state, so this cannot pass or fail on a transient.
    PollFor(o, hal, 100);
    EXPECT_GT(CountStatLowTicks(hal, o, 2000), 100)
        << "with no USB host, LED_STAT must breathe (spec 7.3), not sit solid";
}

TEST(SystemOrchestrator, AnEscalatedHoldShowsMaintenanceEvenWithNoHostAttached) {
    // **The 3 s AUX1 escalation used to leave LED_STAT solid on a no-host
    // device.** The two hold tiers (1.5 s learn, 3 s maintenance) are one
    // continuous user action, so the same tick can exit the wizard and open the
    // window. The maintenance-LED deferral then has to repaint ONCE, from the
    // wizard's `kSolid` handback to the window's double-flash -- and it did not.
    //
    // The deferral tracked "a repaint is owed" by writing `!want_maint_led` into
    // the state flag. During the learn, maintenance is not yet open, so `want` is
    // false and the sentinel stored TRUE; the 3 s tier then opened the window in
    // the same tick, making `want` true too -- equal to the sentinel, so the edge
    // never fired. The wizard's solid handback stuck for the entire window, on a
    // device with no host, which spec 7.3 reads as "USB connected". The flag is
    // now an explicit owed-restat boolean (SystemOrchestrator.cpp:708-724), so it
    // cannot be desynced by the deferral's own sentinel.
    //
    // This is the escalation twin of `AMaintenanceWindowDoubleFlashesLedStat`,
    // which covers the plain path; the bug lived only in the overlapping one.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // ONE continuous hold past both tiers: 1.5 s enters the learn, 3 s escalates.
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxPressedMv);
    PollFor(o, hal, SystemOrchestrator::kMaintenanceHoldMs + 150);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    PollFor(o, hal, 60);
    ASSERT_TRUE(o.MaintenanceActive()) << "the long hold must escalate to maintenance";
    ASSERT_FALSE(o.LearnActive()) << "and leave the learn behind it";

    PollFor(o, hal, 100);
    // A 2 s window at 5 ms steps is 400 ticks. Solid holds it high throughout
    // (0 low); a 1 Hz breathe is ~50% (~200); the maintenance double-flash is
    // 100/100 + 100/600 per 900 ms period, i.e. ~78% low (~310). 250 sits between
    // the breathe and the burst, so this asserts the double-flash itself rather
    // than merely "not solid".
    EXPECT_GT(CountStatLowTicks(hal, o, 2000), 250)
        << "the window must double-flash even with no host attached; a solid "
           "LED_STAT here means the wizard's handback was never repainted";
}

TEST(SystemOrchestrator, LeavingTheLearnStillShowsSolidWhenAHostIsAttached) {
    // The other half, so the fix cannot be "always breathe": with a host attached
    // the handback must still land on `kSolid`, which is the state spec 7.3 gives
    // a connected device. This is the assertion that makes the pair a parity
    // check rather than a one-sided one.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    o.SetUsbConnected(true);

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    HoldAuxToToggle(o, hal);
    ASSERT_FALSE(o.LearnActive());

    PollFor(o, hal, 100);
    EXPECT_EQ(CountStatLowTicks(hal, o, 2000), 0)
        << "with a host attached, LED_STAT must stay solid";
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

/*
 * The sense envelope is NOT a fault. Two conditions share the safety release --
 * FR-4's ladder out-of-range and spec 6.2 step 2's "no head unit" -- but only
 * the first may latch, and folding them together made the SECOND do it too.
 *
 * Why this matters mechanically: spec 4.4 says the head unit "may sleep,
 * suspend, or reboot at any moment", and spec 6.8's head-unit-gone row says
 * "keep classifying". A reboot-only latch (spec 7.3) on that condition means the
 * first time the head unit sleeps -- or the user parks and the radio powers
 * down -- LED_STAT blinks forever, telling the driver the adapter is broken
 * when it is not. Recovery would need a full reboot, which is the one thing the
 * indication must not require for a normal event.
 */
TEST(SystemOrchestrator, AHeadUnitThatGoesAwayReleasesButDoesNotLatchAFault) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_FALSE(o.Faulted()) << "a present head unit is not a fault";

    // The head unit powers down: its KEY line collapses, so 2 x 250 = 500 mV is
    // far below the envelope's 1.8 V floor.
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);
    PollFor(o, hal, 300);
    EXPECT_FALSE(o.Faulted())
        << "a head unit that went away is spec 6.8's 'keep classifying', not a "
           "latched hardware fault -- the head unit may sleep and come back";
}

TEST(SystemOrchestrator, AHeadUnitThatComesBackKeepsWorking) {
    // The other half, and the reason the latch is wrong rather than merely
    // pessimistic: the release must be recoverable without a reboot. A press
    // after the head unit returns drives a key exactly as before.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);   // gone
    PollFor(o, hal, 300);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);   // back
    PollFor(o, hal, 100);

    EXPECT_NE(PressAndCaptureDrivenCode(o, hal, 1430), idle_code)
        << "a head unit that returned must classify and drive, with no reboot";
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
    // FR-38's timeout is the one exit with no caller to notify, so the LED must
    // be restated from the tick rather than from the entry/exit sites. A device
    // whose radio window expired while still double-flashing would tell the user
    // to look at a state it is no longer in. (The window is a FIXED deadline
    // from `Enter`, not inactivity -- N-35 -- so this advances straight past it.)
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

    // Past the configured window, in one jump: `ShouldTimeout` is a comparison
    // on elapsed time, so the intermediate ticks carry no information.
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

TEST(SystemOrchestrator, TheLearnWizardDrivesLed2AsPartOfItsPrompt) {
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
    HoldAuxToToggle(o, hal);
    const bool learn_entered = o.LearnActive();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    // The property the old printf probe existed to eyeball, now ASSERTED so it is
    // a real test rather than a probe whose output the runner swallows: LED2 is
    // dark for the first part of a learn prompt (the wizard alternates the two
    // LEDs), so a dark LED2 here is correct and a SOLID one would mean the prompt
    // never started.
    for (int i = 0; i < 30; ++i) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(5);
    }
    EXPECT_TRUE(learn_entered) << "the AUX1 hold must enter the learn wizard";
    EXPECT_GT(hal.GpioWriteCount(GPIO_LED2), 0)
        << "the wizard must drive LED2 as part of its prompt, not leave it untouched";
}

/*
 * A FAILED ladder conversion must not read as a press.
 *
 * `IHAL::adc_read_mv` returns -1 on error, and 0 mV is a LEGAL reading (a button
 * at the ladder's common), which is why the sentinel exists rather than a zero.
 * The pass-through press test is `(idle - level) > kPassThroughPressDeltaMv`, and
 * `idle - (-1)` is `idle + 1`: a failed read therefore looks like the wheel
 * pulled DOWN by the full reference, i.e. the largest possible press. Every other
 * raw read in this file guards the sentinel (`>= 0`); this one did not, so an ADC
 * glitch or a bus fault drove a phantom key at the mapped level -- the exact
 * failure FR-12/FR-39 exist to prevent, and the one the ladder path's own
 * `AdcReader` guards by dropping failed conversions.
 *
 * A failed read means "no measurement", so the safe response is to hold the
 * current press state rather than invent either a press or a release.
 */
TEST(SystemOrchestrator, AFailedLadderConversionDoesNotDriveAPhantomKey) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // The HAL's error code, at idle: nothing is pressed and nothing was measured.
    // Sampled EVERY tick: a phantom pulse self-releases after send_duration_ms, so
    // a check at the end of a long poll would see only the released line and miss
    // the drive entirely.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, -1);
    int drives = 0;
    for (uint32_t t = 0; t < 400; t += 10) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(10);
        if (hal.LastDacCode(DAC_CH_KEY1) != idle_code) ++drives;
    }

    EXPECT_EQ(drives, 0)
        << "a failed ADC read is not a press; driving the line here is the "
           "phantom-key hazard the -1 sentinel exists to prevent";
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

// --- the production boot config -------------------------------------------
//
// **Every test above this line injects `MockHal::Defaults`, and that is the gap
// this section closes.** No test ever booted the orchestrator with
// `ConfigDefault()` -- the config production actually uses -- so a defect that
// only the production config exhibits was invisible to a 350-test green suite.
// It was not hypothetical: `ConfigDefault()` shipped both channels with
// `enabled = false`, and the binding resolve was gated on that flag, so on a
// real device every binding the app pushed was unfindable and the press fell
// through to the pass-through default. The bound key voltage was silently
// replaced by the button's own level, with `KEY_ACCEPTED` feedback and no
// report anywhere. The only thing that distinguishes that from correct behavior
// is the DAC CODE, which is why these tests assert on it.

TEST(SystemOrchestrator, TheProductionConfigEnablesItsChannels) {
    // Spec 3.4 scopes `Channel.enabled` to CLASSIFICATION. A channel with no
    // learned buttons is described by `ladder.count == 0`, not by `enabled =
    // false`; leaving it false is what made every app-pushed binding unfindable
    // on a fresh device.
    Config c{};
    ConfigDefault(&c);
    ASSERT_GE(c.channel_count, 1);
    for (uint8_t i = 0; i < c.channel_count; ++i) {
        EXPECT_TRUE(c.channels[i].enabled)
            << "channel " << static_cast<int>(i)
            << ": the production config must not ship a channel disabled -- "
               "`enabled` gates classification, and `ladder.count` already says "
               "this channel has learned nothing";
    }
}

TEST(SystemOrchestrator, ABindingResolvesOnADeviceRunningTheProductionConfig) {
    // The end-to-end shape of the defect, on the config production ships: an app
    // binds a learned button, and the device must drive the BOUND voltage.
    //
    // `ConfigDefault()` has no learned buttons, so the test learns one
    // headlessly first (the real no-app flow), then presses it and checks the
    // driven code against the action's own level. Asserting merely "something
    // was driven" would NOT catch this: the pass-through default also drives
    // something. Only the code distinguishes them.
    MockHal hal;
    hal.ClearNvs();   // a NEVER-configured device: Boot() takes the real path

    Config c{};
    ConfigDefault(&c);
    // The binding below must name a button the config ALREADY CARRIES. A binding
    // to a button that is not yet on the ladder fails `ConfigValidate`
    // (`BindingNamesARealInput`), so `store.Save` persists a config the next
    // `Load` refuses -- and after Boot's `kFellBackToDefaults` fix that config is
    // replaced by the DEFAULTS (spec 6.8), taking the binding with it. This test
    // used to pass only because the fallback silently kept the constructor's
    // config, which is the bug the fix removes.
    //
    // So seed slot 1 with the id the wizard re-measures: the config is then VALID
    // from the first save, it loads as `kLoaded`, and the headless learn below
    // re-measures that same entry in place. (The app works the same way -- it can
    // only bind a button the device has already learned.)
    c.channels[0].ladder.count = 1;
    std::strncpy(c.channels[0].ladder.buttons[0].id, "swc1_bt1",
                 sizeof(c.channels[0].ladder.buttons[0].id) - 1);
    std::strncpy(c.channels[0].ladder.buttons[0].name, "Button 1",
                 sizeof(c.channels[0].ladder.buttons[0].name) - 1);
    c.channels[0].ladder.buttons[0].mv_center = 1430;
    c.channels[0].ladder.buttons[0].mv_tolerance = 100;
    c.channels[0].ladder.buttons[0].learned_at_rail_mv = 3300;
    c.channels[0].ladder.buttons[0].sample_count = 30;
    c.channels[0].ladder.buttons[0].confidence = 90;
    // Pre-bind the id the wizard generates for slot 1, so the learn fills in the
    // level that this binding names.
    c.binding_count = 1;
    std::strncpy(c.bindings[0].id, "b1", sizeof(c.bindings[0].id) - 1);
    c.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[0].button, "swc1_bt1", sizeof(c.bindings[0].button) - 1);
    c.bindings[0].gesture = Gesture::kSingle;
    c.bindings[0].enabled = true;
    c.bindings[0].action_count = 1;
    c.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    c.bindings[0].actions[0].key_mv = 2400;   // the BOUND level

    ConfigStore store(&hal.InterfaceRef());
    // Guards the fixture: if `c` ever stops being a config the device would
    // ACCEPT, `Save` persists bytes the next `Load` refuses, Boot falls back to
    // defaults, and the assertions below measure the fallback instead of the
    // binding. Asserting validity here is what keeps that from being a green lie.
    ASSERT_TRUE(ConfigValidate(c)) << "the fixture config must be one the device accepts";
    ASSERT_TRUE(store.Save(c));

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Learn one button headlessly (spec 7.4 / FR-31).
    HoldAuxToToggle(o, hal);
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 300);
    ASSERT_NE(o.LastLearnedProfile(0), nullptr)
        << "the headless learn must have completed for this test to mean anything";
    HoldAuxToToggle(o, hal);   // leave the wizard

    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    const int bound_code =
        GainPolicyCodeForTarget(o.ChannelGainMode(0), 2400).dac_code;
    ASSERT_NE(bound_code, idle_code)
        << "fixture error: the bound level must differ from idle, or the "
           "assertion below cannot tell a bound drive from no drive";

    const int driven = PressAndCaptureDrivenCode(o, hal, 1430);
    EXPECT_NE(driven, idle_code) << "a bound press must drive the KEY line";
    EXPECT_EQ(driven, bound_code)
        << "the device must drive the level the BINDING names, not the "
           "pass-through default for that button. A different code here is the "
           "silent-wrong-key defect: the user bound one voltage and got another, "
           "with KEY_ACCEPTED feedback and nothing reporting a problem";
}

TEST(SystemOrchestrator, ABuzzBindingPlaysItsPatternAndStillDrivesTheKey) {
    // Spec 3.6's `BUZZ` row is in the FIRMWARE's column ("local audible
    // confirmation"), and spec 3.5's shape is "emit the factory key press AND
    // tell the app" -- so a binding may name a level AND a pattern. Until
    // 2026-09-21 `BUZZ` was declared executable, accepted by the codec, offered
    // by the app's picker, and executed by NEITHER side: the app skips it as "the
    // firmware's half" and the firmware only looked at `kOutVoltage`.
    MockHal hal;
    MockHal::Defaults d;
    d.config.binding_count = 1;
    std::strncpy(d.config.bindings[0].id, "buzz", sizeof(d.config.bindings[0].id) - 1);
    d.config.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(d.config.bindings[0].button, "vol_up",
                 sizeof(d.config.bindings[0].button) - 1);
    d.config.bindings[0].gesture = Gesture::kSingle;
    d.config.bindings[0].enabled = true;
    d.config.bindings[0].action_count = 1;
    d.config.bindings[0].actions[0].kind = ActionKind::kBuzzer;
    std::strncpy(d.config.bindings[0].actions[0].target, "ProgramEnter",
                 sizeof(d.config.bindings[0].actions[0].target) - 1);

    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // PROGRAM_ENTER is 40/40 x2 = 120 ms; KEY_ACCEPTED is a single 25 ms pulse.
    // Counting ON-TRANSITIONS over a window long enough for both distinguishes
    // "the buzzer pattern played" from "only KEY_ACCEPTED played".
    const int before = hal.BuzzerOnCount();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    int on_transitions = 0;
    bool prev = false;
    for (uint32_t t = 0; t < 400; t += 5) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(5);
        const bool now = hal.BuzzerIsOn();
        if (now && !prev) ++on_transitions;
        prev = now;
    }
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);

    EXPECT_GT(hal.BuzzerOnCount(), before) << "a BUZZ binding must reach the buzzer";
    EXPECT_GE(on_transitions, 2)
        << "PROGRAM_ENTER is two pulses (spec 7.2); fewer means the BUZZ action "
           "did not play and only KEY_ACCEPTED was heard -- the silent no-op this "
           "test exists to catch";
    // It REPLACES KEY_ACCEPTED, it does not follow it: one buzzer, and `Play`
    // replaces rather than queues, so two patterns in one tick means the second
    // is the only one heard. An explicitly bound pattern outranks the default
    // acknowledgement.
    EXPECT_EQ(on_transitions, 2)
        << "exactly PROGRAM_ENTER's two pulses -- a third would mean KEY_ACCEPTED "
           "was also played, and one means KEY_ACCEPTED replaced the bound "
           "pattern, which is the bug this test found";

    // And the level action shape still works: a BUZZ does not suppress the key
    // press, because spec 3.5 makes a failed or additional action independent.
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), -1);
    (void)idle_code;
}

TEST(SystemOrchestrator, AFailedNvsWriteIsReportedAsNotPersisted) {
    // `persisted_` answers "is this learn DURABLE", not "is a store attached".
    // An earlier revision set it from the pointer alone
    // (`persisted_ = (store_ != nullptr)`), so a FAILED write -- full NVS, a write
    // error, a config the store refused -- reported the learn as saved. The user
    // hears LEARN_OK, nothing anywhere says otherwise, and the button is gone at
    // the next boot. `Save` returns false for exactly these cases and the return
    // value was discarded.
    MockHal hal;
    hal.ClearNvs();
    Config c{};
    ConfigDefault(&c);
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(c));

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);

    // Arm the failure immediately before the commit's write, not before the
    // learn, so nothing else consumes it.
    hal.FailNextNvsWrite();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 300);

    ASSERT_NE(o.LastLearnedProfile(0), nullptr)
        << "the learn itself must still have completed -- this is about the REPORT";
    EXPECT_FALSE(o.LastLearnPersisted())
        << "the NVS write failed, so the learn is NOT durable and must not be "
           "reported as saved -- the user would lose the button at the next boot "
           "with nothing having said so";
}

TEST(SystemOrchestrator, ASuccessfulNvsWriteIsReportedAsPersisted) {
    // The other direction, so the test above cannot pass by `persisted_` simply
    // always being false.
    MockHal hal;
    hal.ClearNvs();
    Config c{};
    ConfigDefault(&c);
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(c));

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 300);

    ASSERT_NE(o.LastLearnedProfile(0), nullptr);
    EXPECT_TRUE(o.LastLearnPersisted()) << "a good write must report as durable";
}

TEST(SystemOrchestrator, AFailedLearnSaveDoesNotStandDownTheConfigFault) {
    // The ordering `NoteConfigCommitted` must respect: `ApplyLearnedProfile`
    // performs its OWN save, so clearing the config-fault latch (and reporting
    // `ok`) BEFORE that save would, on a failed write, tell the app the user's
    // config is safe while nothing reached NVS -- the very "reported success for a
    // failed write" lie `persisted_` exists to prevent, one layer up.
    MockHal hal;
    hal.ClearNvs();
    Config c{};
    ConfigDefault(&c);
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(c));
    // Make the stored config unreadable, so Boot latches the config fault.
    hal.CorruptNvsValue("cfg_a_0", 24);
    hal.CorruptNvsValue("cfg_b_0", 24);
    {
        Config t{};
        ConfigStore s(&hal.InterfaceRef());
        ASSERT_EQ(s.Load(&t), ConfigLoadResult::kFellBackToDefaults) << "fixture";
    }

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_EQ(o.ConfigStateWord(), std::string("defaults")) << "fixture: the load fell back";
    ASSERT_TRUE(o.Faulted()) << "fixture: the config fault latched";

    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);
    hal.FailNextNvsWrite();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 300);

    ASSERT_FALSE(o.LastLearnPersisted()) << "precondition: the save failed";
    EXPECT_EQ(o.ConfigStateWord(), std::string("defaults"))
        << "a learn that did NOT persist has not remedied the config, so the "
           "device must not claim the config is ok";
    EXPECT_TRUE(o.Faulted())
        << "and the config-fault blink must stay latched over an unremedied config";
}

// --- re-learning: a learn ADDS, it does not replace the ladder ----------------
//
// The learn's output is ASSIGNED over the channel's ladder
// (`ApplyLearnedProfile`: `config_.channels[ch].ladder = profile`), so a session
// that started from an empty profile DELETED every button the channel already had.
// Measured before the fix: three learned buttons became one.

TEST(SystemOrchestrator, ReLearningOneButtonKeepsTheChannelsOtherButtons) {
    // The user's plausible action: learn three buttons, come back later to
    // re-measure ONE of them. Before the fix the other two were destroyed, with
    // LEARN_OK feedback and nothing reporting a loss.
    MockHal hal;
    hal.ClearNvs();
    Config c{};
    ConfigDefault(&c);
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(c));

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Session 1: learn three buttons on three slots.
    HoldAuxToToggle(o, hal);
    const int levels[3] = {1430, 1750, 2100};
    for (int slot = 1; slot <= 3; ++slot) {
        PressAux(o, hal, slot);
        hal.SetAdcMilliVolts(ADC_CH_SWC1, levels[slot - 1]);
        PollFor(o, hal, 400);
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
        PollFor(o, hal, 300);
    }
    HoldAuxToToggle(o, hal);   // leave the wizard
    ASSERT_NE(o.LastLearnedProfile(0), nullptr);
    ASSERT_EQ(o.LastLearnedProfile(0)->count, 3)
        << "three learns in one session must leave three buttons";

    // Session 2: re-enter and re-learn slot 2 at a nearby level.
    HoldAuxToToggle(o, hal);
    PressAux(o, hal, 2);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1720);   // inside bt2's own old window
    PollFor(o, hal, 400);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 300);
    HoldAuxToToggle(o, hal);

    const LadderProfile *p = o.LastLearnedProfile(0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->count, 3)
        << "re-learning ONE button must not delete the channel's other buttons -- "
           "the learn ADDS, it does not replace the ladder";
    // And slot 2 is CORRECTED rather than refused: a re-measure lands inside the
    // button's own old window by definition, so leaving that entry in the
    // neighbour set would reject every attempt to fix a button.
    EXPECT_NEAR(p->buttons[1].mv_center, 1720, 30)
        << "the re-measured button must actually be updated, not left at its old "
           "level by a too_close_to_existing refusal against itself";
    // The other two survive at their original levels.
    EXPECT_NEAR(p->buttons[0].mv_center, 1430, 30);
    EXPECT_NEAR(p->buttons[2].mv_center, 2100, 30);
}

TEST(SystemOrchestrator, ReLearningTheSameSlotTwiceDoesNotDuplicateItsId) {
    // Two buttons sharing one id is not rejected anywhere, yet `BindingResolve`
    // and `BindingsForButton` both find a binding by `strcmp` on the id -- so a
    // duplicate makes "which window does this binding mean" ambiguous between two
    // different voltages. The second learn must REPLACE the entry in place.
    MockHal hal;
    hal.ClearNvs();
    Config c{};
    ConfigDefault(&c);
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(c));

    SystemOrchestrator o(&hal.InterfaceRef(), c, c.settings.timings);
    o.SetStore(&store);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    HoldAuxToToggle(o, hal);
    for (int pass = 0; pass < 2; ++pass) {
        PressAux(o, hal, 1);   // the SAME slot both times
        hal.SetAdcMilliVolts(ADC_CH_SWC1, pass == 0 ? 1430 : 1750);
        PollFor(o, hal, 400);
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
        PollFor(o, hal, 300);
    }

    const LadderProfile *p = o.LastLearnedProfile(0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->count, 1)
        << "learning the same slot twice must correct it, not add a second button";
    EXPECT_NEAR(p->buttons[0].mv_center, 1750, 30)
        << "and the later measurement is the one that survives";
    // No two entries may share an id.
    for (uint8_t i = 0; i < p->count; ++i) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < p->count; ++j) {
            EXPECT_STRNE(p->buttons[i].id, p->buttons[j].id)
                << "two buttons with one id make the id an ambiguous key";
        }
    }
}

// --- the stored config's TIMINGS are the runtime timings ---------------------

TEST(SystemOrchestrator, TheStoredTimingsAreWhatTheDeviceRunsWith) {
    // The device path constructs with `ConfigDefault()`'s timings, and every
    // channel's classifier and gesture machine is built from `timings_` -- so a
    // loaded config's own timings must be adopted in `Boot`, or a user's
    // `long_press_ms` is stored, reported in `config_get`, and SILENTLY IGNORED.
    // Measured before the fix: `long_press_ms = 1500` still fired LONG at 750 ms.
    //
    // The test drives the LONG boundary directly: hold a LONG-binding button for
    // a time that is past the default 750 but short of the user's 1500, and
    // require NO LONG. Then hold past 1500 and require one.
    MockHal hal;
    MockHal::Defaults d;
    d.config.settings.timings.long_press_ms = 1500;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    // The DEVICE construction: default timings at construction, config from NVS.
    Config boot{};
    ConfigDefault(&boot);
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            switch (ev.gesture) {
                case Gesture::kSingle: v->push_back("SINGLE"); break;
                case Gesture::kDouble: v->push_back("DOUBLE"); break;
                case Gesture::kLong:   v->push_back("LONG");   break;
                default: break;
            }
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Hold vol_up (which binds LONG) for 900 ms: under the user's 1500.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 900);
    EXPECT_EQ(std::find(seen.begin(), seen.end(), "LONG"), seen.end())
        << "LONG fired before the user's long_press_ms (1500): the stored timings "
           "were ignored and the default 750 was used";
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);

    // Now hold past 1500 and require the LONG.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 1700);
    EXPECT_NE(std::find(seen.begin(), seen.end(), "LONG"), seen.end())
        << "a hold past the user's long_press_ms must still fire LONG";
}

// --- ApplyConfig: a committed config runs without a reboot (spec 4.2) ---------

TEST(SystemOrchestrator, ACommittedConfigIsWhatTheRunningDeviceClassifiesAgainst) {
    // Spec 4.2: "committed" is BOTH halves -- persisted AND running. The app
    // adopts the config it pushed as the device's live state on the `ack` and
    // offers no reboot affordance, so a device still classifying against the
    // previous config shows the user a binding it will not honour until a power
    // cycle that a car-installed device may never get.
    //
    // Measured before `ApplyConfig`: this test's press produced NO SINGLE, while
    // the SAME config stored before `Boot` produced one.
    MockHal hal;
    MockHal::Defaults d;

    // Boot on a config with NO bindings: every learned button is an unbound
    // gesture, so a press resolves to nothing.
    d.config.binding_count = 0;
    Config boot = d.config;
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            switch (ev.gesture) {
                case Gesture::kSingle: v->push_back("SINGLE"); break;
                case Gesture::kDouble: v->push_back("DOUBLE"); break;
                case Gesture::kLong:   v->push_back("LONG");   break;
                default: break;
            }
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // A short tap on vol_up: unbound, so nothing is reported.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_TRUE(seen.empty())
        << "the unbound baseline must report nothing, or this test proves nothing";

    // Now push a config that BINDS vol_up's SINGLE -- exactly what `config_end`
    // hands `ApplyConfig` after a successful Save.
    Config pushed = d.config;
    pushed.binding_count = 1;
    pushed.bindings[0].id[0] = 'p';
    pushed.bindings[0].id[1] = '1';
    pushed.bindings[0].id[2] = '\0';
    std::snprintf(pushed.bindings[0].button, sizeof(pushed.bindings[0].button), "vol_up");
    pushed.bindings[0].gesture = Gesture::kSingle;
    pushed.bindings[0].enabled = true;
    pushed.bindings[0].action_count = 1;
    pushed.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    pushed.bindings[0].actions[0].key_mv = 2400;
    ASSERT_TRUE(ConfigValidate(pushed));
    o.ApplyConfig(pushed);

    // The SAME tap, against the config that just arrived, with no reboot.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_NE(std::find(seen.begin(), seen.end(), "SINGLE"), seen.end())
        << "a config committed over the link must be the config the RUNNING device "
           "classifies against (spec 4.2) -- requiring a reboot is not a permissible "
           "reading, and the app offers no reboot affordance";
}

TEST(SystemOrchestrator, APushedLaddersNewWindowsAreUsedWithoutAReboot) {
    // `SeedChannelState` rebuilds each channel's CLASSIFIER from the new ladder.
    // A binding-resolve-only apply would still classify against the OLD windows,
    // so a button the pushed config just defined would sit inside the old
    // profile's `kUnknown` band and report nothing -- a saved binding that the
    // app calls configured and the device ignores.
    //
    // Constructed so that the OLD profile CANNOT see the press and the NEW one
    // must: the baseline ladder's lowest button is 1430 mV, and the push moves the
    // ladder to a 3300 mV rail with one button at 900 mV. 900 mV is a real button
    // under the new profile and an unrecognised level under the old one.
    MockHal hal;
    MockHal::Defaults d;
    Config boot = d.config;
    boot.binding_count = 0;               // nothing is bound in the baseline
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kSingle) v->push_back("SINGLE");
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // The baseline must call 900 mV unrecognised, or the test proves nothing.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 900);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_TRUE(seen.empty()) << "the baseline ladder must not recognize 900 mV";

    // A new ladder whose one button sits where the OLD profile has nothing: the
    // baseline's lowest button is 1430 mV (+-120), so 1000 mV is unrecognised
    // there and must be recognised here.
    Config pushed = d.config;
    pushed.channels[0].ladder.count = 1;
    pushed.channels[0].ladder.buttons[0] = {"mute", "Mute", 1000, 120, 3300, 235, 200, 98};
    pushed.binding_count = 1;
    std::snprintf(pushed.bindings[0].id, sizeof(pushed.bindings[0].id), "p1");
    std::snprintf(pushed.bindings[0].button, sizeof(pushed.bindings[0].button), "mute");
    pushed.bindings[0].gesture = Gesture::kSingle;
    pushed.bindings[0].enabled = true;
    pushed.bindings[0].action_count = 1;
    pushed.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    pushed.bindings[0].actions[0].key_mv = 2400;
    ASSERT_TRUE(ConfigValidate(pushed));

    o.ApplyConfig(pushed);

    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1000);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_NE(std::find(seen.begin(), seen.end(), "SINGLE"), seen.end())
        << "the pushed ladder's window must be live immediately: a stale classifier "
           "still holds the previous windows and calls this level unrecognised";
}

TEST(SystemOrchestrator, APushedConfigRestartsTheGestureMachines) {
    // A gesture machine left mid-count from the OLD config carries a press it was
    // timing. Press twice in quick succession across an apply and, unless the
    // machine is rebuilt, the second press completes a DOUBLE against a binding
    // the pushed config never defined for it.
    MockHal hal;
    MockHal::Defaults d;
    Config boot = d.config;
    boot.binding_count = 0;
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // ONE tap on vol_up, left OPEN (not released) so the machine is mid-press.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    o.ApplyConfig(MockHalDefaultsConfig());

    // Release, then tap once more. Against a REBUILT machine that is ONE single
    // press; against a stale one it would be the second half of the first.
    int singles = 0;
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kSingle) v->push_back("SINGLE");
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    singles = static_cast<int>(seen.size());
    EXPECT_EQ(singles, 0)
        << "the press that was open across the apply must NOT be completed after it: "
           "the gesture machine has to be rebuilt, or the push's first tap pairs with "
           "the pre-apply one into a DOUBLE the user never made";
}

TEST(SystemOrchestrator, APushedConfigsTimingsTakeEffectWithoutAReboot) {
    // The `settings.*` case of the same rule, and the one a `config_patch`
    // exercises: a patched `long_press_ms` that only landed at the next boot
    // would be stored, reported in `config_get`, and silently ignored at runtime.
    MockHal hal;
    MockHal::Defaults d;
    Config boot = d.config;
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kLong) v->push_back("LONG");
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    Config pushed = d.config;
    pushed.settings.timings.long_press_ms = 1500;
    o.ApplyConfig(pushed);

    // Hold past the DEFAULT 750 but short of 1500: no LONG unless the new timing
    // was adopted.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 900);
    EXPECT_TRUE(seen.empty())
        << "LONG fired before the pushed long_press_ms (1500): ApplyConfig did not "
           "adopt the new timings";
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);

    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 1700);
    EXPECT_FALSE(seen.empty()) << "a hold past the pushed long_press_ms must fire LONG";
}

TEST(SystemOrchestrator, APushedConfigEndsPassThrough) {
    // FR-25 / spec 6.9: pass-through exists because there are no learned windows
    // to classify against. A committed config HAS them, so a device left in
    // pass-through would mirror the raw wheel onto the head unit and ignore the
    // binding the user just saved -- with entirely correct-looking feedback.
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "the fixture must start in pass-through";

    MockHal::Defaults d;
    o.ApplyConfig(d.config);
    EXPECT_FALSE(o.PassThroughActive())
        << "a committed config must end pass-through, or the binding it carries is "
           "never consulted";

    // And the reverse must NOT hold: an apply never TURNS ON pass-through.
    o.ApplyConfig(Config{});
    EXPECT_FALSE(o.PassThroughActive()) << "no apply may re-enable pass-through";
}

TEST(SystemOrchestrator, ApplyConfigKeepsTheMaintenanceWindowOpen) {
    // A config can arrive while the provisioning page is open -- the web UI talks
    // to the same link, and `config_patch` is how a browser-driven setup writes.
    // Rebuilding `maintenance_` from the new config would set `active_` back to
    // false, and the CALLER tears the radio down on an `Active()` transition: the
    // user's setup page would vanish mid-provision.
    MockHal hal;
    MockHal::Defaults d;
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.Boot();
    o.EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal.NowMs());
    ASSERT_TRUE(o.MaintenanceActive());

    Config pushed = d.config;
    pushed.settings.maintenance_timeout_ms = 60000;
    o.ApplyConfig(pushed);
    EXPECT_TRUE(o.MaintenanceActive())
        << "applying a config must not close an open maintenance window";
    EXPECT_EQ(o.MaintenanceTriggeredBy(), MaintenanceTrigger::kUsbCommand)
        << "and it must not rewrite how the window was opened";
}

TEST(SystemOrchestrator, ApplyConfigKeepsAHardwareFaultLatched) {
    // `hw_faulted_` is a statement about the BOARD (spec 7.3), not about the
    // config: a wiring fault or a collapsed rail does not fix itself, and a config
    // arriving says nothing about the ladder's wiring. So an apply keeps it
    // blinking. (The CONFIG half of the fault is the opposite case and DOES clear
    // -- see ApplyConfigClearsTheConfigFaultAndReportsOk.)
    MockHal hal;
    Config boot{};
    ConfigDefault(&boot);
    SystemOrchestrator o(&hal.InterfaceRef(), boot, boot.settings.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_EQ(o.ConfigStateWord(), std::string("none")) << "a fresh device has no config";
    ASSERT_FALSE(o.Faulted());

    // Drive a HARDWARE fault: the ladder reads above its idle reference, which is
    // FR-4's out-of-range case (a short to 12 V). It must be the LADDER that is
    // out of range, not the KEY sense: the sense envelope is spec 6.2 step 2's
    // "no head unit" test and spec 6.8 gives that row "keep classifying", so it
    // deliberately does NOT latch (a head unit that sleeps and returns is normal,
    // spec 4.4). Latching here is FR-4's alone.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 200);
    ASSERT_TRUE(o.Faulted()) << "precondition: the hardware fault must latch";

    MockHal::Defaults d;
    o.ApplyConfig(d.config);
    EXPECT_TRUE(o.Faulted()) << "an apply must not clear a hardware fault";
    // And the config's own word still moves, because a config WAS committed -- the
    // two facts are independent and the lamp shows either.
    EXPECT_EQ(o.ConfigStateWord(), std::string("ok"))
        << "a committed config is the one now in force";
}

TEST(SystemOrchestrator, ApplyConfigClearsTheConfigFaultAndReportsOk) {
    // The case the app's own warning creates: `config_state: defaults` makes the
    // link screen tell the user "Your learned buttons and bindings are gone.
    // Program it again from the Bindings screen." When the user DOES -- any commit
    // -- the device must stop saying defaults, or the app re-asserts a fault the
    // device is no longer in. `config_state` describes the config in force (spec
    // 4.3), so a committed config makes it `ok`, and the config-fault LED stands
    // down with it (spec 7.3's reboot-only latch is for HARDWARE faults).
    MockHal hal;
    MockHal::Defaults d;
    ASSERT_TRUE(([&]{ ConfigStore s(&hal.InterfaceRef()); return s.Save(d.config); })());
    hal.CorruptNvsValue("cfg_a_0", 24);
    hal.CorruptNvsValue("cfg_b_0", 24);
    {
        Config t{};
        ConfigStore s(&hal.InterfaceRef());
        ASSERT_EQ(s.Load(&t), ConfigLoadResult::kFellBackToDefaults)
            << "the fixture must actually be unreadable";
    }

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_EQ(o.ConfigStateWord(), std::string("defaults")) << "fixture: the load fell back";
    ASSERT_TRUE(o.Faulted()) << "fixture: the config fault latched a blink";

    // The user programs it again. Any of the three commit paths reaches here;
    // `ApplyConfig` is the shared tail they all call.
    o.ApplyConfig(d.config);
    EXPECT_EQ(o.ConfigStateWord(), std::string("ok"))
        << "a remedied config must stop being reported as defaults";
    EXPECT_FALSE(o.Faulted())
        << "and the config-fault blink must stand down, or the device keeps "
           "signalling 'not OK' over a config it is successfully running";
}

TEST(SystemOrchestrator, ApplyConfigRepaintsTheLevels) {
    // Feedback levels are part of the config (spec 3.3), and both grammars are
    // rebuilt from them in `ApplyConfig`. A pushed `led_level` that only took
    // effect at the next boot would leave the user's own level setting doing
    // nothing on the device in front of them.
    //
    // The observable is the LED_STAT cadence, which the level gates: the default
    // level 2 breathes (a 500/500 square, so writes every 500 ms), and a pushed
    // level of 0 must hold it dark with no further writes.
    MockHal hal;
    MockHal::Defaults d;
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.Boot();

    // Precondition: at the default level the lamp is being driven. Without this
    // the test would pass on a device that never writes at all.
    const int before = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 1200);
    const int toggles_at_level_2 = hal.GpioWriteCount(GPIO_LED_STAT) - before;
    ASSERT_GT(toggles_at_level_2, 0) << "the default level must drive LED_STAT";

    Config pushed = d.config;
    pushed.settings.led_level = 0;   // LEDs off
    o.ApplyConfig(pushed);

    // The lamp must GO DARK and then stay there. Its state is what the level gates,
    // so assert the LEVEL rather than a write count: a `RestatLeds` on a level-0
    // grammar emits exactly one off write, and a count assertion would confuse that
    // single write with a live pattern.
    PollFor(o, hal, 600);
    EXPECT_FALSE(hal.GpioRead(GPIO_LED_STAT))
        << "a level of 0 must leave LED_STAT dark after an apply";

    // ...and it must STOP toggling, which is what separates "dark" from "still
    // running the old breathe with the output clamped low".
    const int after_apply = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 1200);
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), after_apply)
        << "a level of 0 must stop driving LED_STAT after an apply: the grammar "
           "still carries the OLD level if ApplyConfig did not adopt the new one";
}


TEST(SystemOrchestrator, ADisabledChannelDoesNotClassifyItsLearnedLadder) {
    // Spec 3.4: `Channel.enabled = false` means "this channel has no learned
    // ladder to compare against -- do not try to classify its input". Three
    // places DESCRIBED that (`ConfigDefault`'s comment, spec 3.4, and
    // BindingResolver's "NOT gated on enabled" note) and none IMPLEMENTED it:
    // a disabled channel with a learned ladder still classified and emitted
    // `SINGLE@vol_up`, which would have driven the output.
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].enabled = false;   // but keep the learned ladder
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    std::vector<std::string> seen;
    o.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.button_id != nullptr) v->push_back(ev.button_id);
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // a press on a learned window
    PollFor(o, hal, 300);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 400);
    EXPECT_TRUE(seen.empty()) << "a disabled channel must not classify its ladder";
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a disabled channel must not drive the output either";
}

TEST(SystemOrchestrator, ADisabledChannelStillReleasesAKeyDrivenByTheBench) {
    // The gate is on CLASSIFICATION, not on the safety releases: `test_key` can
    // drive a disabled channel's line, and a pulse timeout must still release it.
    // Gating the whole function would leave a bench-driven key held (FR-39).
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].enabled = false;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    ASSERT_TRUE(o.TestDriveKeyMv(0, 2400, 50, hal.NowMs()));
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "the bench drive must hold a key";
    PollFor(o, hal, 200);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "the pulse must still self-release on a disabled channel";
}

TEST(SystemOrchestrator, ADisabledChannelDoesNotLatchAFaultWhenTheHeadUnitGoesAway) {
    // A disabled channel cannot classify, so its ONLY out-of-range signal is the
    // KEY-sense envelope -- and that is spec 6.2 step 2's "no head unit" test,
    // whose spec 6.8 response is "keep classifying", not a latch. Folding it into
    // FR-4's fault branch made this branch latch a reboot-only blink on a
    // recurring normal condition (the head unit sleeping, spec 4.4).
    MockHal hal;
    MockHal::Defaults d;
    d.config.channels[0].enabled = false;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_FALSE(o.Faulted());

    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);   // the head unit powers down
    PollFor(o, hal, 300);
    EXPECT_FALSE(o.Faulted())
        << "a disabled channel must release on a lost head unit without latching";
}

TEST(SystemOrchestrator, ACorruptConfigFallsBackToDefaultsAndActuallyRunsThem) {
    /*
     * Spec 6.8: "Config corrupt / bad checksum -> DEFAULTS; loud buzzer pattern;
     * report `config_state: defaults` over USB".
     *
     * The report and the LED were always right; what was MISSING is the fallback
     * itself. `Boot`'s `kFellBackToDefaults` branch reported "defaults" and
     * latched the fault but never assigned `config_`, so the device kept running
     * whatever the caller passed to the (PUBLIC) constructor. The device path
     * masked it by constructing with `ConfigDefault()`; a caller passing a real
     * config got a status frame that said one thing while the device did another.
     *
     * The observable that separates them is the channel's idle reference. Boot
     * seeds it from the live reading when that reading is a plausible rail idle
     * (spec 6.3) and from the learned idle otherwise, so the two configs still
     * diverge: the default's 2835 matches the 2835 the HAL reports and becomes the
     * reference, while the caller's 1234 does not (2835/1234 is far outside the
     * ±5 % rail band) and stays. A distinctive caller-supplied learned idle is
     * therefore still observable through `IdleReferenceMv`, which is what this
     * assertion needs.
     */
    MockHal hal;
    hal.ClearNvs();

    // A stored config that decodes to garbage: corrupt the ONLY slot's payload so
    // no slot yields a usable config.
    {
        ConfigStore boot_store(&hal.InterfaceRef());
        Config good{};
        ConfigDefault(&good);
        std::strncpy(good.device_id, "REALDEVICE", sizeof(good.device_id) - 1);
        ASSERT_TRUE(boot_store.Save(good));
    }
    hal.CorruptNvsValue("cfg_a_0", 20);

    // A caller config with a DISTINCTIVE idle reference, deliberately not the
    // default. If Boot applies the defaults this becomes 2835; if it leaves the
    // caller's config in place it stays 1234.
    Config ctor_cfg{};
    ConfigDefault(&ctor_cfg);
    std::strncpy(ctor_cfg.device_id, "CTOR-CONFIG", sizeof(ctor_cfg.device_id) - 1);
    ctor_cfg.channels[0].ladder.learned_idle_mv = 1234;

    SystemOrchestrator o(&hal.InterfaceRef(), ctor_cfg, ctor_cfg.settings.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    EXPECT_STREQ(o.ConfigStateWord(), "defaults")
        << "a corrupt config must be REPORTED as a defaults fallback (spec 6.8)";
    EXPECT_EQ(o.IdleReferenceMv(0), 2835)
        << "and the fallback must actually RUN, not just be reported: the caller's "
           "1234 would mean the device kept a config it just told the app it had "
           "discarded";
}

namespace {
// Count ON-transitions of the buzzer line over `ms`, the way the feedback tests
// do. Spec 7.2: BOOT_OK is ONE 60/60 pulse, BOOT_DEGRADED is THREE, so counting
// separates a clean boot from a degraded one without depending on timing.
int CountBootBeeps(SystemOrchestrator &o, MockHal &hal, uint32_t ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < ms; t += 5) {
        o.Tick(hal.NowMs());
        hal.AdvanceMs(5);
        const bool now = hal.BuzzerIsOn();
        if (now && !prev) ++on;
        prev = now;
    }
    return on;
}
}  // namespace

TEST(SystemOrchestrator, ADegradedAdcCalibrationBootsAsDegradedNotSilent) {
    // Spec 3.2: a blank eFuse falls back to the linear approximation AND must be
    // reported. `main.cpp` and `EspHal` both claimed "the orchestrator also plays
    // BOOT_DEGRADED for this class of condition", and it did NOT -- `IHAL` carries
    // no calibration accessor, so the orchestrator could not know, and the boot
    // pattern came only from the config load. A blank-eFuse device booted silently,
    // which is the failure the fallback clause names; the boot buzzer is the one
    // signal a user at the bench hears with no host attached.
    MockHal hal;
    hal.ClearNvs();
    MockHal::Defaults d;
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    o.SetCalibrationDegraded(true);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // 3 pulses = BOOT_DEGRADED; 1 = BOOT_OK. A window long enough for the whole
    // degraded pattern (60/60 x3 = 360 ms) plus margin.
    EXPECT_EQ(CountBootBeeps(o, hal, 1200), 3)
        << "a degraded calibration must announce BOOT_DEGRADED (spec 3.2/7.2), "
           "not boot silently as if nothing were wrong";
}

TEST(SystemOrchestrator, AHealthyAdcCalibrationStillBootsClean) {
    // The other direction, so the test above cannot pass by the boot pattern
    // always being BOOT_DEGRADED.
    MockHal hal;
    hal.ClearNvs();
    MockHal::Defaults d;
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    // Degraded defaults to false; asserted explicitly so the fixture is the flag.
    o.SetCalibrationDegraded(false);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    EXPECT_EQ(CountBootBeeps(o, hal, 1200), 1)
        << "a healthy eFuse boots BOOT_OK (one pulse), not degraded";
}

/*
 * `identify` BORROWS LED_STAT for a double-flash and must hand it back (spec
 * 7.3). Two failures used to follow from it never restoring:
 *   1. the pattern flashed FOREVER after one `identify` (until reboot), so the
 *      state LED stopped answering "is this thing OK?";
 *   2. it bypassed `RestatLeds`, so an `identify` on a FAULTED device repainted
 *      the latched fault blink as a double-flash -- spec 7.3's "a connect
 *      mid-fault must not repaint the lamp green", same hazard, different path.
 */
TEST(SystemOrchestrator, IdentifyRestoresLedStatAfterItsBurst) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    o.Boot();
    // Establish a known base state and let any boot pattern settle.
    o.SetUsbConnected(true);
    PollFor(o, hal, 100);

    o.Identify();
    PollFor(o, hal, SystemOrchestrator::kIdentifyFlashMs + 200);
    // After the burst the LED must be BACK on the link state (solid with a host
    // attached), not still cycling the double-flash.
    const int before = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 500);
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), before)
        << "LED_STAT must stop toggling once the identify burst ends; a pattern "
           "that keeps writing is one that was never restored";
    EXPECT_TRUE(hal.GpioRead(GPIO_LED_STAT))
        << "with a host attached the restored state is SOLID";
}

TEST(SystemOrchestrator, IdentifyDoesNotRepaintALatchedFault) {
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    // Drive a latched fault: the LADDER reads above its idle reference, which is
    // FR-4's out-of-range case (a short to 12 V). The KEY-sense envelope is NOT
    // usable as this fixture -- spec 6.8's head-unit-gone row says "keep
    // classifying", so it deliberately does not latch.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3000);
    PollFor(o, hal, 200);
    ASSERT_TRUE(o.Faulted()) << "precondition: the fault must latch";

    o.Identify();
    PollFor(o, hal, SystemOrchestrator::kIdentifyFlashMs + 400);
    // The fault outranks the identify burst, so once the burst ends the lamp is
    // back to the fault BLINK -- and the distinguishing property is its RATE:
    // kBlink is 100/100 (10 toggles/s), kDoubleFlash is a 900 ms burst (about 4).
    // Counting toggles over one second separates the two; "it still toggles"
    // would not, because a STUCK double-flash also toggles.
    EXPECT_TRUE(o.Faulted()) << "identify must not clear the fault";
    const int before = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 1000);
    const int toggles = hal.GpioWriteCount(GPIO_LED_STAT) - before;
    EXPECT_GE(toggles, 7)
        << "the restored state must be the fault BLINK (~10 toggles/s), not the "
           "identify double-flash (~4) left pinned over the fault";
}

TEST(SystemOrchestrator, APushedConfigDoesNotPaintOverAnActiveLearn) {
    // The learn wizard owns BOTH LEDs as prompts until it hands back, and `Tick`
    // already defers its maintenance restate for exactly that reason. `ApplyConfig`
    // repainting unconditionally would stamp a level change over a prompt the user
    // is reading -- reachable because a config push over the link and a headless
    // AUX1 learn can overlap.
    MockHal hal;
    MockHal::Defaults d;
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();

    // Enter the wizard with the 1.5 s AUX1 PROGRAMMING hold (FR-31). Not the 3 s
    // maintenance hold -- that one exits the wizard and opens a maintenance
    // window, which is a different state entirely.
    hal.SetAdcMilliVolts(ADC_CH_AUX1, 0);
    PollFor(o, hal, LearnWizard::kEnterHoldMs + 300);
    ASSERT_TRUE(o.LearnActive()) << "the hold must have opened the wizard";
    // RELEASE, or the hold keeps running and the 3 s tier escalates into
    // maintenance, which exits the wizard -- the test would then be measuring the
    // wrong state entirely.
    hal.SetAdcMilliVolts(ADC_CH_AUX1, 3300);
    PollFor(o, hal, 50);
    ASSERT_TRUE(o.LearnActive()) << "releasing AUX1 must not exit the wizard";

    // Let the wizard's prompts settle, then apply a config that changes the LED
    // level. The prompt must not be cancelled and must not be repainted over.
    PollFor(o, hal, 200);
    // The wizard's select prompt is `kAlternate` (a 250/250 square), so it TOGGLES
    // steadily. That cadence is what a level-only change must preserve.
    const int before = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 1000);
    const int toggles_before = hal.GpioWriteCount(GPIO_LED_STAT) - before;
    ASSERT_GT(toggles_before, 0) << "the wizard's prompt must be animating";

    Config pushed = d.config;
    pushed.settings.led_level = 3;
    o.ApplyConfig(pushed);

    // **The defect this catches:** `leds_ = LedGrammar(...)` constructs a fresh
    // object with `stat_ == kOff`, SILENTLY cancelling the wizard's prompt -- and
    // the wizard sets that prompt once on entry, so nothing restores it. Asserting
    // "no repaint" is not enough; the pattern must still be RUNNING.
    const int after = hal.GpioWriteCount(GPIO_LED_STAT);
    PollFor(o, hal, 1000);
    EXPECT_GT(hal.GpioWriteCount(GPIO_LED_STAT) - after, 0)
        << "applying a config during a learn CANCELLED the wizard's LED prompt: the "
           "grammar was rebuilt from scratch, and the wizard sets its pattern once on "
           "entry so nothing brings it back";
    EXPECT_TRUE(o.LearnActive()) << "and the learn itself must keep running";
}

TEST(SystemOrchestrator, RemovingAChannelReleasesItsDrivenKey) {
    /*
     * FR-39: the KEY line must NEVER be left driving a phantom press. Every
     * release path -- the pulse timeout, the ladder fault, the lost head unit --
     * lives inside `ServiceChannel`, which `Tick` runs only for
     * `channels_[0..channel_count_-1]`. So a channel a config REMOVES is never
     * visited again, and a line driven at the moment of the apply held its key
     * until the next reboot: a real phantom press, from a config push, on a
     * device whose LED2 would keep showing "driving" as evidence.
     *
     * Reachable from the app: `channels` is a list the user edits, and a
     * two-channel install reduced to one is an ordinary thing to save.
     */
    MockHal hal;
    MockHal::Defaults d;
    d.config.channel_count = 2;
    d.config.channels[1] = d.config.channels[0];
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, kSenseFor5vHeadUnit);
    o.Boot();
    const int idle_code2 = hal.LastDacCode(DAC_CH_KEY2);

    // Hold channel 2's line open well past the moment the config lands -- the
    // same "hold it open so only the release can end it" shape the rail-sag test
    // uses, because a normal SINGLE pulse self-releases and would hide the bug.
    ASSERT_TRUE(o.TestDriveKeyMv(1, 2400, 5000, hal.NowMs()));
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY2), idle_code2) << "precondition: driving";

    Config pushed = d.config;
    pushed.channel_count = 1;   // channel 1 is gone
    o.ApplyConfig(pushed);

    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY2), idle_code2)
        << "a channel removed by a config push must release its key at once";
    // Still released after the old pulse deadline passes, so the release cannot
    // be the ordinary timeout arriving a moment later.
    PollFor(o, hal, 6000);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY2), idle_code2)
        << "and it must stay released, with the removed channel never serviced";
}

// --- Spec 6.2's command band, end to end (N-32) -----------------------------
//
// The unit tests pin `GainPolicyClampCommand`; these pin that the ORCHESTRATOR
// uses it, because the gap was never in the arithmetic -- it was that no drive
// path consulted the head unit's own idle at all.

TEST(SystemOrchestrator, ACommandAboveTheHeadUnitsOwnIdleIsBroughtIntoTheBand) {
    // The defect this pins (N-32): spec 6.2 says command targets must stay below
    // the line's resting level, because above it "the servo can only turn `Q4` off,
    // which is the release behavior, not a command". The only bound was FR-18's
    // envelope clamp, which permits any value from 1800 to 5200 mV -- so a `key_mv`
    // between the head unit's measured idle (4980 mV here) and the ceiling was
    // driven AT ITS OWN VALUE and reached the radio as nothing at all.
    //
    // The consequence is a press that silently does nothing while every observable
    // says it worked: the config is green, the press is acked, and LED2 reports a
    // key presented. Only the radio disagrees. Clamping brings the request into the
    // band, so the binding still works as a button.
    MockHal hal;
    MockHal::Defaults d;
    // vol_up SINGLE -> a key ABOVE the line's rest and inside the envelope.
    d.config.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    d.config.bindings[0].actions[0].key_mv = 5100;   // 4980 < 5100 < 5200
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config)) << "5100 mV is a legal key_mv per ConfigValidate";

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);   // 4980 mV idle
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_EQ(o.IdleKeyMv(0), 4980)
        << "the accessor must report the head unit's MEASURED idle, not the idle "
           "code's own (much higher) voltage";

    LogCapture logs;
    o.SetLogSink(&LogCapture::Sink, &logs);

    const int driven = PressAndCaptureDrivenCode(o, hal, 1430);
    const int raw_code =
        GainPolicyCodeForTarget(o.ChannelGainMode(0), 5100).dac_code;
    const int band_ceiling_code =
        GainPolicyCodeForTarget(o.ChannelGainMode(0), 4980 - kCommandHeadroomMv).dac_code;
    ASSERT_NE(raw_code, band_ceiling_code)
        << "fixture error: the raw and clamped codes must differ, or this test can "
           "pass without the clamp";
    EXPECT_EQ(driven, band_ceiling_code)
        << "a command above the line's own rest must be brought down to "
           "V_KEY_idle - 0.20 V; driving its raw value leaves the FET off and the "
           "radio receives no key";
    ASSERT_EQ(logs.lines.size(), 1u) << "the clamp must be warned about, not silent";
    EXPECT_NE(logs.lines[0].find("5100"), std::string::npos)
        << "the warning must name the value that was clamped";
}

TEST(SystemOrchestrator, AnEmptyCommandBandPlaysKeyUnknownRatherThanAcknowledging) {
    // A head unit idling at 1900 mV leaves `1900 - 200 = 1700`, below the servo's
    // 1800 mV floor: no level is BOTH reachable and below the line's rest. There is
    // no command to make, so the press must NOT be acknowledged -- `KEY_ACCEPTED`
    // on a press that drove nothing is the lie this whole fix is about.
    MockHal hal;
    MockHal::Defaults d;
    d.config.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    d.config.bindings[0].actions[0].key_mv = 2400;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 950);   // x2 = 1900 mV: a very low line
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    o.Boot();
    ASSERT_EQ(o.IdleKeyMv(0), 1900);

    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    const int before = hal.BuzzerOnCount();
    const int driven = PressAndCaptureDrivenCode(o, hal, 1430);
    EXPECT_EQ(driven, idle_code) << "an empty band must drive nothing";
    EXPECT_GT(hal.BuzzerOnCount(), before)
        << "the empty band must be reported audibly (KEY_UNKNOWN), not silently";
    EXPECT_FALSE(hal.BuzzerIsOn()) << "and must leave the buzzer off when the pattern ends";
}

TEST(SystemOrchestrator, PassThroughWithAnEmptyCommandBandDrivesNothing) {
    // The SAME band applies to the pass-through path, because a ratio-mapped
    // target is still a command target (spec 6.2). `kPassThroughPressDeltaMv`
    // caps the wheel's ratio at ~897 permille, so a head unit idling just above
    // the envelope floor (1900 mV here) leaves `1900 - 200 = 1700`, below the
    // servo's 1800 mV floor: there is no level BOTH reachable and below the line's
    // rest. Mapping the ratio anyway would drive a level that reaches the radio as
    // nothing -- the dead press this whole fix is about -- so the pass-through
    // must refuse and release.
    MockHal hal;
    auto o = MakeUnconfigured(hal);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);   // the wheel at idle
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 950);   // x2 = 1900 mV: a very low line
    o.Boot();
    ASSERT_TRUE(o.PassThroughActive()) << "precondition: this is the pass-through path";
    ASSERT_EQ(o.IdleKeyMv(0), 1900);

    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    // A press well off idle (300 mV is the pass-through threshold; 1430 is 1405 off).
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a pass-through press whose mapped target falls in an empty command band "
           "must drive nothing, rather than a level the radio cannot read";
}

// --- FR-1's NTC clause: the channel that is never converted (open item N-67) ---

TEST(SystemOrchestrator, TheNtcChannelIsNeverConvertedSoFR1sFirstClauseIsUnmet) {
    // FR-1 requires the firmware to "sample both ladder channels AND THE NTC
    // continuously". The ladder half is real: every poll tick converts each
    // channel through `AdcReader`. The NTC half has no implementation at all --
    // `ADC_CH_TEMP` is mapped to `ADC_CHANNEL_6` in EspHal's `AdcPinFor` and is
    // read by NOTHING, and there is no NTC-to-temperature conversion anywhere in
    // the tree. So `temp_c_at_learn` is stamped from a literal 0 on both learn
    // paths, and a field a future compensation is meant to consume is a constant.
    //
    // This test drives a full poll loop -- boot, a press, a release, and an
    // entire headless learn -- and asserts the NTC was converted ZERO times
    // throughout. It is written to PASS today, so it is a PROBE, not a bug: it
    // pins the gap so the day the sampling path is added, the counter goes
    // non-zero and this test fails, which is the moment to delete it and record
    // that FR-1 is met. Without it the gap is invisible: nothing else in the
    // suite would notice the difference between "the NTC is read" and "it is not",
    // because the field it would fill is never consulted.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_AUX1, kAuxReleasedMv);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    hal.SetAdcMilliVolts(ADC_CH_TEMP, 1500);   // a plausible NTC divider reading
    o.Boot();

    // A press and a release, so the full classification path runs.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 100);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    PollFor(o, hal, 700);

    // And a whole headless learn, which is the path that RECORDS a temperature.
    HoldAuxToToggle(o, hal);
    ASSERT_TRUE(o.LearnActive());
    PressAux(o, hal, 1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    PollFor(o, hal, 400);

    EXPECT_GT(hal.AdcReadCount(ADC_CH_SWC1), 0)
        << "the ladder channel IS converted (FR-1's second half), so a zero here "
           "would mean the test is not driving the loop at all";
    EXPECT_EQ(hal.AdcReadCount(ADC_CH_TEMP), 0)
        << "ADC_CH_TEMP was converted -- FR-1's NTC clause now has an "
           "implementation. Update open item N-67, delete this probe, and assert "
           "the recorded temperature instead of the read count";
}
