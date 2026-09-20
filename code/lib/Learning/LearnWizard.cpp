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

void LearnWizard::Enter(uint64_t now_ms, bool aux_held) {
    state_ = State::kSelectButton;
    profile_ = LadderProfile{};   // a fresh learn, not an edit of the old one
    press_count_ = 0;
    selected_slot_ = 0;
    any_press_ = false;
    last_press_ms_ = now_ms;
    beeps_owed_ = 0;
    aux_was_pressed_ = false;
    aux_seen_idle_ = !aux_held;
    last_result_ = LearnReject::kNone;
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

    if (state_ == State::kSelectButton) {
        ServiceSelect(now_ms);
    } else if (state_ == State::kPrompt) {
        ServicePrompt(channel, now_ms, idle_mv, temp_tenths_c);
    }
    ServiceBeeps();
}

void LearnWizard::ServiceSelect(uint64_t now_ms) {
    const int mv = hal_->adc_read_mv(hal_->ctx, ADC_CH_AUX1);
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
        session_.Start(selected_slot_ - 1, profile_);
        // Spec 7.4 step 3: LEARN_PROMPT, LEDs alternating.
        if (buzzer_ != nullptr) buzzer_->Play(BuzzerPattern::kLearnPrompt);
        if (leds_ != nullptr) leds_->SetStat(LedStatPattern::kAlternate);
    }
}

void LearnWizard::ServicePrompt(int channel, uint64_t now_ms, int idle_mv, int temp_tenths_c) {
    const int level_mv = hal_->adc_read_mv(hal_->ctx, LevelChannelFor(channel));
    // The RAIL during learn (spec 3.4's `learned_at_rail_mv`). There is no rail
    // sense channel on this board (AdcChannel carries SWC1/SWC2/TEMP/AUX1-3/
    // KEY_SENSE1-2 and none of them is +3V3), so this is the board's nominal rail
    // and is recorded as such. It is deliberately NOT read from the HAL.
    const int rail_mv = 3300;

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
        session_.Start(selected_slot_ - 1, profile_);
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
    snprintf(idbuf, sizeof(idbuf), "swc%d_bt%d", channel + 1, slot);
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
        if (profile_.count < kLadderMaxButtons) {
            profile_.buttons[profile_.count] = out;
            ++profile_.count;
        }
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
        // A REAL rejection: say WHY (FR-29) rather than a generic failure.
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
