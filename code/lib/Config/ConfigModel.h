#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"
#include "Gesture/GestureStateMachine.h"
#include "Output/GainPolicy.h"

constexpr uint32_t kConfigSchemaVersion = 1;

constexpr int kMaxChannels          = 2;
constexpr int kMaxBindings          = 32;   // TOTAL, across all channels (spec 3.5)
constexpr int kMaxActionsPerBinding = 2;    // measured against the NVS budget, not chosen
constexpr int kMaxAuxButtons        = 3;
constexpr int kChannelNameLen       = 16;
constexpr int kDeviceIdLen          = 24;
constexpr int kBindingIdLen         = 16;   // slugs: "vol_up", "next"
constexpr int kActionTargetLen      = 40;   // holds com.oetsolutions.swc.ACTION_NAVIGATE (34)
constexpr int kDataPayloadLen       = 48;   // holds geo:40.7608,-111.8910?q=Home (28)

// The maintenance window's ceiling (FR-38). One hour is far past spec 8.2's
// 5-minute default and still bounds the window to something a user could plausibly
// want; a `uint32` maximum would be ~49.7 days, which is "never closes" in
// practice and is the state FR-38 exists to prevent.
constexpr uint32_t kMaintenanceTimeoutMaxMs = 3600000u;

// The key-hold ceiling (FR-15/FR-39). `send_duration_ms` is how long the KEY line
// is DRIVEN, so a `uint32` maximum (~49.7 days) is a phantom key press the user
// cannot end -- the exact hazard FR-39 exists to prevent. The same reasoning that
// bounds `maintenance_timeout_ms` bounds this: the check must be a RANGE, not just
// the magnitude check that refuses a value past the width. Ten seconds is far past
// any real head unit's press recognition (spec 3.7's default is 200 ms) and still
// something a user could mean; `test_key`'s own hold is bounded at 1 s for the
// identical reason.
constexpr uint32_t kSendDurationMaxMs = 10000u;

// These four widths are NOT free. They are a budget input, together with
// kMaxBindings and kMaxActionsPerBinding: the structural worst case -- every
// string field at its declared maximum -- is what ConfigMaxSerializedSize()
// must bound, and it has to fit two NVS slots. At 40/48/16/16 with 32 bindings
// x 2 actions the JSON is 22,407 B -> 11 chunks -> 46,496 B of the partition's
// 48,384 B (96 %). Widening any of them overflows: the 64/128/24 an earlier
// revision declared gives 30,021 B -> 15 chunks -> 63,392 B (131 %), which the
// device cannot store at all. If a field must grow, re-measure before widening
// -- and see spec 3.5's width table, which is the binding statement of this.

// An action is identified by its KIND and params. There is no action id (spec
// 3.6): an earlier revision invented ids 1-63 and a 3-value ActionKind, which
// could not express the 11 kinds and made the product's core case -- one button
// whose SINGLE drives the output while its DOUBLE sends an APP_INTENT -- a shape
// the type could not hold.
// A binding's input, spec 3.5: `SWC1 | SWC2 | AUX1 | AUX2 | AUX3 | ANY`.
// `ANY` is a real value, not a placeholder -- it is how one gesture is bound
// once and honoured from either steering-wheel channel.
enum class BindingChannel : uint8_t {
    kSwc1, kSwc2, kAux1, kAux2, kAux3, kAny,
};

// Declaration order is the Shared contract's append-only order (spec 3.6) and
// must match `kActionKindNames` in ConfigCodec.cpp -- one fact, two spellings.
enum class ActionKind : uint8_t {
    kNone, kOutVoltage, kOutRelease, kAppLaunch, kAppIntent,
    kKeycode, kMedia, kVolume, kSystem, kBuzzer, kAppRaw,
};

struct Action {
    ActionKind kind;
    char       target[kActionTargetLen];   // package / intent action / command / pattern
    char       payload[kDataPayloadLen];   // APP_INTENT's data URI (spec 3.5)
    // The KEY-line voltage for `kOutVoltage`, in millivolts (spec 3.6). It is a
    // VOLTAGE and not a `dac_code`, because a code is coupled to the gain mode:
    // changing `gain_mode` would silently change what every stored code means.
    // 0 means "absent" and is never a valid target -- the output floor is 1800 mV
    // (GainPolicy.h), so the whole range below it is unrepresentable anyway.
    uint16_t   key_mv;
};

// Whether the kind gives `payload` a meaning (spec 3.6's table is the authority).
// Declared here rather than in ConfigCodec.h because Task 11's BindingResolver and
// Task 8's ConfigValidate both need it, and it is a fact about the KIND -- not
// about a particular action. An earlier revision stored it as a per-action bool,
// which let a config claim a payload for a kind that has none.
inline bool ActionTakesPayload(ActionKind k) {
    return k == ActionKind::kAppLaunch || k == ActionKind::kAppIntent ||
           k == ActionKind::kKeycode   || k == ActionKind::kMedia     ||
           k == ActionKind::kVolume    || k == ActionKind::kSystem    ||
           k == ActionKind::kBuzzer    || k == ActionKind::kAppRaw;
}

struct Binding {
    char     id[kBindingIdLen];
    uint8_t  channel;                      // SWC1|SWC2|AUX1..3|ANY
    char     button[kBindingIdLen];        // LadderButton.id, or "NONE"
    Gesture  gesture;
    bool     enabled;
    uint8_t  action_count;                 // 0 is legal and MEANS "swallow the gesture"
    Action   actions[kMaxActionsPerBinding];   // ordered, executed best-effort
};

struct DeviceSettings {
    GestureTimings timings;
    GainPolicy     gain_policy;
    uint8_t        buzzer_level;      // 0..3
    uint8_t        led_level;         // 0..3
    bool           temp_comp_enabled;
    uint32_t       maintenance_timeout_ms;
};

struct OutputProfile {
    GainMode gain_mode;
    // The DAC code the KEY line is held at when idle (spec 3.7: 4095, and spec
    // 6.7 explains why full scale is the SAFE state -- the output only sinks,
    // so a high command releases the line). This is what Boot() writes in its
    // "establish safe idle" step, and what every orchestrator test compares
    // against after a press.
    uint16_t idle_dac_code;
};

struct AuxButtonConfig {
    char    id[kBindingIdLen];
    uint8_t source;                   // spec 3.1: a direct digital/analog input
    int16_t mv_center;
    int16_t mv_tolerance;
};

struct ChannelConfig {
    bool          enabled;
    char          name[kChannelNameLen];
    LadderProfile ladder;
    OutputProfile output;
};

struct Config {
    uint32_t        schema_version;
    char            device_id[kDeviceIdLen];
    uint64_t        updated_at_ms;
    DeviceSettings  settings;
    ChannelConfig   channels[kMaxChannels];
    uint8_t         channel_count;
    AuxButtonConfig aux[kMaxAuxButtons];    // AUX1-AUX3 (spec 3.1)
    uint8_t         aux_count;
    Binding         bindings[kMaxBindings]; // TOP-LEVEL join table (spec 3.1/3.5)
    uint8_t         binding_count;
};

// `sizeof(Config)` is a load-bearing number in this project, not an implementation
// detail. It is what makes the by-value shape a stack overflow -- holding one in a
// local, or returning one from a factory, is an 8 KB frame on a 3,584 B task -- and
// it is the arithmetic every stack-safety comment on those factories quotes. It is
// pinned HERE so the COMPILER keeps it true: `tools/check_stack_usage.py` reads this
// literal rather than carrying its own copy, so there is one home for the number and
// the gate cannot quote a value the model no longer has. A field added or widened
// trips this on purpose -- re-measure the stack budget (spec 6.x) and the NVS budget
// in ConfigCodec.h together with it, then update the number.
static_assert(sizeof(Config) == 8912,
              "sizeof(Config) changed: re-measure the stack and NVS budgets, then update this");
