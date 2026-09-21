#include <gtest/gtest.h>

#include "HAL/DacFrame.h"

/*
 * The MCP4728 Multi-Write frame, byte for byte, against datasheet DS22187E
 * Figure 5-8. These assertions exist because the frame used to be built inline
 * in EspHal.cpp, which the host build EXCLUDES -- so the whole host suite ran
 * against logic that never saw the bytes that reach the DAC. Every expectation
 * below is a value read off the datasheet figure, not off the implementation.
 *
 * The bug these catch: the old encoder emitted FOUR bytes with the command in
 * its own byte (an MCP4725-shaped frame). On the wire the device read byte 0 as
 * the command-and-channel byte -- so it saw command 0x40 WITH channel select
 * 00 (channel A) regardless of the target -- and never addressed B/C/D. No
 * channel was ever written.
 */

// Byte 0 is `0100 0 DAC1 DAC0 UDAC`: the command type bits, the channel select
// and UDAC all share ONE byte. This is the property the old frame broke.
TEST(DacFrame, PacksCommandAndChannelSelectIntoTheFirstByte) {
    uint8_t f[DacFrame::kSize];
    const uint8_t channels[4] = {DacFrame::kChannelA, DacFrame::kChannelB,
                                 DacFrame::kChannelC, DacFrame::kChannelD};
    const uint8_t expected[4] = {0x40, 0x42, 0x44, 0x46};
    for (int i = 0; i < 4; ++i) {
        DacFrame::EncodeSet(f, channels[i], 0, 0, 0, 0);
        EXPECT_EQ(f[0], expected[i])
            << "byte 0 must be command 0x40 with channel select " << i
            << " ORed in; a bare 0x40 means every write addresses channel A";
        EXPECT_EQ(f[0] & DacFrame::kUdacHold, 0)
            << "UDAC must be clear so the write latches on its own ACK";
    }
    // UDAC is byte 0 BIT 0 (`0 1 0 0 0 DAC1 DAC0 UDAC` -- the diagram above and
    // the datasheet figure). Pinning the bit POSITION, not just "the mask bit is
    // clear", is what makes the loop above non-vacuous: `kUdacHold` used to be
    // 0x08, which is bit 3, so the mask named a bit the encoder can never set and
    // the assertion passed no matter what byte 0 was.
    EXPECT_EQ(DacFrame::kUdacHold, 0x01u)
        << "the UDAC mask must name bit 0; any other bit makes the clear-check "
           "above a tautology";
}

// The frame is THREE bytes. Four was the defect: an extra byte pushed data into
// the next transaction's command position.
TEST(DacFrame, IsExactlyThreeBytesPerTheDatasheetFigure) {
    EXPECT_EQ(DacFrame::kSize, 3u);
}

// Byte 1 is `VREF PD1 PD0 Gx D11 D10 D9 D8`. Pin the power-down bits at 6:5 and
// the gain bit at 4 -- the old code put PD at 5:4 of a separate byte.
TEST(DacFrame, PutsPowerDownAtBitsSixToFiveAndGainAtBitFour) {
    uint8_t f[DacFrame::kSize];
    DacFrame::EncodeSet(f, DacFrame::kChannelB, 0, 1 /* 1k pull-down */, 0, 0);
    EXPECT_EQ(f[1], 0x20)
        << "PD1:PD0 = 01 sits at bits 6:5 of byte 1";  // 0010 0000
    EXPECT_EQ(f[1] & 0x10, 0) << "Gx (gain) must be 0: VREF = VDD, not internal";

    DacFrame::EncodeSet(f, DacFrame::kChannelB, 0, 2 /* 100k */, 0, 0);
    EXPECT_EQ(f[1], 0x40);
    DacFrame::EncodeSet(f, DacFrame::kChannelB, 0, 3 /* 500k */, 0, 0);
    EXPECT_EQ(f[1], 0x60);
    DacFrame::EncodeSet(f, DacFrame::kChannelB, 0, 0 /* normal */, 0, 0);
    EXPECT_EQ(f[1], 0x00);
}

TEST(DacFrame, SplitsTheTwelveBitCodeAcrossBytesOneAndTwo) {
    uint8_t f[DacFrame::kSize];
    // 0xABC = 1010 1011 1100 -> high nibble 0xA in byte 1, low byte 0xBC.
    DacFrame::EncodeSet(f, DacFrame::kChannelC, 0, 0, 0, 0xABC);
    EXPECT_EQ(f[1] & 0x0F, 0x0A) << "byte 1's low nibble is D11:D8";
    EXPECT_EQ(f[2], 0xBC) << "byte 2 is D7:D0";
    EXPECT_EQ(f[0], 0x44);
}

TEST(DacFrame, ClampsACodeAboveTwelveBits) {
    uint8_t f[DacFrame::kSize];
    DacFrame::EncodeSet(f, DacFrame::kChannelA, 0, 0, 0, 0xFFFF);
    EXPECT_EQ(f[1] & 0x0F, 0x0F);
    EXPECT_EQ(f[2], 0xFF);
}

// The exact frame for the gain-mode switch: channel B (V_ADJ1) into its 1 kohm
// power-down, code 0. This is the datasheet figure's own worked example shape.
TEST(DacFrame, MatchesTheDatasheetWorkedExampleForTheOneKiloOhmPulldown) {
    uint8_t f[DacFrame::kSize];
    DacFrame::EncodeSet(f, DacFrame::kChannelB, 0, 1, 0, 0);
    EXPECT_EQ(f[0], 0x42);
    EXPECT_EQ(f[1], 0x20);
    EXPECT_EQ(f[2], 0x00);
}

TEST(DacFrame, PowerDownCodeMapsEveryHalMode) {
    EXPECT_EQ(DacFrame::PowerDownCode(DAC_POWER_NORMAL), 0);
    EXPECT_EQ(DacFrame::PowerDownCode(DAC_POWER_GND_1K), 1);
    EXPECT_EQ(DacFrame::PowerDownCode(DAC_POWER_GND_100K), 2);
    EXPECT_EQ(DacFrame::PowerDownCode(DAC_POWER_GND_500K), 3);
}

TEST(DacFrame, MapsEachHalChannelToItsOutputAndRejectsTheRest) {
    uint8_t sel = 0xFF;
    EXPECT_TRUE(DacFrame::SelectForChannel(DAC_CH_KEY1, &sel));
    EXPECT_EQ(sel, DacFrame::kChannelA);
    EXPECT_TRUE(DacFrame::SelectForChannel(DAC_CH_ADJ1, &sel));
    EXPECT_EQ(sel, DacFrame::kChannelB);
    EXPECT_TRUE(DacFrame::SelectForChannel(DAC_CH_KEY2, &sel));
    EXPECT_EQ(sel, DacFrame::kChannelC);
    EXPECT_TRUE(DacFrame::SelectForChannel(DAC_CH_ADJ2, &sel));
    EXPECT_EQ(sel, DacFrame::kChannelD);
    // An out-of-range value must be refused so the caller drops the write
    // rather than addressing a garbage channel.
    EXPECT_FALSE(DacFrame::SelectForChannel((DacChannel)99, &sel));
}
