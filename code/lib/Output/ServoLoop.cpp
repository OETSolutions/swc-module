#include "Output/ServoLoop.h"

#include <stdlib.h>

ServoLoop::ServoLoop(const ServoConfig &cfg) : cfg_(cfg) {}

void ServoLoop::Target(GainMode mode, int target_key_mv) {
    mode_ = mode;
    target_key_mv_ = target_key_mv;
    base_code_ = GainPolicyCodeForTarget(mode, target_key_mv).dac_code;
    code_ = base_code_;
    settled_samples_ = 0;
}

void ServoLoop::Reset() {
    code_ = base_code_;
    settled_samples_ = 0;
}

bool ServoLoop::Update(int measured_sense_mv) {
    // FR-19 / spec 6.5: DISABLED by default in v1. The open-loop code is the
    // primary command and the hardware integrator does the regulating; the trim
    // is a supervisor whose gain is unknown until it is measured on hardware.
    // Returning here (rather than at the call sites) keeps the loop constructible
    // and unit-testable -- FR-19's test row requires exactly that -- while the
    // running system stays open-loop.
    if (!cfg_.enabled) return false;

    // Tracking mode needs no trim: V_ADJ follows V_DAC, so the summing node is
    // already at unity and any correction would be fighting the servo.
    if (mode_ == GainMode::kTracking) return false;

    // The sense divider halves the KEY line, so compare like with like.
    const int target_sense_mv = target_key_mv_ / 2;
    const int error_mv = target_sense_mv - measured_sense_mv;

    if (abs(error_mv) <= cfg_.deadband_mv) {
        ++settled_samples_;
        return false;
    }
    settled_samples_ = 0;

    // Convert the SENSE-pin error to DAC codes. One code moves the KEY line by
    // `gain * (kDacFullScaleMv / 4096)` mV, and the sense divider halves that, so
    // in amplified mode (gain 1.82) the sense pin moves ~0.733 mV per code --
    // NOT the 0.4 mV an earlier revision assumed, which is the TRACKING-mode
    // figure (gain 1.00). Using the tracking figure made the computed step ~1.82x
    // too large.
    //
    // That error is currently MASKED by the shipped tuning -- `max_step_codes` is
    // 8, which every error past the 20 mV deadband already saturates, so the loop
    // behaves identically either way. It would bite the moment the step cap is
    // raised while the gain is measured on hardware (spec 6.5): a step 1.82x too
    // large puts the loop gain near unity, where the correction overshoots and
    // rings instead of converging. Fixed while the arithmetic is being read.
    //
    // `gain_milli` is the exact ratio, never a rounded decimal (spec 6.2): 1820
    // for amplified, 1000 for tracking. Tracking returns above, but the formula
    // is written for both so the two cannot drift apart.
    const int gain_milli =
        (mode_ == GainMode::kTracking) ? 1000 : ((kGainR58 + kGainR61) * 1000 / kGainR61);
    // codes per mV of sense error, x1000 to keep it integer:
    //   2 * 4096 * 1e6 / (gain_milli * kDacFullScaleMv)
    const long long codes_per_sense_mv_milli =
        (2LL * (kDacMaxCode + 1) * 1000000LL) / (gain_milli * kDacFullScaleMv);
    int delta = static_cast<int>((static_cast<long long>(error_mv) *
                                  codes_per_sense_mv_milli) / 1000);
    if (delta > cfg_.max_step_codes) delta = cfg_.max_step_codes;
    if (delta < -cfg_.max_step_codes) delta = -cfg_.max_step_codes;

    int next = static_cast<int>(code_) + delta;
    const int lo = static_cast<int>(base_code_) - cfg_.max_total_codes;
    const int hi = static_cast<int>(base_code_) + cfg_.max_total_codes;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    if (next < 0) next = 0;
    if (next > kDacMaxCode) next = kDacMaxCode;
    if (next == static_cast<int>(code_)) return false;

    code_ = static_cast<uint16_t>(next);
    return true;
}
