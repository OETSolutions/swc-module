#include "KeyLine.h"

#include <Arduino.h>

#include "Adc.h"
#include "Dac.h"
#include "swc_logic/Output.h"

namespace KeyLine {

bool SenseMv(int ch, int *out_key_mv)
{
    const Adc::Ch sc = (ch == 2) ? Adc::kSense2 : Adc::kSense1;
    uint32_t mv = 0;
    if (!Adc::ReadAvgMv(sc, 64, &mv)) return false;
    if (out_key_mv) *out_key_mv = Output::KeyMvFromSenseMv((int)mv);
    return true;
}

int FloatMv(int ch)
{
    Dac::Release(ch);
    // The integrator's time constant is ~10 ms and settling is "tens of ms"
    // (spec 6.5); 150 ms is several time constants plus margin.
    delay(150);

    // Take the maximum of several reads. The float level is where the line comes to
    // rest, so a read taken while it is still rising would understate it -- and
    // understating it would make a reachable command look unreachable.
    int best = 0;
    for (int i = 0; i < 6; ++i) {
        int mv = 0;
        if (SenseMv(ch, &mv) && mv > best) best = mv;
        delay(30);
    }
    return best;
}

bool CommandReachable(int ch, int target_key_mv)
{
    const int ceiling = CommandCeilingMv(ch);
    return ceiling > 0 && target_key_mv <= ceiling;
}

int CommandCeilingMv(int ch)
{
    const int fl = FloatMv(ch);
    const int ceiling = fl - Output::kCommandHeadroomMv;
    return (ceiling < Output::kEnvelopeLowMv) ? 0 : ceiling;
}

}  // namespace KeyLine
