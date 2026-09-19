#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"

struct GestureTimings {
    uint32_t debounce_ms;
    uint32_t double_press_off_ms;
    uint32_t long_press_ms;
    uint32_t send_duration_ms;
};

// Defaults from spec 3.7, so the feel matches the 2022 Pico firmware.
inline GestureTimings GestureTimingsDefault() {
    return GestureTimings{/*debounce_ms=*/25,
                          /*double_press_off_ms=*/500,
                          /*long_press_ms=*/750,
                          /*send_duration_ms=*/200};
}

enum class ChannelLevel { kIdle, kPressed, kUnknown, kFault };

/*
 * Turns a millivolt stream into a debounced, hysteretic level. Two thresholds
 * are not enough here: the ladder's windows are adjacent, so a press that
 * drifts toward a neighbouring window must stay latched to the button the user
 * actually pressed. Entry requires the window; exit requires returning to idle.
 */
class PressClassifier {
public:
    PressClassifier(const LadderProfile &profile, const GestureTimings &timings);

    // level_mv is the calibrated reading and idle_mv the current idle
    // reference; both in millivolts (Task 4). Classification normalizes the
    // former by the latter (spec 6.3).
    ChannelLevel Update(int level_mv, int idle_mv, uint64_t now_ms);

    ChannelLevel Level() const { return level_; }
    uint8_t ButtonIndex() const { return button_index_; }
    void Reset();

private:
    LadderProfile  profile_;
    GestureTimings timings_;
    ChannelLevel   level_ = ChannelLevel::kIdle;
    uint8_t        button_index_ = 0xFF;

    // Candidate awaiting debounce.
    ClassifyResult candidate_ = ClassifyResult::kIdle;
    uint8_t        candidate_index_ = 0xFF;
    uint64_t       candidate_since_ms_ = 0;
    bool           have_candidate_ = false;
};
