#include "System/SystemOrchestrator.h"

#include <new>
#include <cstdio>
#include <cstring>

#include "Bindings/BindingResolver.h"
#include "Config/ConfigDefaults.h"
#include "Config/ConfigStore.h"
#include "Output/GainPolicy.h"

namespace {

// The sense divider is an exact divide-by-2 (spec 2.3), so the KEY line is twice
// the sense reading. This appears in three places (gain selection, the trim
// loop, and here) and is a property of R54/R55, not a tuning value.
constexpr int kSenseDividerRatio = 2;

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

/*
 * `temp_c_at_learn`, in tenths of a degree C, when the NTC has not been read.
 *
 * Spec 6.4 says v1's temperature compensation is a linear correction with a
 * coefficient defaulting to zero, so that it "does not change behavior until the
 * user or a bring-up measurement supplies a non-zero coefficient".
 *
 * **What actually exists is the two halves the coefficient would need and not the
 * correction itself:** `temp_c_at_learn` is recorded per button, and
 * `temp_comp_enabled` is carried through the config. There is no coefficient field,
 * no correction function, and no test of one. An earlier version of this comment
 * (and of the spec paragraph it was written from) claimed "the correction path is
 * present and being testable", which overstated it -- and mattered, because that
 * sentence is the thing that would satisfy FR-17 on review. Recording the input to
 * a correction is not implementing it. See open item N-9.
 *
 * The sentinel is deliberate regardless: 23.5 would be a plausible-looking number
 * that nothing measured, and a plausible number in a field a future engineer uses
 * to compute a correction is worse than a visibly unmeasured one.
 */
constexpr int kTempNotMeasuredTenths = 0;

}  // namespace

SystemOrchestrator::SystemOrchestrator(IHAL *hal, const Config &config,
                                       const GestureTimings &timings)
    : hal_(hal), config_(config), timings_(timings),
      buzzer_(hal, config.settings.buzzer_level),
      leds_(hal, config.settings.led_level),
      // FR-31's headless learn. Constructed with the SAME timings the channels
      // use, so "what counts as an AUX1 press" matches "what counts as a wheel
      // press" -- one debounce definition in the firmware.
      wizard_(hal, &buzzer_, &leds_, Aux1ProfileDefault(), timings),
      // Spec 8.2's 5-minute inactivity window. A config may carry a different
      // one; this default exists so a bare device still has a BOUNDED window
      // rather than an unbounded one.
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
    for (uint8_t i = 0; i < config_.binding_count; ++i) {
        const Binding &b = config_.bindings[i];
        if (!b.enabled) continue;
        if (b.channel != as_swc && b.channel != static_cast<uint8_t>(BindingChannel::kAny)) {
            continue;
        }
        if (strcmp(b.button, button_id) != 0) continue;
        if (b.gesture == Gesture::kDouble) out.has_double = true;
        if (b.gesture == Gesture::kLong) out.has_long = true;
    }
    return out;
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
    int first_idle_key_mv = 0;

    for (uint8_t i = 0; i < n; ++i) {
        const DacChannel key_ch = (i == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;
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
        if (i == 0) head_unit_idle_mv_ = no_head_unit ? 0 : measured_key_idle_mv;

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
        hal_->dac_set_code(hal_->ctx, key_ch, idle_code);
        if (i == 0) first_idle_key_mv = GainPolicyKeyMvForCode(mode, idle_code);
    }

    idle_key_mv_ = first_idle_key_mv;
    // Set BEFORE the state machines are constructed, so anything that observes
    // SafeIdleEstablished() knows the output is already safe (FR-13).
    safe_idle_established_ = true;
}

void SystemOrchestrator::Boot() {
    // 1. Load the config. `kNoConfig` is not a failure -- FR-25 makes an
    //    unconfigured device a transparent pass-through, so the device works
    //    before it is ever configured.
    ConfigStore store(hal_);
    Config loaded{};
    const ConfigLoadResult result = store.Load(&loaded);

    if (result == ConfigLoadResult::kLoaded || result == ConfigLoadResult::kRecoveredFromBackup) {
        config_ = loaded;
        buzzer_ = BuzzerGrammar(hal_, config_.settings.buzzer_level);
        leds_ = LedGrammar(hal_, config_.settings.led_level);
    } else if (result == ConfigLoadResult::kFellBackToDefaults) {
        // Spec 6.8: a corrupt config falls back to defaults AND says so loudly.
        // The defaults keep the output safe, which is the part that matters.
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

    // 2. Establish safe idle. This is BEFORE anything else that could accept a
    //    command (FR-13), and before the per-channel state exists.
    EstablishSafeIdle();

    if (pass_through_) {
        // Channel 0's live ladder reading is the pass-through reference. Read it
        // AFTER safe idle so the KEY line is already released and cannot be
        // pulling the ladder; a reference taken while the output was driving
        // would be a level the user is not holding.
        const int live = hal_->adc_read_mv(hal_->ctx, ADC_CH_SWC1);
        pass_through_idle_mv_ = (live > 0) ? live : 0;
        if (pass_through_idle_mv_ <= 0) {
            // No usable reference: the ladder is unpowered or unreadable. Pass-
            // through is then impossible, and guessing a denominator would map
            // every press to a voltage nothing defined. Serve nothing rather than
            // drive a fabricated key.
            //
            // No logging here on purpose: this file is HOST-compiled (the native
            // suite runs it), so it cannot call esp_log. The state is observable
            // through PassThroughActive(), which is what a test and the link
            // status both read.
            pass_through_ = false;
        }
    }

    // 3. Now the per-channel state.
    channel_count_ = (config_.channel_count <= kMaxChannels) ? config_.channel_count
                                                             : kMaxChannels;
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
        // The trim loop is present but DISABLED in v1: spec 6.5 says open-loop
        // command with the loop off until its gain is measured on hardware, and
        // running a software loop against the hardware integrator is how you
        // build an oscillator.
    }

    // 4. Feedback for the load result. A recovered backup is degraded (the user
    //    should know their newest config was lost); a fallback is an error.
    if (result == ConfigLoadResult::kFellBackToDefaults) {
        buzzer_.Play(BuzzerPattern::kFaultConfig);
    } else if (result == ConfigLoadResult::kRecoveredFromBackup) {
        buzzer_.Play(BuzzerPattern::kBootDegraded);
    } else {
        buzzer_.Play(BuzzerPattern::kBootOk);
    }

    // `LED_STAT` starts blinking when the load FAILED, and breathing otherwise.
    // A degraded boot is reported on the buzzer, which is transient, so the LED
    // is the only lasting record that the running config is not the user's.
    const bool config_faulted = (result == ConfigLoadResult::kFellBackToDefaults);
    if (config_faulted) {
        faulted_ = true;
        leds_.SetStat(LedStatPattern::kBlink);
    } else {
        RestatLeds();
    }

    // 5. USB/BLE would be brought up here (Tasks 16/18). Nothing in this class
    //    starts them, which is what makes FR-42 structural rather than a promise.
}

void SystemOrchestrator::Tick(uint64_t now_ms) {
    for (uint8_t i = 0; i < channel_count_; ++i) {
        ServiceChannel(i, now_ms);
    }
    // FR-31: after the channels, so a learn commit is applied before the next
    // tick classifies against the new profile.
    ServiceLearn(now_ms);
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
    // survives to kEnterHoldMs TOGGLES the wizard, which is what lets the same
    // gesture both enter and leave (spec 7.4 step 5).
    if (aux_pressed) {
        if (!aux_holding_) {
            aux_holding_ = true;
            aux_hold_ms_ = now_ms;
        } else if ((now_ms - aux_hold_ms_) >= kMaintenanceHoldMs && !maint_fired_latch_) {
            // Checked FIRST, and that ordering is load-bearing: the 1.5 s tier
            // below sets `aux_hold_latch_`, so a separate "already fired" branch
            // ahead of this one would swallow every tick after the programming
            // hold and the 3 s tier would be UNREACHABLE -- the escalation spec
            // 8.2 requires would silently never happen. (It did not, until
            // measured.) The two tiers are one escalation, so they share one
            // latch-checked chain rather than each guarding itself.
            // Spec 8.2 nests the two holdings: 1.5 s is PROGRAMMING and 3 s is
            // MAINTENANCE, the shorter a subset of the longer, so holding too long
            // to program escalates cleanly into maintenance rather than into an
            // undefined state. By 3 s the programming hold has already toggled the
            // wizard, so this must fire on its own latch and leave the wizard
            // alone -- otherwise a long hold would enter programming and then
            // immediately exit it again.
            maint_fired_latch_ = true;
            if (wizard_.Active()) {
                // Leave the learn first: a user who over-held is trying to reach
                // maintenance, not to abandon a half-finished programming session
                // in a state they cannot see.
                wizard_.Exit(now_ms);
            }
            maintenance_.Enter(MaintenanceTrigger::kAux1Hold, now_ms);
        } else if ((now_ms - aux_hold_ms_) >= LearnWizard::kEnterHoldMs && !aux_hold_latch_) {
            // Consume the edge. The LATCH, not this flag, is what prevents a
            // second toggle: the flag is cleared so the next release re-arms.
            aux_hold_latch_ = true;
            if (wizard_.Active()) {
                wizard_.Exit(now_ms);
            } else {
                // The learn's idle reference is captured HERE, on entry, because
                // spec 3.4 wants the idle AS MEASURED AT LEARN TIME. During the
                // prompt the user is holding the wheel button, so the live reading
                // is the pressed level and cannot serve -- and the previously
                // STORED idle is exactly what a re-learn is meant to correct.
                const int live = hal_->adc_read_mv(
                    hal_->ctx, (learn_channel_ == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2);
                learn_idle_mv_ = (live > 0)
                                     ? live
                                     : IdleReferenceMv(static_cast<uint8_t>(learn_channel_));
                wizard_.Enter(now_ms, /*aux_held=*/true);
            }
        }
    } else {
        aux_holding_ = false;
        aux_hold_latch_ = false;
        maint_fired_latch_ = false;
    }

    // FR-38: the window closes on its own after 5 minutes of inactivity, so a
    // device left unable to serve input because someone opened a web page cannot
    // happen. Update() is what performs the close.
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
    // DEFERRED while the learn wizard is active: entering maintenance by a 3 s
    // AUX1 hold exits a running learn first, and the wizard's own `Exit` sets
    // LED_STAT solid as its handback. Restating in the same tick would overwrite
    // that with the double-flash. The wizard is the LED owner until it hands back.
    const bool want_maint_led = maintenance_.Active();
    if (wizard_.Active()) {
        // Hold the state flag back so the deferred transition still fires later.
        maintenance_led_state_ = !want_maint_led;
    } else if (want_maint_led != maintenance_led_state_) {
        maintenance_led_state_ = want_maint_led;
        RestatLeds();
    }

    if (wizard_.Active()) {
        // A learn with no usable idle reference cannot measure anything: the
        // press detector would compare a reading against zero and call every
        // level "pressed", then commit a window computed from a fabricated
        // denominator. Refusing is the same direction FR-12 takes.
        if (learn_idle_mv_ > 0) {
            wizard_.Tick(learn_channel_, now_ms, learn_idle_mv_, kTempNotMeasuredTenths);
        }
    }

    if (wizard_.ConsumeCommitted()) {
        learned_channel_ = learn_channel_;
        learned_profile_ = wizard_.Profile();
        ApplyLearnedProfile(learn_channel_, learned_profile_);
    }
    if (wizard_.ConsumeExited()) {
        // The wizard drove the LEDs for its prompts; hand them back so the normal
        // grammars resume rather than leaving a stale solid LED2 behind.
        leds_.Set2(Led2Pattern::kOff);
    }
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

    // Persist. The wizard cannot: it holds no Config and no store. A null store
    // means a bench build without NVS, where the learn is real but not durable --
    // and that is reported rather than silently dropped.
    if (store_ != nullptr) {
        store_->Save(config_);
    }
    persisted_ = (store_ != nullptr);
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
    const GestureEventRecord rec{index, id, ev.gesture, level_mv, ev.at_ms};
    gesture_sink_(gesture_sink_ctx_, rec);
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
    const DacChannel key_ch = (channel_index == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;

    // Shares the servo with the action path, so what the bench measures is what
    // a real press produces -- a direct dac_set_code would bypass the trim loop
    // and measure a different thing.
    cs.servo.Target(gain_mode_[channel_index], key_mv);
    cs.servo.Update(hal_->adc_read_mv(hal_->ctx,
                                      (channel_index == 0) ? ADC_CH_KEY_SENSE1
                                                           : ADC_CH_KEY_SENSE2));
    hal_->dac_set_code(hal_->ctx, key_ch, cs.servo.Code());
    cs.key_driven = true;
    // A test command has no gesture to resolve, so it drives immediately and
    // releases on the caller's hold time.
    cs.key_released_at_ms = now_ms + (hold_ms ? hold_ms : 1);
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
    if (faulted_) {
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

void SystemOrchestrator::Identify() {
    // Both channels, because "which unit is this" is a question about the box,
    // not about one steering-wheel input.
    leds_.SetStat(LedStatPattern::kDoubleFlash);
    buzzer_.Play(BuzzerPattern::kKeyAccepted);
}

void SystemOrchestrator::ReleaseKey(uint8_t index) {
    ChannelState &cs = channels_[index];
    if (!cs.key_driven) return;
    const DacChannel key_ch = (index == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;
    // Re-point the servo at idle BEFORE writing, so a later `Update()` trims
    // toward idle rather than toward the key just released.
    cs.servo.Target(gain_mode_[index],
                    GainPolicyKeyMvForCode(gain_mode_[index], idle_code_[index]));
    hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
    cs.key_driven = false;
}

bool SystemOrchestrator::PresentLevel(uint8_t index, int level_mv, int wheel_idle_mv,
                                      int sense_mv, uint64_t now_ms, DacChannel key_ch) {
    ChannelState &cs = channels_[index];
    // With no head-unit idle there is nothing to map the ratio ONTO, and a
    // fabricated denominator would land every press on a key nothing defined.
    // Refuse rather than guess (spec 6.9's "disabled rather than guessed").
    if (head_unit_idle_mv_ <= 0 || wheel_idle_mv <= 0) return false;

    // The mapping is by RATIO, not by voltage (spec 6.9): the wheel's ladder and
    // the head unit's need not have the same resistances, so copying the incoming
    // millivolts across would land on the wrong key.
    const MilliVolt target = static_cast<MilliVolt>(
        (static_cast<long>(head_unit_idle_mv_) * level_mv) / wheel_idle_mv);
    cs.servo.Target(gain_mode_[index], target);
    cs.servo.Update(sense_mv);
    hal_->dac_set_code(hal_->ctx, key_ch, cs.servo.Code());
    cs.key_driven = true;
    // Held for the recognition time, then released: the head unit must see ONE
    // key event, not a line held down (spec 6.6 -- it is gesture-blind, so a held
    // line is a different thing to it).
    cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
    return true;
}

void SystemOrchestrator::ServiceChannel(uint8_t index, uint64_t now_ms) {
    ChannelState &cs = channels_[index];
    const ChannelConfig &cc = config_.channels[index];

    const DacChannel key_ch = (index == 0) ? DAC_CH_KEY1 : DAC_CH_KEY2;

    // Read the ladder THROUGH FR-3's noise filter, not as a single conversion.
    // A single noisy conversion can flip a classification, which is exactly the
    // failure FR-3 forbids; the median-of-32 window rejects it.
    //
    // The idle reference is the LEARNED idle, not the live reading: it is the
    // ratio denominator, so it must stay pinned to the rail the button centres
    // were measured at (spec 6.3/LadderProfile). The live rail health is a
    // separate check on the KEY sense pin, below.
    cs.reader.Update(now_ms);
    const int level_mv = cs.reader.Value();
    const int idle_ref = cc.ladder.learned_idle_mv;
    const ChannelLevel level = cs.classifier.Update(level_mv, idle_ref, now_ms);

    // The head unit's own idle, live. Outside the envelope the head unit has
    // gone (spec 6.8, VBUS off / rail collapse) and the KEY line must be
    // released rather than held -- FR-39's phantom-key hazard.
    const int sense_mv = hal_->adc_read_mv(hal_->ctx,
                                           (index == 0) ? ADC_CH_KEY_SENSE1 : ADC_CH_KEY_SENSE2);
    const int key_idle_now_mv = sense_mv * kSenseDividerRatio;
    const bool rail_fault = (key_idle_now_mv < kKeyEnvelopeLowMv) ||
                            (key_idle_now_mv > kKeyEnvelopeHighMv);

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
        // The LIVE ladder reading, unfiltered. The reference below was captured
        // this same way at Boot, so comparing like with like needs no filter (and
        // `cs.reader` must warm up for the configured path's debounce).
        int level_now_mv =
            hal_->adc_read_mv(hal_->ctx, (index == 0) ? ADC_CH_SWC1 : ADC_CH_SWC2);

        // Self-heal a reference captured while a button was held. It is read once
        // at Boot; if the user was holding a button at power-on then it is a
        // PRESSED level, which makes every later press look like "less off idle"
        // and can suppress pass-through for the whole session. Seeing the line
        // ABOVE the captured reference means the capture was a press -- no button
        // pulls the ladder UP from true idle -- so adopt the higher value. Only
        // the HIGHEST reading is ever adopted, so a press cannot drag the
        // reference down the way a naive re-capture would.
        if (level_now_mv > pass_through_idle_mv_) {
            pass_through_idle_mv_ = level_now_mv;
            cs.pass_through_pressed = false;
        }
        // `idle` is re-read AFTER the heal so a press in this same tick is not
        // measured against the reference it just replaced.
        const int idle = pass_through_idle_mv_;

        // FR-25 / spec 6.9: "a press is clearly off idle (> 300 mV from the
        // wheel's idle)", read directly off the ADC rather than through the noisy
        // median -- the reference was captured the same way, so like is compared
        // with like. `level_mv`/`cs.reader` stay for the configured path, whose
        // debounce genuinely needs the filter.
        const bool pressed = (idle - level_now_mv) > kPassThroughPressDeltaMv;
        // RISING EDGE only. The pulse self-releases on `send_duration_ms` below,
        // so without this latch the very next tick would see the button still
        // held, `key_driven` false, and re-arm -- the line would pulse once per
        // send_duration instead of once per press, and a held button would emit a
        // key every 200 ms forever. The edge also matches the configured path,
        // where a SINGLE cannot fire twice without a release between.
        const bool rising_edge = pressed && !cs.pass_through_pressed;
        cs.pass_through_pressed = pressed;

        if (rising_edge && head_unit_idle_mv_ > 0) {
            // The head unit's OWN idle is the denominator: the wheel asks for a
            // fraction of a full-scale ladder position, and that fraction is then
            // applied to the head unit's range. Using the output's safe-idle code
            // instead would scale against 5200 mV and push low buttons into the
            // clamp, which is the wrong key rather than a quieter one.
            PresentLevel(index, level_mv, idle, sense_mv, now_ms, key_ch);
        } else if (!pressed) {
            ReleaseKey(index);
        }
        // No gesture state machine, no bindings, no buzzer pattern: there is
        // nothing configured to resolve against, and inventing feedback for an
        // event the user did not bind would be noise.
        //
        // NOTE: this block deliberately no longer RETURNS. The early return that
        // used to sit here skipped both the pulse-timeout release below and the
        // rail-fault release, so a held pass-through press drove the line for as
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
                                                 now_ms};
                    gesture_sink_(gesture_sink_ctx_, rec);
                }
                buzzer_.Play(BuzzerPattern::kKeyUnknown);
            }
        } else {
            cs.unknown_reported = false;
        }

        if (fired) {
            const ResolvedAction resolved = BindingResolve(config_, index, ev);
            // Spec 4.3: the app is told AFTER the device has acted on its own
            // local binding, never before. `event` is fire-and-forget precisely
            // so that a button press is not held hostage to the app being
            // responsive (spec 6.6), and reporting first would make the link part
            // of the key path.
            ReportGesture(index, ev, level_mv);
            if (resolved.found) {
                const Action &a = resolved.action;
                if (a.kind == ActionKind::kOutVoltage && a.key_mv != 0) {
                    // The action names the voltage directly (spec 3.6) -- there is
                    // no head-unit model here and no resistance to convert. One
                    // bounded pulse, held for the recognition time, then released:
                    // the head unit sees a single key event, not a held line.
                    //
                    // FR-18: a key_mv outside the gain mode's envelope is
                    // VALIDATED and CLAMPED, with a warning -- never driven out of
                    // range. The clamp itself lives in GainPolicyCodeForTarget
                    // (both the floor and the ceiling, then a second clamp to the
                    // DAC's range); what was missing was the warning. The action is
                    // clamped rather than refused because the alternative is a
                    // press that silently does nothing, and a clamped level still
                    // reaches the radio as a key.
                    //
                    // The comparison is against `GainDecision::clamped` rather
                    // than a second copy of the envelope bounds, so the envelope
                    // has one definition and this cannot drift from the clamp it
                    // reports on.
                    const GainDecision decision =
                        GainPolicyCodeForTarget(gain_mode_[index], a.key_mv);
                    if (decision.clamped) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "key_mv %d clamped to DAC code %u in gain mode %d",
                                 a.key_mv, static_cast<unsigned>(decision.dac_code),
                                 static_cast<int>(gain_mode_[index]));
                        if (log_sink_ != nullptr) log_sink_(log_sink_ctx_, "WARN", msg);
                    }
                    cs.servo.Target(gain_mode_[index], a.key_mv);
                    cs.servo.Update(sense_mv);
                    hal_->dac_set_code(hal_->ctx, key_ch, cs.servo.Code());
                    cs.key_driven = true;
                    cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
                } else {
                    // Not a level (an OUT_RELEASE, a NONE, or an app-side action
                    // this firmware does not execute). Release rather than hold:
                    // a stale key with no action behind it is the phantom-key
                    // hazard.
                    ReleaseKey(index);
                }
                buzzer_.Play(BuzzerPattern::kKeyAccepted);
            } else {
                // The button was RECOGNISED but its gesture is not bound, so there
                // is no action to run. The device then behaves as a STOCK WHEEL:
                // it presents that button's own level, one bounded pulse. This is
                // the 2022 design's default -- `lookup_single/double/long_press_val`
                // always presented the key, and only `program_alt_key` overrode it --
                // and it is what makes the no-app product work, because a fresh
                // device learned by AUX1 alone has windows but no bindings
                // (`ConfigDefault` ships `binding_count = 0`), and every runtime
                // binding otherwise comes from a config the app pushes.
                //
                // Without this the learned ladder NEVER drove a key: the press
                // resolved to nothing, the line was released, and KEY_UNKNOWN was
                // played -- the exact silent failure the user reported, invisible
                // because the learn itself beeped LEARN_OK.
                //
                // The head unit is gesture-blind, so it does not matter WHICH
                // gesture landed here: single, double and long all present the same
                // button, which is precisely what a stock wheel does. What a
                // gesture MEANS is a binding; the default is that it means the
                // button itself.
                const uint8_t bi = ev.button_index;
                const LadderProfile &ladder = config_.channels[index].ladder;
                if (bi < ladder.count &&
                    PresentLevel(index, ladder.buttons[bi].mv_center, ladder.learned_idle_mv,
                                 sense_mv, now_ms, key_ch)) {
                    buzzer_.Play(BuzzerPattern::kKeyAccepted);
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
    if (level == ChannelLevel::kFault || rail_fault) {
        ReleaseKey(index);
        cs.gestures.Reset();
        // FR-4: an out-of-range channel -- a collapsed rail, an open input, a
        // short to 12 V -- must be REPORTED, not merely survived. Releasing the
        // line is the safety half; this is the half the user can act on. The
        // indication latches (see ReportFault): a wiring fault does not clear
        // itself, and an indication that faded would be a lie.
        ReportFault();
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

extern "C" SystemOrchestrator *SystemOrchestratorCreate(IHAL *hal) {
    if (hal == nullptr) return nullptr;

    // A default config is what FR-25 wants anyway: if NVS holds nothing, this IS
    // the pass-through fallback, and Boot() overwrites it when a config loads.
    // Built by the shared `ConfigDefault()` so the router's config_get reply and
    // this boot config cannot drift apart (Config/ConfigDefaults.h).
    const Config boot_config = ConfigDefault();

    SystemOrchestrator *sys =
        new (std::nothrow) SystemOrchestrator(hal, boot_config, boot_config.settings.timings);
    if (sys == nullptr) return nullptr;

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

