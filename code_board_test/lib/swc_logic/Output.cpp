#include "swc_logic/Output.h"

namespace Output {

const char *ModeName(Mode m)
{
    return m == Mode::kAmplified ? "amplified(1.82)" : "tracking(1.00)";
}

int CodeForTargetKeyMv(Mode m, int target_key_mv)
{
    if (target_key_mv <= 0) return -1;

    // Amplified: V_KEY = 1.82 * V_DAC  ->  V_DAC = V_KEY * 100 / 182
    // Tracking : V_KEY = V_DAC
    int v_dac_mv;
    if (m == Mode::kAmplified) {
        // V_KEY > 5.98 V cannot be produced by a 3.3 V DAC even at full scale.
        // Check before scaling so a huge target is rejected rather than wrapping.
        if ((int64_t)target_key_mv * kGainDen > (int64_t)kDacVrefMv * kGainNum) return -1;
        v_dac_mv = (int)(((int64_t)target_key_mv * kGainDen) / kGainNum);
    } else {
        if (target_key_mv > kDacVrefMv) return -1;
        v_dac_mv = target_key_mv;
    }
    return (int)CodeForDacMv(v_dac_mv);
}

int KeyMvForCode(Mode m, uint16_t code)
{
    return KeyMvForDacMv(m, DacMvForCode(code));
}

uint16_t CodeForDacMv(int v_dac_mv)
{
    if (v_dac_mv <= 0) return 0;
    // 4096, not 4095: full scale is 4095/4096 of VREF, so code 4095 is one LSB
    // below VREF. Using 4095 here would put a consistent -0.8 mV bias on every
    // commanded level.
    int32_t c = ((int32_t)v_dac_mv * 4096 + kDacVrefMv / 2) / kDacVrefMv;
    if (c < 0) c = 0;
    if (c > kDacMaxCode) c = kDacMaxCode;
    return (uint16_t)c;
}

Decision SelectFromIdleKeyMv(int idle_key_mv, Mode *out_mode)
{
    // Absent or ambiguous -> keep the CURRENT mode. The caller passes what it
    // holds; we only overwrite the mode on positive evidence.
    if (idle_key_mv < kEnvelopeLowMv || idle_key_mv > kEnvelopeHighMv) {
        return Decision::kNoHeadUnit;
    }
    // The guard band is HALF-OPEN at the top: [2.6, 3.4), so exactly 3.4 V takes
    // the 5 V branch below.
    //
    // Spec 6.2's steps 3 and 4 overlap at 3.4 V -- step 3 calls "2.6-3.4 V" the
    // ambiguous band, step 4 calls ">= 3.4 V" the 5 V range -- and the tie is
    // broken deliberately rather than arbitrarily: step 4 is written with a
    // comparison operator and step 3 with a range, so the operator wins; and
    // resolving the boundary toward kAmplified is the SAFE direction per this same
    // section's asymmetry (the only dangerous mistake is over-ranging a 3 V unit,
    // so any ambiguous case defaults to 1.82 rather than 1.00).
    if (idle_key_mv >= kGuardLowMv && idle_key_mv < kGuardHighMv) {
        return Decision::kGuardBand;
    }
    if (idle_key_mv >= kGuardHighMv) {
        if (out_mode) *out_mode = Mode::kAmplified;  // 5 V head unit
        return Decision::kRanged5V;
    }
    if (out_mode) *out_mode = Mode::kTracking;  // 3 V head unit, positive evidence
    return Decision::kRanged3V;
}

const char *DecisionName(Decision d)
{
    switch (d) {
        case Decision::kNoHeadUnit: return "NO_HEAD_UNIT";
        case Decision::kGuardBand:  return "GUARD_BAND(hold current mode)";
        case Decision::kRanged5V:   return "5V_RANGE(gain 1.82)";
        case Decision::kRanged3V:   return "3V_RANGE(gain 1.00)";
    }
    return "?";
}

int ClampCommand(int target_key_mv, int idle_key_mv, bool *out_clamped)
{
    if (out_clamped) *out_clamped = false;

    const int upper = (idle_key_mv > 0) ? (idle_key_mv - kCommandHeadroomMv) : kEnvelopeHighMv;
    if (upper < kEnvelopeLowMv) {
        // Empty band: no reachable level below the line's own rest.
        return 0;
    }
    int v = target_key_mv;
    if (v < kEnvelopeLowMv) { v = kEnvelopeLowMv; if (out_clamped) *out_clamped = true; }
    if (v > upper)          { v = upper;          if (out_clamped) *out_clamped = true; }
    return v;
}

}  // namespace Output
