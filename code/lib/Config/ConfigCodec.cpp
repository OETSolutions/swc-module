#include "Config/ConfigCodec.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

// Declared in ConfigCodec.h so Task 9 and Task 15 share this one implementation.
// Defined OUTSIDE the anonymous namespace for the same reason ActionIsWellFormed
// is: a file-local definition links only here, which is how a shared checksum
// quietly becomes two checksums.
uint32_t Crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

namespace {

// Blob header: magic, schema, payload length, CRC over the payload.
struct BlobHeader {
    uint32_t magic;
    uint32_t schema_version;
    uint32_t payload_len;
    uint32_t payload_crc;
};
constexpr uint32_t kBlobMagic = 0x53435743u;  // "SWCC"
static_assert(sizeof(BlobHeader) == kBlobHeaderBytes,
              "ConfigCodec.h's kBlobHeaderBytes must match the real header; Task 9 sizes its "
              "slot buffer from it, so a divergence under-allocates silently");

// The window-distinguishability relation lives in LadderDecode (one home, because
// LearnSession must refuse to commit a profile this validator would reject --
// `ConfigStore::Save` writes whatever it is handed). It is called below via
// LadderWindowsAreDistinguishable; the local copy that used to sit here was the
// second home, and it had already drifted from learn's tolerance floor.
// The decode scratch. `ConfigDecodeJson` decodes into this and copies to the
// caller's `out` only once every field AND the validator have passed, which is
// what keeps spec 3.8's "never partially applied" true.
//
// **Why a static rather than a local.** `sizeof(Config)` is 8,912 B, so a local
// made this function's frame 8,944 B -- and its callers frame 9-18 KB, on a
// 3,584-byte main task and a 4,096-byte TinyUSB task. That is a stack overflow on
// every boot and every `config_get`, invisible to the host suite (the crash needs
// a real task stack) and to the board, which has never been flashed.
//
// One scratch is safe because the decode is not reentrant: it is reached from the
// USB task (`config_end`, `config_get`) and from `Boot` on the main task, never
// from both at once, and never nested within itself. Same reasoning as
// ConfigStore's `g_blob`, which is the precedent for a file-local config buffer.
Config g_decode_scratch{};

bool BindingNamesARealInput(const Config &c, const Binding &b) {
    if (strcmp(b.button, "NONE") == 0) return true;   // gestures on the prog button
    for (uint8_t ch = 0; ch < c.channel_count; ++ch) {
        const LadderProfile &p = c.channels[ch].ladder;
        for (uint8_t i = 0; i < p.count; ++i) {
            if (strcmp(p.buttons[i].id, b.button) == 0) return true;
        }
    }
    for (uint8_t i = 0; i < c.aux_count; ++i) {
        if (strcmp(c.aux[i].id, b.button) == 0) return true;
    }
    return false;
}

}  // namespace

// Declared in ConfigCodec.h, so it is defined OUTSIDE the anonymous namespace --
// Task 11 calls it. A file-local definition would link only for this file, which
// is how the earlier revision's cross-check silently became a re-derivation.
bool ActionIsWellFormed(const Action &a) {
    if (a.kind > ActionKind::kAppRaw) return false;   // the enum is contiguous
    // Every kind needs its non-payload parameter, because that parameter is what
    // the action DOES. An empty one is an action with no effect, which would be
    // stored and reported as a binding that fires.
    //
    // `ActionTakesPayload` is the authority for WHICH kinds those are, and it is
    // called rather than restated. An earlier revision of this function spelled
    // the same eight enumerators out a second time -- two homes for one fact,
    // and they drift the first time a kind is added.
    if (ActionTakesPayload(a.kind) && a.target[0] == '\0') return false;
    // OUT_VOLTAGE carries the KEY-line level, and it is a VOLTAGE rather than a
    // `dac_code` (spec 3.6). A code would be coupled to `gain_mode`, so changing
    // the mode would silently change what every stored code means -- the same
    // "two homes for one level" defect that removed `output.key_values`. Zero
    // means "no level", which would drive the output to a voltage nothing
    // defined; the app is what converts a head unit's resistance to this voltage.
    if (a.kind == ActionKind::kOutVoltage && a.key_mv == 0) return false;
    return true;
}

bool ConfigValidate(const Config &c) {
    if (c.schema_version != kConfigSchemaVersion) return false;
    // An empty device id is refused because `ReadStr` refuses one, which would
    // otherwise make this config encode successfully and then fail to DECODE --
    // a round-trip violation (FR-27) surfacing as a mysterious "corrupt config".
    // Validation is where that must be caught, since validation is the only gate
    // between a hand-built config and the wire.
    if (c.device_id[0] == '\0') return false;
    if (c.channel_count == 0 || c.channel_count > kMaxChannels) return false;
    if (c.settings.timings.debounce_ms == 0) return false;
    if (c.settings.timings.double_press_off_ms < c.settings.timings.debounce_ms) return false;
    // A long-press threshold at or below the double-press window is incoherent:
    // the gesture could be both a LONG and a DOUBLE.
    if (c.settings.timings.long_press_ms <= c.settings.timings.double_press_off_ms) return false;
    if (c.settings.timings.send_duration_ms == 0) return false;
    // Bounded at BOTH ends, like `maintenance_timeout_ms` below and for the same
    // reason: this is how long the KEY line is DRIVEN, so a `uint32` maximum
    // (~49.7 days) pins a phantom press the user cannot release -- the hazard
    // FR-39 exists to prevent. A magnitude check alone refuses only a value past
    // the width (which a raw cast would have wrapped anyway); the in-range maximum
    // is what a patch of `1e10`-wrapped-to-u32-max sailed past.
    if (c.settings.timings.send_duration_ms > kSendDurationMaxMs) return false;
    if (c.settings.buzzer_level > 3 || c.settings.led_level > 3) return false;
    // The maintenance window is BOUNDED, like every other timing above. A zero
    // makes the close test (`now - last_activity >= timeout`) true on the tick
    // the window opens, so maintenance would appear to work and instantly close
    // -- FR-38's feature silently doing nothing. A `uint32` maximum (~49.7 days)
    // is the opposite failure: the window never closes on its own, which is
    // exactly the "device left unable to serve presses" state FR-38 exists to
    // prevent. Neither is a value the user can mean, so it is refused rather than
    // clamped -- the same answer `debounce_ms` and `send_duration_ms` give.
    if (c.settings.maintenance_timeout_ms == 0) return false;
    if (c.settings.maintenance_timeout_ms > kMaintenanceTimeoutMaxMs) return false;

    for (uint8_t ch = 0; ch < c.channel_count; ++ch) {
        const ChannelConfig &cc = c.channels[ch];
        if (cc.name[0] == '\0') return false;
        // The LADDER's own checks live in `LadderProfileIsValid`, one home, because
        // `LearnSession::Commit` must refuse to produce a profile this function
        // would reject -- `ConfigStore::Save` does not validate, so a commit that
        // skipped this would persist a config the next boot cannot load.
        //
        // The `id` is checked HERE rather than in the shared predicate: it is the
        // caller's field, learn cannot invent it, and a profile that is not yet
        // named is a legitimate intermediate for a learn.
        for (uint8_t i = 0; i < cc.ladder.count && i < kLadderMaxButtons; ++i) {
            if (cc.ladder.buttons[i].id[0] == '\0') return false;
        }
        if (!LadderProfileIsValid(cc.ladder)) return false;
    }

    // Bindings are a top-level table (spec 3.1/3.5), so their checks are too.
    if (c.binding_count > kMaxBindings) return false;
    // The AUX table is read the same way as every other counted array, and its
    // count was the one left unbounded. `BindingNamesARealInput` loops
    // `i < c.aux_count` over `c.aux[i]`, so a count past the 3-entry array reads
    // off the end -- in this function, which exists to reject exactly that.
    if (c.aux_count > kMaxAuxButtons) return false;
    for (uint8_t i = 0; i < c.binding_count; ++i) {
        const Binding &b = c.bindings[i];
        if (b.action_count > kMaxActionsPerBinding) return false;
        if (b.channel >= static_cast<uint8_t>(BindingChannel::kAny) + 1) return false;
        // **AUX1 carries two holds and cannot also be a gesture source.** Spec
        // 7.5 gives AUX1 the 1.5 s programming hold and spec 8.2 the 3 s
        // maintenance hold; a binding on the same switch would be ambiguous with
        // those, so it is REFUSED here rather than accepted and then never fired
        // (which is what the whole AUX family used to do, open item N-26). AUX2
        // and AUX3 have no other role and ARE bindable.
        if (b.channel == static_cast<uint8_t>(BindingChannel::kAux1)) return false;
        // `button` is a LadderButton.id, "NONE", or -- for the AUX inputs -- an
        // AUX id. A binding that names neither is a binding to nothing, which
        // would silently never fire; refuse it instead.
        if (!BindingNamesARealInput(c, b)) return false;
        // An empty action list is legal (spec 3.5: it swallows the gesture), so
        // action_count == 0 is NOT an error. What is an error is an action that
        // is neither a known kind nor carries the field its kind requires.
        for (uint8_t a = 0; a < b.action_count; ++a) {
            if (!ActionIsWellFormed(b.actions[a])) return false;
        }
    }
    return true;
}

namespace {

// The wire spelling of every enum in the model. These strings ARE the contract:
// spec 3.7's worked example is written in them, the Android app parses them, and
// spec 4.2's frames carry them. The action kind crosses the wire as a NAME, not
// an ordinal (spec 3.6) -- a numeric id table is the invented thing the spec
// explicitly does not define.
struct EnumName { int value; const char *name; };

constexpr EnumName kChannelNames[] = {
    {static_cast<int>(BindingChannel::kSwc1), "SWC1"},
    {static_cast<int>(BindingChannel::kSwc2), "SWC2"},
    {static_cast<int>(BindingChannel::kAux1), "AUX1"},
    {static_cast<int>(BindingChannel::kAux2), "AUX2"},
    {static_cast<int>(BindingChannel::kAux3), "AUX3"},
    {static_cast<int>(BindingChannel::kAny),  "ANY"},
};

constexpr EnumName kGestureNames[] = {
    {static_cast<int>(Gesture::kNone),   "NONE"},
    {static_cast<int>(Gesture::kSingle), "SINGLE"},
    {static_cast<int>(Gesture::kDouble), "DOUBLE"},
    {static_cast<int>(Gesture::kLong),   "LONG"},
};

// Order is spec 3.6's table order and matches ActionKind's declaration, which the
// Shared contract pins as append-only. These two lists are one fact with two
// spellings; a reorder in either is a defect, not a refactor.
constexpr EnumName kActionKindNames[] = {
    {static_cast<int>(ActionKind::kNone),         "NONE"},
    {static_cast<int>(ActionKind::kOutVoltage),   "OUT_VOLTAGE"},
    {static_cast<int>(ActionKind::kOutRelease),   "OUT_RELEASE"},
    {static_cast<int>(ActionKind::kAppLaunch),    "APP_LAUNCH"},
    {static_cast<int>(ActionKind::kAppIntent),    "APP_INTENT"},
    {static_cast<int>(ActionKind::kKeycode),      "KEYCODE"},
    {static_cast<int>(ActionKind::kMedia),        "MEDIA"},
    {static_cast<int>(ActionKind::kVolume),       "VOLUME"},
    {static_cast<int>(ActionKind::kSystem),       "SYSTEM"},
    {static_cast<int>(ActionKind::kBuzzer),       "BUZZ"},
    {static_cast<int>(ActionKind::kAppRaw),       "APP_RAW"},
};

constexpr EnumName kGainModeNames[] = {
    {static_cast<int>(GainMode::kTracking),   "TRACKING"},
    {static_cast<int>(GainMode::kAmplified),  "AMPLIFIED"},
    {static_cast<int>(GainMode::kAuto),       "AUTO"},
};

constexpr EnumName kGainPolicyNames[] = {
    {static_cast<int>(GainPolicy::kAuto),           "AUTO"},
    {static_cast<int>(GainPolicy::kForceTracking),  "TRACKING"},
    {static_cast<int>(GainPolicy::kForceAmplified), "AMPLIFIED"},
};

const char *NameOf(const EnumName *table, size_t count, int value, const char *fallback) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i].value == value) return table[i].name;
    }
    return fallback;
}

// Returns -1 when the string names no enumerator. The caller decides whether
// that is fatal -- ConfigDecodeJson treats it as such.
int ValueOf(const EnumName *table, size_t count, const char *name) {
    if (name == nullptr) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(table[i].name, name) == 0) return table[i].value;
    }
    return -1;
}

#define NAME_OF(t, v, fb) NameOf(t, sizeof(t) / sizeof(t[0]), static_cast<int>(v), fb)
#define VALUE_OF(t, s)    ValueOf(t, sizeof(t) / sizeof(t[0]), s)

// cJSON returns NULL for an object member that is absent AND for one that is
// present with a JSON null. For a config every required field is required, so
// the two cases are the same failure and this collapses them deliberately.
const cJSON *Member(const cJSON *obj, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

bool ReadU32(const cJSON *obj, const char *key, uint32_t *out) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) return false;
    // Bounded before the cast. A field declared uint32_t that arrives as a
    // larger number would otherwise wrap silently -- which is how
    // updated_at_ms's 1700000000000 became 3487969280 before this check.
    if (v->valuedouble > 4294967295.0) return false;
    // An integer field carrying a fraction is refused, not truncated. Truncating
    // 750.9 to 750 is a config the device accepts and then behaves differently
    // from what was sent -- the same class of wrong-value-accepted as the wrap
    // above, one size smaller. `config_patch` refuses a fraction for these same
    // fields, so truncating here would be two answers to one question.
    if (v->valuedouble != static_cast<double>(static_cast<uint32_t>(v->valuedouble))) return false;
    *out = static_cast<uint32_t>(v->valuedouble);
    return true;
}

// updated_at_ms is the one uint64_t in the model, and it needs its own reader:
// a millisecond epoch stamp is ~1.7e12, far past uint32_t, so reading it with
// ReadU32 truncates it to a wrong-but-plausible number rather than failing.
bool ReadU64(const cJSON *obj, const char *key, uint64_t *out) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) return false;
    // 2^53 is where a double stops representing consecutive integers, so above
    // it the value that arrived is not the value that was sent.
    if (v->valuedouble > 9007199254740992.0) return false;
    // Same fraction rule as ReadU32: a timestamp is a whole number of ms, and
    // truncating one would store a stamp the sender did not write.
    if (v->valuedouble != static_cast<double>(static_cast<uint64_t>(v->valuedouble))) return false;
    *out = static_cast<uint64_t>(v->valuedouble);
    return true;
}

bool ReadU16(const cJSON *obj, const char *key, uint16_t *out) {
    uint32_t v = 0;
    if (!ReadU32(obj, key, &v) || v > 0xFFFFu) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

bool ReadU8(const cJSON *obj, const char *key, uint8_t *out) {
    uint32_t v = 0;
    if (!ReadU32(obj, key, &v) || v > 0xFFu) return false;
    *out = static_cast<uint8_t>(v);
    return true;
}

bool ReadBool(const cJSON *obj, const char *key, bool *out) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v) != 0;
    return true;
}

// A string field is copied with strncpy into a fixed buffer, so a value at or
// over the width would be silently truncated -- which is a config the device
// would accept and then behave differently from what was sent. Spec 3.5's width
// table says a longer value "must be refused by validation, not truncated", so
// the length is checked before the copy and truncation is unreachable.
bool ReadStr(const cJSON *obj, const char *key, char *out, size_t width) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == nullptr) return false;
    const size_t n = strlen(v->valuestring);
    if (n == 0 || n >= width) return false;
    memcpy(out, v->valuestring, n + 1);
    return true;
}

void AddU32(cJSON *obj, const char *key, uint32_t v) {
    cJSON_AddNumberToObject(obj, key, static_cast<double>(v));
}

// spec 3.7 writes temp_c_at_learn as a decimal (23.5) while the struct stores
// tenths of a degree as an int16_t (235). One representation has to give, and it
// is the wire one: the app displays a temperature, and 23.5 is what it should
// show. The tenths field is what makes the round trip exact -- a float on the
// wire would not survive re-encoding byte-identically, which FR-27 requires.
void AddTenths(cJSON *obj, const char *key, int16_t tenths) {
    cJSON_AddNumberToObject(obj, key, static_cast<double>(tenths) / 10.0);
}

bool ReadTenths(const cJSON *obj, const char *key, int16_t *out) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsNumber(v)) return false;
    const double tenths = v->valuedouble * 10.0;
    // The bounds are the int16_t range, checked before the cast so a config
    // claiming 4000 C is refused rather than wrapping to a negative temperature.
    if (tenths < -32768.0 || tenths > 32767.0) return false;
    *out = static_cast<int16_t>(tenths);
    return true;
}

// confidence is stored 0-100 in the struct and written as 0.0-1.0 on the wire
// (spec 3.7: 0.98). Same reasoning as the temperature: the wire form is the one
// a human reads, and the integer is what makes it exact.
void AddConfidence(cJSON *obj, uint8_t confidence) {
    cJSON_AddNumberToObject(obj, "confidence", static_cast<double>(confidence) / 100.0);
}

bool ReadConfidence(const cJSON *obj, uint8_t *out) {
    const cJSON *v = Member(obj, "confidence");
    if (!cJSON_IsNumber(v)) return false;
    const double pct = v->valuedouble * 100.0;
    if (pct < 0.0 || pct > 100.0) return false;
    *out = static_cast<uint8_t>(pct + 0.5);
    return true;
}

// The wire key each kind uses for the struct's two generic string fields (spec
// 3.6). `target` is the "what to do" string and `payload` the data URI; the kind
// decides what to call them. Returning nullptr means the kind has no such
// parameter, and the field is then neither written nor required.
//
// One table rather than two switch statements, because the encoder and the decoder
// must agree exactly -- a kind renamed in one and not the other would encode a
// config the decoder cannot read back, which is precisely the round-trip failure
// FR-27 exists to prevent.
struct ParamKeys { ActionKind kind; const char *target; const char *payload; };

constexpr ParamKeys kParamKeys[] = {
    {ActionKind::kNone,         nullptr,        nullptr},
    {ActionKind::kOutVoltage,   nullptr,        nullptr},   // uses `key_mv`
    {ActionKind::kOutRelease,   nullptr,        nullptr},
    {ActionKind::kAppLaunch,    "package",      nullptr},
    {ActionKind::kAppIntent,    "action",       "data"},
    {ActionKind::kKeycode,      "keycode",      nullptr},
    {ActionKind::kMedia,        "command",      nullptr},
    {ActionKind::kVolume,       "target",       nullptr},
    {ActionKind::kSystem,       "command",      nullptr},
    {ActionKind::kBuzzer,       "pattern",      nullptr},
    {ActionKind::kAppRaw,       "command",      nullptr},
};

const ParamKeys *KeysFor(ActionKind kind) {
    for (size_t i = 0; i < sizeof(kParamKeys) / sizeof(kParamKeys[0]); ++i) {
        if (kParamKeys[i].kind == kind) return &kParamKeys[i];
    }
    return nullptr;
}

const char *TargetKeyFor(ActionKind kind) {
    const ParamKeys *k = KeysFor(kind);
    return k == nullptr ? nullptr : k->target;
}

const char *PayloadKeyFor(ActionKind kind) {
    const ParamKeys *k = KeysFor(kind);
    return k == nullptr ? nullptr : k->payload;
}

// The inverse: which struct field a wire key fills. Returns 0 for no match, 1 for
// `target`, 2 for `payload` -- an enum would be tidier but this keeps the lookup
// in one place with the table above.
int FieldForWireKey(ActionKind kind, const char *key) {
    const ParamKeys *k = KeysFor(kind);
    if (k == nullptr) return 0;
    if (k->target != nullptr && strcmp(k->target, key) == 0) return 1;
    if (k->payload != nullptr && strcmp(k->payload, key) == 0) return 2;
    return 0;
}

cJSON *EncodeActions(const Binding &b) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) return nullptr;
    for (uint8_t a = 0; a < b.action_count; ++a) {
        const Action &act = b.actions[a];
        cJSON *o = cJSON_CreateObject();
        if (o == nullptr) { cJSON_Delete(arr); return nullptr; }
        cJSON_AddStringToObject(o, "kind",
                                NAME_OF(kActionKindNames, act.kind, "NONE"));
        // Each kind NAMES its own string slots on the wire (spec 3.6): APP_INTENT
        // carries `action` and `data`, APP_LAUNCH `package`, KEYCODE `keycode`,
        // MEDIA/SYSTEM/APP_RAW `command`, VOLUME `target`, BUZZ `pattern`. The
        // struct keeps the two generic fields because the executor only needs to
        // know "the string and the payload"; the wire keeps the kind's own names
        // because spec 3.7 is written in them and the Android app parses them.
        //
        // A field the kind does not use is OMITTED rather than written empty, so
        // a NONE action is exactly `{"kind":"NONE"}` and the wire form never
        // claims a parameter its kind does not have.
        const char *target_key = TargetKeyFor(act.kind);
        const char *payload_key = PayloadKeyFor(act.kind);
        if (target_key != nullptr && act.target[0] != '\0') {
            cJSON_AddStringToObject(o, target_key, act.target);
        }
        if (payload_key != nullptr && act.payload[0] != '\0') {
            cJSON_AddStringToObject(o, payload_key, act.payload);
        }
        if (act.key_mv != 0) AddU32(o, "key_mv", act.key_mv);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *EncodeBindings(const Config &c) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) return nullptr;
    for (uint8_t i = 0; i < c.binding_count; ++i) {
        const Binding &b = c.bindings[i];
        cJSON *o = cJSON_CreateObject();
        if (o == nullptr) { cJSON_Delete(arr); return nullptr; }
        cJSON_AddStringToObject(o, "id", b.id);
        cJSON_AddStringToObject(o, "channel",
                                NAME_OF(kChannelNames, b.channel, "SWC1"));
        cJSON_AddStringToObject(o, "button", b.button);
        cJSON_AddStringToObject(o, "gesture",
                                NAME_OF(kGestureNames, b.gesture, "NONE"));
        cJSON_AddBoolToObject(o, "enabled", b.enabled);
        cJSON *actions = EncodeActions(b);
        if (actions == nullptr) { cJSON_Delete(o); cJSON_Delete(arr); return nullptr; }
        // Added even when empty. An empty actions list is a distinct, legal
        // state -- "swallow this gesture" (spec 3.5) -- and omitting the key
        // would make it indistinguishable from a decoder's missing-field error.
        cJSON_AddItemToObject(o, "actions", actions);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *EncodeChannels(const Config &c) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) return nullptr;
    for (uint8_t i = 0; i < c.channel_count; ++i) {
        const ChannelConfig &cc = c.channels[i];
        cJSON *o = cJSON_CreateObject();
        if (o == nullptr) { cJSON_Delete(arr); return nullptr; }
        cJSON_AddStringToObject(o, "name", cc.name);
        cJSON_AddBoolToObject(o, "enabled", cc.enabled);

        cJSON *ladder = cJSON_CreateObject();
        if (ladder == nullptr) { cJSON_Delete(o); cJSON_Delete(arr); return nullptr; }
        AddU32(ladder, "source", cc.ladder.source);
        // The struct field is `learned_idle_mv` and the wire key is `idle_mv`
        // (spec 3.7). The struct keeps the longer name because the difference
        // between the LEARNED idle and the CURRENT one is what FR-30's rail
        // health check is made of; the wire keeps the short one because the
        // example the app and the web page are written against uses it.
        AddU32(ladder, "idle_mv", cc.ladder.learned_idle_mv);
        cJSON *buttons = cJSON_CreateArray();
        if (buttons == nullptr) { cJSON_Delete(ladder); cJSON_Delete(o); cJSON_Delete(arr); return nullptr; }
        for (uint8_t k = 0; k < cc.ladder.count; ++k) {
            const LadderButton &btn = cc.ladder.buttons[k];
            cJSON *bo = cJSON_CreateObject();
            if (bo == nullptr) { cJSON_Delete(buttons); cJSON_Delete(ladder); cJSON_Delete(o); cJSON_Delete(arr); return nullptr; }
            cJSON_AddStringToObject(bo, "id", btn.id);
            cJSON_AddStringToObject(bo, "name", btn.name);
            AddU32(bo, "mv_center", btn.mv_center);
            AddU32(bo, "mv_tolerance", btn.mv_tolerance);
            AddU32(bo, "learned_at_rail_mv", btn.learned_at_rail_mv);
            AddTenths(bo, "temp_c_at_learn", btn.temp_c_at_learn);
            AddU32(bo, "sample_count", btn.sample_count);
            AddConfidence(bo, btn.confidence);
            cJSON_AddItemToArray(buttons, bo);
        }
        cJSON_AddItemToObject(ladder, "buttons", buttons);
        cJSON_AddItemToObject(o, "ladder", ladder);

        cJSON *output = cJSON_CreateObject();
        if (output == nullptr) { cJSON_Delete(o); cJSON_Delete(arr); return nullptr; }
        cJSON_AddStringToObject(output, "gain_mode",
                                NAME_OF(kGainModeNames, cc.output.gain_mode, "TRACKING"));
        AddU32(output, "idle_dac_code", cc.output.idle_dac_code);
        cJSON_AddItemToObject(o, "output", output);

        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *EncodeAux(const Config &c) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) return nullptr;
    for (uint8_t i = 0; i < c.aux_count; ++i) {
        const AuxButtonConfig &a = c.aux[i];
        cJSON *o = cJSON_CreateObject();
        if (o == nullptr) { cJSON_Delete(arr); return nullptr; }
        cJSON_AddStringToObject(o, "id", a.id);
        AddU32(o, "source", a.source);
        // AuxButtonConfig holds these as int16_t while LadderButton holds its
        // equivalents as MilliVolt (uint16_t). Written the same way on the wire
        // so the app does not need two readers for one concept.
        cJSON_AddNumberToObject(o, "mv_center", a.mv_center);
        cJSON_AddNumberToObject(o, "mv_tolerance", a.mv_tolerance);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *EncodeSettings(const DeviceSettings &s) {
    cJSON *o = cJSON_CreateObject();
    if (o == nullptr) return nullptr;
    AddU32(o, "debounce_ms", s.timings.debounce_ms);
    AddU32(o, "double_press_off_ms", s.timings.double_press_off_ms);
    AddU32(o, "long_press_ms", s.timings.long_press_ms);
    AddU32(o, "send_duration_ms", s.timings.send_duration_ms);
    cJSON_AddStringToObject(o, "gain_policy",
                            NAME_OF(kGainPolicyNames, s.gain_policy, "AUTO"));
    AddU32(o, "buzzer_level", s.buzzer_level);
    AddU32(o, "led_level", s.led_level);
    cJSON_AddBoolToObject(o, "temp_comp_enabled", s.temp_comp_enabled);
    AddU32(o, "maintenance_timeout_ms", s.maintenance_timeout_ms);
    cJSON_AddBoolToObject(o, "maintenance_on_boot", s.maintenance_on_boot);
    return o;
}

bool DecodeActions(const cJSON *arr, Binding *b) {
    if (!cJSON_IsArray(arr)) return false;
    const int n = cJSON_GetArraySize(arr);
    if (n < 0 || n > kMaxActionsPerBinding) return false;
    b->action_count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) return false;
        Action &act = b->actions[i];
        const cJSON *kind = Member(o, "kind");
        if (!cJSON_IsString(kind)) return false;
        const int k = VALUE_OF(kActionKindNames, kind->valuestring);
        if (k < 0) return false;
        act.kind = static_cast<ActionKind>(k);
        // Absent means empty, not missing: EncodeActions omits a field the kind
        // does not use, so a NONE action is `{"kind":"NONE"}` with no target.
        act.target[0] = '\0';
        act.payload[0] = '\0';
        act.key_mv = 0;
        // The kind's own parameter names (spec 3.6), read through the same table
        // the encoder writes from. An unknown key is ignored rather than
        // rejected -- forward compatibility for a v2 field is the schema
        // version's job, not the key loop's.
        for (const cJSON *f = o->child; f != nullptr; f = f->next) {
            if (f->string == nullptr) continue;
            switch (FieldForWireKey(act.kind, f->string)) {
                case 1:
                    if (!ReadStr(o, f->string, act.target, sizeof(act.target))) return false;
                    break;
                case 2:
                    if (!ReadStr(o, f->string, act.payload, sizeof(act.payload))) return false;
                    break;
                default:
                    break;
            }
        }
        if (Member(o, "key_mv") != nullptr &&
            !ReadU16(o, "key_mv", &act.key_mv)) return false;
    }
    return true;
}

bool DecodeBindings(const cJSON *arr, Config *c) {
    if (!cJSON_IsArray(arr)) return false;
    const int n = cJSON_GetArraySize(arr);
    if (n < 0 || n > kMaxBindings) return false;
    c->binding_count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) return false;
        Binding &b = c->bindings[i];
        if (!ReadStr(o, "id", b.id, sizeof(b.id))) return false;
        const cJSON *ch = Member(o, "channel");
        if (!cJSON_IsString(ch)) return false;
        const int chv = VALUE_OF(kChannelNames, ch->valuestring);
        if (chv < 0) return false;
        b.channel = static_cast<uint8_t>(chv);
        if (!ReadStr(o, "button", b.button, sizeof(b.button))) return false;
        const cJSON *g = Member(o, "gesture");
        if (!cJSON_IsString(g)) return false;
        const int gv = VALUE_OF(kGestureNames, g->valuestring);
        if (gv < 0) return false;
        b.gesture = static_cast<Gesture>(gv);
        if (!ReadBool(o, "enabled", &b.enabled)) return false;
        if (!DecodeActions(Member(o, "actions"), &b)) return false;
    }
    return true;
}

bool DecodeChannels(const cJSON *arr, Config *c) {
    if (!cJSON_IsArray(arr)) return false;
    const int n = cJSON_GetArraySize(arr);
    if (n < 1 || n > kMaxChannels) return false;
    c->channel_count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) return false;
        ChannelConfig &cc = c->channels[i];
        if (!ReadStr(o, "name", cc.name, sizeof(cc.name))) return false;
        if (!ReadBool(o, "enabled", &cc.enabled)) return false;

        const cJSON *ladder = Member(o, "ladder");
        if (!cJSON_IsObject(ladder)) return false;
        if (!ReadU8(ladder, "source", &cc.ladder.source)) return false;
        uint32_t idle_mv = 0;
        if (!ReadU32(ladder, "idle_mv", &idle_mv) || idle_mv > 0xFFFFu) return false;
        cc.ladder.learned_idle_mv = static_cast<MilliVolt>(idle_mv);
        const cJSON *buttons = Member(ladder, "buttons");
        if (!cJSON_IsArray(buttons)) return false;
        const int bn = cJSON_GetArraySize(buttons);
        if (bn < 0 || bn > kLadderMaxButtons) return false;
        cc.ladder.count = static_cast<uint8_t>(bn);
        for (int k = 0; k < bn; ++k) {
            const cJSON *bo = cJSON_GetArrayItem(buttons, k);
            if (!cJSON_IsObject(bo)) return false;
            LadderButton &btn = cc.ladder.buttons[k];
            if (!ReadStr(bo, "id", btn.id, sizeof(btn.id))) return false;
            if (!ReadStr(bo, "name", btn.name, sizeof(btn.name))) return false;
            if (!ReadU16(bo, "mv_center", &btn.mv_center)) return false;
            if (!ReadU16(bo, "mv_tolerance", &btn.mv_tolerance)) return false;
            if (!ReadU16(bo, "learned_at_rail_mv", &btn.learned_at_rail_mv)) return false;
            if (!ReadTenths(bo, "temp_c_at_learn", &btn.temp_c_at_learn)) return false;
            if (!ReadU16(bo, "sample_count", &btn.sample_count)) return false;
            if (!ReadConfidence(bo, &btn.confidence)) return false;
        }

        const cJSON *output = Member(o, "output");
        if (!cJSON_IsObject(output)) return false;
        const cJSON *gm = Member(output, "gain_mode");
        if (!cJSON_IsString(gm)) return false;
        const int gmv = VALUE_OF(kGainModeNames, gm->valuestring);
        if (gmv < 0) return false;
        cc.output.gain_mode = static_cast<GainMode>(gmv);
        if (!ReadU16(output, "idle_dac_code", &cc.output.idle_dac_code)) return false;
    }
    return true;
}

bool DecodeAux(const cJSON *arr, Config *c) {
    if (arr == nullptr) { c->aux_count = 0; return true; }   // optional
    if (!cJSON_IsArray(arr)) return false;
    const int n = cJSON_GetArraySize(arr);
    if (n < 0 || n > kMaxAuxButtons) return false;
    c->aux_count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) return false;
        AuxButtonConfig &a = c->aux[i];
        if (!ReadStr(o, "id", a.id, sizeof(a.id))) return false;
        if (!ReadU8(o, "source", &a.source)) return false;
        uint32_t mv = 0;
        if (!ReadU32(o, "mv_center", &mv) || mv > 0x7FFFu) return false;
        a.mv_center = static_cast<int16_t>(mv);
        if (!ReadU32(o, "mv_tolerance", &mv) || mv > 0x7FFFu) return false;
        a.mv_tolerance = static_cast<int16_t>(mv);
    }
    return true;
}

bool DecodeSettings(const cJSON *o, DeviceSettings *s) {
    if (!cJSON_IsObject(o)) return false;
    if (!ReadU32(o, "debounce_ms", &s->timings.debounce_ms)) return false;
    if (!ReadU32(o, "double_press_off_ms", &s->timings.double_press_off_ms)) return false;
    if (!ReadU32(o, "long_press_ms", &s->timings.long_press_ms)) return false;
    if (!ReadU32(o, "send_duration_ms", &s->timings.send_duration_ms)) return false;
    const cJSON *gp = Member(o, "gain_policy");
    if (!cJSON_IsString(gp)) return false;
    const int gpv = VALUE_OF(kGainPolicyNames, gp->valuestring);
    if (gpv < 0) return false;
    s->gain_policy = static_cast<GainPolicy>(gpv);
    if (!ReadU8(o, "buzzer_level", &s->buzzer_level)) return false;
    if (!ReadU8(o, "led_level", &s->led_level)) return false;
    if (!ReadBool(o, "temp_comp_enabled", &s->temp_comp_enabled)) return false;
    if (!ReadU32(o, "maintenance_timeout_ms", &s->maintenance_timeout_ms)) return false;
    // OPTIONAL, unlike every field above: a config written before FR-33's
    // next-boot trigger existed has no `maintenance_on_boot`, and the on-disk
    // format is JSON with no schema bump to lean on. An ABSENT field means
    // "false" (the default), which is also what a config that never set it means
    // -- so refusing a fieldless config would strand every device configured by
    // an older firmware, and defaulting a PRESENT but malformed one to false
    // would be the silent-wrong-value shape. Hence: absent -> false, present ->
    // must parse as a bool or the whole decode fails.
    s->maintenance_on_boot = false;
    const cJSON *mob = Member(o, "maintenance_on_boot");
    if (mob != nullptr && !ReadBool(o, "maintenance_on_boot", &s->maintenance_on_boot)) {
        return false;
    }
    return true;
}

}  // namespace

size_t ConfigEncodeJson(const Config &c, char *out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return 0;

    // Every add is checked for allocation failure. cJSON returns NULL rather
    // than aborting, and a partial tree would otherwise be printed as a valid
    // but incomplete config -- the failure mode FR-27's byte-identical round
    // trip exists to catch, arrived at from the other side.
    bool ok = true;
    ok = ok && cJSON_AddNumberToObject(root, "schema_version",
                                       static_cast<double>(c.schema_version)) != nullptr;
    ok = ok && cJSON_AddStringToObject(root, "device_id", c.device_id) != nullptr;
    ok = ok && cJSON_AddNumberToObject(root, "updated_at_ms",
                                       static_cast<double>(c.updated_at_ms)) != nullptr;
    cJSON *settings = ok ? EncodeSettings(c.settings) : nullptr;
    if (settings != nullptr) cJSON_AddItemToObject(root, "settings", settings);
    else ok = false;
    cJSON *aux = ok ? EncodeAux(c) : nullptr;
    if (aux != nullptr) cJSON_AddItemToObject(root, "aux", aux);
    else ok = false;
    cJSON *channels = ok ? EncodeChannels(c) : nullptr;
    if (channels != nullptr) cJSON_AddItemToObject(root, "channels", channels);
    else ok = false;
    cJSON *bindings = ok ? EncodeBindings(c) : nullptr;
    if (bindings != nullptr) cJSON_AddItemToObject(root, "bindings", bindings);
    else ok = false;

    if (!ok) { cJSON_Delete(root); return 0; }

    // PrintUnformatted, not Print. The blob CRCs exactly these bytes, they cross
    // USB and NVS, and whitespace is flash the device never reads. It is also
    // what Task 8's ANewerSchemaVersionIsRefused assumes: it searches for the
    // substring "schema_version":1, and cJSON_Print emits ": 1" with a space.
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return 0;

    const size_t n = strlen(text);
    if (n + 1 > out_len) { cJSON_free(text); return 0; }
    memcpy(out, text, n + 1);
    cJSON_free(text);
    return n;
}

bool ConfigDecodeJson(const char *json, size_t len, Config *out) {
    if (json == nullptr || out == nullptr || len == 0) return false;
    // cJSON_ParseWithLength, not cJSON_Parse: the transport is byte-exact and
    // the caller knows the length, so the parser must not be free to read past
    // it looking for a terminator.
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) return false;

    Config &c = g_decode_scratch;
    c = Config{};
    bool ok = cJSON_IsObject(root) != 0;
    ok = ok && ReadU32(root, "schema_version", &c.schema_version);
    // A schema the firmware does not implement is refused HERE, as its own
    // condition, rather than left to fail somewhere downstream. Refusing early
    // is what makes "newer schema" distinguishable from "malformed config", and
    // it is the whole point of versioning the blob (spec 3.8).
    if (ok && c.schema_version != kConfigSchemaVersion) {
        cJSON_Delete(root);
        return false;
    }
    ok = ok && ReadStr(root, "device_id", c.device_id, sizeof(c.device_id));
    ok = ok && ReadU64(root, "updated_at_ms", &c.updated_at_ms);
    ok = ok && DecodeSettings(Member(root, "settings"), &c.settings);
    ok = ok && DecodeAux(Member(root, "aux"), &c);
    ok = ok && DecodeChannels(Member(root, "channels"), &c);
    ok = ok && DecodeBindings(Member(root, "bindings"), &c);

    cJSON_Delete(root);
    // Nothing is written to *out until every field has decoded AND the result
    // validates. A partially-applied config is worse than none (spec 3.8).
    if (!ok || !ConfigValidate(c)) return false;
    *out = c;
    return true;
}

size_t ConfigEncodeBlob(const Config &c, uint8_t *out, size_t out_len) {
    if (out == nullptr || out_len <= sizeof(BlobHeader)) return 0;
    // The payload IS the JSON -- the header is prepended to the same bytes
    // ConfigEncodeJson produces. Packing the struct instead would give the two
    // forms separate codecs to drift apart.
    const size_t n = ConfigEncodeJson(c, reinterpret_cast<char *>(out) + sizeof(BlobHeader),
                                      out_len - sizeof(BlobHeader));
    if (n == 0) return 0;

    BlobHeader h{};
    h.magic          = kBlobMagic;
    h.schema_version = c.schema_version;
    h.payload_len    = static_cast<uint32_t>(n);
    h.payload_crc    = Crc32(out + sizeof(BlobHeader), n);
    memcpy(out, &h, sizeof(h));
    return sizeof(BlobHeader) + n;
}

bool ConfigDecodeBlob(const uint8_t *in, size_t len, Config *out) {
    if (in == nullptr || out == nullptr || len < sizeof(BlobHeader)) return false;
    BlobHeader h{};
    memcpy(&h, in, sizeof(h));
    if (h.magic != kBlobMagic) return false;
    if (h.schema_version != kConfigSchemaVersion) return false;
    // Bounded before the subtraction so a hostile or torn payload_len cannot
    // wrap: `len - sizeof(BlobHeader)` is the bytes actually available.
    if (h.payload_len > len - sizeof(BlobHeader)) return false;
    const uint8_t *payload = in + sizeof(BlobHeader);
    if (Crc32(payload, h.payload_len) != h.payload_crc) return false;
    return ConfigDecodeJson(reinterpret_cast<const char *>(payload), h.payload_len, out);
}

size_t ConfigBlobTotalLength(const uint8_t *in, size_t len) {
    if (in == nullptr || len < sizeof(BlobHeader)) return 0;
    BlobHeader h{};
    memcpy(&h, in, sizeof(h));
    if (h.magic != kBlobMagic) return 0;
    if (h.schema_version != kConfigSchemaVersion) return 0;
    if (h.payload_len > ConfigMaxSerializedSize()) return 0;
    return sizeof(BlobHeader) + h.payload_len;
}
