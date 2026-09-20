#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"
#include "Feedback/BuzzerGrammar.h"
#include "Feedback/LedGrammar.h"
#include "Gesture/PressClassifier.h"
#include "HAL/IHAL.h"
#include "Learning/LearnSession.h"

// The AUX1 input's own one-button profile: a momentary switch to ground, so its
// idle IS the rail and its window is wide. Declared here rather than inlined at
// the call site because the caller and the wizard must agree on the threshold --
// a second definition is a second answer to "what counts as an AUX1 press".
LadderProfile Aux1ProfileDefault();

/*
 * The headless learn wizard (spec 7.4 / FR-31): teach a button with no app and
 * no link, using AUX1 as the select button plus buzzer and LED prompts, because
 * the user may not have the head unit out of the dash.
 *
 * **AUX1 press counting uses the SAME PressClassifier as the steering-wheel
 * channels**, with a one-button ladder profile supplied by the caller. A second
 * debounce implementation is the two-homes defect with a timing bug attached:
 * the two would disagree about what counts as a press, and only one of them
 * would ever be tested.
 *
 * The beep COUNT is the menu depth -- a fixed-pitch buzzer cannot say "which
 * button" with pitch (spec 7.2/7.5), so it says it with repetitions:
 * `PROGRAM_STEP` is ONE pulse and the caller repeats it n times. This class is
 * that caller, and it re-Plays on its own because `BuzzerGrammar::Play` while
 * busy REPLACES rather than queues.
 */
class LearnWizard {
public:
    enum class State {
        kIdle,          // not learning
        kSelectButton,  // counting AUX1 presses to choose which slot
        kPrompt,        // the user is holding the physical button; sampling
        kExit,          // finished, prompts already played
    };

    LearnWizard(IHAL *hal, BuzzerGrammar *buzzer, LedGrammar *leds,
                const LadderProfile &aux_profile, const GestureTimings &timings);

    // AUX1 held this long enters (spec 7.5's trigger). Long enough that a stray
    // touch does not start a learn in traffic, hence the deliberate hold.
    static constexpr uint32_t kEnterHoldMs = 1500;
    // A pause this long after the last AUX1 press ends selection.
    static constexpr uint32_t kSelectGapMs = 1200;
    // A safety cap on one prompt: if the level never returns to idle, commit what
    // we have rather than wedging the wizard on a stuck input.
    static constexpr uint32_t kPromptMaxHoldMs = 5000;

    void Enter(uint64_t now_ms);
    void Exit(uint64_t now_ms);

    bool Active() const { return state_ != State::kIdle && state_ != State::kExit; }
    State CurrentState() const { return state_; }

    // One tick. Reads AUX1 and the channel being learned from the HAL, so the
    // host tests drive it purely through MockHal's ADC values and clock.
    void Tick(int channel, uint64_t now_ms);

    // How many AUX1 presses have been counted toward the selection.
    int PressCount() const { return press_count_; }
    // The 1-based button slot being selected.
    int SelectedSlot() const { return selected_slot_; }

    // The profile being filled, and how many buttons it holds.
    const LadderProfile &Profile() const { return profile_; }
    int ButtonCount() const { return profile_.count; }

    // The most recent outcome, so a caller can surface it (FR-29).
    LearnReject LastResult() const { return last_result_; }

private:
    void ServiceSelect(uint64_t now_ms);
    void ServicePrompt(int channel, uint64_t now_ms);
    // Re-Play the beep sequence the current press count implies, one pulse at a
    // time, because Play() replaces rather than queues.
    void ServiceBeeps();

    IHAL          *hal_;
    BuzzerGrammar *buzzer_;
    LedGrammar    *leds_;

    PressClassifier aux_;         // the ONE debounce implementation
    LearnSession    session_;

    State      state_ = State::kIdle;
    LadderProfile profile_{};
    LadderProfile aux_profile_{};

    int      press_count_ = 0;
    int      selected_slot_ = 0;
    uint64_t last_press_ms_ = 0;
    bool     any_press_ = false;

    // Beep sequencing: `beeps_owed_` pulses still to play, re-Played as the
    // previous one finishes.
    int beeps_owed_ = 0;
    bool aux_was_pressed_ = false;

    uint64_t prompt_started_ms_ = 0;
    // The prompt waits for a press before it starts sampling, so this tracks
    // whether the user has actually pressed yet.
    bool     prompt_pressed_ = false;
    LearnReject last_result_ = LearnReject::kNone;
};
