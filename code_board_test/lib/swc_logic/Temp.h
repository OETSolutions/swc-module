#pragma once

// NTC thermistor math, pure and host-testable.
//
// RT1 is a 10k B3380 NTC in a divider with R29 10k to +3V3 (netlist: R29 pin1 =
// 3V3, pin2 = TEMP_ADC; RT1 pin1 = TEMP_ADC, pin2 = GND-side, via the C19 filter).
// So the ADc measures the NTC's own drop and the resistance is:
//
//   R_ntc = R29 * V_ntc / (V_rail - V_ntc)
//
// with V_rail the measured 3V3, NOT an assumed 3.30. That distinction matters: the
// spec's bring-up step 3 sweeps the rail 3.14-3.47 V, which is a +/-5% swing in the
// denominator and therefore a several-degree error in the reported temperature if
// 3.30 is hard-coded.
//
// The spec is explicit that this sensor is NOT used to correct anything in v1
// (6.4): the ladder's own drift is unknown for this vehicle, so no correction is
// implemented and none is claimed. This tool reports the temperature so a bring-up
// session can MEASURE that drift. It deliberately computes no coefficient.

#include <stdint.h>

namespace Temp {

constexpr float kR0Ohm    = 10000.0f;   // NTC nominal resistance
constexpr float kT0Kelvin = 298.15f;    // at 25 C
constexpr float kBeta     = 3380.0f;    // B3380
constexpr float kSeriesOhm = 10000.0f;  // R29

// NTC resistance from the measured divider voltage. Returns -1 for a non-physical
// reading (0 V, at or above the rail) so the caller reports "sensor open/short"
// rather than an absurd resistance.
float ResistanceFromMv(float v_ntc_mv, float v_rail_mv);

// Temperature in Celsius from NTC resistance. Returns NAN for a non-positive
// resistance.
float CelsiusFromResistance(float r_ntc_ohm);

// The whole path. Returns NAN when the reading is not physical.
float CelsiusFromMv(float v_ntc_mv, float v_rail_mv);

// The nominal temperature error a wrong rail assumption would produce, for the log.
// Kept as a function so the claim is computed rather than asserted in prose.
float ErrorFromAssumedRailC(float v_ntc_mv, float v_rail_real_mv, float v_rail_assumed_mv);

}  // namespace Temp
