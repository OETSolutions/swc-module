#pragma once

#include <stdint.h>

#include "Output/GainPolicy.h"

struct ServoConfig {
    int max_step_codes;     // largest correction any single update may apply
    int deadband_mv;        // error below which we stop moving entirely
    int max_total_codes;    // total authority away from the open-loop code
    int samples_to_settle;  // consecutive in-deadband updates before Settled()
};

inline ServoConfig ServoConfigDefault() {
    return ServoConfig{/*max_step_codes=*/8,
                       /*deadband_mv=*/20,
                       /*max_total_codes=*/120,
                       /*samples_to_settle=*/4};
}

/*
 * A deliberately small, bounded correction on top of the open-loop code.
 *
 * This exists because the op-amp integrator already removes most error; the
 * loop only mops up resistor tolerance and servo offset. Two hard limits make
 * that safe: a per-update step cap (no jumps) and a total authority cap (no
 * slow walk to an extreme when the target is unreachable). Both are tested --
 * an unbounded integrator here would fight the hardware integrator and ring.
 */
class ServoLoop {
public:
    explicit ServoLoop(const ServoConfig &cfg);

    void Target(GainMode mode, int target_key_mv);

    // Returns true if the code changed. measured_sense_mv is what the sense
    // divider reads, i.e. half the KEY line.
    bool Update(int measured_sense_mv);

    uint16_t Code() const { return code_; }
    bool     Settled() const { return settled_samples_ >= cfg_.samples_to_settle; }
    void     Reset();

private:
    ServoConfig cfg_;
    GainMode    mode_ = GainMode::kAmplified;
    int         target_key_mv_ = 0;
    uint16_t    base_code_ = 0;
    uint16_t    code_ = 0;
    int         settled_samples_ = 0;
};
