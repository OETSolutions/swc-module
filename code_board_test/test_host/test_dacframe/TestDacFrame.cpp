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
    // The code's high nibble is in the LOW nibble of the byte preceding the low
    // byte. Everything else in those bytes is NOT decoded: the power-mode/VREF/gain
    // fields could not be located on the board (writing all four power modes changes
    // no byte in the response), so the decoder reports them UNKNOWN rather than
    // inventing a value.
    // 0xA8: only the LOW nibble (8) is a code bit; the upper nibble is not decoded.
    const DacFrame::ChannelReg r = DacFrame::DecodeChannel(0xA8, 0x5C);
    TEST_ASSERT_EQUAL_HEX16(0x85C, r.code);
    TEST_ASSERT_EQUAL_UINT8(DacFrame::kPowerModeUnknown, r.power_mode);
    TEST_ASSERT_EQUAL_UINT8(DacFrame::kPowerModeUnknown, r.gain);
    TEST_ASSERT_EQUAL_UINT8(DacFrame::kPowerModeUnknown, r.vref);
}

static void test_read_decode_ignores_the_config_bits(void)
{
    // Only the low nibble of the high byte carries code bits; the upper nibble of
    // that byte is not decoded, so changing it must not change the code.
    const DacFrame::ChannelReg a = DacFrame::DecodeChannel(0x08, 0x5C);
    const DacFrame::ChannelReg b = DacFrame::DecodeChannel(0xF8, 0x5C);
    TEST_ASSERT_EQUAL_HEX16(a.code, b.code);
    TEST_ASSERT_EQUAL_HEX16(0x85C, a.code);
}

static void test_read_decode_roundtrips_a_written_code(void)
{
    // Encode a write, build the read bytes the device returns for it, and check the
    // decoder recovers the CODE. The code is all that round-trips: the config fields
    // are not decodable from the response (see DacFrame.h), so they are not asserted.
    for (uint16_t code = 0; code <= 4095; code += 137) {
        uint8_t f[3];
        DacFrame::EncodeSet(f, DacFrame::kChannelC, DacFrame::kGnd1k, code);

        // The write frame's byte 1 low nibble carries the code's high nibble. On the
        // wire the read response puts it in the low nibble of the byte before the low
        // byte (measured), so rebuild exactly that.
        const uint8_t hi = (uint8_t)(f[1] & 0x0F);
        const DacFrame::ChannelReg r = DacFrame::DecodeChannel(hi, f[2]);

        TEST_ASSERT_EQUAL_HEX16(code, r.code);
    }
}

static void test_read_channel_offsets_match_the_measured_layout(void)
{
    // Measured on the board with four distinct markers: A at [2], B at [8], C at
    // [14], D at [20] -- a 6-byte stride from a first offset of 2. NOT the 2-byte
    // stride the datasheet's phrasing suggests, which is what an earlier version of
    // this decoder assumed and why the read-back appeared to fail.
    TEST_ASSERT_EQUAL_size_t(2,  DacFrame::ChannelOffset(DacFrame::kChannelA));
    TEST_ASSERT_EQUAL_size_t(8,  DacFrame::ChannelOffset(DacFrame::kChannelB));
    TEST_ASSERT_EQUAL_size_t(14, DacFrame::ChannelOffset(DacFrame::kChannelC));
    TEST_ASSERT_EQUAL_size_t(20, DacFrame::ChannelOffset(DacFrame::kChannelD));
    TEST_ASSERT_EQUAL_size_t(6, DacFrame::kReadStride);
    // Every offset needs its preceding byte inside the response.
    TEST_ASSERT_TRUE(DacFrame::ChannelOffset(DacFrame::kChannelA) > 0);
    TEST_ASSERT_TRUE(DacFrame::ChannelOffset(DacFrame::kChannelD) < DacFrame::kReadAllBytes);
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
    RUN_TEST(test_read_decode_ignores_the_config_bits);
    RUN_TEST(test_read_decode_roundtrips_a_written_code);
    RUN_TEST(test_read_channel_offsets_match_the_measured_layout);
    RUN_TEST(test_read_lengths);
    return UNITY_END();
}
