#include "swc_logic/Temp.h"

#include <math.h>

namespace Temp {

float ResistanceFromMv(float v_ntc_mv, float v_rail_mv)
{
    // Non-physical: no drive, or the drop is the whole rail (NTC shorted, or the
    // pin is being pulled to the rail by a fault). Both are real bring-up failures
    // and both must say so instead of returning a number.
    if (v_ntc_mv <= 0.0f) return -1.0f;
    if (v_rail_mv <= 0.0f) return -1.0f;
    if (v_ntc_mv >= v_rail_mv) return -1.0f;

    return kSeriesOhm * v_ntc_mv / (v_rail_mv - v_ntc_mv);
}

float CelsiusFromResistance(float r_ntc_ohm)
{
    if (!(r_ntc_ohm > 0.0f)) return NAN;
    // 1/T = 1/T0 + ln(R/R0)/B
    const float inv_t = (1.0f / kT0Kelvin) + (logf(r_ntc_ohm / kR0Ohm) / kBeta);
    if (inv_t <= 0.0f) return NAN;
    return (1.0f / inv_t) - 273.15f;
}

float CelsiusFromMv(float v_ntc_mv, float v_rail_mv)
{
    const float r = ResistanceFromMv(v_ntc_mv, v_rail_mv);
    if (r < 0.0f) return NAN;
    return CelsiusFromResistance(r);
}

float ErrorFromAssumedRailC(float v_ntc_mv, float v_rail_real_mv, float v_rail_assumed_mv)
{
    const float real_c = CelsiusFromMv(v_ntc_mv, v_rail_real_mv);
    const float fake_c = CelsiusFromMv(v_ntc_mv, v_rail_assumed_mv);
    if (isnan(real_c) || isnan(fake_c)) return NAN;
    return fake_c - real_c;
}

}  // namespace Temp
