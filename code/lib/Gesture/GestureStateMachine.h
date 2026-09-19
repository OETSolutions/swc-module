#pragma once

#include <stdint.h>

#include "Gesture/PressClassifier.h"

enum class Gesture { kNone, kSingle, kDouble, kLong };

struct GestureEvent {
    Gesture  gesture;
    uint8_t  button_index;
    uint64_t at_ms;
};

/*
 * Presses to gestures. Deterministic and clock-injected (FR-8): every timing
 * decision comes from the now_ms argument, so the whole grammar is exercised
 * with no sleeps.
 *
 * SINGLE is necessarily delayed by the double-press window -- it cannot be
 * emitted until we know a second press is not coming. LONG is not delayed: it
 * fires the moment the threshold elapses, because a held button must act
 * immediately rather than on release (FR-10).
 */
class GestureStateMachine {
public:
    explicit GestureStateMachine(const GestureTimings &timings);

    // Returns true and fills *out when a gesture completed. out may be null.
    bool Update(ChannelLevel level, uint8_t button_index, uint64_t now_ms, GestureEvent *out);

    void Reset();

private:
    void Emit(Gesture g, uint64_t now_ms, GestureEvent *out);

    GestureTimings timings_;
    bool           pressed_ = false;
    uint8_t        button_ = 0xFF;
    uint64_t       press_started_ms_ = 0;
    uint64_t       released_at_ms_ = 0;
    bool           long_fired_ = false;
    bool           awaiting_second_ = false;   // first press seen, inside double window
    bool           pending_single_ = false;
};
