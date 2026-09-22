#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"
#include "Feedback/BuzzerGrammar.h"
#include "Feedback/LedGrammar.h"
#include "HAL/IHAL.h"
#include "Learning/LearnSession.h"

// The AUX1 input's own one-button profile: a momentary switch to ground, so its
// idle IS the rail and its window is wide. Declared here rather than inlined at
// the call site because the caller and the wizard must agree on the threshold --
// a second definition is a second answer to "what counts as an AUX1 press".
LadderProfile Aux1ProfileDefault();

/*
 * One input the headless learn may program (spec 7.4/7.5, FR-31).
 *
 * `is_ladder` separates the two kinds of source the learn serves: a steering-wheel
 * LADDER (SWC1/SWC2), where a press is one of many levels and learning ADDS A
 * BUTTON to the channel's ladder; and an AUX SWITCH (AUX2/AUX3), which is pressed
 * or not, where learning records the switch's own window. The distinction is
 * carried here rather than re-derived by the wizard because the CALLER owns the
 * Config and knows which inputs exist.
 */
struct LearnInput {
    uint8_t              wire_channel;  // the `event` frame's channel value
    AdcChannel           adc;           // the pin this input's level is read from
    int                  idle_mv;       // this input's LIVE idle; 0 => not usable
    bool                 is_ladder;     // SWC ladder vs AUX switch
    const LadderProfile *existing;      // ladder seed; nullptr => an empty ladder
    // The config's own id for this input. A ladder has NO id here -- its button
    // ids are generated (`swc<ch>_bt<n>`) because a ladder button is not a config
    // entry until it is learned. An AUX SWITCH's id IS a config entry's
    // (`aux[].id`), so the caller supplies it and the learn does not invent one.
    const char          *id;
};

// SWC1, SWC2, AUX2, AUX3 -- the inputs a headless learn can name. AUX1 is the
// modifier (the programming hold) and is never a target.
constexpr int kMaxLearnInputs = 4;

struct LearnInputs {
    LearnInput in[kMaxLearnInputs];
    uint8_t    count = 0;
};

/*
 * The headless learn wizard (spec 7.4 / FR-31): teach a button with no app and
 * no link, using AUX1 as the MODIFIER, because the user may not have the head
 * unit out of the dash.
 *
 * **The interaction is the 2022 one, and it is stateless from the user's side.**
 * Hold AUX1, perform the press on WHICHEVER input is being programmed, release
 * AUX1. The wizard names the input by which one LEFT ITS IDLE -- there is no menu
 * and no channel selection (spec 7.5). This is exactly how the 2022 firmware
 * named the key: `swc_input_driver.cpp`'s `is_key_pressed()` set `key = KEY1` (or
 * `KEY2`) from whichever input's average had moved off its baseline, and
 * `program_alt_key(level, key, is_double_press)` captured level and input in one
 * operation while the modifier was held.
 *
 * **The wizard owns no Config and no store.** It measures and names the input; the
 * CALLER applies the result (spec 7.4). That keeps it host-testable with no NVS
 * and leaves the one write path in the caller.
 */
class LearnWizard {
public:
    enum class State {
        kIdle,       // not learning
        kArmed,      // AUX1 held; watching for an input to leave its idle
        kSampling,   // an input has been named; its level is being measured
        kExit,       // finished, prompts already played
    };

    LearnWizard(IHAL *hal, BuzzerGrammar *buzzer, LedGrammar *leds);

    // AUX1 held this long enters the learn (spec 7.5's trigger). Long enough that
    // a stray touch does not start a learn in traffic, hence the deliberate hold.
    static constexpr uint32_t kEnterHoldMs = 1500;
    // A safety cap on one measurement: if the input never returns to idle, commit
    // what we have rather than sampling forever, so a stuck line cannot wedge the
    // wizard.
    static constexpr uint32_t kSampleMaxHoldMs = 8000;
    // How far off its idle an input must move before it counts as the one being
    // programmed. Well clear of ADC noise and well under any real button's offset
    // (the smallest window in spec 3.7's example is 110 mV wide).
    static constexpr int kDetectMv = 40;
    // A learned SWITCH window's half-width. An AUX switch reads near the common
    // when pressed and at the rail when not, so half the rail admits every real
    // press without reaching the idle band -- the same reasoning as the AUX1
    // profile's own wide window.
    static constexpr int kSwitchToleranceMv = 1600;

    /*
     * Begin a learn: AUX1 has been held past `kEnterHoldMs`. Plays PROGRAM_ENTER
     * and hands the LEDs to the wizard.
     *
     * No input is named here, and that is the whole point: the user has not
     * touched a wheel button yet -- that IS the next step -- so naming one would
     * be the channel-selection menu this interaction deliberately does not have.
     */
    void Arm(uint64_t now_ms);

    /*
     * One tick. Reads the candidate inputs (and, once one is named, that input's
     * level) through the HAL, so the host tests drive it purely through MockHal's
     * ADC values and clock.
     *
     * `inputs` is supplied by the caller EVERY tick because it carries the LIVE
     * idle of each candidate, which moves with the rail; a wizard that cached them
     * would compare a reading against a stale denominator.
     */
    void Tick(const LearnInputs &inputs, uint64_t now_ms, int temp_tenths_c);

    /*
     * AUX1 released. Commits the input being measured (spec 7.5: "release AUX1 ->
     * BEEP PROGRAM_SAVED ... the binding is written"), or cancels if no input was
     * ever named. The caller applies `Profile()` when this returns `kNone` AND
     * `ConsumeCommitted()` was consumed.
     */
    LearnReject Release(uint64_t now_ms);

    /*
     * Abandon a running learn with NO commit: the 3 s maintenance escalation, or a
     * link teardown. Distinct from `Release` because committing here would store a
     * half-measured button the user never finished.
     */
    void Abandon(uint64_t now_ms);

    bool Active() const { return state_ == State::kArmed || state_ == State::kSampling; }
    State CurrentState() const { return state_; }

    // True for exactly one tick after a successful commit, then cleared. The
    // CALLER persists: this class holds no Config and no store.
    bool ConsumeCommitted();

    // True for exactly one tick after Release()/Abandon(), then cleared.
    bool ConsumeExited();

    // Which input the learn named, and whether it is a ladder or a switch. Valid
    // once an input has been detected; the caller uses these to decide how to
    // apply `Profile()`.
    uint8_t TargetWireChannel() const { return target_wire_channel_; }
    bool    TargetIsLadder() const { return target_is_ladder_; }

    /*
     * The profile the learn produced. For a ladder input it is the channel's
     * existing buttons (rebased onto the live rail) PLUS the measured button, so
     * the caller can ASSIGN it over the channel's ladder without deleting what was
     * already there (spec 7.4). For a switch input it holds one button -- the
     * measured switch window -- and the caller writes its centre and tolerance
     * into that AUX input.
     */
    const LadderProfile &Profile() const { return profile_; }
    int ButtonCount() const { return profile_.count; }

    // The most recent outcome, so a caller can surface it (FR-29).
    LearnReject LastResult() const { return last_result_; }

private:
    void Detect(const LearnInputs &inputs, uint64_t now_ms);
    // Name this input as the target and prepare to measure it. `first_mv` is the
    // reading that revealed it, used to recognise a RE-LEARN of an existing button
    // before the measurement starts (a re-measure lands inside the old window by
    // definition, so leaving that entry in the neighbour set would refuse the
    // button for being too close to ITSELF).
    void BeginOn(const LearnInput &in, int first_mv);
    void Sample(const LearnInput &in, uint64_t now_ms, int temp_tenths_c);
    // Turn the measurement into the profile. Called ONLY from `Release`, so the
    // commit point is the AUX1 release and there is exactly one.
    void Finish(uint64_t now_ms);
    // The seed index the level `mv` is a re-measure of, or -1 for a new button.
    int  MatchingExistingIndex(const LadderProfile *p, int mv) const;
    // The id for a newly measured ladder button: `swc<ch>_bt<n>` with `n` the next
    // free slot number, so a headless learn names its button the way the old slot
    // menu did.
    void NewSlotId(int channel_index, char *buf, size_t n) const;

    IHAL          *hal_;
    BuzzerGrammar *buzzer_;
    LedGrammar    *leds_;

    LearnSession session_;

    State         state_ = State::kIdle;
    // The committed ladder: the seed (existing buttons, rebased) plus the measured
    // button. And, separately, the session's NEIGHBOUR set, which is the seed
    // MINUS the entry being re-measured -- the two differ only for a re-learn, and
    // keeping both is what lets a re-learn correct a button instead of being
    // refused as too close to itself.
    LadderProfile profile_{};
    LadderProfile neighbour_{};

    uint8_t    target_wire_channel_ = 0;
    bool       target_is_ladder_ = false;
    AdcChannel target_adc_ = ADC_CH_SWC1;
    int        target_idle_mv_ = 0;
    int        target_existing_index_ = -1;
    // The target's own id from the caller's list, for a SWITCH (an AUX config
    // entry names itself). A ladder's ids are generated, so this is unused there.
    const char *in_id_ = nullptr;
    // The rail the SEEDED buttons are in, so the first measurement can rescale
    // them onto the live rail -- a profile has ONE denominator, and a re-learn on
    // a moved rail must rebase the siblings or the WRONG button fires.
    int        seed_idle_mv_ = 0;
    bool       seed_framed_ = false;
    uint64_t   sample_started_ms_ = 0;
    bool       have_measurement_ = false;

    // Switch accumulation (an AUX input is not a ladder, so it does not go through
    // LearnSession's ladder gates -- a short to ground reads ~0 mV, which
    // `LadderProfileIsValid` refuses as an `mv_center`).
    int64_t    switch_sum_mv_ = 0;
    int        switch_count_ = 0;

    LearnReject last_result_ = LearnReject::kNone;
    // One-tick flags, cleared by the matching Consume*().
    bool       committed_ = false;
    bool       exited_ = false;
};
