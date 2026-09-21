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
 * Which gestures a button's bindings actually cover (spec 6.6).
 *
 * This is what makes the resolve ADAPTIVE. A button that binds neither DOUBLE
 * nor LONG has no ambiguity to wait out: the press is already a SINGLE the
 * moment it is classified, and delaying it by the double-press window would add
 * ~500 ms of lag to the commonest gesture in the product for no reason. A button
 * that DOES bind one of them must wait, because the two bindings name different
 * output voltages and driving an early guess would make the head unit act twice.
 *
 * Defaults to `true`/`true`: the conservative assumption is that a button could
 * mean anything, which is also the behaviour every unconstrained caller had
 * before this parameter existed.
 */
struct GestureBindings {
    bool has_double = true;
    bool has_long = true;
};

/*
 * Presses to gestures. Deterministic and clock-injected (FR-8): every timing
 * decision comes from the now_ms argument, so the whole grammar is exercised
 * with no sleeps.
 *
 * SINGLE is delayed only by the ambiguity its button actually has (spec 6.6):
 *   - no DOUBLE, no LONG bound -> emitted at the press, immediately
 *   - LONG bound               -> waits for the threshold (LONG) or release
 *                                 (SINGLE)
 *   - DOUBLE bound             -> waits out the double-press window after
 *                                 release before settling on SINGLE
 *
 * LONG is not delayed: it fires the moment the threshold elapses, because a held
 * button must act immediately rather than on release (FR-10). It is also what
 * the head unit is given as a single pulse -- a long press is not a held key,
 * it is a different command (spec 6.6).
 */
class GestureStateMachine {
public:
    explicit GestureStateMachine(const GestureTimings &timings);

    // Returns true and fills *out when a gesture completed. out may be null.
    bool Update(ChannelLevel level, uint8_t button_index, uint64_t now_ms,
                GestureEvent *out, const GestureBindings &bindings = {});

    void Reset();

    /*
     * The button of the press currently in flight, or 0xFF when none is.
     *
     * The caller needs it because the resolve is PER BUTTON (spec 6.6 rule 3):
     * `Update` takes the `GestureBindings` of the button being resolved, and once
     * a press is released the classifier reports no button at all -- so the
     * caller cannot ask it which button's bindings to supply for the release. The
     * machine already tracks the button internally; this exposes the same fact
     * rather than making the caller keep a second copy that could disagree.
     */
    uint8_t Button() const { return button_; }

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
