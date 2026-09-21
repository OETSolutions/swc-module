#pragma once

// Output-stage arithmetic: the servo transfer function, gain-mode selection and
// the DAC code that reaches a target KEY voltage.
//
// Pure logic, no Arduino and no IDF, so the host test suite can assert it. This is
// deliberate: these are the numbers that decide what voltage lands on a car's
// steering-wheel input line, and they are the exact place two shipped defects
// lived (spec 2.3, 6.2):
//
//   * The ADJ channel was put in tracking mode but its CODE was never written, so
//     a 3 V head unit was driven at 1.82x -- the over-range direction -- while
//     every power-mode assertion still read correct.
//   * The gain was written as 1.812 (a truncated decimal) instead of the ratio.
//
// Both are invisible to a power-mode check and both are caught by the assertions
// in test/test_logic.

#include <stdint.h>

namespace Output {

// ---------------------------------------------------------------------------
// The transfer function, from the netlist's resistors (spec 2.3):
//
//   V_KEY = (1 + R58/R61)*V_DAC - (R58/R61)*V_ADJ
//
// R58 = 82k, R61 = 100k. The ratio is 0.82 EXACTLY, so the amplified gain is
// 1.82 exactly. Do not write 1.812.
// ---------------------------------------------------------------------------
constexpr int kR58 = 82;
constexpr int kR61 = 100;
constexpr int kGainNum = kR58 + kR61;  // 182
constexpr int kGainDen = kR61;         // 100

enum class Mode {
    kAmplified = 0,  // ADJ channel powered down to 1k -> V_ADJ = 0 -> gain 1.82
    kTracking  = 1,  // ADJ channel carries the same code -> gain 1.00
};

// Which head-unit range a mode serves, for reporting.
const char *ModeName(Mode m);

// The gain as a rational, so no caller rounds it. Multiply first, divide last.
constexpr int GainNum(Mode m) { return m == Mode::kAmplified ? kGainNum : kGainDen; }
constexpr int GainDen(Mode) { return kGainDen; }

// The V_ADJ voltage that mode implies, given the signal DAC voltage.
// Amplified: the ADJ pin is pulled to GND through 1k, and the summing node draws
// essentially nothing through R61, so V_ADJ is 0 for our purposes -- this is a
// DEFINED path, not leakage, which is why the spec rejects a series MOSFET here.
constexpr int AdjMvFor(Mode m, int v_dac_mv)
{
    return m == Mode::kAmplified ? 0 : v_dac_mv;
}

// V_KEY for a commanded DAC voltage, in both halves of the relation.
inline int KeyMvForDacMv(Mode m, int v_dac_mv)
{
    const int adj = AdjMvFor(m, v_dac_mv);
    return (kGainNum * v_dac_mv - kR58 * adj) / kGainDen;
}

// The DAC code for a target KEY voltage, unclamped. Returns -1 when the target is
// unreachable (above the rail) so a caller cannot silently accept a saturated code.
//
// Amplified: V_DAC = V_KEY * 100 / 182
// Tracking : V_DAC = V_KEY
int CodeForTargetKeyMv(Mode m, int target_key_mv);

// The KEY voltage a DAC code would produce, using integer arithmetic that matches
// what the DAC actually does (code * VREF / 4096, not /4095 -- the MCP4728's full
// scale is 4095/4096 of VREF, one LSB below VREF).
int KeyMvForCode(Mode m, uint16_t code);

// The DAC voltage a code produces. ONE definition of the scaling; every other
// function here goes through it.
constexpr int kDacVrefMv = 3300;
constexpr int kDacMaxCode = 4095;
inline int DacMvForCode(uint16_t code) { return (int)(((int32_t)code * kDacVrefMv) / 4096); }

// The code for a DAC voltage, clamped to the 12-bit range.
uint16_t CodeForDacMv(int v_dac_mv);

// ---------------------------------------------------------------------------
// Envelope and band (spec 6.2).
// ---------------------------------------------------------------------------
constexpr int kEnvelopeLowMv  = 1800;  // below this the servo has no authority
constexpr int kEnvelopeHighMv = 5200;
constexpr int kGuardLowMv     = 2600;  // the two ranges are indistinguishable here
constexpr int kGuardHighMv    = 3400;
constexpr int kCommandHeadroomMv = 200;

// Spec 6.2's AUTO selection, from the measured idle KEY voltage.
//
// The asymmetry is the safety argument, not an oversight: the only dangerous
// mistake is OVER-ranging a 3 V unit, so the default is the amplified mode
// whenever the measurement is absent or ambiguous, and tracking is chosen only on
// positive evidence (below the guard band).
enum class Decision {
    kNoHeadUnit,   // outside the envelope: absent, off, or miswired
    kGuardBand,    // indistinguishable: hold the current mode, re-measure later
    kRanged5V,     // amplified, gain 1.82
    kRanged3V,     // tracking, gain 1.00
};

Decision SelectFromIdleKeyMv(int idle_key_mv, Mode *out_mode);

const char *DecisionName(Decision d);

// Clamp a command target into `[kEnvelopeLowMv, idle - kCommandHeadroomMv]`.
// Returns 0 when the band is EMPTY -- a head unit idling below
// kEnvelopeLowMv + kCommandHeadroomMv leaves no reachable level below its own
// rest, and 0 is the shared "nothing to command" value the caller turns into a
// release rather than a guess (spec 6.2).
int ClampCommand(int target_key_mv, int idle_key_mv, bool *out_clamped);

// The sense node is a buffered, divided copy of the KEY line: an exact /2.
inline int KeyMvFromSenseMv(int sense_mv) { return sense_mv * 2; }
inline int SenseMvFromKeyMv(int key_mv) { return key_mv / 2; }

}  // namespace Output
