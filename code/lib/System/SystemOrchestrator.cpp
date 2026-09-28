#include "System/SystemOrchestrator.h"

#include <new>
#include <cstdio>
#include <cstring>

#include "Bindings/BindingResolver.h"
#include "Analog/LadderDecode.h"
#include "Analog/NtcConvert.h"
#include "Config/ConfigDefaults.h"
#include "Config/ConfigStore.h"
#include "Output/GainPolicy.h"
#include "Output/GestureLevels.h"

// FR-33's no-config maintenance trigger (spec 8.2, "Reset-reason + no-config")
// keys on the reset reason being a POWER-ON, via `SWC_RST_POWERON` (defined in
// `HAL/IHAL.h`, because this file is HOST-compiled and cannot see the platform's
// `esp_system.h`). `SWC_RST_POWERON` mirrors IDF's `ESP_RST_POWERON`; `EspHal.cpp`
// static_asserts the mirror, and `IHAL.h` documents the conservative rule -- 0
// ("unknown") is NOT treated as a cold boot.

namespace {

// The sense divider is an exact divide-by-2 (spec 2.3), so the KEY line is twice
// the sense reading. This appears in three places (gain selection, the trim
// loop, and here) and is a property of R54/R55, not a tuning value.
constexpr int kSenseDividerRatio = 2;

// How far from the idle reference a settled reading may sit and still be taken
// as idle drift rather than a press or a fault, in permille. The +3V3 rail's own
// tolerance is +/-5% (spec 6.3), so the reference must follow a slow move; a real
// button sits far lower (the idle-adjacent worst case is ~830), and a short to a
// supply reads ABOVE idle -- which LadderClassify treats as a fault at +3%
// (spec 6.3's idle margin). Adopting only [1000-60, 1000+30] tracks a gradual
// rail move while rejecting a step to a press or a short.
constexpr int kIdleRefTrackPermille = 60;   // lower bound: rail drift, not a press
constexpr int kIdleRefMarginPermille = 30;  // upper bound: idle, not a short

/*
 * How far off idle the wheel must move before pass-through calls it a press.
 *
 * Deliberately generous: pass-through has no learned window to compare against,
 * so the only question it can answer is "is this clearly not idle". A small
 * threshold would fire on the ladder's noise floor; a large one would miss a
 * button whose level is close to idle.
 */
constexpr int kPassThroughPressDeltaMv = 300;

// Spec 6.2 step 2: a KEY idle outside this envelope means no head unit at all.
constexpr int kKeyEnvelopeLowMv  = kOutputFloorMv;    // 1800
constexpr int kKeyEnvelopeHighMv = kOutputCeilingMv;  // 5200

// While this device is DRIVING a key, a sense reading at or above this is one we
// produced, not a fault: every command is clamped to `kKeyEnvelopeLowMv` or
// above, so a driven line can never legitimately read below the floor except on a
// real collapse. The margin covers the servo's own undershoot and the ADC's
// calibration of the commanded level (measured on the bench: a commanded 1800 mV
// read back at ~1790 mV). Below this, the line has sagged away under a held key
// -- the rail fault FR-39 must release (see `HeadUnitGone`).
constexpr int kFaultSagMarginMv = 200;
constexpr int kFaultSagMaxMv = kKeyEnvelopeLowMv - kFaultSagMarginMv;  // 1600

// The maintenance hold is `SystemOrchestrator::kMaintenanceHoldMs` (the class
// declares it, so a test or caller names the same number). It lives there rather
// than here because the nesting with the programming hold is part of the AUX1
// gesture's contract, and the maintenance class has no idea AUX1 exists.

/*
 * The top of AUX1's press window, in millivolts (1700 with the default profile).
 *
 * Derived from `Aux1ProfileDefault()` rather than written again, because a second
 * number for "AUX1 is pressed" is a second answer, and the hold detector and the
 * wizard's own classifier would then disagree about the same switch -- exactly at
 * the boundary a user's finger lands on.
 */
int Aux1PressedMaxMv() {
    const LadderProfile p = Aux1ProfileDefault();
    return p.buttons[0].mv_center + p.buttons[0].mv_tolerance;
}

// The ADC channel an AUX input's index names. AUX1..3 are contiguous in
// `AdcChannel`, so the mapping is an offset rather than a switch, and the one
// caller (`ServiceAux`) passes an index it has already bounded against
// `aux_count_`.
AdcChannel AuxAdcChannel(uint8_t aux_index) {
    return static_cast<AdcChannel>(static_cast<int>(ADC_CH_AUX1) + aux_index);
}


}  // namespace

SystemOrchestrator::SystemOrchestrator(IHAL *hal, const Config &config,
                                       const GestureTimings &timings)
    : hal_(hal), config_(config), timings_(timings),
      buzzer_(hal, config.settings.buzzer_level),
      leds_(hal, config.settings.led_level),
      // FR-31's headless learn. Constructed with the SAME timings the channels
      // use, so "what counts as an AUX1 press" matches "what counts as a wheel
      // press" -- one debounce definition in the firmware.
      wizard_(hal, &buzzer_, &leds_),
      // Spec 8.2's default maintenance window (5 minutes). A config may carry a
      // different one; this default exists so a bare device still has a BOUNDED
      // window rather than an unbounded one.
      maintenance_(hal, 300000) {}

/*
 * The bindings for ONE button's gestures, which is the granularity spec 6.6
 * rule 3 actually names ("the button binds ... the wait before driving").
 *
 * **A per-CHANNEL scan was the earlier shape, and using it for the resolve was a
 * real bug.** A channel-wide scan reports `has_double` if ANY of the channel's
 * buttons binds DOUBLE, so a button that binds nothing -- or binds only SINGLE --
 * still paid the 500 ms double-press window, and worse, a HELD unbound button
 * emitted a `LONG` it has no binding for: a gesture the config never assigned,
 * reported to the app as `event{gesture:long}`. Spec 6.6 rule 3 is explicit that
 * the wait is "a per-button property, not a per-device one", and the struct's own
 * comment in GestureStateMachine.h says "which gestures a BUTTON's bindings
 * cover". The channel-wide helper was removed with this, so there is no second
 * scan left for a reader to mistake for the right one.
 *
 * `button_index` is an index into the channel's ladder; the binding names the
 * button's `id`, so this is the same join `BindingResolve` performs.
 */
GestureBindings SystemOrchestrator::BindingsForButton(uint8_t channel_index,
                                                      uint8_t button_index) const {
    GestureBindings out;
    out.has_double = false;
    out.has_long = false;
    if (channel_index >= config_.channel_count) return out;
    if (config_.binding_count > kMaxBindings) return out;
    const LadderProfile &ladder = config_.channels[channel_index].ladder;
    if (button_index >= ladder.count) return out;
    const char *button_id = ladder.buttons[button_index].id;

    const uint8_t as_swc = (channel_index == 0)
                               ? static_cast<uint8_t>(BindingChannel::kSwc1)
                               : static_cast<uint8_t>(BindingChannel::kSwc2);
    bool any_match = false;
    for (uint8_t i = 0; i < config_.binding_count; ++i) {
        const Binding &b = config_.bindings[i];
        if (!b.enabled) continue;
        if (b.channel != as_swc && b.channel != static_cast<uint8_t>(BindingChannel::kAny)) {
            continue;
        }
        if (strcmp(b.button, button_id) != 0) continue;
        any_match = true;
        if (b.gesture == Gesture::kDouble) out.has_double = true;
        if (b.gesture == Gesture::kLong) out.has_long = true;
    }

    // **A button that binds NOTHING still needs DOUBLE and LONG detected**, because
    // its gestures are meaningful through the DEFAULT presentation (spec 6.6 rule
    // 4): an unbound button's single/double/long present three distinct slot levels
    // so the head unit -- taught them -- can fire three functions. That is the
    // no-app product. Leaving `has_double`/`has_long` false here (the adaptive
    // "no ambiguity to resolve" shortcut) made the machine emit SINGLE at the press
    // and NEVER produce a DOUBLE or LONG at all, so the two extra functions a user
    // programmed did nothing -- the exact failure this feature exists to fix.
    //
    // The latency this adds is inherent, not accidental: you cannot know a tap is
    // not a double until the double-press window closes, which is what makes three
    // gestures on one key distinguishable. The 2022 firmware did the same
    // (`check_is_double_press_key` always returned true).
    //
    // A button that DOES bind something keeps the adaptive rule: if it binds only a
    // SINGLE, its double/long are not the default (the app chose this button's
    // gestures), so it resolves at the press with no window.
    if (!any_match) {
        out.has_double = true;
        out.has_long = true;
    }
    return out;
}

// Spec 6.2 step 2's "no head unit" test, made usable per-tick.
//
// **The envelope is on `V_KEY_idle` -- the line's resting level -- but while this
// device drives a key pulse, its own sense node reads the DEVICE'S OWN output.
// There is no other actor on the line.** So a naive per-tick envelope check is a
// verdict about this device, not the head unit, and it is wrong in the exact
// case the product's headless path creates:
//
//   A learned-only button (`binding_count == 0`, the `ConfigDefault` a headless
//   learn runs on) has no head unit to map onto (`head_unit_idle_mv_` is 0), so
//   it presents the button's RATIO onto the command band's 1800 mV FLOOR (spec
//   6.2). The sense node then reads ~1800 mV, which is AT the envelope's low
//   edge, so the check fired, the tail released and RESET the gesture machine,
//   the line floated back up, and the still-held button re-classified on the next
//   tick and re-emitted. Measured on the bench rig: 26-177 `event` frames for a
//   2 s hold, and because the reset discarded the press state a held button could
//   never advance to LONG.
//
// The discriminator is therefore not "is the line driven" but "is the reading
// consistent with what we are driving":
//
//   1. **A driven line is judged only on a deep sag.** Our own commands clamp to
//      the envelope floor, so a reading at or above `kFaultSagMaxMv` is one we
//      produced -- not a fault. A rail collapse, the other out-of-envelope case
//      while driving, drives the line far BELOW the floor and still releases
//      (FR-39's phantom-key hazard; `ARailSagDuringAPressReleasesTheKey`).
//   2. **A released line is judged on the whole envelope** -- the spec's own
//      reading -- and the verdict must persist for `kHeadUnitGoneSettleMs`, so a
//      line settling after a pulse, or a rail coming up at boot, is not mistaken
//      for an absent head unit.
bool SystemOrchestrator::HeadUnitGone(uint8_t index, int sense_mv, uint64_t now_ms) {
    if (index >= kMaxChannels) return false;
    ChannelState &cs = channels_[index];
    if (sense_mv < 0) {
        // A failed conversion is not a reading (the `-1` sentinel; 0 mV is legal).
        // HOLD the settle clock rather than treating it as out-of-envelope.
        return cs.envelope_bad_since_ms != 0 &&
               (now_ms - cs.envelope_bad_since_ms) >= kHeadUnitGoneSettleMs;
    }
    const int key_idle_now_mv = sense_mv * kSenseDividerRatio;
    const bool out = (key_idle_now_mv < kKeyEnvelopeLowMv) ||
                     (key_idle_now_mv > kKeyEnvelopeHighMv);
    if (!out) {
        cs.envelope_bad_since_ms = 0;
        return false;
    }
    // Guard 1: while driving, only a sag below anything our own commands can
    // reach is a fault. The command band floor IS the envelope low edge, so this
    // is what keeps the headless default's own level from reading as "gone".
    if (cs.key_driven && key_idle_now_mv >= kFaultSagMaxMv) {
        cs.envelope_bad_since_ms = 0;
        return false;
    }
    // Guard 2: persist the verdict.
    if (cs.envelope_bad_since_ms == 0) {
        cs.envelope_bad_since_ms = now_ms;
        return false;
    }
    return (now_ms - cs.envelope_bad_since_ms) >= kHeadUnitGoneSettleMs;
}

void SystemOrchestrator::EstablishSafeIdle() {
    // Step 1 of spec 6.1: the DAC is already safe from its EEPROM (full-scale
    // signal, gain channels powered down), and the firmware's job is to VERIFY
    // and then hold that state before anything else runs (FR-13).
    //
    // A channel that is disabled still gets the idle write. A channel the user
    // turned off must not leave its KEY line in an unknown state, and the cost
    // of writing it is one I2C transaction at boot.
    const uint8_t n = (config_.channel_count <= kMaxChannels) ? config_.channel_count
                                                              : kMaxChannels;

    for (uint8_t i = 0; i < n; ++i) {
        const DacChannel adj_ch = (i == 0) ? DAC_CH_ADJ1 : DAC_CH_ADJ2;

        // Measure the head unit's idle before choosing gain: V_KEY_idle is twice
        // the sense reading (spec 6.2 step 1). With no head unit the sense pin
        // floats and the reading is outside the envelope, which resolves to the
        // amplified default -- the safe direction (spec 6.2's asymmetry).
        const int sense_mv = hal_->adc_read_mv(hal_->ctx,
                                               (i == 0) ? ADC_CH_KEY_SENSE1 : ADC_CH_KEY_SENSE2);
        const int measured_key_idle_mv = sense_mv * kSenseDividerRatio;
        const bool no_head_unit = (measured_key_idle_mv < kKeyEnvelopeLowMv) ||
                                  (measured_key_idle_mv > kKeyEnvelopeHighMv);
        const int for_gain = no_head_unit ? 0 : measured_key_idle_mv;
        // Remembered for FR-25's pass-through: the ratio is taken against the
        // HEAD UNIT's own idle, not against the output's safe-idle code (which is
        // full scale, 5200 mV, and would push every mapped level to the ceiling).
        // PER CHANNEL, like the wheel idle and the gain mode above it: spec 6.2
        // samples `/SENSEn` per channel, and the two head-unit inputs are
        // independent. Keeping only channel 0's made channel 1's pass-through
        // dead whenever channel 0 had no head unit (and mapped onto the wrong
        // idle when the two differed).
        head_unit_idle_mv_[i] = no_head_unit ? 0 : measured_key_idle_mv;

        // A channel's own `gain_mode` overrides the device-wide policy, but only
        // when it names a CONCRETE gain. `kAuto` -- the value the spec's example
        // uses -- defers to `settings.gain_policy`, which is the AUTO rule FR-14
        // actually specifies. Before `kAuto` existed this branch was dead: every
        // config the codec would accept already named a concrete mode, so
        // `settings.gain_policy` was never read and AUTO was unreachable.
        const GainMode requested = config_.channels[i].output.gain_mode;
        const GainPolicy policy =
            (requested == GainMode::kTracking)
                ? GainPolicy::kForceTracking
                : ((requested == GainMode::kAmplified) ? GainPolicy::kForceAmplified
                                                       : config_.settings.gain_policy);
        const GainMode mode = GainPolicySelect(policy, for_gain);
        gain_mode_[i] = mode;

        // Gain 1.82 needs V_ADJ at 0 V, which is the 1 kohm pulldown power-down
        // mode; gain 1.00 needs V_ADJ tracking the signal channel (spec 2.3).
        hal_->dac_power_mode(hal_->ctx, adj_ch,
                             (mode == GainMode::kAmplified) ? DAC_POWER_GND_1K
                                                            : DAC_POWER_NORMAL);

        uint16_t idle_code = config_.channels[i].output.idle_dac_code;
        // Release is "command ABOVE the head unit's idle voltage" (FR-16), so
        // the idle code must actually reach above the measured line. A code that
        // only just fails to do that would leave Q4 conducting, which is a key
        // held down -- the hazard of spec 6.7. Full scale is the spec default.
        if (idle_code < kDacMaxCode && measured_key_idle_mv > 0) {
            const int idle_reachable_mv = GainPolicyKeyMvForCode(mode, idle_code);
            if (idle_reachable_mv <= measured_key_idle_mv) idle_code = kDacMaxCode;
        }

        idle_code_[i] = idle_code;
        DriveKeyCode(i, idle_code);
    }

    // Set BEFORE the state machines are constructed, so anything that observes
    // SafeIdleEstablished() knows the output is already safe (FR-13).
    safe_idle_established_ = true;

    // ...and then PROVE it, which is FR-13's step 3b and was implemented by
    // nothing (open item N-21). This runs after the writes so it checks what the
    // part actually holds, not what the firmware believes it sent.
    VerifySafeIdleIdleCodes();
}

bool SystemOrchestrator::VerifySafeIdleIdleCodes() {
    const uint8_t n = (config_.channel_count <= kMaxChannels) ? config_.channel_count
                                                              : kMaxChannels;
    bool all_ok = true;
    for (uint8_t i = 0; i < n; ++i) {
        const DacChannel ch = (i == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;
        uint16_t read_code = 0;
        if (!hal_->dac_read_code(hal_->ctx, ch, &read_code)) {
            // The bus did not answer. That is §6.8's persistent failure; the HAL
            // has already latched `dac_faulted`, and the line's state is whatever
            // the failed write left.
            dac_verify_failed_ = true;
            all_ok = false;
            continue;
        }
        if (read_code != idle_code_[i]) {
            // The part answers, with a code the firmware did not write. The read
            // value is exactly what this check says is untrustworthy, so driving
            // it would be §6.8's forbidden "guessed code". Re-assert the IDLE code
            // instead -- the idle code IS the released state (spec 6.7), so this
            // is the row's "release the line" -- and report the fault. See the
            // header comment.
            //
            // Deliberately NOT `ReleaseKey(i)`: that reads `channels_[i]`, which
            // `SeedChannelState` has not built yet at this point in Boot, and it
            // early-returns unless `key_driven` is set, which nothing has done.
            DriveKeyCode(i, idle_code_[i]);
            // The HAL latch cannot carry this: the transaction SUCCEEDED. Latching
            // it here is what makes a wrong value falsify FR-37's gate.
            dac_verify_failed_ = true;
            all_ok = false;
        }
    }
    if (!all_ok) ReportDacFault();
    return all_ok;
}

int SystemOrchestrator::SampleNtcTenthsC() {
    const int node_mv = hal_->adc_read_mv(hal_->ctx, ADC_CH_TEMP);
    // A -1 is a FAILED CONVERSION, not a cold board: 0 mV is a legal reading
    // (N-43). Hold the last good value and report that, rather than storing "0 C"
    // for a temperature nothing measured -- the exact lie `kTempNotMeasuredTenths`
    // exists to prevent. The very first failure, before any good reading, leaves
    // the sentinel in place.
    if (node_mv < 0) return last_ntc_tenths_c_;
    int tenths = 0;
    if (!Ntc::NodeMvToTenthsC(node_mv, kNominalRailMv, &tenths)) {
        // Off the divider's valid span (an open or shorted part, a rail fault).
        // Same direction: hold, do not fabricate.
        return last_ntc_tenths_c_;
    }
    last_ntc_tenths_c_ = static_cast<int16_t>(tenths);
    return last_ntc_tenths_c_;
}

void SystemOrchestrator::ReportDacFault() {
    if (dac_fault_reported_) return;
    dac_fault_reported_ = true;
    // The LAMP is the fault channel (spec 7.3) and this is a hardware condition,
    // so it latches exactly as FR-4's wiring fault does.
    ReportFault();
    // **§7.2's `FAULT_DAC` finally has a caller.** The pattern's meaning is
    // "I2C/DAC fault", which is precisely this condition -- distinct from the
    // ladder/rail faults, which play their own `FAULT_INPUT` (N-10) rather than
    // this one: playing FAULT_DAC for a wiring fault would name the wrong part. Playing it is the audible half the row asks for
    // ("report a fault"), and the buzzer's OFF level cannot suppress a FAULT_*
    // pattern (spec 7.2).
    buzzer_.Play(BuzzerPattern::kFaultDac);
}

/*
 * Per-channel state, derived from `config_` and the LIVE ADC readings.
 *
 * Called from `Boot` and from `ApplyConfig`, so a config that arrives over the
 * link is classified by exactly the same construction as one stored before boot
 * (spec 4.2). Returns whether any channel found a pass-through reference, which
 * only `Boot` consults -- after boot, `pass_through_` has already been decided
 * and `ApplyConfig` never revives a disabled pass-through.
 */
LadderProfile SystemOrchestrator::AuxProfileFor(const AuxButtonConfig &a) {
    LadderProfile p{};
    // An AUX input is a switch pulled to the rail and shorted to ground when
    // pressed (spec 2.4: "electrically identical [to the SWC channels] but use a
    // 1 kohm series resistor"), so its idle IS the rail. The nominal +3V3 is the
    // same value `Aux1ProfileDefault` uses and for the same reason: this profile
    // only has to answer "pressed vs not", not to be accurate about the rail.
    p.source = a.source;
    p.learned_idle_mv = kNominalRailMv;
    p.count = 1;
    std::strncpy(p.buttons[0].id, a.id, sizeof(p.buttons[0].id) - 1);
    // A window whose centre is at or below zero is a config the validator should
    // have refused; refuse to build a classifier from it rather than making every
    // reading a match. An empty profile (count 0) classifies nothing, which is the
    // safe direction: an AUX input with no window fires no binding.
    const int centre = a.mv_center;
    const int half = a.mv_tolerance;
    if (centre <= 0 || half < 0) {
        p.count = 0;
        return p;
    }
    p.buttons[0].mv_center = static_cast<MilliVolt>(centre);
    p.buttons[0].mv_tolerance = static_cast<MilliVolt>(half);
    p.buttons[0].learned_at_rail_mv = kNominalRailMv;
    return p;
}

bool SystemOrchestrator::SeedChannelState() {
    bool any_reference = false;
    for (uint8_t i = 0; i < channel_count_; ++i) {
        ChannelState &cs = channels_[i];
        cs.classifier = PressClassifier(config_.channels[i].ladder, timings_);
        cs.gestures = GestureStateMachine(timings_);
        // FR-3's filter, bound to this channel's own ADC input. Per-channel
        // because the two ladders are independent inputs (FR-9): one shared
        // window would let a press on SWC1 move SWC2's reported level.
        cs.reader.Bind(*hal_, (i == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2);
        cs.servo = ServoLoop(ServoConfigDefault());
        // No cached binding set: the resolve reads the PRESSED BUTTON's bindings
        // each tick (`BindingsForButton`), because the granularity is per button
        // (spec 6.6 rule 3) and the button is not known until classification.
        cs.key_driven = false;
        cs.key_released_at_ms = 0;
        // FR-25's edge detector. A stale `true` left from the previous config
        // suppresses the FIRST rising edge after the swap, so a button the user is
        // holding through a learn commit would not be seen until they let go and
        // pressed again -- a "the button I just taught does nothing" report that
        // looks exactly like a classification bug.
        cs.pass_through_pressed = false;
        // FR-12's one-report-per-press latch is per-channel and must not survive
        // a config swap: the new profile may name a button the OLD profile called
        // unknown, and a latch still set from the old one would swallow the first
        // report of a press the user just taught the device to recognize.
        cs.unknown_reported = false;
        // The trim loop is present but DISABLED in v1: spec 6.5 says open-loop
        // command with the loop off until its gain is measured on hardware, and
        // running a software loop against the hardware integrator is how you
        // build an oscillator.
        //
        // The configured path's ratio denominator is `V_ADC_idle`. Seed it from
        // the LIVE reading when that reading is a plausible rail idle -- within
        // the +-5% +3V3 tolerance of the learned idle (spec 6.3) -- so a device
        // that boots on a moved rail is classified correctly from the first tick.
        // Otherwise seed from the learned idle, which is the one value KNOWN to be
        // an idle reading: a button held at power-on is far below the rail, so it
        // fails the plausibility test and cannot become the reference.
        //
        // Seeding from the learned idle unconditionally was the bug: on a +5%
        // rail the live idle is ~1050 permille of it, which LadderClassify reads
        // as a fault (its threshold is +3%), and an idle-adjacent button's ratio
        // drifts out of its window at the band edge.
        //
        // The pass-through path (FR-25) has no learned ladder, so its reference
        // can only come from the live reading; it self-heals a boot-time press
        // separately (ServiceChannel).
        //
        // Read AFTER safe idle so the KEY line is already released and cannot be
        // pulling the ladder.
        const int live = hal_->adc_read_mv(hal_->ctx, (i == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2);
        const int learned_idle = config_.channels[i].ladder.learned_idle_mv;
        cs.idle_reference_mv = learned_idle;
        if (learned_idle <= 0) {
            cs.idle_reference_mv = (live > 0) ? live : 0;
        } else if (live > 0) {
            const int r = LadderRatioPermille(live, learned_idle);
            if (r >= 950 && r <= 1050) cs.idle_reference_mv = live;
        }
        if (pass_through_) {
            cs.pass_through_idle_mv = (live > 0) ? live : 0;
            if (cs.pass_through_idle_mv > 0) any_reference = true;
        }
    }

    // The AUX gesture inputs (AUX2/AUX3). Re-seeded here rather than only at Boot
    // so a config that arrives over the link picks up its new windows, exactly as
    // the wheel channels do -- one derivation, so the two paths cannot classify an
    // AUX press differently.
    //
    // AUX1 (index 0) is deliberately NOT seeded: it is the programming and
    // maintenance hold (spec 7.5/8.2), and a binding on it is refused by the
    // validator, so servicing it as a gesture source would be a second meaning for
    // the same switch.
    aux_count_ = (config_.aux_count <= kMaxAuxButtons) ? config_.aux_count : kMaxAuxButtons;
    for (uint8_t i = 0; i < aux_count_; ++i) SeedAuxState(i);
    return any_reference;
}

/*
 * (Re)build one AUX input's servicing state from its config entry.
 *
 * **ONE home, because two callers must agree.** `SeedChannelState` runs it for
 * every AUX input on a config apply, and a headless learn of an AUX switch runs it
 * for the one input it just wrote -- where the config has changed without going
 * through `ApplyConfig`. Deriving the classifier in two places is how the two
 * would drift about what counts as a press on that switch.
 */
void SystemOrchestrator::SeedAuxState(uint8_t aux_index) {
    if (aux_index >= kMaxAuxButtons || aux_index >= aux_count_) return;
    AuxState &as = aux_[aux_index];
    as.classifier = PressClassifier(AuxProfileFor(config_.aux[aux_index]), timings_);
    as.gestures = GestureStateMachine(timings_);
    as.reader.Bind(*hal_, AuxAdcChannel(aux_index));
    // The same per-press latch reset the channels do, and for the same reason: a
    // stale latch would swallow the first report of a press on the new window.
    as.unknown_reported = false;
}

void SystemOrchestrator::Boot() {
    // 1. Load the config. `kNoConfig` is not a failure -- FR-25 makes an
    //    unconfigured device a transparent pass-through, so the device works
    //    before it is ever configured.
    //
    // **The loaded config is a file-local static, not a local, and that is a
    // safety requirement rather than an optimization.** `sizeof(Config)` is
    // 8,912 B; as a local it made this function's frame 18,704 B (it also held a
    // by-value `ConfigDefault()`), and `Boot` runs on the 3,584-byte main task
    // via `SystemOrchestratorCreate`. The real worst case is worse than the frame
    // suggests: the chain is Boot -> ConfigStore::Load -> ConfigDecodeBlob ->
    // ConfigDecodeJson, which summed to ~36 KB of frame for a 3.5 KB stack. That
    // is a guaranteed boot crash on hardware, and it is invisible on the host --
    // the host suite runs these on an 8 MB thread stack, and the board has never
    // been flashed.
    //
    // One static is safe: `Boot` runs exactly once, from `SystemOrchestratorCreate`,
    // before the link exists, so nothing else can be decoding concurrently.
    static Config loaded;
    loaded = Config{};
    ConfigStore store(hal_);
    const ConfigLoadResult result = store.Load(&loaded);

    if (ConfigLoadResultIsUsable(result)) {
        config_ = loaded;
        buzzer_ = BuzzerGrammar(hal_, config_.settings.buzzer_level);
        leds_ = LedGrammar(hal_, config_.settings.led_level);
    } else if (result == ConfigLoadResult::kFellBackToDefaults) {
        // Spec 6.8: a corrupt config falls back to DEFAULTS. That has to actually
        // ASSIGN them, and the earlier revision did not -- it reported
        // `config_state: "defaults"`, latched the fault LED, and left `config_`
        // as the constructor's argument. The constructor is PUBLIC, so a caller
        // that passed a non-default config kept running it while the status frame
        // and the LED both claimed defaults: the report and the reality disagreed,
        // which is the exact class of lie spec 6.8 exists to prevent. The device
        // path hides it because it constructs with `ConfigDefault()`, and no test
        // caught it because the store and the constructor carried the SAME config,
        // so "reported defaults" and "running defaults" were indistinguishable.
        //
        // Safe to do here: the output's SAFE state (FR-13) is a DAC code derived
        // from the gain mode and the envelope, not from `config_`, and
        // `EstablishSafeIdle` runs below against the defaults -- which are the
        // same shape it would have used anyway. `timings_` is assigned from
        // `config_.settings.timings` further down, so the fallback propagates to
        // the classifiers too.
        ConfigDefault(&config_);
        buzzer_ = BuzzerGrammar(hal_, config_.settings.buzzer_level);
        leds_ = LedGrammar(hal_, config_.settings.led_level);
    }
    // kNoConfig leaves config_ as supplied (the caller's defaults) -- the
    // pass-through case. Nothing here starts a link, so "no config" cannot
    // become "no steering wheel".
    //
    // FR-25 / spec 6.9: mark it and auto-detect the reference the wheel will be
    // measured against. With no config there is no LEARNED idle, so the live
    // reading is the only one available -- and a live reading is exactly what
    // spec 6.3's ratio normalization needs.
    pass_through_ = (result == ConfigLoadResult::kNoConfig);

    // The RUNTIME timings come from the loaded config, and re-deriving them here
    // is load-bearing. The device path (`SystemOrchestratorCreate`) constructs
    // with `ConfigDefault()`'s timings, and every channel's classifier and gesture
    // machine is built from `timings_` in step 3 below -- so without this a user's
    // `long_press_ms` / `double_press_off_ms` / `debounce_ms` / `send_duration_ms`
    // were stored, reported in `config_get`, and SILENTLY IGNORED at runtime.
    // Measured before the fix: a config with `long_press_ms = 1500` still fired
    // LONG at the default 750 ms.
    //
    // `settings.timings` is the authority, not the constructor's argument. A
    // caller that passes different timings (every host test, to drive the loop
    // faster) is overridden by a stored config -- which is correct, because the
    // stored config is what the device will run with, and a test that wants
    // specific timings should store them.
    timings_ = config_.settings.timings;
    // Same reasoning for the maintenance window: the constructor hardcodes spec
    // 8.2's 5-minute default, but a config may carry its own, and `Boot` is the
    // only place that has the loaded config.
    maintenance_ = MaintenanceMode(hal_, config_.settings.maintenance_timeout_ms);

    // Record what the load actually did, so `status`'s `config_state` carries the
    // CONFIG's state rather than the output's (see ConfigStateWord).
    switch (result) {
        case ConfigLoadResult::kLoaded:             config_state_ = BootConfigState::kOk; break;
        case ConfigLoadResult::kNoConfig:           config_state_ = BootConfigState::kNone; break;
        case ConfigLoadResult::kRecoveredFromBackup: config_state_ = BootConfigState::kRecovered; break;
        case ConfigLoadResult::kFellBackToDefaults: config_state_ = BootConfigState::kDefaults; break;
    }

    // 2. Establish safe idle. This is BEFORE anything else that could accept a
    //    command (FR-13), and before the per-channel state exists.
    EstablishSafeIdle();

    // 3. Now the per-channel state.
    channel_count_ = (config_.channel_count <= kMaxChannels) ? config_.channel_count
                                                             : kMaxChannels;
    // FR-25's pass-through reference is captured PER CHANNEL inside
    // `SeedChannelState`, because the two SWC inputs are independent wheels
    // (FR-9) with their own idles. A single device-wide reference taken from
    // channel 0 made the SECOND channel's idle look like a press on the first
    // whenever the two differed by more than kPassThroughPressDeltaMv -- a key
    // driven every tick with nothing held, which is the phantom-key hazard
    // FR-15/FR-39 exist to prevent. A disconnected second input (reading ~0) did
    // it unconditionally.
    const bool any_reference = SeedChannelState();

    // No usable reference on ANY channel: the ladders are unpowered or unreadable.
    // Pass-through is then impossible, and guessing a denominator would map every
    // press to a voltage nothing defined. Serve nothing rather than drive a
    // fabricated key. (A channel with no reference of its own serves nothing while
    // its siblings still pass through -- see ServiceChannel.)
    //
    // No logging here on purpose: this file is HOST-compiled (the native suite
    // runs it), so it cannot call esp_log. The state is observable through
    // PassThroughActive(). (That accessor's only reader today is a test: the
    // `status` frame does not carry a pass-through field, so nothing on the LINK
    // reports it.) The mode is keyed on `kNoConfig`, i.e. the wire word `none`
    // -- spec 4.3: "`none` is FR-25's supported pass-through device, deliberately
    // NOT `defaults`". `defaults` is `kFellBackToDefaults`, the opposite
    // condition: a config that EXISTS and could not be read. An earlier version
    // of this comment named `defaults` as the pass-through condition and claimed
    // the app inferred the mode from it, which is backwards in the condition and
    // false about the app (nothing in it reads `config_state` for pass-through).
    if (pass_through_ && !any_reference) pass_through_ = false;

    // 3b. FR-33's next-boot maintenance trigger (spec 8.2, "config flag on next
    //     boot"). This is the third of §8.2's four entry triggers and the only
    //     one that is a stored setting; the other three are a live USB command,
    //     a live AUX1 hold, and the reset-reason case wired in step 3c below.
    //
    //     **CONSUMED HERE, and the consume is PERSISTED.** Spec 8.2 scopes this
    //     trigger to the NEXT boot, and nothing on the exit path can clear a
    //     stored field (only a whole-config commit rewrites `settings`). Left
    //     set, it would reopen the window on every boot for the life of the
    //     config -- an unbounded maintenance window, the state FR-38 exists to
    //     forbid. So it opens the window ONCE and is then cleared and saved.
    //
    //     **Ordered AFTER the pass-through resolution above, deliberately.**
    //     That step can set `pass_through_ = false`, but it does not return, so
    //     this runs either way and the ordering is for clarity rather than
    //     correctness. It IS ordered BEFORE step 4's feedback so the boot
    //     patterns below are chosen for a window that is already open.
    if (config_.settings.maintenance_on_boot) {
        // Clear in memory FIRST, unconditionally, so the flag is spent for this
        // process whatever the save does. A save that fails therefore costs the
        // user persistence, not a stuck window: the next boot re-reads the OLD
        // stored config and opens once more, which is a bounded, recoverable
        // over-trigger rather than an unbounded one.
        config_.settings.maintenance_on_boot = false;

        // Persist ONLY when the config in force is durable
        // (`BootConfigState::kOk`/`kRecovered`). Saving under a config that just
        // fell back to defaults, or that never existed, would write the DEFAULTS
        // over whatever the user had -- the read-modify-write collapse this
        // project already paid for once. With no durable config there is nothing
        // to update, so the in-memory clear above is the whole action.
        //
        // **The LOCAL `store` from the load above, NOT the `store_` member.**
        // `store_` is null here on the real device: `SetStore` is called from
        // `UsbLinkStart`, which runs AFTER `SystemOrchestratorCreate`/`Boot`, so a
        // `store_ != nullptr` guard skipped this save entirely and the flag was
        // never spent -- measured on the DUT, where a config with the flag set
        // reopened the window on every boot. The host tests missed it because they
        // call `SetStore` BEFORE `Boot`, an order the device never uses. The local
        // store is already at hand (it did the load) and writes to the same NVS
        // keys, so it is the correct handle and needs no member.
        if (config_state_ == BootConfigState::kOk ||
            config_state_ == BootConfigState::kRecovered) {
            store.Save(config_);
        }

        maintenance_.Enter(MaintenanceTrigger::kConfigFlag, hal_->now_ms(hal_->ctx));
    }

    // 3c. §8.2's fourth entry trigger: "Reset-reason + no-config — first-ever boot
    //     with no config offers provisioning." This was the last unreachable
    //     trigger (N-13); the earlier note that it was "redundant" was WRONG on two
    //     counts. First, it predated `IHAL::reset_reason` (N-58), so the reason
    //     simply could not be read when that decision was written. Second, it
    //     conflated two different things: FR-25's pass-through (the device keeps
    //     SERVING the ladder with no config) is not the same as OFFERING the
    //     provisioning window a first-time user needs. A fresh board with no config
    //     is exactly the device that most needs a way in, and a user with no app and
    //     no wheel attached has no other trigger.
    //
    //     **Why it is keyed on a POWER-ON rather than on "no config" alone.** A
    //     software reboot (`reboot` command, an OTA, the 10 s WDT) must NOT re-open
    //     the window on a still-unconfigured device -- that is FR-38's unbounded
    //     window, one reboot away. Only a cold start (a fresh board being brought up)
    //     qualifies. `kNone` covers FR-33's "first-ever boot" case and also a device
    //     whose config partition was erased; a `kDefaults`/`kRecovered` boot has a
    //     config and does not want this. Ordered after 3b, so the stored-flag
    //     trigger is the more specific one when both could apply.
    if (config_state_ == BootConfigState::kNone &&
        hal_->reset_reason(hal_->ctx) == SWC_RST_POWERON) {
        maintenance_.Enter(MaintenanceTrigger::kNoConfigAtBoot, hal_->now_ms(hal_->ctx));
    }

    // 4. Feedback for the load result. A recovered backup is degraded (the user
    //    should know their newest config was lost); a fallback is an error.
    //
    // A degraded ADC CALIBRATION folds in here as SPEC 3.2 requires: a blank
    // eFuse is the same class of condition as a recovered config -- the device
    // runs, but with knowledge it did not have -- so it, too, is announced as
    // `BOOT_DEGRADED` rather than only logged. `Play` REPLACES, so the more
    // severe pattern must be chosen, not played-then-overwritten: a config
    // fallback (`kFaultConfig`) outranks a calibration fallback, which outranks a
    // clean boot. The ordering below states that precedence once.
    if (result == ConfigLoadResult::kFellBackToDefaults) {
        buzzer_.Play(BuzzerPattern::kFaultConfig);
    } else if (result == ConfigLoadResult::kRecoveredFromBackup || calibration_degraded_) {
        buzzer_.Play(BuzzerPattern::kBootDegraded);
    } else {
        buzzer_.Play(BuzzerPattern::kBootOk);
    }

    // `LED_STAT` starts blinking when the load FAILED, and breathing otherwise.
    // A degraded boot is reported on the buzzer, which is transient, so the LED
    // is the only lasting record that the running config is not the user's.
    const bool config_faulted = (result == ConfigLoadResult::kFellBackToDefaults);
    if (config_faulted) {
        // The CONFIG latch, not `ReportFault`'s hardware one: spec 7.3's
        // reboot-only rule is stated for "a hardware condition that does not fix
        // itself", and a corrupt config is not one -- a commit that persists a
        // valid config remediates it, and `ApplyConfig` clears this.
        config_faulted_ = true;
        leds_.SetStat(LedStatPattern::kBlink);
    } else {
        RestatLeds();
    }

    // 5. USB/BLE would be brought up here (Tasks 16/18). Nothing in this class
    //    starts them, which is what makes FR-42 structural rather than a promise.
}

/*
 * Adopt a config that arrived over the link (spec 4.2: committed means
 * PERSISTED **and** running).
 *
 * **Why this is not a reboot.** The app adopts the config it pushed as the
 * device's live state the moment the `ack` arrives, and offers no reboot
 * affordance. So a device still classifying against the previous config makes
 * the app and the device disagree about the bindings the user is looking at: the
 * user saves a binding, sees the app call the device configured, and the button
 * does nothing until a power cycle that a car-installed device may never get.
 * The headless learn path already applied its commit immediately
 * (`ApplyLearnedProfile`, whose comment calls the stale alternative "the worst
 * version of the bug"); this is the whole-config case of the same rule.
 *
 * **A REJECTED config never reaches here.** Every caller applies only after
 * `ConfigStore::Save` returned true, so a refusal leaves BOTH halves untouched:
 * the stored config is the old one and the running config is the old one.
 *
 * **The config's own state IS re-derived, because this function changes it.** The
 * `status` frame's `config_state` describes the config the device is RUNNING, so a
 * commit that puts the user's validated config in force returns it to `ok` and
 * clears the config-fault LED -- see `ConfigStateWord`. It used to be frozen at the
 * boot value, which made the app tell a user who had just re-programmed the device
 * that it had "lost its configuration ... program it again": the field naming a
 * fault the device was no longer in, the exact lie spec 6.8 exists to prevent.
 * `hw_faulted_` is the opposite case and is deliberately NOT touched here: a wiring
 * fault or a collapsed rail is a hardware condition that does not fix itself (spec
 * 7.3), and a config arriving is not evidence about the ladder's wiring. Likewise
 * `pass_through_` is not re-decided here -- see below.
 */
void SystemOrchestrator::ApplyConfig(const Config &c) {
    // FR-25 / spec 6.9: a config has been committed, so there ARE learned windows
    // to classify against and pass-through must end -- the same rule
    // `ApplyLearnedProfile` follows, and for the same reason. A device left in
    // pass-through would mirror the raw wheel onto the head unit and ignore every
    // binding the user just saved, with entirely correct-looking feedback.
    //
    // The reverse is deliberately NOT done: a committed config never TURNS ON
    // pass-through. Pass-through is for a device with no configuration at all,
    // and reaching this function means one exists.
    pass_through_ = false;

    // Spec 4.2 + 4.3: this commit puts the user's validated config in force, so
    // `config_state` describes THAT config and the config-fault LED stands down.
    // See the function comment for why `hw_faulted_` is untouched.
    NoteConfigCommitted();

    config_ = c;

    // `channel_count_` is derived from the config and indexes every per-channel
    // array below, so it is set BEFORE anything reads it. A config carrying more
    // channels than the device has is clamped, exactly as in `Boot`.
    const uint8_t new_count = (config_.channel_count <= kMaxChannels)
                                  ? config_.channel_count
                                  : kMaxChannels;

    // FR-39: a channel this config REMOVES must not leave its KEY line driven.
    // `Tick` services only `channels_[0..channel_count_-1]`, and the pulse
    // timeout, the rail-fault release and the head-unit-gone release all live
    // inside that per-channel service -- so a channel that stops being serviced
    // is never visited again. A `test_key` pulse or a resolved action still in
    // flight when the config lands would hold a phantom key until the next
    // reboot, which is exactly the hazard §6.7/FR-39 exist to prevent. Released
    // against the OLD idle code and gain mode, which are the settings the line
    // was actually driven under -- `EstablishSafeIdle` below only re-derives the
    // channels the NEW config keeps, so a removed channel's pair is still the
    // correct release level for it.
    for (uint8_t i = new_count; i < channel_count_ && i < kMaxChannels; ++i) {
        ReleaseKey(i);
    }
    channel_count_ = new_count;

    // Same re-derivation as `Boot` step 1, and load-bearing for the same reason:
    // every classifier and gesture machine below is built from `timings_`, and
    // both feedback grammars carry the config's levels. Without this the user's
    // `long_press_ms` would be stored, reported in `config_get`, and silently
    // ignored at runtime -- measured once before the fix as a config with
    // `long_press_ms = 1500` still firing LONG at the default 750 ms.
    timings_ = config_.settings.timings;
    // `SetTimeout`, NOT a fresh `MaintenanceMode`: a config can arrive while the
    // provisioning window is OPEN (the web UI talks to the same link), and
    // assigning a newly constructed object would set `active_` back to false and
    // tear the radio down under a user who is mid-provision. See SetTimeout.
    maintenance_.SetTimeout(config_.settings.maintenance_timeout_ms);
    // `SetLevel`, NOT a fresh grammar, and for the same reason one level up: both
    // grammars carry a pattern IN FLIGHT, and a newly constructed one starts at
    // `kNone`/`kOff`, so assignment would SILENTLY CANCEL what is showing. The learn
    // wizard owns both as its prompts (it sets them once on entry, not per tick),
    // and a config push can land mid-learn -- the user's prompt would stop with no
    // explanation. `Boot` may assign because nothing is in flight there yet.
    buzzer_.SetLevel(config_.settings.buzzer_level);
    leds_.SetLevel(config_.settings.led_level);

    // The OUTPUT before the per-channel state, because `SeedChannelState` reads
    // the ADC with the KEY line already released: a key still driven from the old
    // config would be pulling its own ladder and could be mistaken for a press.
    EstablishSafeIdle();
    SeedChannelState();

    // Repaint, so a config that changed a feedback level is visible on the LEDs
    // rather than only in the next `status` frame. A HARDWARE fault is deliberately
    // not cleared (see the function comment): `RestatLeds` ranks it first, so the
    // lamp correctly stays blinking after the apply. The CONFIG fault is the other
    // way round -- `NoteConfigCommitted` above cleared it, so this repaint is what
    // actually returns the lamp to its normal state.
    //
    // **Deferred while the learn wizard is active**, exactly as `Tick` defers its
    // own restate: the wizard owns BOTH LEDs as its prompts until it hands back
    // (`Exit` sets LED_STAT solid itself), so repainting here would stamp a level
    // change over a prompt the user is reading. This is reachable -- the learning
    // screen is gone from the product, but a config push over the link and a
    // headless AUX1 learn can overlap, and a `config_patch` of `settings.led_level`
    // is the shortest way to hit it.
    if (!wizard_.Active()) RestatLeds();
}

void SystemOrchestrator::Tick(uint64_t now_ms) {
    // Spec 6.8's I2C row: a write that fails on the key path must be REPORTED, not
    // merely survived. The HAL latches `dac_faulted` on any failed transmit, and
    // `ReportDacFault` is an edge, so this is one pattern per boot however many
    // writes fail afterwards.
    if (hal_ != nullptr && hal_->dac_faulted(hal_->ctx)) ReportDacFault();
    // FR-1's NTC clause: "sample ... the NTC continuously". Until this existed the
    // channel was read only by the learn paths, so a device serving the wheel never
    // sampled it and `status.temp_c` was permanently `null`. On a fixed cadence
    // rather than every tick -- see `kNtcSampleIntervalMs` for why the physics
    // wants 1 Hz and not 100 Hz.
    //
    // The unsigned compare is the same clock-rewind guard `ShouldTimeout` uses: a
    // `now_ms` earlier than the last sample (a caller bug or a clock reset) must
    // not sample on every tick forever.
    if (now_ms >= ntc_next_sample_ms_) {
        ntc_next_sample_ms_ = now_ms + kNtcSampleIntervalMs;
        SampleNtcTenthsC();
    }
    for (uint8_t i = 0; i < channel_count_; ++i) {
        ServiceChannel(i, now_ms);
        // FR-19's trim, AFTER the channel so it sees this tick's drive state. It
        // is a no-op unless the loop is enabled AND a pulse is on the line AND the
        // servo has settled (see `ServiceTrim`).
        ServiceTrim(i, now_ms);
    }
    // The AUX gesture inputs, after the wheel channels so a shared resolve cannot
    // reorder what the head unit sees from a single tick's presses.
    ServiceAux(now_ms);
    // FR-31: after the channels, so a learn commit is applied before the next
    // tick classifies against the new profile.
    ServiceLearn(now_ms);
    RestoreLedsAfterIdentify(now_ms);
    UpdateLed2ForDrivingState();
    buzzer_.Update(now_ms);
    leds_.Update(now_ms);
}

/*
 * FR-31's headless learn: enter and leave on an AUX1 hold, drive the wizard, and
 * persist what it commits.
 *
 * **Why the hold is detected here from a raw ADC read rather than by the
 * wizard's own classifier.** The hold must be recognized while a learn is
 * RUNNING (it is the exit gesture), and the wizard's classifier is busy counting
 * selection presses at that moment. Two consumers of one debounced signal would
 * each have to know the other's phase; a plain threshold on the raw reading has
 * exactly one job and cannot disagree with itself.
 *
 * **The threshold is the wizard's own AUX1 press window**, taken from the profile
 * rather than written again, for the same reason: a second definition of "AUX1 is
 * held" is a second answer.
 */
void SystemOrchestrator::ServiceLearn(uint64_t now_ms) {
    if (hal_ == nullptr) return;

    const int aux_mv = hal_->adc_read_mv(hal_->ctx, ADC_CH_AUX1);
    const bool aux_pressed = (aux_mv >= 0) && (aux_mv < Aux1PressedMaxMv());

    // The rising edge starts the hold clock; any release clears it. A hold that
    // survives to kEnterHoldMs ARMS the wizard (spec 7.5), and the RELEASE that
    // follows is what commits (spec 7.5: "release the button -> the setting is
    // stored"). The hold no longer toggles a modal state -- the interaction is
    // stateless from the user's side, which is what the 2022 design did and what
    // avoids colliding with the 3 s maintenance tier.
    if (aux_pressed) {
        if (!aux_holding_) {
            aux_holding_ = true;
            aux_hold_ms_ = now_ms;
        } else if ((now_ms - aux_hold_ms_) >= kMaintenanceHoldMs && !maint_fired_latch_) {
            // Checked FIRST, and that ordering is load-bearing: the 1.5 s tier
            // below sets `aux_hold_latch_`, so a separate "already fired" branch
            // ahead of this one would swallow every tick after the programming
            // hold and the 3 s tier would be UNREACHABLE -- the escalation spec
            // 8.2 requires would silently never happen.
            //
            // Spec 8.2 nests the two holdings: 1.5 s is PROGRAMMING and 3 s is
            // MAINTENANCE, the shorter a subset of the longer, so holding too long
            // to program escalates cleanly into maintenance. **A user who over-held
            // is trying to reach maintenance, not to finish a programming session**,
            // so a running learn is ABANDONED (no commit) before the window opens --
            // committing here would store a half-measured button the user was not
            // trying to complete.
            maint_fired_latch_ = true;
            if (wizard_.Active()) {
                wizard_.Abandon(now_ms);
                // The abandoned hold may have been holding a programming level on
                // the line; let it go, or the escalation to maintenance would leave
                // a key driven (spec 6.7's held-key hazard).
                ReleaseProgramHold();
            }
            maintenance_.Enter(MaintenanceTrigger::kAux1Hold, now_ms);
            // N-61: say WHY on the buzzer, which the app-opened path cannot. A
            // user holding AUX1 at the car has no phone in hand, so the one
            // channel they have is sound -- and without this the AUX1 hold is
            // indistinguishable from the app opening the window, which the enum's
            // own doc-comment claimed the buzzer existed to distinguish. Played
            // on the ENTER edge (the latch above), not per tick.
            buzzer_.Play(BuzzerPattern::kProgramEnter);
        } else if ((now_ms - aux_hold_ms_) >= LearnWizard::kEnterHoldMs && !aux_hold_latch_) {
            // The hold has armed the wizard. The LATCH, not this flag, prevents a
            // second arm: the flag is cleared so the next release re-arms.
            aux_hold_latch_ = true;
            if (!wizard_.Active()) wizard_.Arm(now_ms);
        }
    } else {
        // AUX1 released: COMMIT whatever was being programmed (spec 7.5). Done
        // here, on the release edge, rather than in the wizard, because the hold
        // detector is what sees the edge.
        if (wizard_.Active() && aux_holding_) {
            wizard_.Release(now_ms);
        }
        // Spec §7.5: "release the AUX1 line to release the output". The line the
        // programming hold was HOLDING must be let go now -- the head unit has
        // captured the level, and leaving it driven is a held key (spec 6.7's
        // hazard). This runs whether or not the wizard had committed, so a hold
        // that named no input still releases.
        ReleaseProgramHold();
        aux_holding_ = false;
        aux_hold_latch_ = false;
        maint_fired_latch_ = false;
    }

    // FR-38: the window closes on its own after the configured timeout, so a
    // device left unable to serve input because someone opened a web page cannot
    // happen. Update() is what performs the close.
    //
    // **The close is measured from the LAST ACTIVITY, not from entry** (N-35,
    // resolved with the radio). The activity source is the maintenance HTTP
    // server: `MaintenanceRadioRequestCount` counts every request it serves, and
    // `main.cpp`'s poll loop calls `NoteMaintenanceActivity` whenever that count
    // has moved. So a user reading the status page or typing a WiFi password is
    // not reaped mid-task, which is what spec 8.2 and FR-38 specify.
    //
    // The wiring lives in `main.cpp` rather than here because the HTTP server is
    // device-only (`MaintenanceRadio.cpp`, the third host-excluded TU) and this
    // file is host-compiled: naming it here would cost this state machine its
    // tests. This call site therefore only performs the close, and an earlier
    // comment claimed the close was ALREADY on inactivity while nothing produced
    // an activity event at all -- the false-comment shape that kept N-35
    // invisible for a revision.
    maintenance_.Update(now_ms);

    // Restate the LEDs when the maintenance window opens or closes, whatever
    // opened or closed it. `Update` above performs the timeout close, which is the
    // one transition with no caller to notify -- so the indication cannot be driven
    // from the entry/exit sites alone.
    //
    // There is deliberately NO `faulted_` term here: the fault-outranks-maintenance
    // ordering lives in `RestatLeds` and nowhere else, so asking about the fault a
    // second time would be a second answer to the same question. A fault that
    // arrives during an open window repaints through `ReportFault`'s own
    // `RestatLeds` call, from the channel loop that runs before this.
    //
    // DEFERRED while the learn wizard is active: the wizard owns LED_STAT until
    // it hands back, so repainting mid-learn would fight its prompts.
    //
    // The deferred repaint is tracked as an explicit "owed" flag rather than by
    // writing a sentinel into `maintenance_led_state_`. The sentinel version
    // wrote `!want_maint_led` -- which is only guaranteed to mismatch the FUTURE
    // state when that state is false. The 3 s AUX1 tier opens maintenance in the
    // SAME tick it exits the wizard, so the post-defer want is TRUE while the
    // sentinel had already stored true: the comparison saw no edge, never
    // repainted, and the wizard's `kSolid` handback stuck -- a no-host device
    // showing "USB connected" for the whole maintenance window, where spec 8.2
    // requires the double-flash. The flag now means exactly what the header says
    // it means (the last restated state) and cannot be desynced by the deferral.
    const bool want_maint_led = maintenance_.Active();
    if (wizard_.Active()) {
        leds_owed_restat_ = true;
    } else if (leds_owed_restat_ || want_maint_led != maintenance_led_state_) {
        leds_owed_restat_ = false;
        maintenance_led_state_ = want_maint_led;
        RestatLeds();
    }

    if (wizard_.Active()) {
        // The candidate inputs, each with its LIVE idle -- the wizard judges
        // "which one left idle" against these, so a cached value would compare a
        // reading to a stale denominator. SWC1/SWC2 are ladders (their button ids
        // are generated); AUX2/AUX3 are switches and carry their config ids.
        // AUX1 is the modifier and is deliberately absent.
        LearnInputs inputs;
        for (uint8_t i = 0; i < channel_count_ && i < kMaxChannels && inputs.count < kMaxLearnInputs;
             ++i) {
            LearnInput &in = inputs.in[inputs.count++];
            in.wire_channel = i;
            in.adc = (i == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2;
            in.idle_mv = IdleReferenceMv(i);
            in.is_ladder = true;
            in.existing = &config_.channels[i].ladder;
            in.id = nullptr;
        }
        for (uint8_t i = 1; i < aux_count_ && i < kMaxAuxButtons && inputs.count < kMaxLearnInputs;
             ++i) {
            LearnInput &in = inputs.in[inputs.count++];
            in.wire_channel = AuxWireChannel(i);
            in.adc = AuxAdcChannel(i);
            // An AUX switch's idle IS the rail (spec 2.4), and its live reading is
            // the reference the classifier normalizes against (`ServiceAux` uses
            // the same constant, so the two cannot disagree).
            in.idle_mv = kNominalRailMv;
            in.is_ladder = false;
            in.existing = nullptr;
            in.id = config_.aux[i].id;
        }
        wizard_.Tick(inputs, now_ms, SampleNtcTenthsC());
    }

    if (wizard_.ConsumeCommitted()) {
        ApplyLearnedResult(wizard_.TargetWireChannel(), wizard_.TargetIsLadder(),
                           wizard_.Profile());
    }
    if (wizard_.ConsumeExited()) {
        // The wizard drove the LEDs for its prompts; hand them back so the normal
        // grammars resume rather than leaving a stale solid LED2 behind.
        leds_.Set2(Led2Pattern::kOff);
    }
}

/*
 * Apply what a headless learn just produced. TWO kinds of result, because a
 * headless learn serves two kinds of input (spec 7.4/7.5):
 *
 *  - a LADDER (SWC1/SWC2): the profile is the channel's whole ladder (seed plus
 *    the measured button), so it is ASSIGNED over the channel's ladder and the
 *    channel is re-seeded;
 *  - a SWITCH (AUX2/AUX3): the profile holds one measured window, which is
 *    written into that AUX input's `aux[]` entry -- an AUX input's window IS its
 *    config (spec 3.1), not a ladder.
 *
 * The one home for both is here, so the two learn paths (this and
 * `CommandRouter::HandleLearnCommit`) write a config the same way.
 */
void SystemOrchestrator::ApplyLearnedResult(uint8_t wire_channel, bool is_ladder,
                                            const LadderProfile &profile) {
    if (is_ladder) {
        const int channel = static_cast<int>(wire_channel);
        if (channel < 0 || channel >= channel_count_) return;
        learned_channel_ = channel;
        learned_profile_ = profile;
        ApplyLearnedProfile(channel, profile);
        return;
    }

    // A switch: find the `aux[]` entry this wire channel names and write the
    // measured window into it. The wire channel is `kAuxWireChannelBase + index`.
    const int aux_index = static_cast<int>(wire_channel) - static_cast<int>(kAuxWireChannelBase);
    if (aux_index < 0 || aux_index >= kMaxAuxButtons) return;
    if (profile.count == 0) return;
    const LadderButton &b = profile.buttons[0];

    if (aux_index >= config_.aux_count) {
        // The input is present in the hardware but has never been configured, so
        // there is no entry to write. Grow the table to include it, which is what
        // learning an AUX switch for the first time means.
        config_.aux_count = static_cast<uint8_t>(aux_index + 1);
        memset(&config_.aux[aux_index], 0, sizeof(config_.aux[aux_index]));
    }
    AuxButtonConfig &a = config_.aux[aux_index];
    // The id, if the entry had none -- a fresh entry needs one, and the learn
    // generated `aux<n>` for exactly this.
    if (a.id[0] == '\0' && b.id[0] != '\0') {
        strncpy(a.id, b.id, sizeof(a.id) - 1);
    }
    a.source = static_cast<uint8_t>(aux_index + 1);   // 1-based, spec 3.1
    a.mv_center = static_cast<int16_t>(b.mv_center);
    a.mv_tolerance = static_cast<int16_t>(b.mv_tolerance);

    // Re-seed the AUX servicing state so the new window takes effect on the next
    // tick rather than at the next boot.
    SeedAuxState(static_cast<uint8_t>(aux_index));

    // Persist, and report durability the same way the ladder path does -- the
    // RESULT of Save, not the pointer.
    persisted_ = (store_ != nullptr) && store_->Save(config_);
    if (persisted_) NoteConfigCommitted();
}

void SystemOrchestrator::ApplyLearnedProfile(int channel, const LadderProfile &profile) {
    if (channel < 0 || channel >= kMaxChannels) return;
    if (channel >= channel_count_) return;

    // The profile carries its own idle reference: the wizard measured every
    // centre against the idle captured at learn time, so it is already correct
    // here and must not be re-derived from the current reading.
    config_.channels[channel].ladder = profile;

    // FR-25's pass-through ends HERE, and forgetting this is how a headless learn
    // silently does nothing on a fresh device. Pass-through RETURNS EARLY from
    // ServiceChannel -- it exists precisely because there are no learned windows to
    // classify against -- so a device that learned a button while still in
    // pass-through would beep LEARN_OK and then ignore the button it just taught,
    // with entirely correct-looking feedback. The device now HAS windows, so the
    // condition that justified pass-through no longer holds.
    //
    // The learned PROFILE is the trigger, not a stored config: a user can teach a
    // level with no app and no config, and that is the case this has to serve.
    pass_through_ = false;

    // Rebuild the classifier so the new windows take effect immediately. Without
    // this the device would keep classifying against the OLD profile until the
    // next boot, and the button the user just taught would do nothing -- with
    // perfectly correct-looking feedback, which is the worst version of the bug.
    channels_[channel].classifier = PressClassifier(profile, timings_);
    channels_[channel].gestures.Reset();
    channels_[channel].reader.Reset();

    // No cached binding set to rebuild: the resolve reads the pressed button's
    // bindings fresh each tick (`BindingsForButton`), so a profile change is
    // picked up with no second copy to refresh.

    // Persist. The wizard cannot: it holds no Config and no store.
    //
    // **The RESULT is what `persisted_` must report, not the POINTER.** An earlier
    // revision wrote `persisted_ = (store_ != nullptr)`, which answers "is there a
    // store attached" and calls that "the learn was saved". So a full NVS, a failed
    // write, or a config the store refused all reported the learn as durable: the
    // user hears LEARN_OK, the app is told nothing is wrong, and the button is gone
    // at the next boot with no explanation anywhere. `Save` returns false precisely
    // for those cases, and the return value was being discarded.
    //
    // A null store is still a real distinction worth keeping -- a bench build with
    // no NVS learns correctly and cannot persist -- and it is reported the same way
    // as a failed write, because the user-visible fact is identical: not durable.
    persisted_ = (store_ != nullptr) && store_->Save(config_);

    // A learned profile that PERSISTED is a committed config (spec 7.4 / 4.2): the
    // user has just programmed the device, so `config_state` describes the config
    // now in force and a config-fault latch stands down -- the same rule as
    // `ApplyConfig`. **Gated on `persisted_`, and ordered after the save**, because
    // the reverse would report `ok` and stand the fault down over a config that
    // never reached NVS: the device would come back corrupt -- or defaulted -- at
    // the next boot, having told the app, in the meantime, that the user's config
    // was safe. That is the "reported success for a failed write" lie
    // `persisted_`'s own fix above exists to prevent, one layer up.
    if (persisted_) NoteConfigCommitted();
}

GestureBindings SystemOrchestrator::AuxBindingsForInput(uint8_t aux_index,
                                                       const char *input_id) const {
    GestureBindings out;
    // The conservative default, like `BindingsForButton`: a caller that cannot
    // name the input must not have its press resolved early.
    out.has_double = true;
    out.has_long = true;
    if (aux_index >= aux_count_ || config_.binding_count > kMaxBindings) return out;
    // On release the input's own id is supplied by the caller; the gesture
    // machine holds the press's button index, which for an AUX input is 0 (its
    // only button). Both name the same input, so use the config's id.
    const char *id = input_id;
    if (id == nullptr) {
        if (aux_[aux_index].gestures.Button() != 0) return out;
        id = config_.aux[aux_index].id;
    }
    if (id[0] == '\0') return out;
    const uint8_t as_aux =
        static_cast<uint8_t>(BindingChannel::kAux1) + aux_index;
    out.has_double = false;
    out.has_long = false;
    for (uint8_t i = 0; i < config_.binding_count; ++i) {
        const Binding &b = config_.bindings[i];
        if (!b.enabled) continue;
        if (b.channel != as_aux && b.channel != static_cast<uint8_t>(BindingChannel::kAny)) {
            continue;
        }
        if (strcmp(b.button, id) != 0) continue;
        if (b.gesture == Gesture::kDouble) out.has_double = true;
        if (b.gesture == Gesture::kLong) out.has_long = true;
    }
    return out;
}

void SystemOrchestrator::ReportGesture(uint8_t index, const GestureEvent &ev, int level_mv) {
    if (gesture_sink_ == nullptr) return;

    // A BOUNDS check, not a validation: `button_index` is an array index, and an
    // index that reached here stale would read past a fixed-size array rather than
    // merely produce a wrong frame. `Emit` in the gesture machine writes
    // `button_`, which `Reset()` sets to 0xFF, so a gesture completing after a
    // reset is exactly the case where the index is not a button at all. There is
    // nothing to report then, and reporting nothing is also correct: the device
    // did not classify a button. (An EMPTY id needs no check here -- the config
    // codec refuses to decode one, so every loaded config has real ids.)
    if (ev.button_index >= config_.channels[index].ladder.count) return;

    // The button's learned id, which is what the app's bindings grid and ladder
    // view are keyed by -- an index would force every consumer to re-derive a
    // mapping the device already has.
    const char *id = config_.channels[index].ladder.buttons[ev.button_index].id;
    const GestureEventRecord rec{index, id, ev.gesture, level_mv, IdleReferenceMv(index),
                                 ev.at_ms};
    gesture_sink_(gesture_sink_ctx_, rec);
}

void SystemOrchestrator::ReportInputGesture(uint8_t wire_channel, const char *button_id,
                                            Gesture g, int level_mv, uint64_t at_ms) {
    if (gesture_sink_ == nullptr) return;
    // An AUX switch has no learned idle of its own -- it is a switch, not a
    // ladder -- so the denominator the classifier used is the nominal rail, the
    // same constant `ServiceAux` normalizes against. Reporting the same value the
    // decision was made on is what keeps the app's ratio a copy of the device's.
    const GestureEventRecord rec{wire_channel, button_id, g, level_mv, kNominalRailMv, at_ms};
    gesture_sink_(gesture_sink_ctx_, rec);
}

/*
 * The AUX gesture inputs (AUX2/AUX3), spec 3.1/3.5.
 *
 * An AUX input is a switch, not a ladder, so this is simpler than
 * `ServiceChannel`: one window, no pass-through (spec 6.9's pass-through is a
 * property of an unconfigured WHEEL), no servo. What it shares is the parts that
 * must not have two implementations -- the classifier, the gesture machine, the
 * FR-12 unknown report and the binding resolve.
 *
 * **AUX1 is not serviced here.** It is the programming hold (1.5 s) and the
 * maintenance hold (3 s) per spec 7.5/8.2, so it starts at index 1.
 */
void SystemOrchestrator::ServiceAux(uint64_t now_ms) {
    // While the headless learn is armed, an AUX press is being TAUGHT, not driven
    // -- the same suppression `ServiceChannel` applies, for the same reason: the
    // user holds AUX1 to program, so a press during the hold must not fire a
    // binding. The readers still run so the wizard's own detection sees the level.
    if (wizard_.Active()) {
        for (uint8_t i = 0; i < aux_count_; ++i) aux_[i].reader.Update(now_ms);
        return;
    }
    for (uint8_t i = 1; i < aux_count_; ++i) {
        AuxState &as = aux_[i];
        const AuxButtonConfig &ac = config_.aux[i];

        // FR-3's filter, the same one the wheel channels use, so AUX noise
        // rejection has one definition.
        as.reader.Update(now_ms);
        const MilliVolt level_mv = as.reader.Value();
        // Idle is the rail for a switch to ground, and `AuxProfileFor` stores it
        // as the profile's `learned_idle_mv`; the classifier normalizes against
        // the SAME value so an AUX press is measured on the scale its window was
        // written in.
        const int idle_mv = kNominalRailMv;
        const ChannelLevel level = as.classifier.Update(level_mv, idle_mv, now_ms);

        // The pressed input's own bindings, with the same per-button granularity
        // the channels use (spec 6.6 rule 3). An AUX input has one button, so the
        // index is always 0 -- but the gesture machine's tracked button is read on
        // release, so this is not a constant.
        GestureBindings for_button;
        {
            const char *pressed_id = (level == ChannelLevel::kPressed) ? ac.id : nullptr;
            for_button = AuxBindingsForInput(i, pressed_id);
        }
        GestureEvent ev{};
        const bool fired = as.gestures.Update(level, as.classifier.ButtonIndex(), now_ms, &ev,
                                              for_button);

        if (level == ChannelLevel::kUnknown) {
            if (!as.unknown_reported) {
                as.unknown_reported = true;
                // FR-12 for an AUX input: a press outside the window is reported
                // with a null button, exactly as an unlearned wheel level is. The
                // level is not a learned button's, so naming one would be the guess
                // FR-12 forbids.
                ReportInputGesture(AuxWireChannel(i), nullptr, Gesture::kNone, level_mv, now_ms);
                buzzer_.Play(BuzzerPattern::kKeyUnknown);
            }
        } else {
            as.unknown_reported = false;
        }

        if (!fired) continue;

        const ResolvedBinding resolved = BindingResolveAux(config_, i, ev);
        ReportInputGesture(AuxWireChannel(i), ac.id, ev.gesture, level_mv, ev.at_ms);
        if (resolved.found) {
            // AUX actions drive KEY channel 0: the spec gives an AUX input no
            // output line of its own, and the primary head-unit input is the
            // choice that needs no new config field.
            RunBindingActions(0, resolved, now_ms);
        } else {
            // Recognised but unbound: spec 6.6 rule 4's pass-through is a property
            // of the WHEEL's own level, and an AUX switch has no ladder level to
            // present, so there is nothing for the head unit to see. The press is
            // reported (above) and otherwise inert -- which is the honest
            // behaviour, not a fabricated key voltage.
            buzzer_.Play(BuzzerPattern::kKeyUnknown);
        }
    }
}

bool SystemOrchestrator::TestDriveKeyMv(uint8_t channel_index, int key_mv, uint32_t hold_ms,
                                       uint64_t now_ms) {
    if (channel_index >= channel_count_) return false;
    // The envelope is the servo's contract (spec 6.2). Refusing out-of-range is
    // the point of the check: a bench command that could exceed the clamp would
    // be a way to discover the clamp is missing.
    if (key_mv < kOutputFloorMv || key_mv > kOutputCeilingMv) return false;
    if (!safe_idle_established_) return false;

    ChannelState &cs = channels_[channel_index];

    // Shares the servo with the action path, so what the bench measures is what
    // a real press produces -- a direct dac_set_code would bypass the trim loop
    // and measure a different thing.
    //
    // The sense read is deliberately NOT fed to `Update()` here: it precedes the
    // write, so it is the line's idle level. `ServiceTrim()` trims the driven
    // line after it settles -- see `DriveBoundLevelMv` for the measurement that
    // found this.
    cs.servo.Target(gain_mode_[channel_index], key_mv);
    DriveKeyCode(channel_index, cs.servo.Code());
    cs.key_driven = true;
    // A test command has no gesture to resolve, so it drives immediately and
    // releases on the caller's hold time.
    cs.key_released_at_ms = now_ms + (hold_ms ? hold_ms : 1);
    cs.trim_next_ms = now_ms + kServoTrimSettleMs;
    return true;
}

/*
 * Spec 7.3's normal `LED_STAT`: solid when a host is attached, breathing when
 * not. Called on boot and on every link transition.
 *
 * The fault wins, because spec 7.3 says the LEDs answer "is this thing OK?" at a
 * glance and a device that is both faulted and connected is NOT OK. Without this
 * precedence a link connect mid-fault would quietly repaint the LED to green,
 * which is the one reading the LED exists to prevent.
 */
void SystemOrchestrator::RestatLeds() {
    if (Faulted()) {
        leds_.SetStat(LedStatPattern::kBlink);
        return;
    }
    // Maintenance's window is shown as a double-flash (spec 8.2: "LED_STAT stops
    // the maintenance double-flash" on exit, which presumes it started one).
    // Ranked below the fault, because a device that is both faulted and in
    // maintenance is NOT OK, and above the link states, because a maintenance
    // window is a deliberate state the user opened and can otherwise not see.
    if (maintenance_.Active()) {
        leds_.SetStat(LedStatPattern::kDoubleFlash);
        return;
    }
    leds_.SetStat(usb_connected_ ? LedStatPattern::kSolid : LedStatPattern::kBreathe);
}

/*
 * LED2, the ACTIVITY channel (spec 7.3): solid while a key is actually being
 * presented, off otherwise.
 *
 * **This is the one LED2 state that is a real diagnostic, and it was
 * unreachable.** Spec 7.3 says the user can see the adapter holding a key, "which
 * distinguishes 'the adapter is doing something wrong' from 'the head unit is
 * ignoring it'" -- but nothing ever set anything except `kOff`: the only `Set2`
 * callers were the learn wizard. A user debugging a dead button had no way to
 * tell a firmware that never drove the line from a radio that never listened.
 *
 * **Derived from the channels, not set at each driven/released site.** There are
 * eight places that flip `key_driven`, and a `Set2` at each would be eight chances
 * to miss one and leave the LED lying. Asking the state which channel is driving
 * cannot disagree with the state.
 *
 * **It defers to the learn wizard, and to nothing else.** The wizard drives both
 * LEDs as prompts, so the derivation must not fight it. Maintenance is
 * deliberately NOT deferred to: the wizard is the only LED2 owner, and the
 * maintenance indication is `LED_STAT`'s double-flash (spec §8.2), which this
 * never touches. Deferring to maintenance as well was in the first version of this
 * function and was wrong -- the symptom would have been a missing activity LED for
 * the life of a maintenance window, which nothing would have caught.
 *
 * **Be precise about what this guard buys, because during the learn's PROMPT
 * phase it is belt-and-braces, not load-bearing.** While `leds_` is in
 * `kAlternate`, `LedGrammar::Update` makes LED2 the complement of `LED_STAT` and
 * IGNORES `Set2` entirely -- so a `Set2(kSolid)` there has no effect whether this
 * guard exists or not. The guard still earns its place for the window between the
 * wizard leaving `kAlternate` and `ConsumeExited` handing the LEDs back, where a
 * call here WOULD land. I tried four ways to write a test that fails when this
 * guard is deleted and could not, which is the honest reason this comment says
 * "belt-and-braces" instead of pointing at a test.
 */
void SystemOrchestrator::UpdateLed2ForDrivingState() {
    if (wizard_.Active()) return;

    bool any_driving = false;
    for (uint8_t i = 0; i < channel_count_; ++i) {
        if (channels_[i].key_driven) {
            any_driving = true;
            break;
        }
    }
    const Led2Pattern want = any_driving ? Led2Pattern::kSolid : Led2Pattern::kOff;
    if (want != led2_driving_) {
        led2_driving_ = want;
        leds_.Set2(want);
    }
}

void SystemOrchestrator::Identify(bool flash, bool buzz) {
    // Both channels, because "which unit is this" is a question about the box,
    // not about one steering-wheel input.
    //
    // `flash` and `buzz` are separate because the caller (spec 4.3's `identify`)
    // offers them as separate patterns: a user looking at the wheel wants the
    // buzzer, one looking at the box wants the flash. Collapsing them -- which the
    // router used to do -- is an "accepts a field and ignores it" defect, since a
    // `buzz` request also drove the LEDs.
    //
    // The double-flash BORROWS LED_STAT and must hand it back (spec 7.3). Every
    // other borrower restores: the learn wizard's `Exit` sets the link state, a
    // maintenance close repaints via `RestatLeds`, and a USB transition does too.
    // This one used to simply set the pattern, so the LED double-flashed FOREVER
    // after an `identify` (until the next reboot), and it bypassed `RestatLeds`
    // entirely -- so an `identify` on a FAULTED device repainted the latched fault
    // blink to a double-flash, the exact "must not repaint the lamp green" hazard
    // spec 7.3 forbids. The restore is driven from Tick so the fault precedence
    // lives in ONE place (`RestatLeds`) rather than being re-derived here.
    if (flash) {
        leds_.SetStat(LedStatPattern::kDoubleFlash);
        // Only the flash BORROWS LED_STAT, so only it needs the restore window --
        // a buzz-only identify must not leave a phantom latch that repaints the
        // LEDs 1.5 s later.
        identify_active_ = true;
        identify_until_ms_ = hal_->now_ms(hal_->ctx) + kIdentifyFlashMs;
    }
    if (buzz) buzzer_.Play(BuzzerPattern::kKeyAccepted);
}

void SystemOrchestrator::RestoreLedsAfterIdentify(uint64_t now_ms) {
    if (!identify_active_) return;
    if (now_ms < identify_until_ms_) return;
    identify_active_ = false;
    // The wizard owns both LEDs while it runs, so let it keep them; otherwise
    // repaint through `RestatLeds`, which already ranks fault > maintenance >
    // link and is the single answer to "what should LED_STAT show now".
    if (!wizard_.Active()) RestatLeds();
}

/*
 * Let go of a line the programming hold was holding (spec §7.5). Called on the
 * AUX1 release edge from `ServiceLearn`, which is the one place that sees the
 * edge AND still knows which channel was being held -- by then the wizard has
 * already handed back and no longer reports a target.
 */
void SystemOrchestrator::ReleaseProgramHold() {
    if (program_hold_channel_ >= kMaxChannels) return;
    ReleaseKey(program_hold_channel_);
    program_hold_channel_ = 0xFF;
    program_hold_slot_ = -1;
}

void SystemOrchestrator::ReleaseKey(uint8_t index) {
    ChannelState &cs = channels_[index];
    if (!cs.key_driven) return;
    // Re-point the servo at idle BEFORE writing, so a later `Update()` trims
    // toward idle rather than toward the key just released.
    cs.servo.Target(gain_mode_[index],
                    GainPolicyKeyMvForCode(gain_mode_[index], idle_code_[index]));
    // Through `DriveKeyCode`, not a bare KEY write: in tracking mode the idle
    // code must reach V_ADJ too, or the release leaves the 1.82 gain engaged and
    // the "idle" command over-drives the 3 V line -- a held-key hazard (spec 6.7).
    DriveKeyCode(index, idle_code_[index]);
    cs.key_driven = false;
}

/*
 * FR-19's trim, serviced from `Tick` rather than at command time.
 *
 * **Why not at command time: measured, and it was wrong.** The sense reading
 * available in `DriveBoundLevelMv`/`TestDriveKeyMv` is taken BEFORE the command is
 * written, so it is the line's idle level. Feeding that to `ServoLoop::Update`
 * made the enabled loop apply one full `max_step` in the wrong direction on every
 * press: measured on the FR-19 bench, the static error was +11.7 mV with the loop
 * enabled versus disabled -- exactly one 8-code step at gain 1.82 -- with no added
 * noise. The loop can only trim toward the code it drove if it measures that code's
 * RESULT, which means running after the servo has settled.
 *
 * A no-op unless the loop is enabled (the shipped posture is disabled) and a pulse
 * is actually on the line: trimming a released line would move the IDLE code,
 * which is a held-key hazard, not a trim.
 */
void SystemOrchestrator::ServiceTrim(uint8_t index, uint64_t now_ms) {
    ChannelState &cs = channels_[index];
    if (!cs.key_driven) return;
    // The unsigned compare is the same clock-rewind guard the NTC cadence uses: a
    // `now_ms` earlier than the last update must not run the trim every tick.
    if (now_ms < cs.trim_next_ms) return;
    cs.trim_next_ms = now_ms + kServoTrimIntervalMs;

    const int sense_mv = hal_->adc_read_mv(hal_->ctx,
                                           (index == 0) ? ADC_CH_KEY_SENSE1 : ADC_CH_KEY_SENSE2);
    // A -1 is a FAILED conversion, not a measurement (N-43's rule): hold rather
    // than trim toward a sentinel.
    if (sense_mv < 0) return;

    // Only rewrite the DAC when the loop actually moved the code: `Update` returns
    // false inside the deadband, and a redundant I2C write every cadence tick is
    // both pointless and a second thing that can latch a DAC fault.
    if (cs.servo.Update(sense_mv)) DriveKeyCode(index, cs.servo.Code());
}

void SystemOrchestrator::DriveKeyCode(uint8_t index, uint16_t code) {
    const DacChannel key_ch = (index == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;
    hal_->dac_set_code(hal_->ctx, key_ch, code);
    // Tracking mode needs V_ADJ == V_DAC (spec 2.3). Amplified mode already has
    // the ADJ channel in its 1 kohm power-down (set once at gain selection), so
    // mirroring there would defeat the 1.82 gain the mode exists for.
    if (gain_mode_[index] == GainMode::kTracking) {
        const DacChannel adj_ch = (index == 0) ? DAC_CH_ADJ1 : DAC_CH_ADJ2;
        hal_->dac_set_code(hal_->ctx, adj_ch, code);
    }
}

// Spec 3.6's `OUT_VOLTAGE`: drive a channel to a level the action NAMES. There is
// no head-unit model here and no resistance to convert -- the app is what converts
// a head unit's resistance to this voltage, and the action carries the voltage.
//
// Spec 6.2's command band comes FIRST, and it is the bound that was missing
// entirely. "Command targets must stay inside `[min_ladder, V_KEY_idle - 0.20 V]`
// so the sink FET is never asked to drive above the line's own resting level --
// above that point the servo can only turn `Q4` off, which is the release
// behavior, not a command." The envelope clamp is NOT that bound: it permits any
// target from 1800 to 5200 mV, so a `key_mv` sitting between the head unit's own
// idle and the envelope ceiling was accepted by `ConfigValidate`, sent back as an
// ack and persisted -- and then drove the radio to nothing at all, because the
// output only sinks. A press that silently does nothing is the failure the user
// cannot tell from a broken adapter.
//
// Returns false on the empty band: a head unit idling so low that no reachable
// level is below its rest. There is no command to make then, and the caller
// reports the press as un-acknowledged rather than quietly releasing it, which is
// the same direction FR-12 takes for a level matching no window.
bool SystemOrchestrator::DriveBoundLevelMv(uint8_t index, int key_mv,
                                           uint64_t now_ms) {
    ChannelState &cs = channels_[index];
    bool band_clamped = false;
    const int target_key_mv =
        GainPolicyClampCommand(key_mv, IdleKeyMv(index), &band_clamped);
    if (target_key_mv == 0) return false;

    // FR-18: a key_mv outside the gain mode's envelope is VALIDATED and CLAMPED,
    // with a warning -- never driven out of range. The clamp itself lives in
    // GainPolicyCodeForTarget (both the floor and the ceiling, then a second clamp
    // to the DAC's range); what was missing was the warning. The action is clamped
    // rather than refused because the alternative is a press that silently does
    // nothing, and a clamped level still reaches the radio as a key.
    //
    // The comparison is against `GainDecision::clamped` rather than a second copy
    // of the envelope bounds, so the envelope has one definition and this cannot
    // drift from the clamp it reports on.
    const GainDecision decision =
        GainPolicyCodeForTarget(gain_mode_[index], target_key_mv);
    if (band_clamped || decision.clamped) {
        char msg[128];
        snprintf(msg, sizeof(msg), "key_mv %d clamped to DAC code %u in gain mode %d",
                 key_mv, static_cast<unsigned>(decision.dac_code),
                 static_cast<int>(gain_mode_[index]));
        if (log_sink_ != nullptr) log_sink_(log_sink_ctx_, "WARN", msg);
    }
    // One bounded pulse, held for the recognition time, then released: the head
    // unit sees a single key event, not a held line.
    //
    // **The trim is NOT applied here.** `sense_mv` was read BEFORE this command
    // was written, so it is the line's IDLE level, not the result of the code
    // about to be driven -- feeding it to `Update()` made the loop apply one full
    // `max_step` in the wrong direction every press (measured on the FR-19 bench:
    // +11.7 mV of static error with the loop ENABLED versus DISABLED, which is
    // exactly one 8-code step at gain 1.82). The loop is serviced instead by
    // `ServiceTrim()`, once the driven line has settled, so it trims toward the
    // code it actually wrote.
    cs.servo.Target(gain_mode_[index], target_key_mv);
    DriveKeyCode(index, cs.servo.Code());
    cs.key_driven = true;
    cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
    // Arm the trim clock: the first update waits for the servo to settle.
    cs.trim_next_ms = now_ms + kServoTrimSettleMs;
    return true;
}

void SystemOrchestrator::RunBindingActions(uint8_t index,
                                           const ResolvedBinding &resolved,
                                           uint64_t now_ms) {
    // The tail's default acknowledgement is KEY_ACCEPTED for every action that
    // produced a key. The one case that must not be acknowledged is an OUT_VOLTAGE
    // whose command band is EMPTY: nothing was driven, so the press is reported as
    // unknown instead and this suppresses the default.
    bool play_default_ack = true;
    // Spec 3.6 puts `BUZZ` in the FIRMWARE's column; the action carries the named
    // pattern. Zero means none was bound.
    BuzzerPattern bound = BuzzerPattern::kNone;

    for (uint8_t i = 0; i < resolved.action_count; ++i) {
        const Action &a = resolved.actions[i];
        switch (a.kind) {
            case ActionKind::kOutVoltage:
                if (!DriveBoundLevelMv(index, a.key_mv, now_ms)) {
                    // The empty command band: nothing was driven, so the press must
                    // not be acknowledged as if it had. Report it and suppress the
                    // tail's default -- the same direction the unrecognised-level
                    // path takes.
                    play_default_ack = false;
                    buzzer_.Play(BuzzerPattern::kKeyUnknown);
                }
                break;
            case ActionKind::kOutRelease:
            case ActionKind::kNone:
                // An explicit release, or the explicit no-op. Releasing rather than
                // holding is right: a stale key with no action behind it is the
                // phantom-key hazard, and a NONE action cannot leave one driven.
                ReleaseKey(index);
                break;
            case ActionKind::kBuzzer:
                // An unknown pattern name parses to kNone, which `Play` treats as
                // "stop" -- that would leave the press silent rather than
                // acknowledged, so an unnameable pattern falls back to
                // KEY_ACCEPTED: a value this build cannot honour is inert, not a
                // different pattern AND not a missing acknowledgement.
                bound = BuzzerPatternFromName(a.target);
                break;
            default:
                // An app-owned kind (spec 3.6's "Executed by" column: Android). The
                // phone receives the same `event` frame and runs its half from the
                // same list; the firmware's half is the hardware key press, which
                // spec 3.5 says a failed app-side action must NEVER prevent. So skip
                // it -- do NOT release. Releasing was the defect: a binding stored
                // `[APP_INTENT, OUT_VOLTAGE]` (app action first) drove NO key at
                // all, and the user saw the press silently do nothing on the wire
                // they were watching.
                break;
        }
    }

    // A BUZZ REPLACES KEY_ACCEPTED rather than joining it, and that is forced by
    // the hardware: there is one buzzer and `Play` replaces rather than queues
    // (spec 7.2), so playing both in one tick would mean whatever went second is
    // the only thing heard. Since the user explicitly bound a pattern for this
    // gesture, that pattern is what they asked for; KEY_ACCEPTED is the DEFAULT
    // acknowledgement, and an explicit BUZZ overrides a default. (Playing BUZZ
    // second was measured: it is exactly as silent, because KEY_ACCEPTED would be
    // the one replaced.)
    if (play_default_ack) {
        // The DEFAULT acknowledgement is the optional per-press click, OFF unless
        // the user enabled it ("normal switch operation should not cause a beep").
        // An EXPLICIT BUZZ action is not gated: the user asked for that pattern by
        // name, so it always plays.
        if (bound != BuzzerPattern::kNone) {
            buzzer_.Play(bound);
        } else if (config_.settings.key_click_enabled) {
            buzzer_.Play(BuzzerPattern::kKeyAccepted);
        }
    }
}

bool SystemOrchestrator::PresentLevel(uint8_t index, int level_mv, int wheel_idle_mv,
                                      uint64_t now_ms) {
    ChannelState &cs = channels_[index];
    // This channel's own head-unit idle. With none there is nothing to map the
    // ratio ONTO, and a fabricated denominator would land every press on a key
    // nothing defined. Refuse rather than guess (spec 6.9's "disabled rather than
    // guessed").
    const int head_unit_idle_mv = head_unit_idle_mv_[index];
    if (head_unit_idle_mv <= 0 || wheel_idle_mv <= 0) return false;

    // The mapping is by RATIO, not by voltage (spec 6.9): the wheel's ladder and
    // the head unit's need not have the same resistances, so copying the incoming
    // millivolts across would land on the wrong key.
    //
    // **Clamp BEFORE the narrowing cast, because the cast can wrap into range.**
    // The ratio is `head_unit_idle * level / wheel_idle`; with a small
    // `wheel_idle` (a ladder whose LEARNED idle is near zero -- a miswired or
    // unpowered wheel, which `ConfigValidate` permits because it only bounds
    // `learned_idle_mv` to a plausible ADC reading) the product exceeds 65535 and
    // `static_cast<uint16_t>` reduces it modulo 65536. A wrapped value commonly
    // lands back INSIDE [1800, 5200], so it is indistinguishable from a real
    // target and the downstream clamp in `GainPolicyCodeForTarget` sees nothing
    // wrong -- the device drives a key voltage nothing defined. Measured over the
    // reachable ranges: ~620k (level, wheel_idle) pairs wrap into the envelope;
    // `wheel_idle <= 230 mV` is enough to reach it.
    //
    // The bound is the OUTPUT ceiling, the same value `GainPolicyCodeForTarget`
    // clamps to, so an over-large ratio saturates exactly where it already would
    // have. Saturation is the safe direction here (spec 6.2's only dangerous error
    // is over-ranging a 3 V head unit, which the ceiling clamp is what prevents).
    const MilliVolt target =
        GainPolicyMapWheelLevelToHeadUnit(head_unit_idle_mv, level_mv, wheel_idle_mv);

    // Spec 6.2's command band applies here too: a RATIO-mapped target is still a
    // command target, and this is the other place one is produced. The band is
    // what bounds it below the line's own rest, and the mapping above can land
    // past that ceiling -- `kPassThroughPressDeltaMv` caps the wheel's ratio at
    // ~897 permille (300 mV off a rail that cannot exceed the 2900 mV ADC
    // ceiling), so against a head unit idling under ~1934 mV the mapped value
    // sits above the rest AND the band's own ceiling is below the servo's floor.
    // That band is EMPTY, and a press there has no command to make: refuse rather
    // than drive a level that reaches the radio as nothing -- the caller releases,
    // which is the same direction the missing-denominator case above takes.
    const int banded = GainPolicyClampCommand(target, head_unit_idle_mv, nullptr);
    if (banded == 0) return false;

    cs.servo.Target(gain_mode_[index], banded);
    DriveKeyCode(index, cs.servo.Code());
    cs.key_driven = true;
    // Held for the recognition time, then released: the head unit must see ONE
    // key event, not a line held down (spec 6.6 -- it is gesture-blind, so a held
    // line is a different thing to it).
    cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
    // The trim is serviced by `ServiceTrim()`, not here: see `DriveBoundLevelMv`.
    cs.trim_next_ms = now_ms + kServoTrimSettleMs;
    return true;
}

void SystemOrchestrator::ServiceChannel(uint8_t index, uint64_t now_ms) {
    ChannelState &cs = channels_[index];
    const ChannelConfig &cc = config_.channels[index];

    /*
     * **While the headless learn is armed, a press is being TAUGHT -- but a button
     * being PROGRAMMED presents a HELD level so the head unit can capture it.**
     *
     * This is the 2022 interaction, restored (spec §7.5). The old firmware reached
     * `PROGRAM_ALT_KEY` while the modifier was held and held that gesture's pot
     * value on the output, so the user could teach the head unit "this voltage is
     * this function"; releasing the modifier released the line. The earlier
     * behaviour here SUPPRESSED the output entirely during the hold, which is why
     * the user's Pico flow "didn't work" -- there was nothing on the KEY line to
     * program.
     *
     * The phantom-key hazard the old suppression guarded against is closed a
     * different way now: the pressed button is delivered only as the level the
     * user is deliberately programming (its own level, or a chosen gesture's
     * slot), and only on the channel being programmed -- NOT as whatever action
     * its bindings would run. `cs.gestures.Reset()` still runs, so a half-
     * recognised press cannot complete into a binding action after the hold.
     *
     * The SAFETY releases still run (a pulse timeout, a lost head unit), because
     * they are not classification: a key driven before the learn started must
     * still be let go.
     */
    if (wizard_.Active()) {
        cs.reader.Update(now_ms);
        if (cs.key_driven && now_ms >= cs.key_released_at_ms) ReleaseKey(index);

        if (index == wizard_.TargetChannelIndex()) {
            // The channel the user is programming: drive the gesture's predefined
            // slot level, HELD for as long as the gesture stands, so the head unit
            // captures a stable, distinct voltage. This is the 2022
            // `PROGRAM_ALT_KEY` hold, restored.
            //
            // **The unresolved case presents the SINGLE slot, not the button's raw
            // centre.** The user's goal is THREE distinct functions per key with no
            // app (single/double/long). If an unresolved press presented the raw
            // centre and a resolved SINGLE presented a slot, the head unit would
            // capture a different voltage depending on WHEN the user pressed "set",
            // and the button's plain function would be ambiguous. Presenting
            // SINGLE's slot from the first tick makes every gesture a slot and the
            // four cases (unresolved, SINGLE, DOUBLE, LONG) collapse to three
            // stable levels, matching what normal operation later sends.
            const Gesture g = (wizard_.ProgrammedGesture() == Gesture::kNone)
                                  ? Gesture::kSingle
                                  : wizard_.ProgrammedGesture();
            const int slot = GestureSlotIndex(wizard_.TargetButtonOrdinal(), g);
            if (slot >= 0) {
                if (slot != program_hold_slot_ || !cs.key_driven) {
                    // A new gesture (or the hold lapsed): drive the level. Once
                    // driven it is HELD -- re-writing the DAC every tick would be
                    // pointless I2C traffic and a second thing that can latch a DAC
                    // fault, so the steady state below only keeps the hold alive.
                    const int target_mv = GestureSlotLevelMv(slot, IdleKeyMv(index));
                    if (target_mv > 0) DriveBoundLevelMv(index, target_mv, now_ms);
                    program_hold_slot_ = slot;
                } else {
                    // Push the safety-release deadline forward so the branch-top
                    // release does not cut the hold short after `send_duration_ms`.
                    // The DAC is NOT re-written here.
                    cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
                }
                // Remember the channel so the release on AUX1-let-go can let the
                // line go even after the wizard has handed back (see ServiceLearn).
                program_hold_channel_ = index;
            }
        } else {
            // A press on some OTHER input during the hold is being taught, not
            // driven: discard any half-recognised gesture so it cannot complete.
            cs.gestures.Reset();
        }
        return;
    }

    // Spec 3.4: `Channel.enabled` gates whether this channel's LEARNED LADDER is
    // classified -- "this channel has no learned ladder to compare against, do not
    // try to classify its input". It is NOT a binding gate (that is
    // `Binding.enabled`, checked per binding in `BindingResolve`) and NOT an
    // opt-out from being serviced at all (that is `channel_count`).
    //
    // **Nothing read this flag until now**, in three places that DESCRIBED it and
    // none that implemented it: `ConfigDefault`'s own comment, spec 3.4, and
    // `BindingResolver.cpp` ("NOT gated on `cfg.channels[..].enabled` -- spec 3.4:
    // `enabled` gates whether the channel's ladder is CLASSIFIED"). Measured
    // before the fix: a channel with `enabled = false` and a learned ladder still
    // classified, emitted `SINGLE@vol_up`, and would have driven the output.
    //
    // A disabled channel still runs the SAFETY releases below (a pulse timeout, or
    // a rail fault releasing a key someone drove with `test_key`), because those
    // are not classification. `level` is forced to kIdle so neither the pass-
    // through branch nor the gesture machine can fire.
    if (!cc.enabled) {
        cs.reader.Update(now_ms);
        // Release, but do NOT latch. This envelope is spec 6.2 step 2's "no head
        // unit" test, and spec 6.8's head-unit-gone row requires "keep
        // classifying" -- because spec 4.4 says the head unit "may sleep,
        // suspend, or reboot at any moment". Latching a reboot-only fault
        // (ReportFault) on a recurring normal condition leaves LED_STAT blinking
        // forever the first time the head unit sleeps, reporting a fault the
        // device is not in. The latch belongs to the LADDER's own out-of-range
        // (FR-4), which this branch cannot see because the channel is disabled.
        //
        // The envelope test itself lives in `HeadUnitGone`, which refuses to
        // judge the line while it is being driven (spec 6.2: the envelope is on
        // `V_KEY_idle`) and requires the verdict to hold for a settle.
        const int sense_mv =
            hal_->adc_read_mv(hal_->ctx, (index == 0) ? ADC_CH_KEY_SENSE1 : ADC_CH_KEY_SENSE2);
        if (HeadUnitGone(index, sense_mv, now_ms)) {
            ReleaseKey(index);
            cs.gestures.Reset();
            return;
        }
        if (cs.key_driven && now_ms >= cs.key_released_at_ms) ReleaseKey(index);
        return;
    }

    // Read the ladder THROUGH FR-3's noise filter, not as a single conversion.
    // A single noisy conversion can flip a classification, which is exactly the
    // failure FR-3 forbids; the median-of-32 window rejects it.
    //
    // The ratio denominator is `V_ADC_idle` "measured now" (spec 6.3), NOT the
    // learned idle: both numerator and denominator then scale with the 3V3 rail,
    // which is what makes `n` invariant to it. A denominator pinned to the
    // LEARNED idle instead lets the ratio drift with the rail's own tolerance
    // (~5% across the 3.14-3.47 V band), which pushes an idle-adjacent button --
    // FR-6's worst case -- out of its window at the band edges. It also leaves
    // `LadderClassify`'s FR-30 check inert, because that compares its reference
    // argument against `profile.learned_idle_mv` (the same value). Found
    // 2026-09-22.
    //
    // `idle_reference_mv` is the live idle, re-adopted from settled readings that
    // are idle DRIFT rather than a press or a short: within
    // [1000-`kIdleRefTrackPermille`, 1000+`kIdleRefMarginPermille`] permille of
    // the current reference. Reading above that is a short to a supply (fault);
    // below it is a button. Adopting every idle-ish reading tracks a moving rail;
    // adopting blindly would let a press become the reference.
    cs.reader.Update(now_ms);
    if (cs.reader.Settled()) {
        const int settled_mv = cs.reader.Value();
        if (cs.idle_reference_mv <= 0) {
            cs.idle_reference_mv = settled_mv;
        } else {
            const int ratio = LadderRatioPermille(settled_mv, cs.idle_reference_mv);
            if (ratio >= 1000 - kIdleRefTrackPermille &&
                ratio <= 1000 + kIdleRefMarginPermille) {
                cs.idle_reference_mv = settled_mv;
            }
        }
    }
    const int level_mv = cs.reader.Value();
    const ChannelLevel level = cs.classifier.Update(level_mv, cs.idle_reference_mv, now_ms);

    // The head unit's own idle, live. Outside the envelope the head unit has
    // gone (spec 6.2 step 2's "no head unit", spec 6.8's head-unit-gone row) and
    // the KEY line must be released rather than held -- FR-39's phantom-key
    // hazard. This is NOT a latch: the head unit may sleep and come back (spec
    // 4.4), so the response is "release and keep classifying". Named for what it
    // measures rather than "rail_fault", which invited exactly the mistake of
    // folding it in with FR-4's ladder fault.
    //
    // The test is `HeadUnitGone`, which refuses to judge `V_KEY_idle` while a
    // pulse is on the line (the sense node then reads THIS device's own output)
    // and requires the verdict to persist -- see its comment for the self-trip it
    // closes. `sense_mv` is still read here because `PresentLevel` and the servo
    // trim consume the raw value.
    const int sense_mv = hal_->adc_read_mv(hal_->ctx,
                                           (index == 0) ? ADC_CH_KEY_SENSE1 : ADC_CH_KEY_SENSE2);
    const bool head_unit_gone = HeadUnitGone(index, sense_mv, now_ms);

    // FR-25 / spec 6.9: with no config there are no learned windows to classify
    // against, so the press is detected by the ratio moving off idle and mapped
    // onto the head unit's own idle. This runs BEFORE the classifier, because
    // the classifier's windows do not exist in this mode.
    //
    // The ratio is the same quantity spec 6.3 normalizes by -- level/idle x 1000
    // -- so a wheel whose idle is 2835 mV and whose button sits at 1430 mV asks
    // for 504 permille of the head unit's idle. The wheel's and the head unit's
    // resistances need not match, and this mapping does not care.
    if (pass_through_) {
        // This channel's own reference, or NONE. A channel whose ladder was
        // unreadable at Boot drives nothing while its siblings still pass through
        // (spec 6.9's "disabled rather than guessed" is per input -- one dead
        // wheel must not disable the other). This does NOT return: the safety
        // releases below (a pulse timeout, a rail fault) must still run for a
        // channel this build never drove from a press, or a `test_key` pulse in
        // flight would never be released.
        if (cs.pass_through_idle_mv > 0) {
            // The LIVE ladder reading, unfiltered. The reference below was
            // captured this same way at Boot, so comparing like with like needs no
            // filter (and `cs.reader` must warm up for the configured path's
            // debounce).
            //
            // **A FAILED CONVERSION IS NOT A READING, and the sentinel is -1, not
            // 0** (`IHAL::adc_read_mv`: 0 mV is a legal level at the ladder's
            // common). Unchecked, `idle - (-1)` is `idle + 1` -- the largest
            // possible press by this test's own arithmetic -- so one bad
            // conversion, or a bus fault held across ticks, drove a phantom key at
            // the mapped level while nothing was pressed. That is the hazard
            // FR-12/FR-39 exist to prevent, and it is why `AdcReader` drops failed
            // conversions rather than averaging them in as zero. The three sense
            // reads in this file guard it; `LearnWizard` did not guard two of its
            // own (fixed with the same reasoning, N-43).
            //
            // HOLD the previous press state rather than resolving either way: a
            // failure says "no measurement", so treating it as a release would cut
            // a real press short, and treating it as a press invents one. Assuming
            // the state is unchanged makes both edges false below, so no key is
            // driven or released HERE, and the tail of this function still runs its
            // safety releases (a pulse timeout, a rail fault) -- they are not
            // classification.
            const int raw_level_mv =
                hal_->adc_read_mv(hal_->ctx, (index == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2);
            const bool have_reading = raw_level_mv >= 0;
            const int level_now_mv = raw_level_mv;

            // Self-heal a reference captured while a button was held. It is read
            // once at Boot; if the user was holding a button at power-on then it is
            // a PRESSED level, which makes every later press look like "less off
            // idle" and can suppress pass-through for the whole session. Seeing the
            // line ABOVE the captured reference means the capture was a press -- no
            // button pulls the ladder UP from true idle -- so adopt the higher
            // value. Only the HIGHEST reading is ever adopted, so a press cannot
            // drag the reference down the way a naive re-capture would.
            //
            // PER CHANNEL: this heals only THIS channel's reference. Healing a
            // shared one from whichever channel happened to read higher is what
            // made a two-channel device drive a phantom key on the lower-idle
            // channel. Gated on `have_reading`: a failed read is not a level ABOVE
            // the reference, and letting -1 through here would move the reference
            // DOWN -- the one direction the heal is designed never to take.
            if (have_reading && level_now_mv > cs.pass_through_idle_mv) {
                cs.pass_through_idle_mv = level_now_mv;
                cs.pass_through_pressed = false;
            }
            // `idle` is re-read AFTER the heal so a press in this same tick is not
            // measured against the reference it just replaced.
            const int idle = cs.pass_through_idle_mv;

            // FR-25 / spec 6.9: "a press is clearly off idle (> 300 mV from the
            // wheel's idle)", read directly off the ADC rather than through the
            // noisy median -- the reference was captured the same way, so like is
            // compared with like. `level_mv`/`cs.reader` stay for the configured
            // path, whose debounce genuinely needs the filter.
            //
            // A failed read holds the previous press state instead: `pressed` is
            // left as it was, so neither edge can fire and this branch neither
            // drives nor releases a key. Without `have_reading` here, `idle - (-1)`
            // is `idle + 1` -- the largest possible press by this very comparison
            // -- and a single bad conversion drove a phantom key.
            const bool pressed =
                have_reading ? ((idle - level_now_mv) > kPassThroughPressDeltaMv)
                             : cs.pass_through_pressed;
            // RISING EDGE only. The pulse self-releases on `send_duration_ms`
            // below, so without this latch the very next tick would see the button
            // still held, `key_driven` false, and re-arm -- the line would pulse
            // once per send_duration instead of once per press, and a held button
            // would emit a key every 200 ms forever. The edge also matches the
            // configured path, where a SINGLE cannot fire twice without a release
            // between.
            const bool rising_edge = pressed && !cs.pass_through_pressed;
            // The FALLING edge ends a pass-through press. Testing `!pressed` alone
            // released ANY driven line on any idle tick, including a `test_key`
            // pulse this branch never started: measured, a 200 ms bench hold was
            // cut to zero on a pass-through device, so the bench could not
            // exercise the output at all on an unconfigured board.
            const bool falling_edge = !pressed && cs.pass_through_pressed;
            cs.pass_through_pressed = pressed;

            if (rising_edge && head_unit_idle_mv_[index] > 0) {
                // The head unit's OWN idle is the denominator: the wheel asks for
                // a fraction of a full-scale ladder position, and that fraction is
                // then applied to the head unit's range. Using the output's
                // safe-idle code instead would scale against 5200 mV and push low
                // buttons into the clamp, which is the wrong key rather than a
                // quieter one.
                PresentLevel(index, level_mv, idle, now_ms);
            } else if (falling_edge) {
                ReleaseKey(index);
            }
        }
        // No gesture state machine, no bindings, no buzzer pattern: there is
        // nothing configured to resolve against, and inventing feedback for an
        // event the user did not bind would be noise.
        //
        // NOTE: this block deliberately no longer RETURNS. The early return that
        // used to sit here skipped both the pulse-timeout release below and the
        // head-unit-gone release, so a held pass-through press drove the line for as
        // long as the button was held (spec 6.6 rule 2 requires a bounded pulse)
        // and a head unit that went away mid-press was never released (FR-39's
        // phantom key). Release is not configuration-dependent.
    } else {
        GestureEvent ev{};
        // The bindings are the PRESSED BUTTON's, not the channel's (spec 6.6 rule
        // 3). While the press is in flight the classifier names the button; on
        // release it names none, so the machine's own tracked button is used --
        // the press being resolved is the one it is holding. Falling back to a
        // channel-wide scan here would make a button that binds nothing wait out
        // a sibling's double window and emit a LONG it has no binding for.
        const uint8_t pressed_button = (level == ChannelLevel::kPressed)
                                           ? cs.classifier.ButtonIndex()
                                           : cs.gestures.Button();
        const GestureBindings for_button = BindingsForButton(index, pressed_button);
        const bool fired = cs.gestures.Update(level, cs.classifier.ButtonIndex(), now_ms, &ev,
                                              for_button);

        // FR-12, stated in the spec three times (§6.3, §7.2, and the FR table): a
        // press that matches no learned window is reported as `event{button:
        // null}` and beeps KEY_UNKNOWN, and is NEVER guessed at.
        //
        // `GestureStateMachine` treats kUnknown as not-pressed, so no gesture can
        // fire for it -- which is the "never guessed" half, and correct. But that
        // also means nothing else in this function sees it, so before this branch
        // existed the device ignored an unrecognised press COMPLETELY: no event,
        // no beep, nothing. The user's own button silently did nothing, with no
        // way to tell that from a broken adapter.
        //
        // Reported once per press (the latch), because a held level is
        // unrecognised on every tick and would otherwise flood the link at 100
        // events/s.
        if (level == ChannelLevel::kUnknown) {
            if (!cs.unknown_reported) {
                cs.unknown_reported = true;
                if (gesture_sink_ != nullptr) {
                    // A null id is the whole point: naming a button here would be
                    // the guess FR-12 forbids, and no binding can match a null.
                    const GestureEventRecord rec{index, nullptr, Gesture::kNone, level_mv,
                                                 IdleReferenceMv(index), now_ms};
                    gesture_sink_(gesture_sink_ctx_, rec);
                }
                buzzer_.Play(BuzzerPattern::kKeyUnknown);
            }
        } else {
            cs.unknown_reported = false;
        }

        if (fired) {
            const ResolvedBinding resolved = BindingResolve(config_, index, ev);
            // Spec 4.3: the app is told AFTER the device has acted on its own
            // local binding, never before. `event` is fire-and-forget precisely
            // so that a button press is not held hostage to the app being
            // responsive (spec 6.6), and reporting first would make the link part
            // of the key path.
            ReportGesture(index, ev, level_mv);
            if (resolved.found) {
                RunBindingActions(index, resolved, now_ms);
            } else {
                // The button was RECOGNISED but its gesture is not bound. In the
                // FIRST-INSTALL case -- the user programmed a button headlessly and
                // the head unit captured a level per gesture -- the device must
                // present a DISTINCT, STABLE level per gesture so the function the
                // head unit was taught actually fires.
                //
                // **This is the 2022 default, restored (spec §6.6 rule 4).** The
                // old firmware's `lookup_single/double/long_press_val` sent a
                // different level per gesture; presenting the button's ONE centre
                // for all three (which this used to do) made the head unit's
                // gesture-blind input see the same command for every gesture, so
                // the double/long functions a user programmed did nothing --
                // exactly the "it doesn't work" the user reported.
                //
                // The level comes from the predefined slot table (`GestureLevels`),
                // indexed by the button's ordinal and the gesture, so the SAME
                // gesture always sends the SAME voltage regardless of rail or
                // temperature -- the compensation property the user asked for.
                const uint8_t bi = ev.button_index;
                const LadderProfile &ladder = config_.channels[index].ladder;
                const int slot = GestureSlotIndex(bi, ev.gesture);
                bool presented = false;
                if (slot >= 0) {
                    const int slot_mv = GestureSlotLevelMv(slot, head_unit_idle_mv_[index]);
                    if (slot_mv > 0) {
                        presented = DriveBoundLevelMv(index, slot_mv, now_ms);
                    }
                }
                if (presented) {
                    // The per-press click is OPTIONAL and OFF by default (the user:
                    // "normal switch operation should not cause a beep"). An
                    // explicit BUZZ action still plays (that is a deliberate ask),
                    // and KEY_UNKNOWN below still plays, because that reports a
                    // press that matched nothing.
                    if (config_.settings.key_click_enabled) {
                        buzzer_.Play(BuzzerPattern::kKeyAccepted);
                    }
                } else if (bi < ladder.count &&
                           PresentLevel(index, ladder.buttons[bi].mv_center,
                                        ladder.learned_idle_mv, now_ms)) {
                    // No gesture slot (a ladder with more buttons than the table
                    // admits): fall back to the button's own level, the stock-wheel
                    // behaviour. A gesture on such a button cannot be distinguished
                    // -- which is why the table is sized for the full ladder -- but
                    // the button still works as a plain press.
                    if (config_.settings.key_click_enabled) {
                        buzzer_.Play(BuzzerPattern::kKeyAccepted);
                    }
                } else {
                    // No usable head-unit idle (or no such button): there is nothing
                    // to map the level ONTO, and a fabricated denominator would land
                    // on a key nothing defined. Release rather than guess, and say
                    // so -- FR-12's direction.
                    ReleaseKey(index);
                    buzzer_.Play(BuzzerPattern::kKeyUnknown);
                }
            }
        }
    }

    // FR-39 / spec 6.8: a fault, or a lost head unit, releases in the same tick,
    // on BOTH paths -- a collapsed rail is not a configuration issue. Everything
    // in flight is discarded: a half-recognised gesture must not reach the radio.
    // This sits outside the branch because the pass-through path used to return
    // before reaching it, leaving the line driven against a head unit that had
    // gone away.
    //
    // The two conditions release, but only ONE of them latches. `level == kFault`
    // is FR-4's ladder out-of-range (a short to 12 V, an open input): a wiring
    // condition that does not fix itself, so it reports and latches `blink`
    // (spec 7.3's reboot-only rule). `head_unit_gone` is the KEY-sense envelope,
    // which is spec 6.2 step 2's "no head unit" test and spec 6.8's head-unit-gone
    // row -- and that row's response is "keep classifying", because spec 4.4 says
    // the head unit "may sleep, suspend, or reboot at any moment" and spec 6.2
    // makes gain re-evaluation happen "on `/VBUS_VALID` transitions". Latching a
    // reboot-only fault on a recurring NORMAL condition leaves LED_STAT blinking
    // forever the first time the head unit sleeps or the user unplugs the car
    // radio, reporting a fault the device is not in.
    const bool ladder_fault = (level == ChannelLevel::kFault);
    if (ladder_fault || head_unit_gone) {
        ReleaseKey(index);
        cs.gestures.Reset();
        if (ladder_fault) {
            // FR-4: an out-of-range channel -- an open input, a short to 12 V --
            // must be REPORTED, not merely survived. Releasing the line is the
            // safety half; this is the half the user can act on. The indication
            // latches (see ReportFault): a wiring fault does not clear itself, and
            // an indication that faded would be a lie.
            //
            // The buzzer is the audible half (N-10). Spec 7.2 had no pattern for a
            // wiring fault -- the FAULT_* patterns named subsystems, and a collapsed
            // rail or an open ladder input is neither -- so a user with `led_level`
            // 0 got NO fault indication at all. `FAULT_INPUT` closes that.
            //
            // Guarded on the ONCE-ONLY edge: `hw_faulted_` is the latch, so the
            // pattern is armed only on the transition into the fault. `ReportFault`
            // sets it, so checking `!hw_faulted_` first is the edge detector, and a
            // fault that persists for many ticks does not re-Play (which with a
            // one-buzzer replace-not-queue grammar would restart the pattern every
            // tick and never let it finish).
            const bool first = !Faulted();
            ReportFault();
            if (first) buzzer_.Play(BuzzerPattern::kFaultInput);
        }
        return;
    }

    // End the pulse once the recognition time has elapsed. Checked every tick, so
    // a press that is never released cannot leave the line driven -- and this
    // includes the pass-through pulse, which set `key_released_at_ms` and then
    // (before this was restructured) returned above it.
    if (cs.key_driven && now_ms >= cs.key_released_at_ms) {
        ReleaseKey(index);
    }
}

// --- C-linkage entry points for src/main.c -----------------------------------
//
// The constructor and Boot() keep taking a config so the host tests can drive
// them directly; these wrappers are the device path, where the config comes from
// NVS and nobody has one to hand at construction time.

extern "C" SystemOrchestrator *SystemOrchestratorCreate(IHAL *hal, bool calibration_degraded) {
    if (hal == nullptr) return nullptr;

    // A default config is what FR-25 wants anyway: if NVS holds nothing, this IS
    // the pass-through fallback, and Boot() overwrites it when a config loads.
    // Built by the shared `ConfigDefault()` so the router's config_get reply and
    // this boot config cannot drift apart (Config/ConfigDefaults.h).
    //
    // **A file-local static, not a local.** `sizeof(Config)` is 8,912 B, and this
    // function runs on the 3,584-byte main task; by value it made a frame of
    // 8,944 B before `Boot` (18 KB) was even entered. It is written once, at
    // start-up, and only read here, so there is nothing to race with.
    static Config boot_config;
    ConfigDefault(&boot_config);
    const Config &bc = boot_config;

    SystemOrchestrator *sys =
        new (std::nothrow) SystemOrchestrator(hal, bc, bc.settings.timings);
    if (sys == nullptr) return nullptr;

    // Spec 3.2: a blank eFuse means every reading is on the linear approximation,
    // and that must be REPORTED, not silent. It arrives as an ARGUMENT rather than
    // a call to `EspHalCalibrationIsDegraded()` here, because this file is
    // HOST-compiled and `EspHal.cpp` is the one translation unit the host build
    // excludes -- naming it here would fail the native link. The device caller
    // (`src/main.cpp`) already has the answer from the HAL. A host build's MockHal
    // is never degraded, so the native suite sees the clean-boot path unless a
    // test sets the flag explicitly.
    sys->SetCalibrationDegraded(calibration_degraded);

    // Safe idle FIRST (FR-13), before the caller can start any link. A device
    // that cannot reach its safe state must be loud rather than quietly running:
    // Boot() plays the fault pattern and SafeIdleEstablished() stays false.
    sys->Boot();
    return sys;
}

extern "C" void SystemOrchestratorTick(SystemOrchestrator *sys, uint64_t now_ms) {
    if (sys == nullptr) return;
    sys->Tick(now_ms);
}

extern "C" bool SystemOrchestratorSafeIdle(const SystemOrchestrator *sys) {
    return sys != nullptr && sys->SafeIdleEstablished();
}

extern "C" bool SystemOrchestratorOutputVerified(const SystemOrchestrator *sys) {
    return sys != nullptr && sys->OutputVerified();
}

extern "C" bool SystemOrchestratorIsMaintenanceActive(const SystemOrchestrator *sys) {
    return sys != nullptr && sys->MaintenanceActive();
}

extern "C" const char *SystemOrchestratorConfigStateWord(const SystemOrchestrator *sys) {
    // Never null: the caller passes this straight to a `%s`, and a null there is
    // undefined behaviour rather than an "unknown" reading.
    return (sys != nullptr) ? sys->ConfigStateWord() : "unknown";
}

extern "C" void SystemOrchestratorNoteMaintenanceActivity(SystemOrchestrator *sys, uint64_t now_ms) {
    // No null check needed for the MODE -- `NoteMaintenanceActivity` is a
    // documented no-op on a closed window -- but the ORCHESTRATOR pointer can be
    // null on the boot-failure path, and the caller (a poll loop) should not have
    // to know that.
    if (sys != nullptr) sys->NoteMaintenanceActivity(now_ms);
}

