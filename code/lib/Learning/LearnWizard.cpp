#include "Learning/LearnWizard.h"

#include <stdio.h>
#include <string.h>

namespace {

// Which ADC channel the physical button being learned is on. The wizard is told
// the channel index, not the ADC channel, so the mapping lives here once.
AdcChannel LevelChannelFor(int channel) {
    return (channel == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2;
}

// The AUX1 input's own profile: one button, and it is a SWITCH TO GROUND, so a
// press reads near zero and a release sits at the rail. The window is wide
// because this is a switch, not a ladder -- there is no adjacent button to stay
// clear of, so the only job is "pressed vs not pressed".
constexpr int kAuxPressedMv = 100;

// How far the level must move off the idle reference before the user counts as
// "pressing the button" during the prompt. 40 mV is well clear of ADC noise and
// well under any real button's offset (the smallest window in spec 3.7's example
// is 110 mV wide).
constexpr int kPromptPressDetectMv = 40;

// The generated id for a slot: `swc<ch>_bt<slot>`, with `ch` and `slot` 1-based.
//
// ONE definition, because two places need it and they must agree exactly: the
// generator (which names the button it stores) and the neighbour exclusion (which
// must remove that same entry from the set the session checks against -- remove the
// wrong one and re-learning a button is refused as too close to itself). Written
// twice, a change to either would silently break the correction path.
void FormatSlotId(char *buf, size_t n, int channel_index, int slot) {
    snprintf(buf, n, "swc%d_bt%d", channel_index + 1, slot);
}

}  // namespace

LadderProfile Aux1ProfileDefault() {
    LadderProfile p{};
    p.source = 0;
    // AUX inputs are pulled to the rail and shorted to ground when pressed, so
    // the idle IS the rail. Nominal 3.3 V is close enough for a threshold: the
    // exact rail is not a thing this profile needs to be accurate about, only
    // "pressed vs not".
    p.learned_idle_mv = 3300;
    p.count = 1;
    p.buttons[0].mv_center = kAuxPressedMv;
    // 1600 mV of half-width at a 3300 mV idle is ~485 permille, so the accept
    // window reaches ~1700 mV: anything below that is a press, anything at the
    // rail is not.
    p.buttons[0].mv_tolerance = 1600;
    return p;
}

LearnWizard::LearnWizard(IHAL *hal, BuzzerGrammar *buzzer, LedGrammar *leds,
                         const LadderProfile &aux_profile, const GestureTimings &timings)
    : hal_(hal), buzzer_(buzzer), leds_(leds),
      aux_(aux_profile, timings), aux_profile_(aux_profile) {}

void LearnWizard::Enter(uint64_t now_ms, bool aux_held, const LadderProfile *existing) {
    state_ = State::kSelectButton;
    // SEED from what the channel already has, rather than starting empty, because
    // `Commit`'s profile is ASSIGNED over the channel's ladder by the caller -- so
    // starting empty means a learn that adds nothing but also DELETES everything.
    // Re-entering the wizard to re-measure one button used to wipe the rest
    // (measured: three learned buttons became one). A null `existing` is the
    // genuinely-empty case: a test driving a bare wizard, or a channel with no
    // learned ladder yet.
    profile_ = LadderProfile{};
    if (existing != nullptr) {
        const uint8_t n = (existing->count < kLadderMaxButtons) ? existing->count
                                                                : kLadderMaxButtons;
        for (uint8_t i = 0; i < n; ++i) profile_.buttons[i] = existing->buttons[i];
        profile_.count = n;
        // The frame the seeded buttons' millivolts are in, so the first tick can
        // rescale them onto the LIVE rail this session measures at. A profile has
        // ONE denominator, so leaving the seeded entries in their old frame while
        // the newly measured button lands in the live one makes the stored ladder
        // self-inconsistent -- see `Tick`.
        seed_idle_mv_ = existing->learned_idle_mv;
    } else {
        seed_idle_mv_ = 0;
    }
    seed_framed_ = false;
    press_count_ = 0;
    selected_slot_ = 0;
    any_press_ = false;
    last_press_ms_ = now_ms;
    beeps_owed_ = 0;
    aux_was_pressed_ = false;
    aux_seen_idle_ = !aux_held;
    last_result_ = LearnReject::kNone;
    // `prompt_pressed_` is cleared ONLY here and in ServicePrompt, and this is the
    // one that matters for an abandoned prompt. Exit(now_ms) can arrive while the
    // state is kPrompt -- the AUX1 hold is the same gesture at 1.5 s and 3 s, so a
    // user who over-holds to program lands in maintenance and exits the running
    // learn -- and ServicePrompt's clear never runs on that path. Left set, the
    // next learn skips its wait-for-press gate, samples the idle line on the first
    // tick, and rejects with "at_idle" before the user's hand is on the button:
    // the LEARN_REJECT tone fires at the instant the prompt starts and the wizard
    // is back at the menu, so the press it was waiting for can never be seen.
    prompt_pressed_ = false;
    aux_.Reset();

    if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kProgramEnter);
    // Spec 7.4 step 1: LED_STAT alternates, LED2 off.
    if (leds_ != nullptr) {
        leds_->SetStat(LedStatPattern::kAlternate);
        leds_->Set2(Led2Pattern::kOff);
    }
}

void LearnWizard::Exit(uint64_t now_ms) {
    (void)now_ms;   // the exit prompt is immediate; the parameter is kept for a
                    // caller that may want a timed fade and to match Enter().
    state_ = State::kExit;
    beeps_owed_ = 0;
    exited_ = true;
    if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kProgramExit);
    if (leds_ != nullptr) leds_->SetStat(LedStatPattern::kSolid);
}

bool LearnWizard::ConsumeCommitted() {
    const bool v = committed_;
    committed_ = false;
    return v;
}

bool LearnWizard::ConsumeExited() {
    const bool v = exited_;
    exited_ = false;
    return v;
}

void LearnWizard::Tick(int channel, uint64_t now_ms, int idle_mv, int temp_tenths_c) {
    if (hal_ == nullptr) return;
    // Rescale the SEEDED buttons from the rail they were stored against onto the
    // LIVE rail, once, before anything compares or commits them.
    //
    // A `LadderProfile` has ONE `learned_idle_mv` and every button's millivolts
    // are in that frame, but this session measures the new button on the live rail
    // and stamps the live idle as the profile's denominator. If the rail has moved
    // since the seeded buttons were learned, leaving them alone stores two frames
    // under one denominator, and classification then reads them on the wrong
    // scale: the press lands in whichever window it now falls inside and the WRONG
    // button fires. Measured through the real learn path -- two buttons 200 mV
    // apart learned at 2835 mV, one re-learned at 2693 mV (-5 %): pressing the
    // other fired the 794-permille button when the press was at 864.
    //
    // Done here, on the first tick with a usable reference, rather than in Enter:
    // Enter does not receive the live idle. `NeighbourSetExcludingTheSlotBeingLearned`
    // reads `profile_`, so the session's neighbour set and the committed ladder
    // both see the converted values -- one conversion, one frame.
    if (!seed_framed_ && idle_mv > 0 && seed_idle_mv_ > 0) {
        LadderProfileRebase(profile_, seed_idle_mv_, idle_mv);
        seed_framed_ = true;
    }
    // Remembered so the id-based neighbour exclusion can build the id this learn
    // is about to produce; `Tick` is the only place the channel arrives.
    channel_ = channel;

    if (state_ == State::kSelectButton) {
        ServiceSelect(now_ms);
    } else if (state_ == State::kPrompt) {
        ServicePrompt(channel, now_ms, idle_mv, temp_tenths_c);
    }
    ServiceBeeps();
}

void LearnWizard::ServiceSelect(uint64_t now_ms) {
    const int mv = hal_->adc_read_mv(hal_->ctx, ADC_CH_AUX1);
    // **A FAILED CONVERSION IS NOT A PRESS.** `IHAL::adc_read_mv` returns -1 on
    // error (0 mV is a legal reading -- AUX1 shorts to ground when pressed, so 0
    // is the FULLY PRESSED level here). Fed to the classifier, -1 becomes a ratio
    // of ~0 permille against the profile's 3300 mV idle, which lands INSIDE the
    // AUX window (centre 30 permille, half-width 485) and classifies as
    // **kPressed** -- so one bad conversion counted a selection press the user
    // never made, and a run of them would walk the slot menu with nobody
    // touching the button. Hold instead: no press, no re-arm, no advancement.
    // `SystemOrchestrator`'s own AUX1 read guards this sentinel (N-43).
    if (mv < 0) return;
    // The same classifier the SWC channels use, so "what counts as a press" has
    // exactly one definition in the firmware.
    const ChannelLevel lvl = aux_.Update(mv, aux_profile_.learned_idle_mv, now_ms);
    const bool pressed = (lvl == ChannelLevel::kPressed);

    // Re-arm the gate on the RAW level, not on the classifier's latched one.
    //
    // This is the whole mechanism that keeps the ENTERING hold from being counted
    // as the first selection press -- without it slot 1 is unreachable and the user
    // asking for the first button gets the second. The classifier needs several
    // ticks to latch and reports kIdle meanwhile, so a gate that waits for a
    // classifier-level "not pressed" is satisfied by its own debounce delay and the
    // hold is counted anyway. (Both alternatives were tried and measured, and both
    // counted it.) A raw comparison against the profile's own window has no
    // latency, and it lets the gate arm while AUX1 is genuinely released, so a
    // programmatically-requested learn -- where no hold is in flight -- counts the
    // user's first press normally.
    if (mv >= (aux_profile_.buttons[0].mv_center + aux_profile_.buttons[0].mv_tolerance)) {
        aux_seen_idle_ = true;
    }

    if (pressed && !aux_was_pressed_ && aux_seen_idle_) {
        ++press_count_;
        any_press_ = true;
        last_press_ms_ = now_ms;
        // Spec 7.4 step 2: PROGRAM_STEP is ONE pulse and the caller repeats it
        // n times. Re-Play on each press is what makes the count audible.
        beeps_owed_ = 1;
        if (leds_ != nullptr) leds_->Set2(Led2Pattern::kFlick);
    }
    aux_was_pressed_ = pressed;

    // A pause ends selection. Zero presses is not a selection -- it is a stray
    // hold, and entering a learn on it would be a phantom.
    if (any_press_ && (now_ms - last_press_ms_) >= kSelectGapMs) {
        selected_slot_ = press_count_;
        state_ = State::kPrompt;
        prompt_started_ms_ = now_ms;
        session_.Start(NeighbourSetExcludingTheSlotBeingLearned());
        // Spec 7.4 step 3: LEARN_PROMPT, LEDs alternating.
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnPrompt);
        if (leds_ != nullptr) leds_->SetStat(LedStatPattern::kAlternate);
    }
}

/*
 * `profile_` minus the entry this learn is REPLACING, which is what the session
 * needs as its neighbour set.
 *
 * The id is the key, and the id is this class's own vocabulary: it generates
 * `swc<ch>_bt<slot>`, so it can compute the id it is about to produce and remove
 * exactly that entry. Deciding by id rather than by array index is the part that
 * matters, because `profile_` is seeded from the channel's ladder and an APP-made
 * ladder uses app slugs in whatever order it likes -- index `slot - 1` is a
 * different button than slot `slot` as soon as that happens.
 *
 * Without this, re-learning a button is refused: the session would see the old
 * entry as a neighbour, and a re-measure lands inside the old window by
 * definition, so every attempt to correct a button reports
 * `too_close_to_existing` -- blaming the button for being too close to itself.
 * The exclusion compares against the ONE id `FormatSlotId` generates for this
 * slot (1-based, `swc<ch>_bt<slot>`); there is no second spelling to hedge over.
 */
LadderProfile LearnWizard::NeighbourSetExcludingTheSlotBeingLearned() const {
    LadderProfile out = profile_;
    const int slot = selected_slot_ > 0 ? selected_slot_ : 1;

    // The id `ServicePrompt` will generate for this slot, spelled the SAME way and
    // in ONE place. Hedging across two conventions here would be a second home for
    // the id format -- the defect this project keeps re-finding -- so it is worth
    // being exact: the generator numbers slots from 1 (`slot 1 -> "swc1_bt1"`, the
    // convention `SystemOrchestratorTest` already asserts).
    char learning_id[32];
    FormatSlotId(learning_id, sizeof(learning_id), channel_, slot);

    uint8_t kept = 0;
    for (uint8_t i = 0; i < out.count && i < kLadderMaxButtons; ++i) {
        if (strcmp(profile_.buttons[i].id, learning_id) == 0) continue;
        out.buttons[kept++] = profile_.buttons[i];
    }
    out.count = kept;
    return out;
}

void LearnWizard::ServicePrompt(int channel, uint64_t now_ms, int idle_mv, int temp_tenths_c) {
    const int level_mv = hal_->adc_read_mv(hal_->ctx, LevelChannelFor(channel));
    // **A FAILED CONVERSION IS NOT A READING.** `IHAL::adc_read_mv` returns -1 on
    // error (0 mV is legal), and `off_idle = level_mv - idle_mv` turns -1 into a
    // large NEGATIVE excursion -- `(off_idle < -kPromptPressDetectMv)` is true --
    // so a bad conversion was read as the user pressing, and `AddSample(-1, ...)`
    // then latched `out_of_range_seen_`, which `Commit` reports as
    // `out_of_range`. One ADC glitch anywhere in the prompt therefore blamed the
    // user's wiring for a learn that never got a usable measurement. Skip the
    // tick instead; the prompt holds its state and the safety cap still bounds it.
    if (level_mv < 0) return;
    // The RAIL during learn (spec 3.4's `learned_at_rail_mv`). There is no rail
    // sense channel on this board (AdcChannel carries SWC1/SWC2/TEMP/AUX1-3/
    // KEY_SENSE1-2 and none of them is +3V3), so this is the board's nominal rail
    // and is recorded as such. It is deliberately NOT read from the HAL. The
    // constant is shared with the app-driven learn path in `CommandRouter`, so the
    // two cannot record a different value for the same field.
    const int rail_mv = kNominalRailMv;

    // WAIT for the user to actually press. Sampling on every tick rejected the
    // learn with "at_idle" before the user's hand was even on the button, which
    // made the wizard unusable -- and the rejection blamed the user for
    // something they had not done yet. Spec 7.4's step 3 (prompt) and step 4
    // (user holds the button) are two different moments.
    const int off_idle = level_mv - idle_mv;
    const bool pressing = (off_idle < -kPromptPressDetectMv) || (off_idle > kPromptPressDetectMv);
    if (!prompt_pressed_) {
        if (!pressing) return;          // still waiting for a press
        prompt_pressed_ = true;
        prompt_started_ms_ = now_ms;
        session_.Start(NeighbourSetExcludingTheSlotBeingLearned());
    }

    if (pressing) {
        session_.AddSample(level_mv, idle_mv, static_cast<MilliVolt>(rail_mv),
                           static_cast<int16_t>(temp_tenths_c), now_ms);
        // Keep sampling until the user lets go, or the safety cap fires.
        if ((now_ms - prompt_started_ms_) < kPromptMaxHoldMs) return;
    }
    // Committing on RELEASE, without sampling the release reading itself. That
    // last reading is the level travelling back to idle, so including it would
    // put the whole press-to-idle swing into the spread and reject every learn as
    // too noisy -- which is exactly what happened before this line existed.

    LadderButton out{};
    // The slot's id is generated, because a headless learn has no app to name it
    // (the app path supplies a real slug). e.g. "swc1_bt2".
    //
    // Formatted into a buffer large enough for any `int`, then copied with an
    // explicit bound. Formatting straight into the 16-byte field tripped
    // -Werror=format-truncation on the device compiler, which is right to
    // complain: it cannot prove an `int` stays short, and neither can a reader.
    const int slot = selected_slot_ > 0 ? selected_slot_ : 1;
    char idbuf[32];
    FormatSlotId(idbuf, sizeof(idbuf), channel, slot);
    memset(out.id, 0, sizeof(out.id));
    memcpy(out.id, idbuf, strlen(idbuf) < sizeof(out.id) ? strlen(idbuf) : sizeof(out.id) - 1);
    char namebuf[32];
    snprintf(namebuf, sizeof(namebuf), "Button %d", slot);
    memset(out.name, 0, sizeof(out.name));
    memcpy(out.name, namebuf,
           strlen(namebuf) < sizeof(out.name) ? strlen(namebuf) : sizeof(out.name) - 1);

    const LearnReject r = session_.Commit(&out);
    last_result_ = r;
    if (r == LearnReject::kNone) {
        // REPLACE IN PLACE when this id is already on the profile, rather than
        // appending a second button with the same id.
        //
        // Appending was reachable and wrong in two ways. `profile_` is seeded from
        // the channel's ladder (see Enter), so re-measuring a button the channel
        // already carries would add a duplicate; and within one session the slot
        // menu can select the same slot twice. A duplicate id is not rejected
        // anywhere -- `ConfigValidate` does not check it -- yet `BindingResolve`
        // and `BindingsForButton` both find a binding by `strcmp` on the id, so two
        // buttons sharing one id make "which window does this binding mean"
        // ambiguous between two different voltages.
        int target = -1;
        for (uint8_t i = 0; i < profile_.count; ++i) {
            if (strcmp(profile_.buttons[i].id, out.id) == 0) { target = i; break; }
        }
        if (target >= 0) {
            profile_.buttons[target] = out;
        } else if (profile_.count < kLadderMaxButtons) {
            profile_.buttons[profile_.count] = out;
            ++profile_.count;
        } else {
            // At kLadderMaxButtons with no matching id there is no room to add.
            // The measurement is a real one and the SESSION accepted it, but it
            // cannot be stored -- so this is a REJECTION, not a commit.
            //
            // Reporting it as `kNone` was a silent-failure defect, and the two
            // learn paths disagreed about it: `HandleLearnCommit` (the app path)
            // refuses with `no_space` for the same condition. The caller persists
            // on `kNone` (`ConsumeCommitted` -> `ApplyLearnedProfile` ->
            // `store_->Save`), so LEARN_OK played while the ladder went back to NVS
            // UNCHANGED and `LastLearnPersisted()` returned true -- the user hears
            // success and the button is not there. Spec 7.4 step 6 makes "accept"
            // mean "BEEP LEARN_OK, store LadderButton"; FR-29 requires a rejection
            // to say WHY. Reachable with no contrivance: a ladder the app filled to
            // 16 with app slugs, selected headlessly (the wizard generates its own
            // `swc1_bt<n>`, so it matches no id and takes this append path), or a
            // seventeenth AUX1 press.
            last_result_ = LearnReject::kNoSpace;
        }
    }

    if (last_result_ == LearnReject::kNone) {
        // The IDLE REFERENCE, and forgetting it is how the learned button ends up
        // dead. A LadderProfile is useless without it: spec 6.3 normalizes every
        // centre by the idle the button was measured at, and `LadderClassify`
        // reports kFault rather than classifying when the reference is zero. So a
        // profile whose buttons were filled but whose idle stayed at the
        // default-constructed 0 would commit happily, beep LEARN_OK, and then
        // classify nothing -- correct-looking feedback over a button that does not
        // work. The session holds the idle it actually measured against.
        profile_.learned_idle_mv = session_.LearnedIdleMv();
        committed_ = true;   // the caller persists; see ConsumeCommitted()
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnOk);
    } else {
        // A REAL rejection: say WHY (FR-29) rather than a generic failure. The
        // full-ladder case above lands here too, which is what makes a learn that
        // stored nothing sound like the refusal it is.
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnReject);
    }

    // Back to selection for the next button (spec 7.4 step 7), whether the learn
    // succeeded or not -- a rejection must not wedge the wizard on one prompt.
    state_ = State::kSelectButton;
    prompt_pressed_ = false;
    press_count_ = 0;
    any_press_ = false;
    aux_was_pressed_ = false;
    aux_.Reset();
    last_press_ms_ = now_ms;
}

void LearnWizard::ServiceBeeps() {
    if (beeps_owed_ <= 0 || buzzer_ == nullptr) return;
    // Play() REPLACES while busy, so re-Play only once the previous pulse has
    // finished -- otherwise the count collapses into one continuous tone and the
    // "menu depth" the beeps are supposed to convey is lost.
    if (!buzzer_->Busy()) {
        buzzer_->Play(BuzzerPattern::kProgramStep);
        --beeps_owed_;
    }
}
