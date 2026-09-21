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
// Read (DS22187E 5.6.6). Two commands, and the difference matters for testing:
//
//   READ_DAC (0x08-ish) -> the four channels' INPUT REGISTERS, i.e. what was last
//                          written. 8 bytes.
//   READ_ALL (0x09-ish) -> input registers, then EEPROM, then status. 24 bytes.
//
// The exact byte layout is pinned in the host test; the constants are named so the
// device code and the test refer to the same numbers rather than two literals.
// ---------------------------------------------------------------------------
constexpr uint8_t kAddrGeneralCall = 0x00;
constexpr uint8_t kReadCmdDac      = 0x08;  // read the four input registers
constexpr uint8_t kReadCmdAll      = 0x09;  // + EEPROM + status

constexpr size_t kReadDacBytes = 8;
constexpr size_t kReadAllBytes = 24;

// One channel's decoded input register, as the read returns it.
struct ChannelReg {
    uint16_t code;       // D11..D0
    uint8_t  power_mode; // PD1:PD0
    uint8_t  vref;       // 1 = internal 2.048 V, 0 = VDD
    uint8_t  gain;       // 0 = x1, 1 = x2
};

// Decode one 2-byte channel slot from a read response.
//
// Byte layout per channel (datasheet Figure 5-11, "Read Command"):
//   byte 0:  D11 D10 D9 D8  PD1 PD0 Gx VREF
//   byte 1:  D7  D6  D5 D4 D3 D2 D1 D0
//
// Note the nibble/field ORDER differs from the write frame's byte 1: here the code
// is the HIGH nibble and the config fields are the low nibble, whereas Multi-Write
// byte 1 has config high and code low. Getting that backwards yields a code that
// looks plausible and is wrong by a factor that varies per channel -- so the host
// test round-trips a written frame through these decoders.
inline ChannelReg DecodeChannel(const uint8_t b0, const uint8_t b1)
{
    ChannelReg r{};
    r.code       = (uint16_t)(((uint16_t)(b0 >> 4) << 8) | b1);
    r.power_mode = (uint8_t)((b0 >> 2) & 0x03);
    r.gain       = (uint8_t)((b0 >> 1) & 0x01);
    r.vref       = (uint8_t)(b0 & 0x01);
    return r;
}

// The channel slot's byte offset within a read response: 2 bytes per channel,
// in A,B,C,D order. Named because both the decoder and the test index by it.
inline size_t ChannelOffset(uint8_t dac_sel) { return (size_t)(dac_sel & 0x03) * 2; }

}  // namespace DacFrame
