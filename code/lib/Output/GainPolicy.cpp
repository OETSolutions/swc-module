#include "Output/GainPolicy.h"

#include <algorithm>

namespace {
// milli-units of the (1 + R58/R61) ratio, so the arithmetic stays integral.
constexpr int kRatioMilli      = 1000 + (kGainR58 * 1000) / kGainR61;  // 1820
// The V_ADJ coefficient (0.82). KeyMv below collapses the V_ADJ term away in
// both modes rather than evaluating it, so this constant documents the divider
// pair rather than feeding the arithmetic. It is deliberately unused -- the
// project builds with -Wall -Wextra -Werror, which rejects an unreferenced
// constexpr, so it carries the attribute.
[[maybe_unused]] constexpr int kAdjRatioMilli = (kGainR58 * 1000) / kGainR61;  // 820

int CodeToDacMv(uint16_t code) {
    const int c = std::min<int>(code, kDacMaxCode);
    return (c * kDacFullScaleMv + kDacMaxCode / 2) / kDacMaxCode;
}

// V_ADJ is 0V in amplified mode: the channel is powered down into its 1k
// pulldown and contributes nothing. In tracking mode V_ADJ mirrors V_DAC, so
// the (1+R58/R61)*V_DAC and (R58/R61)*V_ADJ terms collapse to V_DAC exactly.
int KeyMv(GainMode mode, int dac_mv) {
    if (mode == GainMode::kTracking) return dac_mv;
    return (kRatioMilli * dac_mv) / 1000;
}
}  // namespace

GainMode GainPolicySelect(GainPolicy policy, int measured_idle_key_mv) {
    switch (policy) {
        case GainPolicy::kForceTracking:   return GainMode::kTracking;
        case GainPolicy::kForceAmplified:  return GainMode::kAmplified;
        case GainPolicy::kAuto:
        default:
            break;
    }
    // Inside the guard band the head unit's bias is close enough to our amplified
    // envelope that 1.82 gives the widest usable span; outside it we must match
    // the line rather than fight it. Defaulting to amplified is deliberate -- it
    // is the mode the learned ladder was captured against.
    return (measured_idle_key_mv >= kGuardLowMv && measured_idle_key_mv <= kGuardHighMv)
               ? GainMode::kAmplified
               : GainMode::kTracking;
}

int GainPolicyKeyMvForCode(GainMode mode, uint16_t code) {
    return KeyMv(mode, CodeToDacMv(code));
}

GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv) {
    const int clamped_target = std::clamp(target_key_mv, kOutputFloorMv, kOutputCeilingMv);
    const bool clamped = (clamped_target != target_key_mv);

    // Invert: dac_mv = target / ratio, where ratio is 1.0 or 1.82.
    const int ratio_milli = (mode == GainMode::kTracking) ? 1000 : kRatioMilli;
    const int dac_mv = (clamped_target * 1000) / ratio_milli;

    long long code = (static_cast<long long>(dac_mv) * kDacMaxCode + kDacFullScaleMv / 2) /
                     kDacFullScaleMv;
    code = std::clamp<long long>(code, 0, kDacMaxCode);

    // The inverse of a rounding division can land one code outside the envelope.
    // Walk it back until the *achievable* voltage is in range -- this is the
    // property the exhaustive test asserts, so it must hold at every code.
    GainDecision d{mode, static_cast<uint16_t>(code), clamped};
    while (d.dac_code > 0 && KeyMv(mode, CodeToDacMv(d.dac_code)) > kOutputCeilingMv) {
        --d.dac_code;
    }
    while (d.dac_code < kDacMaxCode && KeyMv(mode, CodeToDacMv(d.dac_code)) < kOutputFloorMv) {
        ++d.dac_code;
    }
    return d;
}
