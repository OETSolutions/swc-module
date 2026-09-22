#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HAL/IHAL.h"

/*
 * The MCP4728 Multi-Write frame, as pure, host-testable logic.
 *
 * Why this is a separate module and not inline in EspHal.cpp: EspHal is the one
 * lib/ translation unit the host build excludes (it is the only one that names
 * ESP-IDF driver headers), so ANY byte layout written inside it is unchecked by
 * the entire host suite. That is not a hypothetical gap. An earlier version of
 * this frame emitted FOUR bytes with the command in its own byte --
 *   frame[0] = 0x40          (command)
 *   frame[1] = DAC1 DAC0 | UDAC
 *   frame[2] = high nibble
 *   frame[3] = low byte
 * -- which is the MCP4725's shape, not the MCP4728's. The MCP4728 Multi-Write
 * frame is THREE bytes and packs the command type bits, the channel-select bits
 * and UDAC into ONE byte (datasheet DS22187E Figure 5-8). Every write therefore
 * carried an extra byte: the device read the channel-select byte as the
 * command-and-channel byte, the config byte as data, and the low data byte as
 * a further byte of the NEXT frame -- so no channel was ever addressed and no
 * code ever latched. Nothing in the host suite could see it, because the code
 * that built the frame was not compiled there.
 *
 * So the frame is built HERE, in a header with no IDF dependency, and asserted
 * against the datasheet's bit layout by `DacFrameTest` on the host. Keep the
 * encode functions free of anything that would make them un-buildable on the
 * host; that property is the whole point of the file.
 *
 * Wire format (DS22187E section 5.6.2, Figure 5-8):
 *
 *   byte 0: 0 1 0 0 0 DAC1 DAC0 UDAC
 *           ^^^^^^^ ^^^^ ^^^^ ^^^^
 *           C2 C1 C0    |     \_ 1 = hold the input register; 0 = latch the
 *                       |             output on the 3rd byte's ACK. We always
 *                       |             send 0 so a single-channel write lands
 *                       |             immediately, with no ~LDAC pulse.
 *                       \_ channel: 00=A, 01=B, 10=C, 11=D
 *
 *   byte 1: VREF PD1 PD0 Gx D11 D10 D9 D8
 *           ^^^^ ^^^^^^^ ^^ ^^^^^^^^^^^^
 *           |     |      |   \_ the four MSBs of the 12-bit code
 *           |     |      \_ gain: 0 = x1, 1 = x2 (only meaningful with the
 *           |     |            internal reference; we run VREF = VDD, so 0)
 *           |     \_ power-down: 00 normal, 01 = 1k to GND, 10 = 100k, 11 = 500k
 *           \_ 0 = VDD (our design), 1 = internal 2.048 V
 *
 *   byte 2: D7 D6 D5 D4 D3 D2 D1 D0   (the low eight bits of the code)
 */

namespace DacFrame {

// The channel-select field's value for each MCP4728 output. These are the raw
// DAC1:DAC0 codes, NOT a left-shifted mask; EncodeSet does the shift.
constexpr uint8_t kChannelA = 0;  // U4.VOUTA -> channel 1 signal
constexpr uint8_t kChannelB = 1;  // U4.VOUTB -> /V_ADJ1
constexpr uint8_t kChannelC = 2;  // U4.VOUTC -> channel 2 signal
constexpr uint8_t kChannelD = 3;  // U4.VOUTD -> /V_ADJ2

// Command-type bits C2:C1:C0 = 0:1:0, in their positions (byte 0 bits 7:5).
constexpr uint8_t kCommandMultiWrite = 0x40;
// UDAC, byte 0 bit 0 (the wire-format diagram above and the datasheet's
// Figure 5-8 both put it there -- `0 1 0 0 0 DAC1 DAC0 UDAC`). Named so the "we
// send 0" decision is visible at the call site rather than implied by a missing
// `|=`. It was 0x08 (bit 3), which is not UDAC at all: the mask was wrong, and
// the test that "asserted UDAC is clear" read a bit the encoder could never set,
// so it passed vacuously. Sending bit 3 set would have asked the part for
// something undefined; the mask must name the real bit for the test to bite.
constexpr uint8_t kUdacHold = 0x01;

constexpr size_t kSize = 3;

// Encode one Multi-Write frame for a single channel. `code` is clamped to 12
// bits; `vref`/`gain` are the single bits from byte 1; `pd` is the two-bit
// power-down code (0 normal, 1 = 1k to GND, ...). UDAC is always clear, so the
// addressed channel's output updates on the frame's final ACK.
inline void EncodeSet(uint8_t out[kSize], uint8_t dac_sel, uint8_t vref,
                      uint8_t pd, uint8_t gain, uint16_t code)
{
    code &= 0x0FFF;
    out[0] = (uint8_t)(kCommandMultiWrite | ((dac_sel & 0x03) << 1));
    out[1] = (uint8_t)(((vref & 1u) << 7) | ((pd & 0x03u) << 5) |
                       ((gain & 1u) << 4) | ((code >> 8) & 0x0F));
    out[2] = (uint8_t)(code & 0xFF);
}

// The power-down mode's PD1:PD0 code, from the HAL's enum. Kept here beside the
// encoder so the mapping and the bit position it lands in are one edit apart.
inline uint8_t PowerDownCode(DacPowerMode mode)
{
    switch (mode) {
        case DAC_POWER_NORMAL:   return 0;
        case DAC_POWER_GND_1K:   return 1;
        case DAC_POWER_GND_100K: return 2;
        case DAC_POWER_GND_500K: return 3;
        default:                 return 0;
    }
}

// The channel-select field for a HAL channel ORDINAL. Returns false for a value
// that is not a real output, so callers can drop the write rather than address a
// garbage channel.
//
// **The parameter is an integer, not a `DacChannel`, and that is a correctness
// requirement rather than a style choice.** This function's contract includes
// "rejects a value that is not a real output", and a `DacChannel` parameter makes
// that half of the contract UNEXPRESSIBLE without undefined behaviour: forming a
// `DacChannel` outside its enumerator range and then loading it (which the
// `switch` does) is UB, and UBSan flags it -- `load of value 99, which is not a
// valid value for type 'DacChannel'`. So the defensive case could only ever be
// TESTED by executing UB, which is the same shape as an unchecked cast: the check
// exists on paper and is unsound in fact.
//
// Taking the ordinal by value makes both the guard and its test well-defined.
// Callers that have a real `DacChannel` name it explicitly with a static_cast at
// the call site, so the valid path is still checked by the compiler.
inline bool SelectForChannel(uint8_t ch_ordinal, uint8_t *out)
{
    switch (ch_ordinal) {
        case kChannelA: *out = kChannelA; return true;   // DAC_CH_KEY1
        case kChannelB: *out = kChannelB; return true;   // DAC_CH_ADJ1
        case kChannelC: *out = kChannelC; return true;   // DAC_CH_KEY2
        case kChannelD: *out = kChannelD; return true;   // DAC_CH_ADJ2
        default:        return false;
    }
}

}  // namespace DacFrame
