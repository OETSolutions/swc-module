#include "Learning/LearnWizard.h"

#include <stdio.h>
#include <string.h>

namespace {

// The generated id for a ladder button: `swc<ch>_bt<slot>`, both numbers 1-based.
// ONE definition, because the caller that recognises a re-learn and this
// generator must agree exactly on the spelling.
void FormatSlotId(char *buf, size_t n, int channel_index, int slot) {
    snprintf(buf, n, "swc%d_bt%d", channel_index + 1, slot);
}

// Copy a C string into a fixed field with an explicit bound. `snprintf` straight
// into a 16-byte field trips -Werror=format-truncation on the device compiler,
// which cannot prove the source stays short -- and neither can a reader.
void CopyField(char *dst, size_t dst_n, const char *src) {
    memset(dst, 0, dst_n);
    const size_t len = (src != nullptr) ? strlen(src) : 0;
    if (len > 0) memcpy(dst, src, len < dst_n ? len : dst_n - 1);
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
    p.buttons[0].mv_center = 100;
    // 1600 mV of half-width at a 3300 mV idle is ~485 permille, so the accept
    // window reaches ~1700 mV: anything below that is a press, anything at the
    // rail is not.
    p.buttons[0].mv_tolerance = 1600;
    return p;
}

LearnWizard::LearnWizard(IHAL *hal, BuzzerGrammar *buzzer, LedGrammar *leds)
    : hal_(hal), buzzer_(buzzer), leds_(leds) {
    // The wizard no longer debounces anything itself: the AUX1 HOLD is detected by
    // the caller from raw reads (it must work while a learn is running -- it is the
    // commit gesture), and the target input is measured by raw level off idle. So
    // no classifier lives here and no timings are consulted.
}

void LearnWizard::Arm(uint64_t now_ms) {
    (void)now_ms;
    state_ = State::kArmed;
    profile_ = LadderProfile{};
    neighbour_ = LadderProfile{};
    in_id_ = nullptr;
    target_wire_channel_ = 0;
    target_is_ladder_ = false;
    target_adc_ = ADC_CH_SWC1;
    target_idle_mv_ = 0;
    target_existing_index_ = -1;
    seed_idle_mv_ = 0;
    seed_framed_ = false;
    sample_started_ms_ = 0;
    have_measurement_ = false;
    switch_sum_mv_ = 0;
    switch_count_ = 0;
    last_result_ = LearnReject::kNone;

    if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kProgramEnter);
    // Spec 7.4 step 1: LED_STAT alternates while a learn is running.
    if (leds_ != nullptr) {
        leds_->SetStat(LedStatPattern::kAlternate);
        leds_->Set2(Led2Pattern::kOff);
    }
}

void LearnWizard::Abandon(uint64_t now_ms) {
    (void)now_ms;
    state_ = State::kExit;
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

int LearnWizard::MatchingExistingIndex(const LadderProfile *p, int mv) const {
    if (p == nullptr) return -1;
    for (uint8_t i = 0; i < p->count && i < kLadderMaxButtons; ++i) {
        const int d = mv - static_cast<int>(p->buttons[i].mv_center);
        const int ad = (d < 0) ? -d : d;
        if (ad <= static_cast<int>(p->buttons[i].mv_tolerance)) return static_cast<int>(i);
    }
    return -1;
}

void LearnWizard::NewSlotId(int channel_index, char *buf, size_t n) const {
    // One past the highest slot number already present, so a re-learn never reuses
    // a live button's id and a fresh learn numbers from the channel's own buttons.
    // Parsing the ids rather than using the count keeps that true when an entry
    // was dropped for a re-measure.
    int max_slot = 0;
    for (uint8_t i = 0; i < profile_.count && i < kLadderMaxButtons; ++i) {
        int ch = 0, slot = 0;
        if (sscanf(profile_.buttons[i].id, "swc%d_bt%d", &ch, &slot) == 2) {
            if (ch == channel_index + 1 && slot > max_slot) max_slot = slot;
        }
    }
    FormatSlotId(buf, n, channel_index, max_slot + 1);
}

/*
 * Name the input that left its idle.
 *
 * **This is the whole of "no channel selection"** (spec 7.5). The 2022 firmware
 * named the key the same way -- `is_key_pressed()` set `key = KEY1`/`KEY2` from
 * whichever input's average had moved off its baseline -- so the user holds the
 * modifier, presses the button they mean, and the input is the one that moved.
 *
 * The FIRST input off idle wins, searched in a fixed order. A user programs one
 * button at a time so there is no ambiguity in practice; and if two inputs are off
 * idle at once (a stuck line, or a press held from before the hold), a
 * deterministic choice is what a test can pin rather than an arbitrary one.
 */
void LearnWizard::Detect(const LearnInputs &inputs, uint64_t now_ms) {
    for (uint8_t i = 0; i < inputs.count && i < kMaxLearnInputs; ++i) {
        const LearnInput &in = inputs.in[i];
        if (in.idle_mv <= 0) continue;   // no usable reference: cannot judge idle
        const int mv = hal_->adc_read_mv(hal_->ctx, in.adc);
        // A FAILED CONVERSION IS NOT A PRESS (`IHAL::adc_read_mv` returns -1; 0 mV
        // is legal). Unchecked, -1 against a positive idle is the largest possible
        // excursion and would name an input nobody touched.
        if (mv < 0) continue;
        const int off = mv - in.idle_mv;
        if ((off < 0 ? -off : off) < kDetectMv) continue;
        BeginOn(in, mv);
        (void)now_ms;
        return;
    }
}

void LearnWizard::BeginOn(const LearnInput &in, int first_mv) {
    target_wire_channel_ = in.wire_channel;
    target_is_ladder_ = in.is_ladder;
    target_adc_ = in.adc;
    target_idle_mv_ = in.idle_mv;
    in_id_ = in.id;
    state_ = State::kSampling;
    sample_started_ms_ = 0;
    have_measurement_ = false;
    switch_sum_mv_ = 0;
    switch_count_ = 0;

    if (in.is_ladder) {
        // SEED the ladder so a learn ADDS a button rather than replacing the
        // channel's ladder (spec 7.4): the caller ASSIGNS `Profile()` over the
        // channel's ladder, so starting empty would delete every button already
        // there -- measured once as three learned buttons collapsing to one.
        profile_ = LadderProfile{};
        if (in.existing != nullptr) {
            const uint8_t n = (in.existing->count < kLadderMaxButtons)
                                  ? in.existing->count
                                  : kLadderMaxButtons;
            for (uint8_t k = 0; k < n; ++k) profile_.buttons[k] = in.existing->buttons[k];
            profile_.count = n;
            seed_idle_mv_ = in.existing->learned_idle_mv;
        } else {
            seed_idle_mv_ = 0;
        }
        // Rebase the seeded siblings onto the LIVE rail before anything compares
        // them: a profile has ONE `learned_idle_mv`, and the new button is measured
        // in the live frame (spec 6.3). Skipping this is silent -- the press
        // matches whichever window it now falls inside and the WRONG button fires.
        if (seed_idle_mv_ > 0 && in.idle_mv > 0) {
            LadderProfileRebase(profile_, seed_idle_mv_, in.idle_mv);
        }
        seed_framed_ = true;

        // A RE-MEASURE of an existing button is EXCLUDED from the neighbour set:
        // the press lands inside that button's own old window by definition, so
        // leaving it in refuses the button as too close to ITSELF and makes it
        // impossible to correct (spec 7.4). Matched by VOLTAGE -- with no slot menu
        // there is no id to match on; the id is the outcome of the match.
        target_existing_index_ = MatchingExistingIndex(&profile_, first_mv);
        neighbour_ = profile_;
        if (target_existing_index_ >= 0 &&
            target_existing_index_ < static_cast<int>(neighbour_.count)) {
            for (uint8_t k = static_cast<uint8_t>(target_existing_index_);
                 k + 1 < neighbour_.count; ++k) {
                neighbour_.buttons[k] = neighbour_.buttons[k + 1];
            }
            --neighbour_.count;
        }
        session_.Start(neighbour_);
    } else {
        // A switch: no ladder to seed, and the one thing it must not collide with
        // is itself, so the neighbour set is empty. An AUX input has a single
        // window, and `kSwitchToleranceMv` never reaches the idle band.
        profile_ = LadderProfile{};
        profile_.learned_idle_mv = static_cast<MilliVolt>(in.idle_mv);
        target_existing_index_ = -1;
    }

    // Spec 7.4 step 3: LEARN_PROMPT -- the device has the input and is measuring.
    if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnPrompt);
    if (leds_ != nullptr) leds_->SetStat(LedStatPattern::kAlternate);
}

void LearnWizard::Tick(const LearnInputs &inputs, uint64_t now_ms, int temp_tenths_c) {
    if (hal_ == nullptr) return;
    if (state_ == State::kArmed) {
        Detect(inputs, now_ms);
        return;
    }
    if (state_ != State::kSampling) return;

    // The target's LIVE idle from the caller's list, which may have moved since it
    // was named -- comparing a reading against a stale denominator is how an
    // idle-adjacent button reads as permanently pressed.
    for (uint8_t i = 0; i < inputs.count && i < kMaxLearnInputs; ++i) {
        const LearnInput &in = inputs.in[i];
        if (in.wire_channel != target_wire_channel_ || in.is_ladder != target_is_ladder_) {
            continue;
        }
        if (in.idle_mv > 0) target_idle_mv_ = in.idle_mv;
        Sample(in, now_ms, temp_tenths_c);
        return;
    }
    // The target vanished (a config swap mid-learn): there is nothing left to
    // measure, so end the session without inventing a button from stale samples.
    Abandon(now_ms);
}

void LearnWizard::Sample(const LearnInput &in, uint64_t now_ms, int temp_tenths_c) {
    const int mv = hal_->adc_read_mv(hal_->ctx, in.adc);
    if (mv < 0) return;   // a failed read is not a measurement

    const int off = mv - target_idle_mv_;
    if ((off < 0 ? -off : off) < kDetectMv) return;   // at idle: not a press

    if (sample_started_ms_ == 0) sample_started_ms_ = now_ms;
    // The safety cap: a stuck line would otherwise sample forever. Once it has
    // passed, stop adding readings but KEEP the state -- the user still finishes by
    // releasing AUX1, and what was collected is a real hold.
    if ((now_ms - sample_started_ms_) >= kSampleMaxHoldMs) {
        have_measurement_ = true;
        return;
    }

    if (in.is_ladder) {
        // Sample only while the button is held. The release reading is NOT folded
        // in: it is the level travelling back to idle, and including it puts the
        // whole press-to-idle swing into the spread, failing every learn as too
        // noisy (spec 7.4).
        session_.AddSample(mv, target_idle_mv_, kNominalRailMv,
                           static_cast<int16_t>(temp_tenths_c), now_ms);
    } else {
        switch_sum_mv_ += mv;
        ++switch_count_;
    }
    have_measurement_ = true;
}

/*
 * Turn the measurement into the committed profile. Called ONLY from `Release`, so
 * the commit point is the AUX1 release (spec 7.5) and there is exactly one.
 */
void LearnWizard::Finish(uint64_t now_ms) {
    (void)now_ms;
    if (!have_measurement_) {
        last_result_ = LearnReject::kTooFewSamples;
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnReject);
        return;
    }

    if (!target_is_ladder_) {
        // A switch window. The centre is the measured pressed level and the
        // tolerance is a fixed wide half-rail -- there is no neighbour to stay
        // clear of, only "pressed vs not". This bypasses `LearnSession`, whose gates
        // are all about ladder windows and whose `LadderProfileIsValid` refuses a
        // centre of 0 mV, which a switch shorting to ground produces.
        const int mean =
            (switch_count_ > 0) ? static_cast<int>((switch_sum_mv_ + switch_count_ / 2) / switch_count_)
                                : 0;
        // The at-idle gate, which the ladder path gets from `LearnSession::Commit`
        // and this path must have of its own. A light touch that only barely left
        // idle would store a centre INSIDE the classifier's idle band, so
        // `LadderClassify` would call the switch's own press idle and the input
        // would be dead -- LEARN_OK over a switch that never fires. It shares the
        // classifier's own margin (`kIdleMarginPermille`) for the same reason
        // `LearnSession` does: the learn gate must be no weaker than the
        // classifier's.
        const int ratio = LadderRatioPermille(mean, target_idle_mv_);
        if (ratio >= 1000 - kIdleMarginPermille && ratio <= 1000 + kIdleMarginPermille) {
            last_result_ = LearnReject::kAtIdle;
            if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnReject);
            return;
        }
        int centre = mean;
        // A centre at or below zero is one `AuxProfileFor` refuses to build a
        // classifier from (it disables the input), so clamp into the legal window
        // rather than storing a level that silently kills the switch.
        if (centre < 1) centre = 1;
        if (centre > kAdcFullScaleMv12dB) centre = kAdcFullScaleMv12dB;

        LadderButton out{};
        // An AUX input's id IS a config entry's (`aux[].id`); the caller supplies
        // it and the learn does not invent one. Fall back only if none was given.
        const char *id = (in_id_ != nullptr && in_id_[0] != '\0') ? in_id_ : "aux";
        CopyField(out.id, sizeof(out.id), id);
        CopyField(out.name, sizeof(out.name), id);
        out.mv_center = static_cast<MilliVolt>(centre);
        out.mv_tolerance = static_cast<MilliVolt>(kSwitchToleranceMv);
        out.learned_at_rail_mv = kNominalRailMv;
        out.temp_c_at_learn = 0;
        out.sample_count = static_cast<uint16_t>(switch_count_);
        out.confidence = 100;

        profile_.buttons[0] = out;
        profile_.count = 1;
        profile_.learned_idle_mv = static_cast<MilliVolt>(target_idle_mv_);
        last_result_ = LearnReject::kNone;
        committed_ = true;
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnOk);
        return;
    }

    LadderButton out{};
    // A RE-MEASURE keeps the existing id and name; a NEW button gets the next
    // `swc<ch>_bt<n>`. Matching by voltage is what makes a re-learn correct a
    // button instead of adding a duplicate at the same level.
    const int channel_index = static_cast<int>(target_wire_channel_);
    if (target_existing_index_ >= 0 &&
        target_existing_index_ < static_cast<int>(profile_.count)) {
        memcpy(out.id, profile_.buttons[target_existing_index_].id, sizeof(out.id));
        memcpy(out.name, profile_.buttons[target_existing_index_].name, sizeof(out.name));
    } else {
        char idbuf[32];
        NewSlotId(channel_index, idbuf, sizeof(idbuf));
        CopyField(out.id, sizeof(out.id), idbuf);
        int ch = 0, slot = 0;
        char namebuf[32];
        if (sscanf(idbuf, "swc%d_bt%d", &ch, &slot) == 2) {
            snprintf(namebuf, sizeof(namebuf), "Button %d", slot);
        } else {
            snprintf(namebuf, sizeof(namebuf), "Button");
        }
        CopyField(out.name, sizeof(out.name), namebuf);
    }

    const LearnReject r = session_.Commit(&out);
    last_result_ = r;
    if (r == LearnReject::kNone) {
        // REPLACE IN PLACE for a re-measure, APPEND otherwise. One button per
        // measurement; the caller assigns `Profile()` over the channel's ladder.
        if (target_existing_index_ >= 0 &&
            target_existing_index_ < static_cast<int>(profile_.count)) {
            profile_.buttons[target_existing_index_] = out;
        } else if (profile_.count < kLadderMaxButtons) {
            profile_.buttons[profile_.count] = out;
            ++profile_.count;
        } else {
            // The ladder is full and this is a new button: a real measurement that
            // cannot be stored is a REJECTION, not a commit. Reporting it as `kNone`
            // would play LEARN_OK while the caller persisted an unchanged ladder --
            // the silent "success for a lost button" the app path refuses with
            // `no_space` (FR-29, spec 7.4 step 6).
            last_result_ = LearnReject::kNoSpace;
        }
    }

    if (last_result_ == LearnReject::kNone) {
        // The profile's denominator is the idle this measurement was taken against.
        // Without it the committed profile classifies nothing -- spec 6.3 reports a
        // fault on a zero reference -- so a learn would beep LEARN_OK over a button
        // that does not work.
        profile_.learned_idle_mv = session_.LearnedIdleMv();
        committed_ = true;
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnOk);
    } else {
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnReject);
    }
}

LearnReject LearnWizard::Release(uint64_t now_ms) {
    // Spec 7.5: the user lets go of the modifier and the setting is stored.
    if (state_ == State::kIdle || state_ == State::kExit) return last_result_;

    if (state_ == State::kSampling) {
        Finish(now_ms);
    } else {
        // A hold that never named an input is a phantom: there is nothing to
        // store, and saying so is better than committing a button fabricated from
        // an idle line.
        last_result_ = LearnReject::kTooFewSamples;
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnReject);
    }

    state_ = State::kExit;
    exited_ = true;
    if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kProgramExit);
    if (leds_ != nullptr) leds_->SetStat(LedStatPattern::kSolid);
    return last_result_;
}
