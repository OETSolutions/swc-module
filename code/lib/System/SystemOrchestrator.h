#pragma once

#include <stdint.h>

#include "Analog/AdcReader.h"
#include "Config/ConfigModel.h"
#include "Feedback/BuzzerGrammar.h"
#include "Feedback/LedGrammar.h"
#include "Gesture/GestureStateMachine.h"
#include "Gesture/PressClassifier.h"
#include "HAL/IHAL.h"
#include "Output/ServoLoop.h"

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
    };

    void EstablishSafeIdle();
    void ServiceChannel(uint8_t index, uint64_t now_ms);
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

