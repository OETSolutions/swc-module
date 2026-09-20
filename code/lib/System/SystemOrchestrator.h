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

    // One poll tick. Reads the ADC channels, classifies, resolves any completed
    // gesture to an action, drives the output, and advances the feedback
    // grammars. Does no allocation and no I/O beyond the HAL calls.
    void Tick(uint64_t now_ms);

    // The KEY-line idle level, in millivolts, as measured for the selected gain
    // mode. This is the `V_KEY_idle` of spec 6.2 and the reference every
    // command is bounded against.
    int IdleKeyMv() const { return idle_key_mv_; }

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

    // Flash both LED channels and buzz once, so a user can tell WHICH unit they
    // are talking to (spec 4.3's `identify`). Non-blocking: it sets the patterns
    // and returns, and the normal Tick advances them.
    void Identify();

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

    /* The channel's LEARNED idle reference: the ratio denominator (spec 6.3). */
    int IdleReferenceMv(uint8_t channel_index) const
    {
        if (channel_index >= kMaxChannels) return 0;
        return static_cast<int>(config_.channels[channel_index].ladder.learned_idle_mv);
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
     */
    void ReportFault() {
        if (faulted_) return;
        faulted_ = true;
        leds_.SetStat(LedStatPattern::kBlink);
        buzzer_.Play(BuzzerPattern::kFaultDac);
    }

    bool Faulted() const { return faulted_; }

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
        GestureBindings     bindings;
        bool                key_driven = false;    // a pulse is currently on the line
        uint64_t            key_released_at_ms = 0;
        // FR-12: one report and one KEY_UNKNOWN beep per unrecognised press, not
        // one per poll tick. A held level is unrecognised on every tick, so
        // without this latch a held press would emit 100 events/s and play a
        // continuous beep. Cleared when the level returns to idle or a real
        // button, so the next unrecognised press reports again.
        bool                unknown_reported = false;
    };

    void EstablishSafeIdle();
    void ServiceChannel(uint8_t index, uint64_t now_ms);
    // FR-31: the headless learn wizard and the AUX1 hold that drives it.
    void ServiceLearn(uint64_t now_ms);
    void ApplyLearnedProfile(int channel, const LadderProfile &profile);
    // Spec 4.3's `event`. Called at the moment of recognition, from inside
    // ServiceChannel's resolution branch, because a gesture can complete on any
    // tick (a LONG fires when its threshold elapses, a SINGLE when its ambiguity
    // window closes).
    void ReportGesture(uint8_t index, const GestureEvent &ev, int level_mv);
    // What the channel's own bindings say about how long a press must stay
    // undecided. Derived from the config, not assumed -- a button that binds
    // only SINGLE must not wait out the double-press window (spec 6.6).
    GestureBindings BindingsFor(uint8_t channel_index) const;

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
    int            idle_key_mv_ = 0;
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
    // The WHEEL's live idle: the ratio numerator's denominator, and the only
    // reference available with no config to pin one to.
    int  pass_through_idle_mv_ = 0;
    // The HEAD UNIT's measured idle (spec 6.2 step 1), which the pass-through
    // ratio is applied TO. Distinct from `pass_through_idle_mv_`: those are two
    // different ladders, and conflating them maps every press to the wrong key.
    int  head_unit_idle_mv_ = 0;

    // Where a recognized gesture is reported (spec 4.3's `event`). Null until a
    // link registers, and legal to leave null: the device runs without an app.
    GestureSink gesture_sink_ = nullptr;
    void       *gesture_sink_ctx_ = nullptr;
    bool        usb_connected_ = false;
    bool        faulted_ = false;
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
SystemOrchestrator *SystemOrchestratorCreate(IHAL *hal);

// One poll tick. Safe to call on a null orchestrator (does nothing), so the
// caller's loop needs no null check of its own.
void SystemOrchestratorTick(SystemOrchestrator *sys, uint64_t now_ms);

// True once the safe idle state has been written (FR-13).
bool SystemOrchestratorSafeIdle(const SystemOrchestrator *sys);

#ifdef __cplusplus
}
#endif

