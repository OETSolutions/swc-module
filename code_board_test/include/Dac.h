#pragma once

// The MCP4728 transport: I2C writes, power modes, and read-back.
//
// The single most important thing this module does is make the historical
// tracking-mode defect UNREPRESENTABLE. Spec 2.3, consequence 1: in tracking mode
// (3 V head unit) V_ADJ must equal V_DAC on EVERY code write, not just at gain
// selection. The production firmware selected tracking mode, wrote only the power
// mode bit, and left ADJ's code at 0 -- so the amplifier delivered 1.82x, the
// over-range direction, while the power-mode assertion read correct.
//
// The fix is structural rather than a rule to remember: there is NO public
// "write channel A" here. The only way to change a signal channel's code is
// SetSignal(), which derives the ADJ channel's state from the mode and writes both
// halves in one call. A caller cannot express the bug.

#include <stddef.h>
#include <stdint.h>

#include "swc_logic/DacFrame.h"
#include "swc_logic/Output.h"

namespace Dac {

struct Channel {
    const char *name;
    uint8_t frame_sel;   // DacFrame::kChannel*
};

// channel 1 = U4 VOUTA/VOUTB, channel 2 = U4 VOUTC/VOUTD (spec 2.3).
const Channel &SignalChannel(int ch);  // ch is 1 or 2
const Channel &AdjChannel(int ch);

// Bring up I2C, find the device, and park the outputs in the SAFE state.
//
// Safe state matters more than it looks: the MCP4728 EEPROM is supposed to be
// programmed so a cold start resets channel A to FULL SCALE -- the maximum
// command -- so the servo releases both 3 V and 5 V head units (DESIGN.md 4.6).
// This tool does NOT rely on that: it drives both signal channels to full scale
// itself as its first act, so the outputs are released whether or not the EEPROM
// was ever programmed. Test 9 verifies the EEPROM separately.
//
// Returns true if the device answered at the expected address.
bool Begin();

// True if the device acknowledged during Begin().
bool Present();

// The address the device actually answered at, or 0 if none. Test 4 scans and can
// update this; spec item N-4 makes 0x60 a measurement rather than a fact.
uint8_t Address();
void    SetAddress(uint8_t addr);

// Read back the four input registers (8 bytes). Returns false on a bus error or a
// malformed response. This is what makes "did the code actually latch" testable
// with no meter attached.
bool ReadInputRegisters(uint8_t out[DacFrame::kReadDacBytes]);

// Convenience: the decoded register for one channel.
bool ReadChannelReg(uint8_t frame_sel, DacFrame::ChannelReg *out);

// ---------------------------------------------------------------------------
// The output commands. These are the ONLY way to write a channel.
// ---------------------------------------------------------------------------

// Set a signal channel to a DAC code, in a given gain mode.
//
// In kAmplified: the signal channel gets `code`, and the ADJ channel is put in its
// 1 kOhm power-down (V_ADJ = 0 by a defined path, gain 1.82).
// In kTracking : the signal channel gets `code` AND the ADJ channel gets the SAME
// code, in normal mode (V_ADJ = V_DAC, gain 1.00).
//
// Returns the number of frames written (1 or 2), or -1 on a bus error. A caller
// that needs to know the ADJ half happened gets it from the return value, and the
// device test asserts both halves by reading the registers back.
int SetSignal(int ch, Output::Mode mode, uint16_t code);

// Put a signal channel in the released state: full scale, which the servo cannot
// reach (the rail binds first) so the sink FET turns off and the line floats up
// through the head unit's own pull-up. This is the idle/high-Z state and it needs
// no special hardware mode (spec 2.3, consequence 2).
int Release(int ch);

// Raw power-mode write, for the power-mode test only. Not a code write, so it
// cannot be mistaken for one -- and deliberately not reachable from SetSignal.
bool SetPowerModeRaw(uint8_t frame_sel, DacFrame::PowerMode mode);

// A single raw Multi-Write frame, for the frame-format test. Kept public so the
// test can push bytes the normal API cannot produce.
bool WriteRaw(const uint8_t *frame, size_t len, uint8_t addr);

}  // namespace Dac
