#pragma once

#include <stdint.h>

/*
 * The NTC conversion, as pure host-testable logic (FR-1's first clause, open
 * item N-67).
 *
 * **The divider, read from the schematic, not assumed.** `SWC.kicad_sch` has
 * `RT1` (`Device:Thermistor_NTC`, value `10k B3380`) between the `TEMP_ADC` node
 * and `GND`, and `R29` (`Device:R`, `10k`) between `+3V3` and the same node. So
 * the part is on the LOW side and the node voltage RISES as the part gets
 * hotter:
 *
 *     +3V3 ---[ R29 10k ]---+--- ADC_CH_TEMP (IO7)
 *                          |
 *                        [ RT1 10k B3380 ]
 *                          |
 *                         GND
 *
 *     V(node) = 3V3 * R_ntc / (R_series + R_ntc)
 *
 * and inverting for the resistance:
 *
 *     R_ntc = R_series * V / (VDD - V)
 *
 * which needs `V < VDD` -- at or above the rail the inversion is undefined, and a
 * reading there is a fault, not a very hot board.
 *
 * **Why this is a header of its own and not inline in `EspHal.cpp`.** The same
 * reason `DacFrame.h` and `DacRetry.h` are: `EspHal.cpp` is the one lib/ file the
 * host build excludes, so logic written there is executed by no test until the
 * board is on the bench. The maths here is entirely testable on the host and is
 * asserted against the datasheet's own B-constant table below.
 *
 * **The B-constant form, not Steinhart-Hart.** The part is specified as a B3380
 * thermistor with a 10 k nominal at 25 C, which is exactly the two-parameter model:
 *
 *     1/T = 1/T0 + (1/B) * ln(R / R0)          T in kelvin, T0 = 298.15 K
 *
 * Steinhart-Hart's third coefficient buys accuracy only over a span far wider
 * than an automotive cabin, and the part does not carry the coefficients. Adding
 * them would be fitting a model to no data.
 */
namespace Ntc {

// From the schematic: R29 is 10k, RT1 is specified 10k at 25 C.
constexpr int kSeriesOhms     = 10000;
constexpr int kNominalOhms    = 10000;
constexpr int kNominalTenthsC = 250;      // 25.0 C, the R0 reference
constexpr int kBeta            = 3380;

// The rail the divider hangs from is `kNominalRailMv`, declared in
// `LadderDecode.h` and shared with `learned_at_rail_mv` — one name for the board's
// 3V3, so the divider's supply and the recorded rail cannot disagree.

// Refuse absurd resistances rather than report a temperature nothing could have.
// A shorted or open part reads as ~0 or ~rail; the resistance that follows is 0 or
// unbounded, and both land outside any real cabin. The bounds are wide on purpose
// (they are a sanity gate, not a spec) -- about -40 C to +85 C for this part.
constexpr int kMinOhms = 50;
constexpr int kMaxOhms = 1000000;

// Tenths of a degree C, or a sentinel for "no reading".
//
// The sentinel matches `SystemOrchestrator`'s `kTempNotMeasuredTenths` in VALUE
// (0) but is named separately here so the two never have to move together: 0 is
// also a legal temperature (0.0 C), which is why every caller must consult the
// bool and not the value.
constexpr int16_t kNoReadingTenths = 0;

/*
 * Divider inversion: the node voltage to the part's resistance in ohms.
 *
 * Returns false when the voltage is not on the divider's valid span -- at or
 * above the rail (VDD - V <= 0 makes the inversion undefined), or negative (the
 * ADC's -1 error sentinel). A caller must hold its previous temperature rather
 * than treat a failure as a reading; N-43 was exactly that mistake made three
 * times elsewhere in this firmware.
 */
inline bool ResistanceFromMv(int node_mv, int vdd_mv, int *out_ohms)
{
    if (out_ohms == nullptr) return false;
    // `<= 0` on the denominator is the guard that matters: at V == VDD the node is
    // the rail (an open NTC), and past it the arithmetic would produce a NEGATIVE
    // resistance that still passes any magnitude check downstream.
    if (vdd_mv <= 0 || node_mv <= 0) return false;
    const int denom = vdd_mv - node_mv;
    if (denom <= 0) return false;
    // 64-bit on both sides: kSeriesOhms * node_mv is at most 10,000 * 3300 =
    // 3.3e7, which fits int, but a caller may pass a larger series value and the
    // multiplication is the natural place for a future widening to bite.
    const long long r = (static_cast<long long>(kSeriesOhms) * node_mv) / denom;
    if (r < kMinOhms || r > kMaxOhms) return false;
    *out_ohms = static_cast<int>(r);
    return true;
}

/*
 * The B-constant inversion, in TENTHS of a degree C (the unit the config's
 * `temp_c_at_learn` stores, spec 3.4).
 *
 * **Integer maths from first principles, because there is no libm on this path
 * and no floating point in the config.** The exponential is the problem: `ln` has
 * no integer form. It is expanded around the nominal point instead, where the
 * argument is near 1, using the series
 *
 *     ln(x) = 2 * (y + y^3/3 + y^5/5 + ...),  y = (x-1)/(x+1)
 *
 * `y` is computed in fixed point (scaled by 2^16) so the squaring stays exact in
 * 64 bits. **Thirty terms**, because the series converges slowly at the HOT end
 * (`x = R/R0` reaches ~0.06 at 100 C, so `y` approaches -0.9): truncated at the
 * conventional eight, the error at the hot end is +0.8 C, and at thirty it is
 * under 0.25 C across the whole -40 to +120 C span. A tenfold-wider fixed-point
 * scale does not help -- the truncation, not the scale, is the error term.
 *
 * A temperature outside the part's plausible range is REFUSED rather than
 * reported: the caller holds its last good value. The bound is generous (-40 to
 * +150 C) so it only ever catches an arithmetic runaway or a wildly wrong
 * resistance, never a real reading.
 */
constexpr int kSeriesTerms = 30;

inline bool ConvertTenthsC(int ohms, int *out_tenths_c)
{
    if (out_tenths_c == nullptr) return false;
    if (ohms < kMinOhms || ohms > kMaxOhms) return false;

    // y = (R - R0) / (R + R0), in 2^16 fixed point.
    const long long num = static_cast<long long>(ohms) - kNominalOhms;
    const long long den = static_cast<long long>(ohms) + kNominalOhms;
    if (den == 0) return false;
    const long long y = (num << 16) / den;   // |y| < 1, so this fits comfortably

    // ln(R/R0) = 2 * (y + y^3/3 + y^5/5 + ...). Every intermediate stays inside
    // 64 bits: |y| < 2^16, so |y*y| < 2^32 and |term*y2| < 2^48.
    long long sum = y;                        // scaled 2^16
    long long y2 = (y * y) >> 16;             // y^2, scaled 2^16
    long long term = y;                       // y^(2k+1), scaled 2^16
    for (int k = 1; k <= kSeriesTerms; ++k) {
        term = (term * y2) >> 16;             // y^(2k+1)
        sum += term / (2 * k + 1);
    }
    const long long ln_fp = 2 * sum;          // scaled 2^16

    // 1/T = 1/T0 + (1/B) * ln(R/R0). Work in 1e6 K^-1 to keep the divisions
    // integral: 1e6/298.15 = 3354.
    const long long inv_t0 = 3354;            // 1e6 / 298.15
    const long long inv_t = inv_t0 + (ln_fp * 1000000LL) / (static_cast<long long>(kBeta) << 16);
    if (inv_t <= 0) return false;

    // T(kelvin) = 1e6 / inv_t; then tenths of C = (T - 273.15) * 10.
    const long long t_kelvin_milli = (1000000LL * 1000LL) / inv_t;   // millikelvin
    const long long tenths = (t_kelvin_milli - 273150) / 100;        // 0.1 C units
    if (tenths < -400 || tenths > 1500) return false;
    *out_tenths_c = static_cast<int>(tenths);
    return true;
}

/*
 * The whole chain, which is what a caller actually wants: a node millivolt
 * reading to a temperature.
 *
 * `vdd_mv` is an ARGUMENT and not `kNominalRailMv` inlined, because the divider
 * scales with the actual rail: on a board whose 3V3 is 3.25 V the node voltage for
 * a given temperature is lower, and using the nominal would read every
 * temperature low by roughly 1.5 % of the divider -- about 0.4 C at 25 C, and more
 * at the extremes. `learned_at_rail_mv` (spec 3.4) is the measurement of exactly
 * this, which is why the field exists.
 */
inline bool NodeMvToTenthsC(int node_mv, int vdd_mv, int *out_tenths_c)
{
    int ohms = 0;
    if (!ResistanceFromMv(node_mv, vdd_mv, &ohms)) return false;
    return ConvertTenthsC(ohms, out_tenths_c);
}

}  // namespace Ntc
