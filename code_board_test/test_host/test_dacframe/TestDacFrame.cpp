// Host tests for the MCP4728 frame encoding and decoding.
//
// These run on the development machine (env:host), with no board attached. They
// exist because the on-device I2C path cannot be tested without silicon, and a
// wrong byte layout there fails SILENTLY: the production firmware shipped a
// four-byte frame, the device never addressed a channel, and nothing reported an
// error. The only place that defect is catchable is here.
//
// Every assertion cites the datasheet figure it pins.

#include <unity.h>

#include "swc_logic/DacFrame.h"
#include "swc_logic/Output.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Multi-Write, byte 0: `0 1 0 0 0 DAC1 DAC0 UDAC` (DS22187E 5.6.2, Fig 5-8)
// ---------------------------------------------------------------------------
static void test_multiwrite_byte0_command_bits(void)
{
    uint8_t f[3];
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0x000);
    // C2:C1:C0 = 010 in bits 7:5, channel A = 00, UDAC = 0.
    TEST_ASSERT_EQUAL_HEX8(0x40, f[0]);
}

static void test_multiwrite_byte0_channel_select(void)
{
    uint8_t f[3];
    // Channel bits are DAC1:DAC0 at bits 2:1. A=00 B=01 C=10 D=11.
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0);
    TEST_ASSERT_EQUAL_HEX8(0x40, f[0]);
    DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kNormal, 0);
    TEST_ASSERT_EQUAL_HEX8(0x42, f[0]);
    DacFrame::EncodeSet(f, DacFrame::kChannelC, DacFrame::kNormal, 0);
    TEST_ASSERT_EQUAL_HEX8(0x44, f[0]);
    DacFrame::EncodeSet(f, DacFrame::kChannelD, DacFrame::kNormal, 0);
    TEST_ASSERT_EQUAL_HEX8(0x46, f[0]);
}

static void test_multiwrite_udac_is_clear(void)
{
    // UDAC is bit 0. It must be 0 so the addressed output latches on this frame's
    // own final ACK and no ~LDAC pulse is needed. The mask is named at the
    // definition because an earlier revision used bit 3, which is not UDAC at all
    // and made an "assert UDAC is clear" test pass vacuously.
    TEST_ASSERT_EQUAL_HEX8(0x01, DacFrame::kUdacHold);
    uint8_t f[3];
    for (uint8_t ch = 0; ch <= 3; ++ch) {
        DacFrame::EncodeSet(f, ch, DacFrame::kNormal, 0x0FFF);
        TEST_ASSERT_EQUAL_HEX8(0, (f[0] & DacFrame::kUdacHold));
    }
}

// ---------------------------------------------------------------------------
// Multi-Write, byte 1: `VREF PD1 PD0 Gx D11 D10 D9 D8`
// ---------------------------------------------------------------------------
static void test_multiwrite_byte1_powerdown_field(void)
{
    uint8_t f[3];
    // PD1:PD0 live at bits 6:5. The 1k mode (code 01) is the 5 V-range gain
    // selector -- the single most consequential two bits in this codebase.
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[1] & 0x60);
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kGnd1k, 0);
    TEST_ASSERT_EQUAL_HEX8(0x20, f[1] & 0x60);
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kGnd100k, 0);
    TEST_ASSERT_EQUAL_HEX8(0x40, f[1] & 0x60);
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kGnd500k, 0);
    TEST_ASSERT_EQUAL_HEX8(0x60, f[1] & 0x60);
}

static void test_multiwrite_byte1_vref_and_gain_are_clear(void)
{
    // VREF = 0 (VDD reference: U4 runs on +3V3, so the 4.096 V internal mode is
    // invalid) and Gx = 0 (x1; the x2 gain needs the internal reference).
    uint8_t f[3];
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0x0FFF);
    TEST_ASSERT_EQUAL_HEX8(0, f[1] & 0x80);  // VREF
    TEST_ASSERT_EQUAL_HEX8(0, f[1] & 0x10);  // Gx
}

static void test_multiwrite_byte1_carries_high_nibble(void)
{
    uint8_t f[3];
    // Code 0xABC -> high nibble 0xA in byte 1's low bits, low byte 0xBC in byte 2.
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0xABC);
    TEST_ASSERT_EQUAL_HEX8(0x0A, f[1] & 0x0F);
    TEST_ASSERT_EQUAL_HEX8(0xBC, f[2]);
}

static void test_multiwrite_is_exactly_three_bytes(void)
{
    // The whole point. The MCP4725's frame is four bytes with the command in its
    // own byte; sending that to an MCP4728 shifts every field by one and addresses
    // nothing. Asserting the size is what makes the encoder impossible to
    // reintroduce at four.
    TEST_ASSERT_EQUAL_size_t(3, DacFrame::kSetSize);
}

static void test_multiwrite_code_is_masked_to_12_bits(void)
{
    uint8_t f[3];
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 0xFFFF);
    // 0xFFFF & 0x0FFF = 0x0FFF -> byte1 low nibble F, byte2 FF.
    TEST_ASSERT_EQUAL_HEX8(0x0F, f[1] & 0x0F);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f[2]);
}

static void test_multiwrite_full_scale_encoding(void)
{
    // Code 4095 (full scale) is what the release state uses. Byte for channel A,
    // normal mode: 0x40, 0x0F, 0xFF.
    uint8_t f[3];
    DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 4095);
    TEST_ASSERT_EQUAL_HEX8(0x40, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f[2]);
}

// ---------------------------------------------------------------------------
// Read response decoding.
//
// The read layout DIFFERS from the write layout: in a read response each channel
// is two bytes, with the code in the HIGH nibble of byte 0 and the config fields
// in its low nibble. In the write frame byte 1 it is the other way round. Getting
// that backwards produces a plausible-looking but wrong code, so this is
// round-tripped rather than asserted in isolation.
// ---------------------------------------------------------------------------
static void test_read_decode_field_positions(void)
{
    // byte0 = code-high-nibble(4) | PD1:PD0(2) | Gx(1) | VREF(1)
    // 0xA8 = 1010 10 0 0 -> code high 0xA, PD=10(100k), gain 0, vref 0
    const DacFrame::ChannelReg r = DacFrame::DecodeChannel(0xA8, 0x5C);
    TEST_ASSERT_EQUAL_HEX16(0xA5C, r.code);
    TEST_ASSERT_EQUAL_UINT8(DacFrame::kGnd100k, r.power_mode);
    TEST_ASSERT_EQUAL_UINT8(0, r.gain);
    TEST_ASSERT_EQUAL_UINT8(0, r.vref);
}

static void test_read_decode_vref_and_gain(void)
{
    // 0xAB = 1010 10 1 1 -> gain 1, vref 1
    const DacFrame::ChannelReg r = DacFrame::DecodeChannel(0xAB, 0x00);
    TEST_ASSERT_EQUAL_UINT8(1, r.gain);
    TEST_ASSERT_EQUAL_UINT8(1, r.vref);
    TEST_ASSERT_EQUAL_UINT8(DacFrame::kGnd100k, r.power_mode);
}

static void test_read_decode_roundtrips_a_written_frame(void)
{
    // Encode a normal-mode write, then build the read slot the device would return
    // for it, and check the decoder recovers the same code and power mode.
    for (uint16_t code = 0; code <= 4095; code += 137) {
        uint8_t f[3];
        DacFrame::EncodeSet(f, DacFrame::kChannelC, DacFrame::kGnd1k, code);

        // The write frame's byte1 low nibble is the code's high nibble; the read
        // slot puts it back in the high nibble and the config in the low.
        const uint8_t cfg = (uint8_t)((f[1] >> 5) & 0x03);  // PD1:PD0
        const uint8_t slot0 = (uint8_t)(((f[1] & 0x0F) << 4) | (cfg << 2));
        const DacFrame::ChannelReg r = DacFrame::DecodeChannel(slot0, f[2]);

        TEST_ASSERT_EQUAL_HEX16(code, r.code);
        TEST_ASSERT_EQUAL_UINT8(cfg, r.power_mode);
    }
}

static void test_read_channel_offsets_are_two_bytes_apart(void)
{
    TEST_ASSERT_EQUAL_size_t(0, DacFrame::ChannelOffset(DacFrame::kChannelA));
    TEST_ASSERT_EQUAL_size_t(2, DacFrame::ChannelOffset(DacFrame::kChannelB));
    TEST_ASSERT_EQUAL_size_t(4, DacFrame::ChannelOffset(DacFrame::kChannelC));
    TEST_ASSERT_EQUAL_size_t(6, DacFrame::ChannelOffset(DacFrame::kChannelD));
}

static void test_read_lengths(void)
{
    TEST_ASSERT_EQUAL_size_t(8, DacFrame::kReadDacBytes);
    TEST_ASSERT_EQUAL_size_t(24, DacFrame::kReadAllBytes);
    // The read commands are distinct; conflating them returns the wrong length.
    TEST_ASSERT_TRUE(DacFrame::kReadCmdDac != DacFrame::kReadCmdAll);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_multiwrite_byte0_command_bits);
    RUN_TEST(test_multiwrite_byte0_channel_select);
    RUN_TEST(test_multiwrite_udac_is_clear);
    RUN_TEST(test_multiwrite_byte1_powerdown_field);
    RUN_TEST(test_multiwrite_byte1_vref_and_gain_are_clear);
    RUN_TEST(test_multiwrite_byte1_carries_high_nibble);
    RUN_TEST(test_multiwrite_is_exactly_three_bytes);
    RUN_TEST(test_multiwrite_code_is_masked_to_12_bits);
    RUN_TEST(test_multiwrite_full_scale_encoding);
    RUN_TEST(test_read_decode_field_positions);
    RUN_TEST(test_read_decode_vref_and_gain);
    RUN_TEST(test_read_decode_roundtrips_a_written_frame);
    RUN_TEST(test_read_channel_offsets_are_two_bytes_apart);
    RUN_TEST(test_read_lengths);
    return UNITY_END();
}
