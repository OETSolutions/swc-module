#include "System/SystemOrchestrator.h"

#include <gtest/gtest.h>

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
