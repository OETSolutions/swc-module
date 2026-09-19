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

    // One code is roughly kDacFullScaleMv/4096 = 0.8mV at the DAC, so 0.4mV at
    // the sense pin in amplified mode. Convert the error to codes, then clamp.
    int delta = (error_mv * 4096) / (kDacFullScaleMv / 2);
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
