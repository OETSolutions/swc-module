#include "Gesture/PressClassifier.h"

PressClassifier::PressClassifier(const LadderProfile &profile, const GestureTimings &timings)
    : profile_(profile), timings_(timings) {}

void PressClassifier::Reset() {
    level_ = ChannelLevel::kIdle;
    button_index_ = 0xFF;
    candidate_ = ClassifyResult::kIdle;
    candidate_index_ = 0xFF;
    candidate_since_ms_ = 0;
    have_candidate_ = false;
}

ChannelLevel PressClassifier::Update(int level_mv, int idle_mv, uint64_t now_ms) {
    const ClassifyOutcome outcome = LadderClassify(profile_, level_mv, idle_mv);

    // Hysteresis: while pressed, a reading that is merely *between* windows
    // holds the current button rather than releasing. Only a return to idle
    // (or a new settled button) ends the press.
    if (level_ == ChannelLevel::kPressed && outcome.result == ClassifyResult::kUnknown) {
        return level_;
    }

    // A fault is immediate: it must never be delayed by debounce, because the
    // thing being debounced is a hardware condition, not a human finger.
    if (outcome.result == ClassifyResult::kFault) {
        level_ = ChannelLevel::kFault;
        button_index_ = 0xFF;
        have_candidate_ = false;
        return level_;
    }

    if (!have_candidate_ || outcome.result != candidate_ ||
        (outcome.result == ClassifyResult::kButton && outcome.index != candidate_index_)) {
        candidate_ = outcome.result;
        candidate_index_ = (outcome.result == ClassifyResult::kButton) ? outcome.index : 0xFF;
        candidate_since_ms_ = now_ms;
        have_candidate_ = true;
        return level_;
    }

    if (now_ms - candidate_since_ms_ < timings_.debounce_ms) return level_;

    switch (candidate_) {
        case ClassifyResult::kIdle:
            level_ = ChannelLevel::kIdle;
            button_index_ = 0xFF;
            break;
        case ClassifyResult::kButton:
            level_ = ChannelLevel::kPressed;
            button_index_ = candidate_index_;
            break;
        case ClassifyResult::kUnknown:
            level_ = ChannelLevel::kUnknown;
            button_index_ = 0xFF;
            break;
        case ClassifyResult::kFault:
            level_ = ChannelLevel::kFault;
            button_index_ = 0xFF;
            break;
    }
    return level_;
}
