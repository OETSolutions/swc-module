#include "System/SystemOrchestrator.h"

#include <new>

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

}  // namespace

SystemOrchestrator::SystemOrchestrator(IHAL *hal, const Config &config,
                                       const GestureTimings &timings)
    : hal_(hal), config_(config), timings_(timings),
      buzzer_(hal, config.settings.buzzer_level),
      leds_(hal, config.settings.led_level) {}

// The bindings a channel's buttons actually have, which is what makes the
// gesture resolve adaptive (spec 6.6). Scanned from the config rather than
// assumed: a button binding only SINGLE must not inherit the double-press
// window's latency, and a button with no LONG must never emit one.
GestureBindings SystemOrchestrator::BindingsFor(uint8_t channel_index) const {
    GestureBindings out;
    out.has_double = false;
    out.has_long = false;
    if (channel_index >= config_.channel_count) return out;
    if (config_.binding_count > kMaxBindings) return out;

    const uint8_t as_swc = (channel_index == 0)
                               ? static_cast<uint8_t>(BindingChannel::kSwc1)
                               : static_cast<uint8_t>(BindingChannel::kSwc2);
    for (uint8_t i = 0; i < config_.binding_count; ++i) {
        const Binding &b = config_.bindings[i];
        if (!b.enabled) continue;
        if (b.channel != as_swc && b.channel != static_cast<uint8_t>(BindingChannel::kAny)) {
            continue;
        }
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

        const GainMode mode = GainPolicySelect(config_.channels[i].output.gain_mode ==
                                                       GainMode::kTracking
                                                   ? GainPolicy::kForceTracking
                                                   : (config_.channels[i].output.gain_mode ==
                                                              GainMode::kAmplified
                                                          ? GainPolicy::kForceAmplified
                                                          : config_.settings.gain_policy),
                                               for_gain);
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
        cs.bindings = BindingsFor(i);
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

    // 5. USB/BLE would be brought up here (Tasks 16/18). Nothing in this class
    //    starts them, which is what makes FR-42 structural rather than a promise.
}

void SystemOrchestrator::Tick(uint64_t now_ms) {
    for (uint8_t i = 0; i < channel_count_; ++i) {
        ServiceChannel(i, now_ms);
    }
    buzzer_.Update(now_ms);
    leds_.Update(now_ms);
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

void SystemOrchestrator::Identify() {
    // Both channels, because "which unit is this" is a question about the box,
    // not about one steering-wheel input.
    leds_.SetStat(LedStatPattern::kDoubleFlash);
    buzzer_.Play(BuzzerPattern::kKeyAccepted);
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
        const int idle = pass_through_idle_mv_;
        // Off idle by more than the classifier's own idle band means a press.
        // Reusing the same band keeps one definition of "at idle" in the system.
        const bool pressed = (idle - level_mv) > kPassThroughPressDeltaMv;
        if (pressed && !cs.key_driven && head_unit_idle_mv_ > 0) {
            // The head unit's OWN idle is the denominator: the wheel asks for a
            // fraction of a full-scale ladder position, and that fraction is then
            // applied to the head unit's range. Using the output's safe-idle code
            // instead would scale against 5200 mV and push low buttons into the
            // clamp, which is the wrong key rather than a quieter one.
            const MilliVolt target = static_cast<MilliVolt>(
                (static_cast<long>(head_unit_idle_mv_) * level_mv) / idle);
            cs.servo.Target(gain_mode_[index], target);
            cs.servo.Update(sense_mv);
            hal_->dac_set_code(hal_->ctx, key_ch, cs.servo.Code());
            cs.key_driven = true;
            // Held for the recognition time, then released: the head unit must see
            // ONE key event, not a line held down (spec 6.6 -- it is
            // gesture-blind, so a held line is a different thing to it).
            cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
        } else if (!pressed && cs.key_driven) {
            hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
            cs.key_driven = false;
        }
        // No gesture state machine, no bindings, no buzzer pattern: there is
        // nothing configured to resolve against, and inventing feedback for an
        // event the user did not bind would be noise.
        return;
    }

    GestureEvent ev{};
    const bool fired = cs.gestures.Update(level, cs.classifier.ButtonIndex(), now_ms, &ev,
                                          cs.bindings);

    // A fault, or a lost head unit, releases in the same tick. Everything in
    // flight is discarded: a half-recognised gesture must not reach the radio.
    if (level == ChannelLevel::kFault || rail_fault) {
        if (cs.key_driven) {
            cs.servo.Target(gain_mode_[index], GainPolicyKeyMvForCode(gain_mode_[index],
                                                                      idle_code_[index]));
            hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
            cs.key_driven = false;
        }
        cs.gestures.Reset();
        return;
    }

    if (fired) {
        const ResolvedAction resolved = BindingResolve(config_, index, ev);
        // Spec 4.3: the app is told AFTER the device has acted on its own local
        // binding, never before. `event` is fire-and-forget precisely so that a
        // button press is not held hostage to the app being responsive (spec
        // 6.6), and reporting first would make the link part of the key path.
        ReportGesture(index, ev, level_mv);
        if (resolved.found) {
            const Action &a = resolved.action;
            if (a.kind == ActionKind::kOutVoltage && a.key_mv != 0) {
                // The action names the voltage directly (spec 3.6) -- there is
                // no head-unit model here and no resistance to convert. One
                // bounded pulse, held for the recognition time, then released:
                // the head unit sees a single key event, not a held line.
                cs.servo.Target(gain_mode_[index], a.key_mv);
                cs.servo.Update(sense_mv);
                hal_->dac_set_code(hal_->ctx, key_ch, cs.servo.Code());
                cs.key_driven = true;
                cs.key_released_at_ms = now_ms + timings_.send_duration_ms;
            } else {
                // Not a level (an OUT_RELEASE, a NONE, or an app-side action this
                // firmware does not execute). Release rather than hold: a stale
                // key with no action behind it is the phantom-key hazard.
                if (cs.key_driven) {
                    hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
                    cs.key_driven = false;
                }
            }
            buzzer_.Play(BuzzerPattern::kKeyAccepted);
        } else {
            // FR-12: an unlearned press is never guessed at. The failure mode of
            // a wrong guess is the radio doing something the driver did not ask
            // for, which is worse than doing nothing.
            if (cs.key_driven) {
                hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
                cs.key_driven = false;
            }
            buzzer_.Play(BuzzerPattern::kKeyUnknown);
        }
    }

    // End the pulse once the recognition time has elapsed. Checked every tick,
    // so a press that is never released cannot leave the line driven.
    if (cs.key_driven && now_ms >= cs.key_released_at_ms) {
        cs.servo.Target(gain_mode_[index], GainPolicyKeyMvForCode(gain_mode_[index],
                                                                  idle_code_[index]));
        hal_->dac_set_code(hal_->ctx, key_ch, idle_code_[index]);
        cs.key_driven = false;
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

