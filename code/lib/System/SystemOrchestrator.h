#pragma once

#include <stdint.h>

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

private:
    struct ChannelState {
        // Both of these take their profile/timings at construction and have no
        // default constructor, so a default-constructed array needs seeding with
        // an empty profile. Boot reassigns them once the real config is known --
        // which is also after the safe idle state is established, the ordering
        // FR-13 requires. No heap: everything is a value member.
        ChannelState()
            : classifier(LadderProfile{}, GestureTimingsDefault()),
              gestures(GestureTimingsDefault()) {}

        PressClassifier     classifier;
        GestureStateMachine gestures;
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

