#pragma once

#include <stdint.h>

#include "Output/GainPolicy.h"

struct ServoConfig {
    int max_step_codes;     // largest correction any single update may apply
    int deadband_mv;        // error below which we stop moving entirely
    int max_total_codes;    // total authority away from the open-loop code
    int samples_to_settle;  // consecutive in-deadband updates before Settled()
    // FR-19 / spec 6.5: the trim loop ships DISABLED in v1. The spec's posture is
    // "open-loop command with the trim loop present but disabled by default until
    // its gain is measured on hardware", and FR-19's test row states the
    // consequence exactly -- the unit tests "prove the implementation and not the
    // running system". Keeping the switch here (rather than deleting the calls)
    // is what lets both halves be true at once.
    bool enabled;
};

inline ServoConfig ServoConfigDefault() {
    // FR-19 / spec 6.5: DISABLED in the shipped build. The spec's posture is
    // "open-loop command with the trim loop present but disabled by default until
    // its gain is measured on hardware", and FR-19's test row states the
    // consequence exactly -- the unit tests "prove the implementation and not the
    // running system".
    //
    // `SWC_BENCH_TRIM_LOOP` is the bring-up switch that performs the measurement:
    // a bench build defines it, drives the output, and checks that the trim
    // CONVERGES the line (smaller static error) without INJECTING noise (no
    // jitter/oscillation). It is a compile-time flag, not a runtime config field,
    // because enabling the loop is a hardware-characterisation decision -- there
    // is no user-facing reason to turn it on, and a config field would be one more
    // way for a shipped device to end up running a loop tuned against a guess.
    const bool enabled =
#ifdef SWC_BENCH_TRIM_LOOP
        true;
#else
        false;
#endif
    return ServoConfig{/*max_step_codes=*/8,
                       /*deadband_mv=*/20,
                       /*max_total_codes=*/120,
                       /*samples_to_settle=*/4,
                       enabled};
}

/*
 * When the trim is serviced. **Both constants exist because the loop must
 * measure the RESULT of its own action, and the bounded pulse means it can only
 * do that a little after the drive settles.**
 *
 * `kServoTrimSettleMs` is the wait after a command is written before the first
 * trim update: the op-amp integrator (§6.5, ~10 ms) needs several time constants
 * to reach the commanded level, and trimming an unsettled line measures the
 * servo's ramp, not its static error. `kServoTrimIntervalMs` is the update
 * cadence once settled.
 *
 * **The cadence is a measured-plant retune, not the spec's 1-2 Hz.** §6.5's rate
 * was written for a continuously-commanded servo; this device commands a
 * BOUNDED pulse (`send_duration_ms`, default 200 ms), so a 1-2 Hz trim would get
 * at most one update per press and could never null a static error. The interval
 * is well below the 16 Hz analog loop (5x), which is the stability property the
 * spec's rate protects -- the bench (FR-19) is what confirms it does not ring.
 */
inline constexpr uint32_t kServoTrimSettleMs = 60;
inline constexpr uint32_t kServoTrimIntervalMs = 200;

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
    // The config, for a test seam that flips `enabled` (the orchestrator's
    // `SetTrimEnabledForTest`). The shipped switch is compile-time, so this is the
    // only way a host test can reach the loop's orchestration.
    const ServoConfig &Config() const { return cfg_; }
    void SetConfig(const ServoConfig &cfg) { cfg_ = cfg; }
    void     Reset();

private:
    ServoConfig cfg_;
    GainMode    mode_ = GainMode::kAmplified;
    int         target_key_mv_ = 0;
    uint16_t    base_code_ = 0;
    uint16_t    code_ = 0;
    int         settled_samples_ = 0;
};
