#pragma once

#include <stdint.h>

// R58 = 82k, R61 = 100k (spec 6.2). The amplifier is
//   V_KEY = (1 + R58/R61)*V_DAC - (R58/R61)*V_ADJ
// so gain is 1.82 exactly; V_ADJ is 0V when its DAC channel is in the 1k
// pulldown, and tracks the commanded signal when it is not. Use the ratio, not
// a decimal: 1.812 is a misreading and drifts over the envelope.
constexpr int kGainR58 = 82;
constexpr int kGainR61 = 100;

constexpr int kOutputFloorMv   = 1800;
constexpr int kOutputCeilingMv = 5200;
constexpr int kGuardLowMv      = 2600;
constexpr int kGuardHighMv     = 3400;

constexpr int kDacFullScaleMv  = 3300;   // VREF = VDD
constexpr int kDacMaxCode      = 4095;

/*
 * Gain 1.00, gain 1.82, or "decide from the measurement" (spec 6.2).
 *
 * `kAuto` is a CONFIG value, not a gain: it is what a channel's
 * `output.gain_mode` may say, and `Boot` resolves it through `settings.gain_policy`
 * before any arithmetic. Every function that computes a voltage treats anything
 * other than `kTracking` as amplified, so a value that somehow reaches the math
 * unresolved falls to 1.82 -- the safe direction per spec 6.2's asymmetry.
 *
 * It exists because the spec's own worked-example config sets a channel's
 * `gain_mode` to "AUTO", and without this the codec rejected that config
 * outright: `settings.gain_policy` could therefore never be consulted, because
 * every decodable channel already named a concrete mode. FR-14's AUTO rule was
 * implemented and unit-tested but unreachable from any legal config.
 */
enum class GainMode { kTracking = 0, kAmplified = 1, kAuto = 2 };
enum class GainPolicy { kAuto = 0, kForceTracking, kForceAmplified };

struct GainDecision {
    GainMode mode;
    uint16_t dac_code;
    bool     clamped;
};

GainMode GainPolicySelect(GainPolicy policy, int measured_idle_key_mv);

// The KEY voltage this mode would produce for a DAC code, in millivolts.
int GainPolicyKeyMvForCode(GainMode mode, uint16_t code);

// The DAC code to reach a target KEY voltage, clamped into the output envelope.
GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv);
