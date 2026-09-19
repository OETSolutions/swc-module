#include "Gesture/GestureStateMachine.h"

GestureStateMachine::GestureStateMachine(const GestureTimings &timings) : timings_(timings) {}

void GestureStateMachine::Reset() {
    pressed_ = false;
    button_ = 0xFF;
    press_started_ms_ = 0;
    released_at_ms_ = 0;
    long_fired_ = false;
    awaiting_second_ = false;
    pending_single_ = false;
}

void GestureStateMachine::Emit(Gesture g, uint64_t now_ms, GestureEvent *out) {
    if (g == Gesture::kNone) return;
    if (out) *out = GestureEvent{g, button_, now_ms};
}

bool GestureStateMachine::Update(ChannelLevel level, uint8_t button_index,
                                 uint64_t now_ms, GestureEvent *out) {
    // A fault clears everything in flight: whatever was half-recognised is no
    // longer trustworthy, and a wrong guess reaches the radio.
    if (level == ChannelLevel::kFault) {
        Reset();
        return false;
    }

    const bool is_pressed = (level == ChannelLevel::kPressed);

    if (is_pressed) {
        // A DOUBLE fires at the *start* of the second press, so this branch can
        // emit as well as the release branch below.
        bool emitted = false;
        if (!pressed_) {
            pressed_ = true;
            button_ = button_index;
            press_started_ms_ = now_ms;
            long_fired_ = false;
            if (awaiting_second_) {
                // Second press inside the window: this is the DOUBLE.
                awaiting_second_ = false;
                pending_single_ = false;
                Emit(Gesture::kDouble, now_ms, out);
                long_fired_ = true;  // sentinel: this press must not emit SINGLE
                emitted = true;
            }
        }
        // FR-10: LONG fires at the threshold, not on release. A held button must
        // act immediately -- waiting for the finger to lift is the difference
        // between "responsive" and "broken" to a driver.
        if (!long_fired_ && (now_ms - press_started_ms_) >= timings_.long_press_ms) {
            long_fired_ = true;
            Emit(Gesture::kLong, now_ms, out);
            return true;
        }
        return emitted;
    }

    // Not pressed. Two time-driven transitions can land here.
    if (pressed_) {
        const uint64_t held = now_ms - press_started_ms_;
        pressed_ = false;
        if (!long_fired_ && held >= timings_.long_press_ms) {
            // Reaching here means the threshold elapsed but no Update() was
            // called while it elapsed (e.g. a 100ms poll). Fire it now, so LONG
            // is never lost.
            long_fired_ = true;
            Emit(Gesture::kLong, now_ms, out);
            return true;
        }
        if (!long_fired_) {
            // Either this was the first press of a possible double, or the
            // second press of one that was already emitted.
            if (awaiting_second_) {
                awaiting_second_ = false;
                pending_single_ = false;
                Emit(Gesture::kDouble, now_ms, out);
                return true;
            }
            pending_single_ = true;
            awaiting_second_ = true;
            released_at_ms_ = now_ms;
        }
        return false;
    }

    // Idle.
    if (awaiting_second_ && now_ms - released_at_ms_ >= timings_.double_press_off_ms) {
        awaiting_second_ = false;
        pending_single_ = false;
        Emit(Gesture::kSingle, now_ms, out);
        return true;
    }
    if (pending_single_ && !awaiting_second_) {
        pending_single_ = false;
    }
    return false;
}
