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
                                 uint64_t now_ms, GestureEvent *out,
                                 const GestureBindings &bindings) {
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
            // The button the PREVIOUS press was on, before this press overwrites
            // it. A double press is the SAME button tapped twice (spec 6.6), so
            // the comparison below needs both.
            const uint8_t prev_button = button_;
            pressed_ = true;
            button_ = button_index;
            press_started_ms_ = now_ms;
            long_fired_ = false;
            if (awaiting_second_ && button_index == prev_button) {
                // Second press of the SAME button inside the window: the DOUBLE.
                awaiting_second_ = false;
                pending_single_ = false;
                Emit(Gesture::kDouble, now_ms, out);
                long_fired_ = true;  // sentinel: this press must not emit SINGLE
                emitted = true;
            } else if (awaiting_second_) {
                // A DIFFERENT button pressed inside the window. This is NOT a
                // double -- two different buttons are two separate presses, and
                // folding them into a DOUBLE would send the SECOND button's
                // double-press command while silently swallowing the first
                // button's press entirely: the driver taps `vol_dn` then `next`
                // and the radio gets `next`'s DOUBLE. That is the wrong-command
                // hazard spec 6.7/FR-12 exist to prevent, so the first press must
                // resolve as its own SINGLE and the new press start fresh.
                awaiting_second_ = false;
                pending_single_ = false;
                // Emit the FIRST button's SINGLE (Emit reads `button_`), then
                // adopt the new button for the press now beginning.
                button_ = prev_button;
                Emit(Gesture::kSingle, now_ms, out);
                button_ = button_index;
                emitted = true;
                // long_fired_ was cleared above, so the new press can still become
                // a LONG; press_started_ms_ is this press's start, already set.
            } else if (!bindings.has_double && !bindings.has_long) {
                // No ambiguity to resolve (spec 6.6): nothing this button binds
                // could differ from a SINGLE, so the press IS the single. Making
                // it wait out the double-press window would add ~500 ms of lag to
                // the commonest gesture in the product for no reason.
                Emit(Gesture::kSingle, now_ms, out);
                long_fired_ = true;  // sentinel: this press is spent
                emitted = true;
            }
        }
        // FR-10: LONG fires at the threshold, not on release. A held button must
        // act immediately -- waiting for the finger to lift is the difference
        // between "responsive" and "broken" to a driver.
        if (!long_fired_ && bindings.has_long &&
            (now_ms - press_started_ms_) >= timings_.long_press_ms) {
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
        if (!long_fired_ && bindings.has_long && held >= timings_.long_press_ms) {
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
            if (bindings.has_double) {
                pending_single_ = true;
                awaiting_second_ = true;
                released_at_ms_ = now_ms;
            } else {
                // LONG-but-not-DOUBLE: the release is the resolution. Only the
                // threshold was in question, and it was not reached.
                Emit(Gesture::kSingle, now_ms, out);
                return true;
            }
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
