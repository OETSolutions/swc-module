#include "Analog/LadderDecode.h"

#include <stdlib.h>

namespace {
// The calibrated ADC ceiling (spec 3.2): above this no pin reading is possible, so
// a value beyond it is a wiring or calibration fault rather than a level. It is
// the ONE shared constant (`kAdcFullScaleMv12dB`, CalibrationCurve.h) rather than
// a local literal -- this file and `LearnSession.cpp` each used to carry their own
// copy, which nothing compared.
constexpr int kAdcCeilingMv = kAdcFullScaleMv12dB;
// The idle reading has collapsed relative to the one learned. Expressed against
// the LEARNED idle, not the ratio: ratio normalization deliberately cancels rail
// movement out of the ratios, so a dead supply looks perfectly normal to it. This
// is the check that catches that (FR-30). (`kIdleMarginPermille`, the idle band,
// lives in the header because LearnSession shares it.)
constexpr int16_t kRailHealthFloorPermille = 200;
}  // namespace

int16_t LadderRatioPermille(int level_mv, int idle_mv) {
    if (idle_mv <= 0) return -1;
    const long long scaled = (static_cast<long long>(level_mv) * 1000LL + idle_mv / 2) / idle_mv;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int idle_mv) {
    ClassifyOutcome out{ClassifyResult::kFault, 0, 0};
    if (idle_mv <= 0) return out;

    const int16_t ratio = LadderRatioPermille(level_mv, idle_mv);
    out.ratio_permille = ratio;

    // FR-30: a 3V3 sag to <=20% of the learned idle is a rail fault. Checked
    // before anything else, because at that level every ratio is garbage.
    //
    // This tests the REFERENCE only. It deliberately does not test the reading:
    // a button near the ladder's common produces a legitimately low voltage
    // (spec 6.3 consequence 2), so a low reading is a press, not a fault. The
    // collapsed reading that matters is caught upstream by the output-envelope
    // check (spec 6.2 step 2), which is a different measurement on a different
    // pin and is where the orchestrator detects a rail collapse.
    if (profile.learned_idle_mv > 0 &&
        idle_mv < (profile.learned_idle_mv * kRailHealthFloorPermille) / 1000) {
        return out;                                                    // reference collapsed
    }

    if (ratio < 0 || ratio > 1000 + kIdleMarginPermille) return out;   // above the reference

    if (ratio >= 1000 - kIdleMarginPermille) {
        out.result = ClassifyResult::kIdle;
        return out;
    }

    // Nearest-centre match, so two overlapping windows resolve deterministically
    // to whichever button the user actually pressed rather than to array order.
    //
    // The centre and half-width are DERIVED from millivolts against the LEARNED
    // idle, not read from stored ratio fields (spec 3.4/3.5: storing both forms
    // does not fit the NVS budget). Deriving is also the more correct of the two:
    // the learned idle is the rail the mv_center values were measured at, so the
    // ratio is the same number either way, but a stored copy could drift from it.
    int best = -1;
    int best_distance = 0;
    for (uint8_t i = 0; i < profile.count && i < kLadderMaxButtons; ++i) {
        const int centre = LadderRatioPermille(profile.buttons[i].mv_center,
                                               profile.learned_idle_mv);
        const int half   = LadderRatioPermille(profile.buttons[i].mv_tolerance,
                                               profile.learned_idle_mv);
        if (centre < 0 || half < 0) continue;
        const int distance = abs(ratio - centre);
        if (distance > half) continue;
        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }

    if (best >= 0) {
        out.result = ClassifyResult::kButton;
        out.index = static_cast<uint8_t>(best);
    } else {
        out.result = ClassifyResult::kUnknown;
    }
    return out;
}

bool LadderWindowsAreDistinguishable(const LadderProfile &p) {
    // No reference means no ratio, so nothing can be said about the windows.
    // The config validator refuses this separately (a learned idle must be a
    // plausible ADC reading); returning false here keeps the two agreeing.
    if (p.learned_idle_mv == 0) return false;
    for (uint8_t i = 0; i < p.count && i < kLadderMaxButtons; ++i) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < p.count && j < kLadderMaxButtons;
             ++j) {
            const int ci = LadderRatioPermille(p.buttons[i].mv_center, p.learned_idle_mv);
            const int cj = LadderRatioPermille(p.buttons[j].mv_center, p.learned_idle_mv);
            const int ti = LadderRatioPermille(p.buttons[i].mv_tolerance, p.learned_idle_mv);
            const int tj = LadderRatioPermille(p.buttons[j].mv_tolerance, p.learned_idle_mv);
            if (ci < 0 || cj < 0 || ti < 0 || tj < 0) return false;
            const int distance  = abs(ci - cj);
            const int tolerance = ti > tj ? ti : tj;
            if (distance <= tolerance) return false;
        }
    }
    return true;
}

void LadderProfileRebase(LadderProfile &p, int from_idle_mv, int to_idle_mv) {
    if (from_idle_mv <= 0 || to_idle_mv <= 0) return;   // no source frame to convert
    if (from_idle_mv == to_idle_mv) return;
    const auto scale = [from_idle_mv, to_idle_mv](int v) -> MilliVolt {
        const long long scaled =
            (static_cast<long long>(v) * to_idle_mv + from_idle_mv / 2) / from_idle_mv;
        // Clamped, not wrapped. `MilliVolt` is a uint16_t and the validator bounds a
        // centre only by the ADC ceiling, NOT relative to `learned_idle_mv` -- so a
        // profile it accepts (a near-zero learned idle with centres at the ceiling)
        // would scale past the type and wrap to a plausible-looking small value.
        // Clamping keeps the profile in ONE frame with a bounded value, which is
        // what the caller's single denominator needs; the input was physically
        // impossible either way, so there is no value here worth preserving.
        if (scaled > kAdcCeilingMv) return static_cast<MilliVolt>(kAdcCeilingMv);
        return static_cast<MilliVolt>(scaled);
    };
    for (uint8_t i = 0; i < p.count && i < kLadderMaxButtons; ++i) {
        LadderButton &b = p.buttons[i];
        b.mv_center = scale(b.mv_center);
        b.mv_tolerance = scale(b.mv_tolerance);
    }
    p.learned_idle_mv = static_cast<MilliVolt>(to_idle_mv);
}

bool LadderProfileIsValid(const LadderProfile &p) {
    // The channel-level bounds, which ConfigValidate checks separately.
    if (p.learned_idle_mv <= 0 || p.learned_idle_mv > kAdcCeilingMv) return false;
    if (p.count > kLadderMaxButtons) return false;
    for (uint8_t i = 0; i < p.count; ++i) {
        const LadderButton &b = p.buttons[i];
        // A button at or above the idle reference is physically impossible: a press
        // pulls the input DOWN. Both bounds are against the ADC ceiling rather than
        // the 3300 mV rail, because no pin reading can exceed the ceiling -- so a
        // value above it is not a measurement. A centre of exactly 0 IS reachable
        // from a learn (an unreadable ADC reports 0, and `FilteredLevelMv` returns
        // 0 for a stale reading), which is the route that motivated this predicate.
        if (b.mv_center == 0 || b.mv_center > kAdcCeilingMv) return false;
        if (b.mv_tolerance == 0) return false;
        // The DERIVED window must be a real one: a tolerance that rounds to zero
        // permille can never match anything.
        if (LadderRatioPermille(b.mv_tolerance, p.learned_idle_mv) <= 0) return false;
    }
    return LadderWindowsAreDistinguishable(p);
}
