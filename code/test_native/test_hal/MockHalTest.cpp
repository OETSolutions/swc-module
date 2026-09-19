#include "MockHAL.h"
#include <gtest/gtest.h>

TEST(MockHalClock, StartsAtZeroAndAdvancesInMilliseconds) {
    MockHal hal;
    EXPECT_EQ(hal.NowMs(), 0u);
    hal.AdvanceMs(750);
    EXPECT_EQ(hal.NowMs(), 750u);
    EXPECT_EQ(hal.NowUs(), 750000u);
}

TEST(MockHalAdc, ReturnsTheProgrammedMillivoltValuePerChannel) {
    MockHal hal;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1234);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 567);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_SWC1), 1234);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_SWC2), 567);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_TEMP), 0);
}

TEST(MockHalDac, RecordsTheLastCodeWrittenPerChannel) {
    MockHal hal;
    hal.DacSetCode(DAC_CH_KEY1, 0x800);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x800);
    hal.DacSetCode(DAC_CH_KEY1, 0x123);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x123);
    // Power mode is a separate call: selecting a gain mode is a power-mode
    // change (spec 2.3) and is not coupled to any code write.
    hal.DacPowerMode(DAC_CH_ADJ1, DAC_POWER_GND_1K);
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K);
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_KEY1), DAC_POWER_NORMAL);
}

TEST(MockHalBuzzer, IsAnOnOffLineNotAGpioPin) {
    MockHal hal;
    EXPECT_FALSE(hal.BuzzerIsOn());
    EXPECT_EQ(hal.BuzzerOnCount(), 0);
    hal.BuzzerOn(true);
    EXPECT_TRUE(hal.BuzzerIsOn());
    EXPECT_EQ(hal.BuzzerOnCount(), 1);

    // The buzzer has its own frozen member (spec 10.2) and is NOT a GpioPin, so
    // driving it must not move any pin counter. If GPIO_BUZZ is ever added back
    // to the enum there are then two routes to one physical line, and this is
    // the assertion that catches it.
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 0)
        << "the buzzer must not be driven through gpio_write";
    EXPECT_FALSE(hal.LastLdac()) << "nothing here touched ~LDAC";
}

TEST(MockHalGpio, ReadsBackWhatWasWrittenAndTracksWriteCount) {
    MockHal hal;
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 0);
    hal.GpioWrite(GPIO_LED_STAT, true);
    EXPECT_TRUE(hal.GpioRead(GPIO_LED_STAT));
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 1);
}

TEST(MockHalNvs, PersistsBytesAcrossCallsAndCanBeMadeToFail) {
    MockHal hal;
    const char payload[] = "config-blob";
    ASSERT_EQ(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
    char out[sizeof(payload)] = {};
    // NvsGet returns the number of bytes copied, not 0.
    ASSERT_EQ(hal.NvsGet("cfg", out, sizeof(out)),
              static_cast<int>(sizeof(payload)));
    EXPECT_STREQ(out, payload);

    hal.FailNextNvsWrite();
    EXPECT_NE(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
}

TEST(MockHalNvs, ATornWriteLeavesTheStoredBlobShortAndReportsFailure) {
    MockHal hal;
    const char payload[] = "config-blob";
    hal.TruncateNextNvsWriteAt(5);
    EXPECT_NE(hal.NvsSet("cfg", payload, sizeof(payload)), 0)
        << "a torn write must report failure, not success";
    char out[sizeof(payload)] = {};
    EXPECT_EQ(hal.NvsGet("cfg", out, sizeof(out)), 5)
        << "readers must see the short blob and reject it on length + CRC";
}

TEST(MockHalNvs, CorruptingOneBitIsVisibleToTheReader) {
    MockHal hal;
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
    hal.CorruptNvsValue("cfg", 2);
    uint8_t out[sizeof(payload)] = {};
    ASSERT_EQ(hal.NvsGet("cfg", out, sizeof(out)),
              static_cast<int>(sizeof(payload)));
    EXPECT_EQ(out[2], 0x02) << "bit 0 of byte 2 must have flipped";
    EXPECT_NE(std::memcmp(out, payload, sizeof(payload)), 0);
}

TEST(MockHalInputs, OnlyBootAndVbusReadBackAsProgrammedInputs) {
    MockHal hal;
    // The two genuinely digital inputs.
    hal.SetGpioInput(GPIO_BOOT, true);
    hal.SetGpioInput(GPIO_VBUS_VALID, true);
    EXPECT_TRUE(hal.GpioRead(GPIO_BOOT));
    EXPECT_TRUE(hal.GpioRead(GPIO_VBUS_VALID));

    // A sense line is an ADC channel (spec 2.2), so it is NOT readable here.
    // This test exists to make that explicit: if SENSE ever reappears as a
    // GpioPin the servo trim loop cannot read it.
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2500);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_KEY_SENSE1), 2500);
}
