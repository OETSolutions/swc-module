#include "Feedback/LedGrammar.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

namespace {
int CountFlashes(MockHal &hal, LedGrammar &g, GpioPin pin, uint32_t total_ms) {
    int on = 0; bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        g.Update(hal.NowMs());
        const bool now = hal.GpioRead(pin);
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(LedGrammar, OffMeansOffOnBothChannels) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.SetStat(LedStatPattern::kOff);
    g.Set2(Led2Pattern::kOff);
    for (int i = 0; i < 200; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_FALSE(hal.GpioRead(GPIO_LED_STAT));
    EXPECT_FALSE(hal.GpioRead(GPIO_LED2));
}

TEST(LedGrammar, DrivingIsContinuousOnLed2NotBlinking) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set2(Led2Pattern::kSolid);
    for (int i = 0; i < 100; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    // "Solid while driving" is the diagnostic (spec 7.3): the user can see the
    // adapter is holding a key, which separates adapter-wrong from radio-ignoring.
    EXPECT_TRUE(hal.GpioRead(GPIO_LED2));
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED2, 1000), 1) << "one rising edge, then held";
}

TEST(LedGrammar, MaintenanceIsADistinctDoubleFlash) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.SetStat(LedStatPattern::kDoubleFlash);
    EXPECT_GE(CountFlashes(hal, g, GPIO_LED_STAT, 3000), 4) << "repeating double flashes";
}

// The burst must be a BURST: a plain 100/100 blink would satisfy the count above
// while being indistinguishable from kBlink, which is the one thing this pattern
// exists not to be. The signature of a burst is two rising edges close together
// followed by a long silence, so the window must be one burst period
// (100+100+100+600 = 900 ms) and no longer -- a longer window catches the start
// of the next burst, and a shorter one sees two flashes from either pattern.
TEST(LedGrammar, DoubleFlashIsABurstNotABlink) {
    const uint32_t kWindowMs = 880;   // just inside one 900 ms burst period

    auto count_rises = [](LedStatPattern p) {
        MockHal hal;
        LedGrammar g(&hal.InterfaceRef(), 3);
        g.SetStat(p);
        int rises = 0;
        bool prev = false;
        for (uint32_t t = 0; t < kWindowMs; t += 5) {
            g.Update(hal.NowMs());
            const bool now = hal.GpioRead(GPIO_LED_STAT);
            if (now && !prev) ++rises;
            prev = now;
            hal.AdvanceMs(5);
        }
        return rises;
    };

    // Burst: 2 flashes per 900 ms. Blink: 5 per 1000 ms -- about 4.4 per 880 ms.
    // The counts differ because the burst has a gap where the blink has a third
    // and fourth pulse.
    EXPECT_EQ(count_rises(LedStatPattern::kDoubleFlash), 2)
        << "two flashes, then a gap -- not an even blink";
    EXPECT_GT(count_rises(LedStatPattern::kBlink), 2)
        << "the blink keeps firing inside the same window";
}

TEST(LedGrammar, TheTwoChannelsHaveIndependentPatterns) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.SetStat(LedStatPattern::kAlternate);
    const int a = CountFlashes(hal, g, GPIO_LED_STAT, 2000);
    const int b = CountFlashes(hal, g, GPIO_LED2, 2000);
    EXPECT_GT(a, 0);
    EXPECT_GT(b, 0);
    // Alternating must genuinely alternate, not just both blink.
    for (int i = 0; i < 400; ++i) {
        g.Update(hal.NowMs());
        EXPECT_FALSE(hal.GpioRead(GPIO_LED_STAT) && hal.GpioRead(GPIO_LED2))
            << "both LEDs on at once is not an alternation";
        hal.AdvanceMs(5);
    }
}

TEST(LedGrammar, LevelZeroSilencesBothChannels) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 0);
    g.SetStat(LedStatPattern::kDoubleFlash);
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED_STAT, 3000), 0);
    g.Set2(Led2Pattern::kSolid);
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED2, 3000), 0);
}

// Spec 7.3's two channels are separate grammars, so setting one must not
// disturb the other. An earlier single flat enum could not express this at all.
TEST(LedGrammar, SettingOneChannelLeavesTheOtherAlone) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set2(Led2Pattern::kSolid);
    for (int i = 0; i < 20; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    ASSERT_TRUE(hal.GpioRead(GPIO_LED2));

    g.SetStat(LedStatPattern::kBlink);
    for (int i = 0; i < 40; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_TRUE(hal.GpioRead(GPIO_LED2)) << "LED2 is still solid while LED_STAT blinks";
}
