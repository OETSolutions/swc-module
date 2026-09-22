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
    EXPECT_TRUE(DacFrame::SelectForChannel(static_cast<uint8_t>(DAC_CH_KEY1), &sel));
    EXPECT_EQ(sel, DacFrame::kChannelA);
    EXPECT_TRUE(DacFrame::SelectForChannel(static_cast<uint8_t>(DAC_CH_ADJ1), &sel));
    EXPECT_EQ(sel, DacFrame::kChannelB);
    EXPECT_TRUE(DacFrame::SelectForChannel(static_cast<uint8_t>(DAC_CH_KEY2), &sel));
    EXPECT_EQ(sel, DacFrame::kChannelC);
    EXPECT_TRUE(DacFrame::SelectForChannel(static_cast<uint8_t>(DAC_CH_ADJ2), &sel));
    EXPECT_EQ(sel, DacFrame::kChannelD);
    // An out-of-range value must be refused so the caller drops the write
    // rather than addressing a garbage channel. 99 is passed as its ORDINAL: a
    // `DacChannel` holding 99 is a value outside the enumerator range, and loading
    // it in the switch is undefined behaviour (UBSan: "load of value 99, which is
    // not a valid value for type 'DacChannel'"), so this defensive case could not
    // be tested without executing UB before the parameter became an integer.
    EXPECT_FALSE(DacFrame::SelectForChannel(99, &sel));
    EXPECT_FALSE(DacFrame::SelectForChannel(static_cast<uint8_t>(DAC_CH_COUNT), &sel))
        << "the sentinel past the last channel must be refused too";
}

/*
 * The READ side: FR-13's step 3b, "VERIFY the DAC is in the safe state (read
 * back)". The byte layout is the Read Command's (DS22187E Figure 5-15, §5.6.5),
 * confirmed against Adafruit's MCP4728 driver, which reads 24 bytes and takes
 * each channel's code from `buf[6n+2] | ((buf[6n+1] & 0x0F) << 8)`.
 *
 * The defect these catch is "no read-back at all" (open item N-21): EspHal called
 * no receive of any kind, so FR-13's verify step was unimplemented and FR-37's
 * health gate had no signal that could say NO.
 */

TEST(DacFrame, DecodesAChannelFromItsInputRegisterAtTheRightOffset) {
    uint8_t buf[DacFrame::kReadBytes] = {};
    // Channel A (offset 0) carries 0xABC; channel C (offset 12) carries 0x123.
    // Distinct values at distinct offsets, so an off-by-one base or a wrong
    // channel-select multiplier fails rather than coinciding.
    buf[0] = 0x00;          // status byte -- deliberately NOT consulted
    buf[1] = 0x0A;          // high nibble of 0xABC
    buf[2] = 0xBC;          // low byte of 0xABC
    buf[12] = 0x00;
    buf[13] = 0x01;
    buf[14] = 0x23;

    uint16_t code = 0;
    ASSERT_TRUE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelA, &code));
    EXPECT_EQ(code, 0x0ABC);
    ASSERT_TRUE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelC, &code));
    EXPECT_EQ(code, 0x0123)
        << "each channel sits at 6-byte strides from A; channel C is 12 bytes in";
    // The middle two are zero-filled here, so they decode to 0 rather than
    // borrowing a neighbour's bytes.
    ASSERT_TRUE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelB, &code));
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelD, &code));
    EXPECT_EQ(code, 0);
}

TEST(DacFrame, TheStatusByteAndTheTopNibbleAreNotPartOfTheCode) {
    // Byte 6n carries RDY/POR/BSY and the address; byte 6n+1 carries VREF PD1 PD0
    // Gx in its TOP nibble and only D11:D8 in its bottom one. A decoder that read
    // the whole of byte 6n+1 would fold the power-down mode into the code and
    // report a mismatch for a perfectly good write.
    uint8_t buf[DacFrame::kReadBytes] = {};
    buf[0] = 0xE7;          // status/address bits -- must not leak into the code
    buf[1] = 0xF0;          // VREF=1, PD=11, Gx=1 -- none of it is D11:D8
    buf[2] = 0x00;
    uint16_t code = 0xFFFF;
    ASSERT_TRUE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelA, &code));
    EXPECT_EQ(code, 0) << "only byte 6n+1's LOW nibble is D11:D8";
}

TEST(DacFrame, RoundTripsEveryCodeThroughTheEncoderAndTheDecoder) {
    // The property that makes the verification meaningful: what EncodeSet puts on
    // the wire is what DecodeReadCode gets back. 0, 1, the low-byte boundary, the
    // 12-bit ceiling and two interior values -- the boundaries are where a mask
    // error shows.
    const uint16_t codes[] = {0, 1, 0x00FF, 0x0100, 0x0ABC, 0x0FFF};
    for (DacChannel ch : {DAC_CH_KEY1, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2}) {
        for (uint16_t code : codes) {
            uint8_t frame[DacFrame::kSize];
            uint8_t sel = 0;
            ASSERT_TRUE(DacFrame::SelectForChannel(static_cast<uint8_t>(ch), &sel));
            DacFrame::EncodeSet(frame, sel, 0, 0, 0, code);

            // A write frame is 3 bytes; the read response is 6 per channel with
            // the SAME two data bytes, so splice the write frame into the read
            // buffer at its channel's offset -- status byte first, exactly as the
            // part would return it.
            uint8_t buf[DacFrame::kReadBytes] = {};
            const size_t base = 6u * sel;
            buf[base + 0] = 0x00;
            buf[base + 1] = frame[1];
            buf[base + 2] = frame[2];

            uint16_t decoded = 0;
            ASSERT_TRUE(DacFrame::DecodeReadCode(buf, sel, &decoded));
            EXPECT_EQ(decoded, code)
                << "channel " << static_cast<int>(ch) << ", code " << code;
        }
    }
}

TEST(DacFrame, RefusesANullBufferOrOutputAndANonChannel) {
    uint8_t buf[DacFrame::kReadBytes] = {};
    uint16_t code = 0;
    EXPECT_FALSE(DacFrame::DecodeReadCode(nullptr, DacFrame::kChannelA, &code));
    EXPECT_FALSE(DacFrame::DecodeReadCode(buf, DacFrame::kChannelA, nullptr));
    // A value past the last output is refused rather than indexed: it would read
    // past the 24-byte response. Passed as an ORDINAL, so the guard is testable
    // without forming an out-of-range DacChannel (see SelectForChannel's note).
    EXPECT_FALSE(DacFrame::DecodeReadCode(buf, 99, &code));
    EXPECT_FALSE(DacFrame::DecodeReadCode(buf, static_cast<uint8_t>(DAC_CH_COUNT), &code));
}
