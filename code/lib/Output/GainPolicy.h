#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"   // MilliVolt

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
// The guard band's UPPER edge (spec 6.2: the 2.6-3.4 V ambiguous band). It is
// deliberately CONSULTED BY NOTHING -- kept as the named value of the band's top
// so the spec figure has one home, not as a threshold. `GainPolicySelect` splits
// at `kGuardLowMv` alone and folds the band into "amplified", because spec 6.2's
// two sides are asymmetric: gain 1.00 is safe only on POSITIVE evidence of a 3 V
// line, and the sole dangerous error is over-ranging a 3 V head unit. A reader who
// assumed this constant guarded something would look for a three-way branch that
// does not exist -- see `GainPolicySelect`'s comment for why the top side has no
// threshold of its own.
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

/*
 * Map a WHEEL ladder level onto the HEAD UNIT's range, by ratio (spec 6.9).
 *
 * The wheel's ladder and the head unit's need not have the same resistances, so
 * the level is expressed as a fraction of the wheel's idle and applied to the
 * head unit's idle. This lives here, beside the other output arithmetic, so it is
 * unit-testable directly: it is the last computation before a voltage reaches the
 * DAC, and it has a wraparound hazard that only a direct test can pin.
 *
 * Returns `head_unit_idle_mv * level_mv / wheel_idle_mv`, SATURATED to
 * `[0, kOutputCeilingMv]`. Saturation is load-bearing rather than cosmetic: the
 * raw product can exceed `MilliVolt`'s 65535 for a small `wheel_idle_mv`, and
 * narrowing it mod 65536 lands back INSIDE the valid 1800-5200 range about half
 * the time -- producing a plausible-looking target that the downstream clamp
 * cannot detect, i.e. a key voltage nothing defined. `0` is returned for a
 * non-positive `wheel_idle_mv` (no denominator, so nothing can be said).
 */
MilliVolt GainPolicyMapWheelLevelToHeadUnit(int head_unit_idle_mv, int level_mv,
                                            int wheel_idle_mv);


// The DAC code to reach a target KEY voltage, clamped into the output envelope.
GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv);

/*
 * Spec 6.2's command headroom: a command target must stay at least this far
 * BELOW the head unit's own measured idle.
 *
 * Above that point the sink FET can only be turned off, which is the RELEASE
 * behavior rather than a command (spec 6.2, spec 6.7). A firmware that wrote the
 * code anyway would report a driven key while the radio received nothing.
 */
constexpr int kCommandHeadroomMv = 200;

/*
 * Clamp a command target into spec 6.2's command band,
 * `[kOutputFloorMv, V_KEY_idle - kCommandHeadroomMv]`.
 *
 * The lower bound is the servo's own floor -- the value `GainPolicyCodeForTarget`
 * already refuses to go below, so the bottom of the commandable range has one
 * definition. The upper bound is the head unit's own resting level, which is the
 * half nothing implemented: the envelope clamp alone permits a target between the
 * line's idle and 5200 mV, and the output only SINKS, so such a target reaches the
 * radio as no key at all.
 *
 * `head_unit_idle_mv` is a MEASUREMENT (spec 6.2 step 1), not a config value, so
 * `<= 0` means no head unit was measured: there is no resting level to stay below
 * and the envelope bounds are the only ones that apply.
 *
 * Returns the clamped target, or **0 when the band is empty** -- a head unit idling
 * below `kOutputFloorMv + kCommandHeadroomMv` leaves no level below its own rest
 * that the servo can still reach. 0 is the same "absent" the rest of the output
 * code uses (`Action::key_mv`, `GainPolicyMapWheelLevelToHeadUnit`), so a caller
 * releases rather than driving a level it cannot justify. `*clamped` reports
 * whether the request had to move; it is null-safe.
 */
int GainPolicyClampCommand(int target_key_mv, int head_unit_idle_mv, bool *clamped);
