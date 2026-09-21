#pragma once

// The MCP4728 wire frames, as pure logic with no Arduino or IDF dependency.
//
// This header is the reason the on-device DAC code has any test coverage at all:
// the encoder is compiled and asserted on the HOST (test/test_logic), while the
// I2C transport that uses it can only run on silicon. A byte layout written
// inline in the I2C path would be checked by nothing.
//
// That is not hypothetical here. The production firmware shipped a FOUR-byte
// frame -- the MCP4725's shape, with the command in its own byte -- and the
// device read the channel-select byte as the command byte, so no channel was ever
// addressed and no output ever changed. Nothing errors; the DAC just does nothing.
// (spec 2.5.1; code/lib/HAL/DacFrame.h records the same history.) The host tests
// in this project pin every byte of both directions against the datasheet.
//
// ---------------------------------------------------------------------------
// Multi-Write (DS22187E 5.6.2, Figure 5-8) -- THREE bytes, single channel:
//
//   byte 0:  0  1  0  0  0  DAC1 DAC0 UDAC
//            C2 C1 C0 -----  channel  latch
//            \_________/      00=A 01=B 10=C 11=D    0 = latch on this frame's ACK
//            command type 010
//
//   byte 1:  VREF  PD1 PD0  Gx  D11 D10 D9 D8
//            0     power-down 0   \_ code high nibble
//            (VDD ref)          (x1)
//
//   byte 2:  D7 D6 D5 D4 D3 D2 D1 D0
//
// This design always sends VREF=0 (VDD reference) and Gx=0 (x1): the DAC runs on
// +3V3 so the 0-3.3 V VDD mode is the one in use, and the x2 gain is only
// meaningful with the internal reference.
//
// UDAC=0 on every write, which is what makes ~LDAC unnecessary: the addressed
// output latches on the frame's own final ACK. ~LDAC (IO48) is therefore never
// pulsed by this tool either -- it is driven HIGH at boot and left there.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace DacFrame {

constexpr uint8_t kChannelA = 0;  // U4.VOUTA -> channel 1 signal
constexpr uint8_t kChannelB = 1;  // U4.VOUTB -> V_ADJ1
constexpr uint8_t kChannelC = 2;  // U4.VOUTC -> channel 2 signal
constexpr uint8_t kChannelD = 3;  // U4.VOUTD -> V_ADJ2

constexpr uint8_t kCommandMultiWrite = 0x40;  // C2:C1:C0 = 010, in bits 7:5
constexpr uint8_t kUdacHold = 0x01;           // byte 0 bit 0
constexpr size_t  kSetSize  = 3;

// Power-down codes, in the PD1:PD0 field of byte 1.
enum PowerMode : uint8_t {
    kNormal = 0,   // 00  normal operation
    kGnd1k  = 1,   // 01  1 kOhm to GND   <- the 5 V-range gain selector
    kGnd100k = 2,  // 10  100 kOhm to GND
    kGnd500k = 3,  // 11  500 kOhm to GND
};

// Encode one Multi-Write frame for one channel. `code` is masked to 12 bits.
inline void EncodeSet(uint8_t out[kSetSize], uint8_t dac_sel, uint8_t power_mode,
                      uint16_t code)
{
    code &= 0x0FFF;
    out[0] = (uint8_t)(kCommandMultiWrite | ((dac_sel & 0x03) << 1));
    out[1] = (uint8_t)(((power_mode & 0x03) << 5) | ((code >> 8) & 0x0F));
    out[2] = (uint8_t)(code & 0xFF);
}

// ---------------------------------------------------------------------------
// Read (DS22187E 5.6.6).
//
// !! THIS LAYOUT IS EMPIRICAL, NOT THE ONE THE DATASHEET LEADS YOU TO EXPECT. !!
//
// The 24-byte "read all" response was mapped ON THE BOARD with four distinct
// patterns written to the four channels (0x111/0x222/0x333/0x444, all normal mode).
// Each marker's position identifies its channel unambiguously, and the result was:
//
//     A(VOUTA) low byte at [2],  high nibble at [1]
//     B(VOUTB) low byte at [8],  high nibble at [7]
//     C(VOUTC) low byte at [14], high nibble at [13]
//     D(VOUTD) low byte at [20], high nibble at [19]
//
// -- i.e. a SIX-byte stride, not the two-byte stride the datasheet's "each channel
// is two bytes" reading implies. The 8-byte read (command 0x08) returns the first
// EIGHT bytes of that same 24-byte response, so for channel A it agrees and for
// B/C/D it does NOT contain them.
//
// WHAT THIS MEANS FOR THE DECODER BELOW, stated plainly rather than hidden:
//   * A channel's CODE is recovered correctly: low byte at `offset`, high nibble in
//     the low nibble of `offset - 1`.
//   * The power-down field could NOT be located. Writing all four PD1:PD0 values to
//     a channel and diffing the response changed NOTHING, so whatever carries the
//     power-mode state is not a byte that moves when that state changes. The
//     power-mode accessors are therefore reported as UNKNOWN and are not asserted
//     against -- a test that cannot observe a field must not claim to have checked
//     it.
//
// The power mode IS verified behaviourally instead, and that is the stronger
// evidence: see the servo tests, where the 1k power-down (V_ADJ = 0, gain 1.82)
// produces the predicted KEY voltage within tens of millivolts.
// ---------------------------------------------------------------------------
constexpr uint8_t kAddrGeneralCall = 0x00;
constexpr uint8_t kReadCmdDac      = 0x08;  // returns the first 8 bytes of the below
constexpr uint8_t kReadCmdAll      = 0x09;  // the full 24-byte response

constexpr size_t kReadDacBytes = 8;
constexpr size_t kReadAllBytes = 24;

// Bytes per channel entry in the read response. 6, measured -- see above.
constexpr size_t kReadStride = 6;


// One channel's decoded entry. `power_mode` is not recoverable from the response;
// kPowerModeUnknown says so rather than returning a plausible-looking zero.
constexpr uint8_t kPowerModeUnknown = 0xFF;

struct ChannelReg {
    uint16_t code;         // D11..D0 -- this IS recovered correctly
    uint8_t  power_mode;   // kPowerModeUnknown: not observable in the response
    uint8_t  vref;         // likewise not observable
    uint8_t  gain;         // likewise not observable
};

// Decode one channel entry. `hi_byte` is the byte preceding the low byte; only its
// low nibble carries code bits, the rest is not decoded.
inline ChannelReg DecodeChannel(const uint8_t hi_byte, const uint8_t lo_byte)
{
    ChannelReg r{};
    r.code       = (uint16_t)(((uint16_t)(hi_byte & 0x0F) << 8) | lo_byte);
    r.power_mode = kPowerModeUnknown;
    r.vref       = kPowerModeUnknown;
    r.gain       = kPowerModeUnknown;
    return r;
}

// A channel's offset in the response: the position of its LOW byte. The high nibble
// is in the low nibble of the byte before it.
//
// The first entry sits at 2, not 0: byte 0 is a header byte (0xC0) and byte 1 is
// channel A's high-nibble byte. Measured, not assumed -- the four-marker map showed
// 0x11 at [2], 0x22 at [8], 0x33 at [14], 0x44 at [20].
constexpr size_t kFirstChannelOffset = 2;

inline size_t ChannelOffset(uint8_t dac_sel)
{
    return kFirstChannelOffset + (size_t)(dac_sel & 0x03) * kReadStride;
}

// Where a channel's low byte sits within the 8-byte read, or -1 if that read does
// not reach it. Only channel A is fully present (offset 2 needs byte 1).
inline int ChannelOffsetInDacRead(uint8_t dac_sel)
{
    const size_t off = ChannelOffset(dac_sel);
    return (off < kReadDacBytes) ? (int)off : -1;
}

}  // namespace DacFrame
