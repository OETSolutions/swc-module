#pragma once

#include <stdint.h>

#include "Analog/AdcReader.h"
#include "Config/ConfigModel.h"
#include "Feedback/BuzzerGrammar.h"
#include "Feedback/LedGrammar.h"
#include "Gesture/GestureStateMachine.h"
#include "Gesture/PressClassifier.h"
#include "HAL/IHAL.h"
#include "Learning/LearnWizard.h"
#include "Maintenance/MaintenanceMode.h"
#include "Output/ServoLoop.h"

// Forward-declared: the orchestrator only needs the POINTER, and including the
// store would pull NVS into every host test that includes this header.
class ConfigStore;

/*
 * The main loop that ties the modules together.
 *
 * It holds only an IHAL* and the pure-logic modules above, so it is host-
 * testable; Task 14 supplies the real EspHal underneath it.
 *
 * The ordering in Boot() is the safety requirement (FR-13, spec 6.7), not a
 * style choice: the output reaches its safe idle state BEFORE anything that
 * could accept a command comes up. A watchdog reset mid-update must not be able
 * to leave a phantom key driven on the head unit.
 */
class SystemOrchestrator {
public:
    SystemOrchestrator(IHAL *hal, const Config &config, const GestureTimings &timings);

    // Establishes the safe idle output, then constructs the per-channel state.
    void Boot();

    /*
     * Tell the orchestrator the ADC fell back to the linear approximation (spec
     * 3.2: blank eFuses). Must be called BEFORE `Boot`, which is what plays the
     * boot pattern -- `Create` does exactly that.
     *
     * **Why this exists.** The spec requires a blank eFuse be REPORTED, not
     * silently mis-scaled: it is the same class as a recovered config, so the
     * device announces `BOOT_DEGRADED` (spec 7.2). `main.cpp` and `EspHal` both
     * claimed "the orchestrator also plays BOOT_DEGRADED for this class of
     * condition", and it did not -- `IHAL` carries no calibration accessor, so the
     * orchestrator had no way to know, and the boot pattern was chosen purely from
     * the config load. A blank-eFuse device booted silently, which is the exact
     * "silently mis-scale every reading" failure the spec's fallback clause names.
     * The flag is injected rather than read from the HAL because `IHAL` is the
     * frozen C contract, and the ONE production caller (`SystemOrchestratorCreate`)
     * already has the answer from `EspHalCalibrationIsDegraded()`.
     */
    void SetCalibrationDegraded(bool degraded) { calibration_degraded_ = degraded; }

    /*
     * Adopt a config that arrived over the link, so it is the config the RUNNING
     * device classifies against (spec 4.2: a committed config takes effect
     * immediately, with no reboot).
     *
     * **Why this exists.** `config_end`/`config_patch`/`learn_commit` all called
     * `ConfigStore::Save` and stopped there, so a config the app had just saved
     * (and adopted as the device's live state on the `ack`) was NOT what the
     * device ran: the classifiers, the gesture machines, the timings and the
     * output derivation were all still the ones `Boot` built. The app showed the
     * user bindings the device would not honour until the next power cycle --
     * which for a car-installed device may never come.
     *
     * This is the same re-derivation `Boot` performs, run again over the new
     * config: the output (gain mode, idle code) via `EstablishSafeIdle`, then the
     * per-channel state via `SeedChannelState`. The headless learn path already
     * did the equivalent for one profile (`ApplyLearnedProfile`); this is the
     * whole-config case.
     *
     * A REJECTED config never reaches here: every caller applies only after
     * `Save` succeeded, so a refusal leaves the running config untouched -- the
     * same rule as the stored one.
     */
    void ApplyConfig(const Config &c);

    // One poll tick. Reads the ADC channels, classifies, resolves any completed
    // gesture to an action, drives the output, and advances the feedback
    // grammars. Does no allocation and no I/O beyond the HAL calls.
    void Tick(uint64_t now_ms);

    /*
     * The HEAD UNIT's measured KEY-line idle for a channel, in millivolts: spec
     * 6.2 step 1's `V_KEY_idle` (`2 x /SENSEn`, measured with the output released).
     *
     * NOT the idle DAC code's own voltage, which is what this used to report. Those
     * are different quantities and only one of them is the spec's "single most
     * important measured number in the system": the idle code is chosen to command
     * *above* the line's rest (spec 6.7 -- idle is a release, and the output only
     * sinks), so its voltage sits near the 5200 mV ceiling, while `V_KEY_idle` is
     * where the head unit's own line rests. The old value was also channel 0's
     * alone, and the two head-unit inputs are independent (spec 6.2 samples
     * `/SENSEn` per channel), so one channel's measurement could stand in for the
     * other.
     *
     * This is the reference spec 6.2's command band is taken against --
     * `GainPolicyClampCommand` keeps every command at least `kCommandHeadroomMv`
     * BELOW it, because above it the servo can only turn `Q4` off, which is the
     * release behavior rather than a command. Returns 0 for an out-of-range channel
     * or before Boot has measured one (spec 6.2 step 2: no head unit).
     */
    int IdleKeyMv(uint8_t channel_index) const
    {
        if (channel_index >= kMaxChannels) return 0;
        return head_unit_idle_mv_[channel_index];
    }

    /*
     * The store a HEADLESS learn writes through (FR-31).
     *
     * Set by the link once, at start-up, because the learn wizard's whole point is
     * that it works with **no app and no host** -- so the orchestrator cannot rely
     * on a caller having a store to hand. A null store means a learn still runs
     * and still beeps but does not persist, which is the honest degradation: the
     * wizard's feedback is real, the save is not.
     */
    void SetStore(ConfigStore *store) { store_ = store; }

    // True while the headless learn wizard is running (FR-31). The caller uses it
    // to keep the normal feedback grammars from fighting the wizard's prompts.
    bool LearnActive() const { return wizard_.Active(); }

    /*
     * FR-33's maintenance window: the only state in which the radio exists.
     *
     * The decision lives here and the radio does NOT: `MaintenanceMode` is pure
     * state, and the NimBLE/wifi_provisioning/web-server work is device-only and
     * lives in the maintenance-only translation units. `maintenance_ == active_`
     * on every tick, so a caller can poll it and bring the radio up or tear it
     * down exactly once per transition.
     *
     * It is NOT exclusive: the orchestrator keeps ticking, so a button press still
     * works while the setup page is open. FR-38's bounded window exists because a
     * device unable to serve input is unacceptable, and that would be pointless if
     * maintenance stopped serving input.
     */
    /*
     * Spec 8.2's maintenance hold: 3 s, deliberately longer than the 1.5 s
     * programming hold so the two gestures cannot be confused. Public because it
     * is the contract a caller or a test reasons about, and a second copy of the
     * number elsewhere is a second answer to "how long is a maintenance hold".
     */
    static constexpr uint32_t kMaintenanceHoldMs = 3000;

    /*
     * How long a released KEY line must read outside the 1.80-5.20 V envelope
     * before this device calls the head unit absent (spec 6.8's "head unit
     * disappears" row). A settle, not a debounce: a line released from a pulse
     * takes a moment to return to rail, and a rail coming up at boot is outside
     * the envelope for that moment. Without it, either is read as an absent head
     * unit. Longer than the pulse-settle, shorter than a head unit's own power
     * blip.
     */
    static constexpr uint32_t kHeadUnitGoneSettleMs = 250;

    /*
     * How long `identify` borrows LED_STAT for its double-flash burst before the
     * normal state is restored (spec 7.3's "borrows while it runs"). Long enough
     * for a user to see the flash, short enough that it reads as an event.
     */
    static constexpr uint32_t kIdentifyFlashMs = 1500;

    bool MaintenanceActive() const { return maintenance_.Active(); }
    MaintenanceTrigger MaintenanceTriggeredBy() const { return maintenance_.Trigger(); }
    void EnterMaintenance(MaintenanceTrigger t, uint64_t now_ms) {
        maintenance_.Enter(t, now_ms);
    }
    void ExitMaintenance() { maintenance_.Exit(); }
    // Bumps the activity clock, so a user typing a PoP is not kicked out mid-task.
    void NoteMaintenanceActivity(uint64_t now_ms) { maintenance_.NoteActivity(now_ms); }

    /*
     * Whether the last headless learn reached durable storage.
     *
     * Distinct from "the learn succeeded": the wizard can commit a real button
     * with no store attached (a bench build without NVS), and reporting that as
     * saved would be a lie the user discovers on the next boot.
     */
    bool LastLearnPersisted() const { return persisted_; }

    /*
     * The channel and slot the last headless learn committed, for the link's
     * benefit (FR-29's reporting). Null when nothing has been committed yet.
     */
    const LadderProfile *LastLearnedProfile(int channel) const {
        if (channel < 0 || channel >= kMaxChannels) return nullptr;
        return (learned_channel_ == channel) ? &learned_profile_ : nullptr;
    }

    // True once the safe idle state has been written (FR-13). Boot() sets it
    // before it does anything else, so a caller that observes it true knows the
    // output is already safe.
    bool SafeIdleEstablished() const { return safe_idle_established_; }

    /*
     * True when the output has been WRITTEN and not one of those writes failed.
     *
     * **This is the falsifiable half of FR-37's health gate, and it exists
     * because `SafeIdleEstablished()` alone could not falsify anything.** That
     * flag is only ever assigned `true` (a single assignment at the end of
     * `EstablishSafeIdle`, which returns void and cannot fail), so the gate in
     * `app_main` that marked an OTA image valid on `SystemOrchestratorSafeIdle()`
     * was a compile-time constant `true` on the device path. Spec §9.8 is explicit
     * that the gate must prove the device "can do its job -- not merely after
     * main() starts", and an image whose I2C bus is dead would then have had its
     * pending rollback CANCELLED: bricked-but-"valid", the exact state FR-37
     * exists to prevent.
     *
     * So the condition reads the HAL's `dac_faulted` latch as well. A write that
     * fails leaves the latch set for the rest of the boot (spec 7.3: a hardware
     * condition that does not fix itself), and this accessor is false from then
     * on -- which is what makes the gate able to say NO.
     */
    bool OutputVerified() const {
        return safe_idle_established_ && !hal_->dac_faulted(hal_->ctx);
    }

    /*
     * Drive the KEY line at `key_mv` for `hold_ms`, then release. This is spec
     * 4.3's `test_key` -- a bench/production check of the output stage -- and it
     * shares the SERVO with the normal action path rather than writing a DAC
     * code directly, so what the bench measures is what a real press produces.
     *
     * Returns false if `key_mv` is outside the output envelope (spec 6.2's
     * 1800..5200 mV). Refusing is required: the envelope is what the servo is
     * allowed to command, and a bench command that could exceed it would be a
     * way to discover that the clamp is missing.
     *
     * It deliberately does NOT wait out a gesture: a test command has no gesture
     * to resolve, so it drives immediately.
     */
    bool TestDriveKeyMv(uint8_t channel_index, int key_mv, uint32_t hold_ms, uint64_t now_ms);

    // Flash both LED channels and/or buzz once, so a user can tell WHICH unit
    // they are talking to (spec 4.3's `identify`). Non-blocking: it sets the
    // patterns and returns, and the normal Tick advances them.
    //
    // `flash` and `buzz` are the frame's two patterns, and they are separate
    // because they serve different users: `flash` borrows LED_STAT for the burst
    // (and so arms the restore window), `buzz` sounds the buzzer alone. The
    // defaults are both-on, which is what the learn/maintenance paths want.
    void Identify(bool flash = true, bool buzz = true);

    /*
     * The channel's most recent FILTERED ladder level, in millivolts (FR-3).
     *
     * This is the value `ServiceChannel` classified on, cached rather than
     * re-read: a caller that sampled the ADC again would get a different
     * conversion and could disagree with what the device just acted on. It is
     * what FR-5's `ladder_sample` stream reports during learn.
     *
     * Returns 0 for an out-of-range channel.
     */
    int FilteredLevelMv(uint8_t channel_index) const
    {
        if (channel_index >= kMaxChannels) return 0;
        return static_cast<int>(channels_[channel_index].reader.Value());
    }

    /*
     * The channel's ratio denominator: `V_ADC_idle`, the LIVE idle (spec 6.3).
     *
     * NOT `ladder.learned_idle_mv`. The learned idle is the rail the button
     * centres were measured at and is what `LadderClassify` derives those centres
     * against; the denominator here is the rail NOW. Pinning this to the learned
     * value would make a learn taken on a moved rail record ratios against the
     * wrong rail, and would leave `LadderClassify`'s FR-30 sag check comparing a
     * value to itself (its reference argument and `profile.learned_idle_mv`
     * would be the same). Seeded at Boot and re-adopted as the rail moves.
     *
     * Returns 0 for an out-of-range channel, or before Boot has established a
     * reference.
     */
    int IdleReferenceMv(uint8_t channel_index) const
    {
        if (channel_index >= kMaxChannels) return 0;
        return channels_[channel_index].idle_reference_mv;
    }

    /*
     * True while the device is serving spec 6.9's transparent pass-through: it
     * booted with no config AND has a usable ladder reference.
     *
     * Public because it is the observable state of FR-25: a test asserts it, and
     * the link's status frame reports whether the device is configured.
     */
    bool PassThroughActive() const { return pass_through_; }

    // A channel's active gain mode, after the boot-time policy decision.
    GainMode ChannelGainMode(uint8_t channel_index) const {
        return (channel_index < kMaxChannels) ? gain_mode_[channel_index] : GainMode::kAmplified;
    }

    /*
     * Spec 4.3's `event`: one recognized gesture, reported to the link.
     *
     * **`button` is the learned id, or NULL when no window matched** (FR-12). The
     * app's bindings grid and its live ladder are both keyed by the id, so sending
     * an index would force every consumer to re-derive a mapping the device already
     * has. A null is not an omission: it is the device saying "a press happened and
     * I did not recognise the level", which is the only way the app can show a user
     * that their button is mis-learned rather than broken.
     *
     * **There is no `confidence`.** Confidence (spec 3.4) is a learned-BUTTON
     * quality score earned at learn time; the press classifier answers "which
     * window contains this reading" and has no degree of belief to report. The
     * earlier spec revision listed a confidence here, which would have meant
     * inventing a number the device never measured.
     *
     * `gesture` is a `Gesture` and `level_mv` the FILTERED level the decision was
     * made on (FR-3) -- the same figure `FilteredLevelMv` reports and the same
     * one `ladder_sample` streams, so the app cannot show a level the device did
     * not decide on.
     */
    struct GestureEventRecord {
        uint8_t     channel_index;
        const char *button_id;
        Gesture     gesture;
        int         level_mv;
        uint64_t    at_ms;
    };

    /*
     * Register the sink a recognized gesture is handed to, and the context to
     * call it with. One slot, no allocation.
     *
     * **Why a sink rather than a return value.** A gesture is not resolved on the
     * tick it is recognized: SINGLE waits out the double-press window when the
     * button binds DOUBLE (spec 6.6), so the completion is emitted from inside
     * `GestureStateMachine::Update` -- one frame deeper than `Tick`. Returning it
     * would mean threading an out-parameter through the whole call chain, and the
     * link could still only see it on the NEXT tick. The sink is called at the
     * moment of recognition instead.
     *
     * **A sink that never returns blocks the poll loop.** The one caller is
     * `UsbLink`, whose sink formats a frame and hands it to the CDC writer, and
     * dropping a frame is preferable to stalling the key path -- so the caller
     * must not block, exactly as `CommandRouter::FrameSink` already requires.
     *
     * A null sink is legal and means "nobody is listening": the device keeps
     * working with no app attached (spec 6.6).
     */
    using GestureSink = void (*)(void *ctx, const GestureEventRecord &ev);
    void SetGestureSink(GestureSink sink, void *ctx) {
        gesture_sink_ = sink;
        gesture_sink_ctx_ = ctx;
    }

    /*
     * Whether a USB host is attached, so `LED_STAT` can be solid rather than
     * breathing (spec 7.3).
     *
     * Driven by the link's connect/disconnect hooks. The orchestrator cannot
     * observe the link itself -- and must not: spec 6.6 requires the device to
     * keep serving presses with the link down, so this is a display input and
     * nothing else. Feedback is never on the key path (FR-21).
     */
    void SetUsbConnected(bool connected) {
        if (usb_connected_ != connected) {
            usb_connected_ = connected;
            RestatLeds();
        }
    }

    /*
     * Report a fault on the LEDs (spec 7.3's `kBlink`, "5 Hz: fault -- the
     * buzzer's FAULT_* says which").
     *
     * **This was the one thing FR-4's "detect and report" had no home for.** The
     * firmware detected an out-of-range channel, released the line and said
     * nothing anywhere: no buzzer, no LED, no frame. The spec's entire fault
     * indication -- `LED_STAT` blink -- was unreachable, because the only
     * `SetStat` callers were the learn wizard and the identify flash.
     *
     * A fault has no timeout and no clear, deliberately: a wiring fault or a
     * collapsed rail is a hardware condition that does not fix itself, so a
     * self-clearing indication would be a lie. Recovery is a reboot, which spec
     * 6.1's startup sequence already handles.
     *
     * **This is the HARDWARE latch only.** Spec 7.3's reboot-only rule is stated
     * for "a wiring fault or a collapsed rail ... a hardware condition that does
     * not fix itself", and those are the faults raised here. A config that fell
     * back to defaults is NOT a hardware condition -- it is remedied by writing a
     * valid config -- so it lives in `config_faulted_`, which a commit clears.
     * The two are separate because they have different lifetimes; sharing one flag
     * would force a wrong choice for whichever is fixed first.
     */
    void ReportFault() {
        if (hw_faulted_) return;
        hw_faulted_ = true;
        // Through RestatLeds, not a bare SetStat, so the precedence lives in one
        // place: fault beats maintenance beats normal. A fault raised while a
        // maintenance window is open would otherwise be painted over by the
        // window's double-flash, and the fault indication is the one that must
        // never be hidden.
        RestatLeds();
        // **No buzzer, deliberately.** Spec 7.2's fault patterns each name a
        // SUBSYSTEM -- `FAULT_DAC` is the I2C/DAC path, `FAULT_CONFIG` is a
        // corrupt config -- and a collapsed rail or an open ladder input is
        // neither. Playing `FAULT_DAC` here would tell the user to look at the
        // wrong part of the board, which is worse than saying nothing, and the
        // spec defines no pattern for a wiring fault. Spec 7.3 makes the LED the
        // continuously-readable fault channel, and this indication latches, so it
        // is still blinking whenever anyone looks. See open item N-10.
    }

    // Either fault kind: the lamp is the single fault channel (spec 7.3), so a
    // reader asking "is this device OK?" must see both, however temporary the
    // config half is.
    bool Faulted() const { return hw_faulted_ || config_faulted_; }

    /*
     * The config's state, as a word for spec 4.3's `status` frame.
     *
     * **The `status` frame reported the wrong quantity under this name.** It sent
     * `config_state` derived from `SafeIdleEstablished()`, so a device whose
     * config had fallen back to defaults -- the exact case spec 6.8 requires be
     * reported as `config_state: defaults` -- answered `"ok"`, and the app could
     * not distinguish a healthy config from a lost one. The fault was only on the
     * buzzer (transient) and the LED (a colour, not a name).
     *
     * `kNoConfig` is `"none"` rather than `"defaults"`: FR-25's pass-through
     * device is a supported state, not a fault, and collapsing the two would make
     * a fresh board report a corrupt config.
     *
     * **This describes the config the device is RUNNING, so a commit updates it.**
     * The boot load is how the value is derived, not what the field is about:
     * `defaults` and `none` both say "the config in force is not the user's", and
     * a committed config puts the user's in force. Freezing it at the boot value
     * made the app tell a user who had just re-programmed the device that it had
     * "lost its configuration ... program it again" -- the report asserting the
     * opposite of the truth, which is the lie this field exists to prevent.
     *
     * A local enum rather than `ConfigLoadResult`: this header forward-declares
     * `ConfigStore` on purpose so including it does not pull NVS into every host
     * test, and the four states here are exactly the ones `Boot` distinguishes.
     */
    enum class BootConfigState { kOk, kNone, kRecovered, kDefaults };

    const char *ConfigStateWord() const {
        switch (config_state_) {
            case BootConfigState::kOk:        return "ok";
            case BootConfigState::kNone:      return "none";
            case BootConfigState::kRecovered: return "recovered";
            case BootConfigState::kDefaults:  return "defaults";
        }
        return "unknown";
    }

    /*
     * A committed config puts the user's config in force, so `config_state` is
     * `ok` and the config-fault LED clears.
     *
     * One home, called from every commit path (`config_end`, `config_patch`, the
     * app's `learn_commit`, and the headless AUX1 learn's `ApplyLearnedProfile`),
     * because three copies of "and now the config is fine" is three chances to
     * forget one -- the shape that produced this field's original boot-only bug.
     *
     * Only the CONFIG fault stands down. `hw_faulted_` is a statement about the
     * board (spec 7.3) and a config arriving says nothing about the ladder's
     * wiring, so it keeps blinking.
     */
    void NoteConfigCommitted() {
        config_state_ = BootConfigState::kOk;
        config_faulted_ = false;
    }

    /*
     * A diagnostic line for the app's log view (spec 4.3's `log` frame).
     *
     * A sink rather than a return value because the events that need reporting
     * happen deep inside the poll loop, where there is no caller to return to.
     * Null is legal and means "nobody is listening" -- the device is fully
     * functional with no app (spec 6.6), so a diagnostic that cannot be delivered
     * is dropped rather than buffered.
     */
    using LogSink = void (*)(void *ctx, const char *level, const char *msg);
    void SetLogSink(LogSink sink, void *ctx) {
        log_sink_ = sink;
        log_sink_ctx_ = ctx;
    }

private:
    struct ChannelState {
        // Both of these take their profile/timings at construction and have no
        // default constructor, so a default-constructed array needs seeding with
        // an empty profile. Boot reassigns them once the real config is known --
        // which is also after the safe idle state is established, the ordering
        // FR-13 requires. No heap: everything is a value member.
        // `reader` is default-constructed as UNBOUND and bound in Boot(), which
        // is the first point the HAL and the channel index are both known.
        ChannelState()
            : classifier(LadderProfile{}, GestureTimingsDefault()),
              gestures(GestureTimingsDefault()),
              reader() {}

        PressClassifier     classifier;
        GestureStateMachine gestures;
        // FR-3's noise filter. One per channel, because the two ladders are
        // independent inputs and sharing a window would let a press on one
        // channel move the other's reported level (FR-9).
        AdcReader           reader;
        ServoLoop           servo{ServoConfigDefault()};
        bool                key_driven = false;    // a pulse is currently on the line
        uint64_t            key_released_at_ms = 0;
        // FR-25: pass-through has no gesture machine to latch a press, and its
        // pulse self-releases after `send_duration_ms`, so it needs its own
        // edge detector -- otherwise a held button re-arms the pulse every tick
        // after each release and emits a key every 200 ms.
        bool                pass_through_pressed = false;
        // FR-25's per-channel wheel idle: the ratio's denominator with no config
        // to pin one to. PER CHANNEL, because the two SWC inputs are independent
        // wheels (FR-9) whose idles need not match -- a single device-wide
        // reference taken from one channel reads every tick of the other as a
        // press whenever their idles differ by more than kPassThroughPressDeltaMv,
        // driving a phantom key with nothing held.
        int                 pass_through_idle_mv = 0;
        // Spec 6.3's ratio denominator for the CONFIGURED path: `V_ADC_idle`,
        // measured now. Seeded at Boot from the live idle and re-adopted as the
        // rail moves. It must NOT be the learned idle: that pins the denominator
        // to the rail the ladder was learned at, so the ratio drifts with the
        // rail's own deviation (~5% across the 3.14-3.47 V band), which pushes an
        // idle-adjacent button out of its window at the band edges and leaves
        // `LadderClassify`'s FR-30 sag check comparing a value to itself (its
        // reference argument and `profile.learned_idle_mv` would be the same).
        int                 idle_reference_mv = 0;
        // FR-12: one report and one KEY_UNKNOWN beep per unrecognised press, not
        // one per poll tick. A held level is unrecognised on every tick, so
        // without this latch a held press would emit 100 events/s and play a
        // continuous beep. Cleared when the level returns to idle or a real
        // button, so the next unrecognised press reports again.
        bool                unknown_reported = false;
        // Spec 6.8's head-unit-gone detection. `V_KEY_idle` is the line's IDLE,
        // so it can only be judged on a RELEASED line -- while this device drives
        // a pulse its own sense node reads that pulse (see `HeadUnitGone`).
        // `envelope_bad_since_ms` is when the released level first read outside
        // the envelope, or 0 when it reads inside; the condition must persist for
        // `kHeadUnitGoneSettleMs` before it counts, so the settle of a released
        // pulse or a rail coming up is not read as an absent head unit.
        uint64_t            envelope_bad_since_ms = 0;
    };

    // Spec 6.2 step 2's "no head unit" test, per channel, with the two guards it
    // needs to be usable per-tick. `sense_mv` is the channel's KEY-sense reading
    // (may be the `-1` failure sentinel). Returns true only when the line is at
    // REST and has read outside the envelope for `kHeadUnitGoneSettleMs`.
    bool HeadUnitGone(uint8_t index, int sense_mv, uint64_t now_ms);

    void EstablishSafeIdle();
    /*
     * (Re)derive every channel's runtime state from `config_` and the live ADC:
     * the classifier, the gesture machine, the bound reader, the servo, the
     * ratio denominator and -- when pass-through is active -- the per-channel
     * wheel idle. Returns true if ANY channel found a usable pass-through
     * reference.
     *
     * **Why it is a method and not three lines of `Boot`.** `Boot` and
     * `ApplyConfig` must derive the per-channel state the SAME way or a config
     * that arrives over the link classifies differently from one that was stored
     * before boot -- the divergence spec 4.2's "takes effect immediately" exists
     * to prevent. One definition is the only way that stays true as either side
     * changes.
     */
    bool SeedChannelState();
    void ServiceChannel(uint8_t index, uint64_t now_ms);
    // FR-31: the headless learn wizard and the AUX1 hold that drives it.
    void ServiceLearn(uint64_t now_ms);
    // Return a channel's KEY line to its safe idle. One definition, because every
    // release must ALSO re-point the servo at the idle level: `ServoLoop::Update`
    // trims toward whatever `Target()` last set, so a release that only wrote the
    // DAC code would let the next update pull the line back toward the released
    // key's voltage.
    void ReleaseKey(uint8_t index);
    /*
     * Write a channel's signal DAC code, and in TRACKING mode mirror the SAME
     * code onto its V_ADJ channel.
     *
     * Spec 2.3 / DESIGN 4.4: tracking mode has gain 1.00 because `V_ADJ` tracks
     * the signal channel's code -- `V_ADJ = V_DAC` cancels the `(R58/R61)`
     * terms. Amplified mode (1.82) is the opposite: `V_ADJ` sits in its defined
     * 1 kohm power-down, contributing nothing. The gain-mode SELECTION writes
     * the power mode; nothing else did, so tracking mode drove a live 1.82
     * against a 3 V head unit -- the over-range direction spec 6.2 calls the
     * only dangerous one. One write point, because every KEY code write must
     * carry its V_ADJ with it or the two can disagree mid-pulse.
     */
    void DriveKeyCode(uint8_t index, uint16_t code);
    /*
     * Present a ladder level on the head unit as ONE bounded pulse, by RATIO.
     *
     * This is spec 6.9's mapping, and it has two callers: the no-config
     * pass-through (FR-25) and the unbound-gesture fallback below. Both are "the
     * user pressed a button the config does not bind, so be the stock wheel".
     *
     * Returns false when there is no usable head-unit idle to map onto.
     * Fabricating a denominator would land every press on a key nothing defined,
     * so the caller must RELEASE rather than drive a guess -- the same direction
     * FR-12 takes for an unrecognised level.
     */
    bool PresentLevel(uint8_t index, int level_mv, int wheel_idle_mv, int sense_mv,
                      uint64_t now_ms);
    void ApplyLearnedProfile(int channel, const LadderProfile &profile);
    // Spec 4.3's `event`. Called at the moment of recognition, from inside
    // ServiceChannel's resolution branch, because a gesture can complete on any
    // tick (a LONG fires when its threshold elapses, a SINGLE when its ambiguity
    // window closes).
    void ReportGesture(uint8_t index, const GestureEvent &ev, int level_mv);
    // What ONE button's own bindings say about how long its press must stay
    // undecided. Derived from the config, not assumed -- a button that binds
    // only SINGLE must not wait out the double-press window (spec 6.6 rule 3,
    // which is a PER-BUTTON property).
    GestureBindings BindingsForButton(uint8_t channel_index, uint8_t button_index) const;

    IHAL          *hal_;
    Config         config_;
    GestureTimings timings_;
    BuzzerGrammar  buzzer_;
    LedGrammar     leds_;
    ChannelState   channels_[kMaxChannels];
    uint8_t        channel_count_ = 0;

    // Per-channel idle DAC codes, written at boot and returned to on release.
    uint16_t       idle_code_[kMaxChannels] = {};
    GainMode       gain_mode_[kMaxChannels] = {};
    bool           safe_idle_established_ = false;

    /*
     * FR-25's transparent pass-through (spec 6.9).
     *
     * True when the device booted with NO stored config. There is then no learned
     * ladder, so classification against learned windows cannot work -- but the
     * wheel must still do something (spec 6.9: a config-loss event degrades to
     * "the steering wheel works like stock" rather than to "does nothing").
     *
     * The mapping is by RATIO, not by voltage. The wheel's ladder and the head
     * unit's need not have the same resistances, so copying the incoming
     * millivolts would land on the wrong key; reproducing the incoming RATIO
     * against the head unit's own auto-detected idle is ladder-independent, which
     * is the same normalization spec 6.3 already uses for the configured case.
     */
    bool pass_through_ = false;
    // The HEAD UNIT's measured idle (spec 6.2 step 1), which the pass-through
    // ratio is applied TO. The WHEEL's idle -- the ratio's denominator -- is
    // PER CHANNEL (`ChannelState::pass_through_idle_mv`), because each SWC input
    // is its own wheel with its own idle. This is per channel too, for the same
    // reason: spec 6.2 samples `/SENSEn` per channel, so two head-unit inputs
    // have two idles. Conflating either pair maps every press to the wrong key.
    int  head_unit_idle_mv_[kMaxChannels] = {};

    // Where a recognized gesture is reported (spec 4.3's `event`). Null until a
    // link registers, and legal to leave null: the device runs without an app.
    GestureSink gesture_sink_ = nullptr;
    void       *gesture_sink_ctx_ = nullptr;
    bool        usb_connected_ = false;
    // The HARDWARE fault latch (spec 7.3): a wiring fault or a collapsed rail,
    // raised through `ReportFault` and never cleared, because the condition does
    // not fix itself.
    bool        hw_faulted_ = false;
    // The CONFIG fault latch (spec 6.8): the boot load fell back to defaults. It
    // is not a hardware condition, so a commit that persists a valid config clears
    // it -- see `ApplyConfig`. Kept separate from `hw_faulted_` for exactly that
    // reason; `Faulted()` ORs them for the lamp.
    bool        config_faulted_ = false;
    // The config's state, for `status`'s `config_state` (see ConfigStateWord).
    // Named for the CONFIG and not the boot because it follows the config through
    // a commit -- a boot-only name invites the boot-only behaviour this field had.
    // Defaults to kOk so a caller that never calls Boot -- a bare test -- does not
    // report a fault it never had.
    BootConfigState config_state_ = BootConfigState::kOk;
    // The LED2 pattern the driving-state derivation last chose. Kept so `Tick`
    // only calls `Set2` on a CHANGE: `Set2` restarts the pattern's phase clock,
    // so re-setting the same value every tick would hold every LED2 pattern at
    // its first step forever.
    Led2Pattern led2_driving_ = Led2Pattern::kOff;
    void UpdateLed2ForDrivingState();
    // `identify` BORROWS LED_STAT for one double-flash burst and must hand it
    // back (spec 7.3). `identify_active_` is the latch; `identify_until_ms_` is
    // when the borrow ends. Restoring is driven from Tick so the fault precedence
    // stays in `RestatLeds` alone.
    bool        identify_active_ = false;
    uint64_t    identify_until_ms_ = 0;
    void RestoreLedsAfterIdentify(uint64_t now_ms);
    LogSink     log_sink_ = nullptr;
    void       *log_sink_ctx_ = nullptr;
    void RestatLeds();

    /*
     * FR-31's headless learn, and the AUX1 hold that enters and leaves it.
     *
     * Everything here is what makes the device programmable with no phone: the
     * wizard is driven by the poll loop, the AUX1 hold is detected on raw ADC
     * reads (it must work even while a learn is running, so it cannot go through
     * the wizard's own classifier), and a commit is written straight to NVS.
     */
    LearnWizard  wizard_;
    ConfigStore *store_ = nullptr;
    // The AUX1 level at the last tick, for the hold detector's edge.
    uint64_t     aux_hold_ms_ = 0;
    bool         aux_holding_ = false;
    // Set when a hold has already toggled, cleared only on release. A continuous
    // hold must toggle ONCE: `aux_holding_` alone would fire again on every tick
    // past the threshold, and a user who held AUX1 a little long would exit the
    // learn they had just entered.
    bool         aux_hold_latch_ = false;
    /*
     * The maintenance state the LED_STAT pattern was last restated for.
     *
     * The maintenance window opens from THREE places that are not `RestatLeds`
     * callers -- the USB command, the AUX1 hold and the boot-time config flag or
     * no-config offer -- and closes on its own 5-minute timeout inside
     * `maintenance_.Update`. Rather than make every one of those call `RestatLeds`
     * and then miss the timeout, `Tick` compares this to `maintenance_.Active()`
     * and restates on the edge. That single comparison is what keeps the
     * double-flash honest for a path nobody remembered to wire.
     *
     * A transition check, not an every-tick restate: `SetStat` restarts the
     * pattern's phase clock, so re-setting the same pattern each tick would hold
     * the double-flash on its first step and never complete a burst.
     */
    bool         maintenance_led_state_ = false;
    // Set when an LED_STAT restate was deferred because the learn wizard owned
    // the LEDs. The pending restate is a FLAG and not a sentinel stored in
    // `maintenance_led_state_`, because the deferral has to survive the wizard
    // exiting into an already-open maintenance window (the 3 s AUX1 escalation
    // does exactly that in one tick) -- a sentinel cannot encode "restate owed"
    // for both desired values.
    bool         leds_owed_restat_ = false;
    // The idle reference captured when a learn started (spec 3.4: the idle AS
    // MEASURED AT LEARN TIME). Captured on entry, because during the prompt the
    // user is holding the wheel button and the live reading is the pressed level.
    int          learn_idle_mv_ = 0;
    // Which channel a wizard learn is filling, and the last profile it committed
    // -- kept so the link can report what the headless learn produced.
    int          learn_channel_ = 0;
    /*
     * FR-33's maintenance window, and the sustained-AUX1 hold that opens it.
     *
     * Spec 8.2 nests the two holdings deliberately: 1.5 s is PROGRAMMING and 3 s
     * is MAINTENANCE, the shorter a subset of the longer, so holding too long to
     * program escalates cleanly into maintenance rather than into an undefined
     * state. `maint_fired_latch_` is what makes the escalation one-way: the
     * programming hold has already toggled the wizard by 3 s, and the maintenance
     * entry must not be lost to it.
     */
    MaintenanceMode  maintenance_;
    bool             maint_fired_latch_ = false;
    int          learned_channel_ = -1;
    LadderProfile learned_profile_{};
    // Whether the last commit reached NVS (see LastLearnPersisted).
    bool         persisted_ = false;
    // The ADC fell back to the linear approximation (spec 3.2, blank eFuses).
    // Injected via `SetCalibrationDegraded`; read once by `Boot` to fold into the
    // boot pattern. See that setter for why it is not on `IHAL`.
    bool         calibration_degraded_ = false;
};

#ifdef __cplusplus
extern "C" {
#else
/* C sees only an opaque handle; the class definition above is C++-only. Guarded
 * so the C++ build does not redeclare the class it already defined. */
typedef struct SystemOrchestrator SystemOrchestrator;
#endif

// C-linkage entry points so `src/main.c` can drive the orchestrator without
// becoming a C++ translation unit. `main.c` is C because ESP-IDF's app_main is
// a C entry point and the IDF headers it needs are C.
//
// The orchestrator is heap-allocated rather than static because Config is large
// (it carries 32 bindings, spec 3.5) and a file-scope instance would sit in .bss
// for the whole run. One allocation at boot, freed never -- the device runs
// until it is reset, and a null return is reported rather than ignored.

// Loads the config and establishes the safe idle output (FR-13). Returns null if
// the allocation fails, which the caller must treat as a fatal boot fault: a
// device that cannot reach its safe idle must not pretend to be running.
//
// `calibration_degraded` is spec 3.2's blank-eFuse flag, which the caller reads
// from the HAL (`EspHalCalibrationIsDegraded`); it is an argument because this
// translation unit is HOST-compiled and cannot name EspHal. It is folded into the
// boot pattern, so a degraded device announces `BOOT_DEGRADED` rather than running
// silently.
SystemOrchestrator *SystemOrchestratorCreate(IHAL *hal, bool calibration_degraded);

// One poll tick. Safe to call on a null orchestrator (does nothing), so the
// caller's loop needs no null check of its own.
void SystemOrchestratorTick(SystemOrchestrator *sys, uint64_t now_ms);

// True once the safe idle state has been written (FR-13).
bool SystemOrchestratorSafeIdle(const SystemOrchestrator *sys);

// True when the safe idle was written AND no DAC write has failed since boot.
// This is FR-37's health condition: what `app_main` must check before marking an
// OTA image valid. `SystemOrchestratorSafeIdle` alone cannot falsify anything
// (see `OutputVerified`), so it is not a substitute.
bool SystemOrchestratorOutputVerified(const SystemOrchestrator *sys);

#ifdef __cplusplus
}
#endif

