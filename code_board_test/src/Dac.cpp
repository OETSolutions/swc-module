#include "Dac.h"

#include <Arduino.h>
#include <Wire.h>

#include "BoardPins.h"
#include "swc_logic/DacFrame.h"

namespace Dac {

static bool    s_present = false;
static uint8_t s_addr    = MCP4728_ADDR;

// The four output channels, in the DAC's own A..D order.
static const Channel kSignal[2] = {
    {"ch1 signal (U4.VOUTA -> KEY1)", DacFrame::kChannelA},
    {"ch2 signal (U4.VOUTC -> KEY2)", DacFrame::kChannelC},
};
static const Channel kAdj[2] = {
    {"ch1 ADJ (U4.VOUTB)", DacFrame::kChannelB},
    {"ch2 ADJ (U4.VOUTD)", DacFrame::kChannelD},
};

static int ChIndex(int ch) { return (ch == 2) ? 1 : 0; }

const Channel &SignalChannel(int ch) { return kSignal[ChIndex(ch)]; }
const Channel &AdjChannel(int ch)    { return kAdj[ChIndex(ch)]; }

uint8_t Address() { return s_addr; }
void    SetAddress(uint8_t addr) { s_addr = addr; }
bool    Present() { return s_present; }

bool WriteRaw(const uint8_t *frame, size_t len, uint8_t addr)
{
    if (len == 0 || len > 32) return false;
    Wire.beginTransmission(addr);
    if (Wire.write(frame, len) != len) { Wire.endTransmission(); return false; }
    return Wire.endTransmission() == 0;
}

// One Multi-Write frame. Returns true if the device ACKed.
static bool WriteFrame(uint8_t dac_sel, DacFrame::PowerMode pd, uint16_t code)
{
    uint8_t frame[DacFrame::kSetSize];
    DacFrame::EncodeSet(frame, dac_sel, (uint8_t)pd, code);
    return WriteRaw(frame, sizeof(frame), s_addr);
}

bool Begin()
{
    // The board has R5/R6 10k pull-ups to +3V3, so the internal ones only help
    // during the brief window before the bus is first driven (as in production).
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(50);

    // Probe. A single zero-length write is a valid address probe on this bus and
    // is what Wire's own scanner uses.
    Wire.beginTransmission(s_addr);
    s_present = (Wire.endTransmission() == 0);
    if (!s_present) return false;

    // Park both channels in the released state. Doing it here, rather than trusting
    // the EEPROM, is what makes the board safe to power up regardless of whether
    // the EEPROM was ever programmed (DESIGN.md 4.6).
    s_present = (Release(1) >= 0) && (Release(2) >= 0);
    return s_present;
}

// ---------------------------------------------------------------------------
// The only code-writing entry point. Both halves of the relation, always.
// ---------------------------------------------------------------------------
int SetSignal(int ch, Output::Mode mode, uint16_t code)
{
    const Channel &sig = SignalChannel(ch);
    if (!WriteFrame(sig.frame_sel, DacFrame::kNormal, code)) return -1;

    const Channel &adj = AdjChannel(ch);
    if (mode == Output::Mode::kAmplified) {
        // 1 kOhm to GND: a DEFINED path, which is exactly what gain 1.82 wants and
        // why the spec chose a DAC channel over a series MOSFET (whose off-state
        // leakage would appear at the KEY line multiplied by R58).
        if (!WriteFrame(adj.frame_sel, DacFrame::kGnd1k, 0)) return -1;
    } else {
        // Tracking: ADJ carries the SAME code. This is the write the production
        // firmware omitted, and the whole reason this function exists.
        if (!WriteFrame(adj.frame_sel, DacFrame::kNormal, code)) return -1;
    }
    return 2;
}

int Release(int ch)
{
    // Full scale on the signal channel. The op-amp rail (4.98 V) binds before this
    // can be reached, so the servo drives the gate low, Q4 turns off and the line
    // floats up. Commanding "above idle" IS the release command.
    //
    // ADJ goes to its 1 kOhm down: that is the 5 V-range mode, which is the
    // conservative default per spec 6.2's asymmetry (a released line in the wrong
    // mode is harmless; the mode is re-measured before any command).
    const Channel &sig = SignalChannel(ch);
    if (!WriteFrame(sig.frame_sel, DacFrame::kNormal, DAC_MAX_CODE)) return -1;
    const Channel &adj = AdjChannel(ch);
    if (!WriteFrame(adj.frame_sel, DacFrame::kGnd1k, 0)) return -1;
    return 2;
}

bool SetPowerModeRaw(uint8_t frame_sel, DacFrame::PowerMode mode)
{
    return WriteFrame(frame_sel, mode, 0);
}

// ---------------------------------------------------------------------------
// Read-back
// ---------------------------------------------------------------------------
bool ReadInputRegisters(uint8_t out[DacFrame::kReadDacBytes])
{
    // DS22187E: the read is initiated by a general-call address with the read
    // command byte, then the device is addressed again for the data phase.
    Wire.beginTransmission(DacFrame::kAddrGeneralCall);
    Wire.write(DacFrame::kReadCmdDac);
    if (Wire.endTransmission() != 0) return false;

    const size_t got = Wire.requestFrom((int)s_addr, (int)DacFrame::kReadDacBytes);
    if (got != DacFrame::kReadDacBytes) return false;

    for (size_t i = 0; i < DacFrame::kReadDacBytes; ++i) {
        if (!Wire.available()) return false;
        out[i] = (uint8_t)Wire.read();
    }
    return true;
}

bool ReadChannelReg(uint8_t frame_sel, DacFrame::ChannelReg *out)
{
    uint8_t buf[DacFrame::kReadDacBytes];
    if (!ReadInputRegisters(buf)) return false;
    const size_t off = DacFrame::ChannelOffset(frame_sel);
    if (off + 1 >= DacFrame::kReadDacBytes) return false;
    *out = DacFrame::DecodeChannel(buf[off], buf[off + 1]);
    return true;
}

}  // namespace Dac
