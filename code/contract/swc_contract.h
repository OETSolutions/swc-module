// GENERATED FILE -- do not edit by hand.
//
// Produced by code/tools/gen_contract.py from code/tools/contract_schema.py.
// Regenerate with:
//     cd code/tools && python3 gen_contract.py
// and commit the result. `test_gen_contract.py` fails if this file and the
// schema disagree, so editing this header directly is caught immediately rather
// than silently drifting from the Kotlin side.
//
// This header exists so the firmware and the app cannot disagree about a field
// or frame name: a rename is a compile error on both sides, not a runtime parse
// failure in a car.
//
// There are deliberately NO numeric action ids here. Spec 3.6: an action is
// identified by its `kind` plus its params, and a generated contract must not
// synthesize ids.
#pragma once
// The wire protocol version. `lib/Link/Ndjson.h` DEFINES this value
// (as `kNdjsonProtocolVersion`); this macro is derived from the same
// schema and the test asserts the two agree. It is not this header's to
// define: the CommandRouter must compile before this task exists.
#define SWC_PROTOCOL_VERSION 1

// The action kinds, in `enum class ActionKind`'s declaration order
// (lib/Config/ConfigModel.h). The ORDINAL is an in-memory position only -- it is
// never written to the wire. A config carries the kind NAME (spec 3.7), because
// an ordinal silently rebinds if a kind is inserted and an older firmware's
// stored config would then mean something else.
enum SwcActionKind {
    SWC_KIND_NONE = 0,
    SWC_KIND_OUT_VOLTAGE = 1,
    SWC_KIND_OUT_RELEASE = 2,
    SWC_KIND_APP_LAUNCH = 3,
    SWC_KIND_APP_INTENT = 4,
    SWC_KIND_KEYCODE = 5,
    SWC_KIND_MEDIA = 6,
    SWC_KIND_VOLUME = 7,
    SWC_KIND_SYSTEM = 8,
    SWC_KIND_BUZZ = 9,
    SWC_KIND_APP_RAW = 10,
};

// The WIRE form of each kind (spec 3.7's `"kind": "OUT_VOLTAGE"`).
// A config uses these strings, so these -- not the enum -- are what must
// stay stable across firmware versions.
#define SWC_ACTION_KIND_NONE "NONE"
#define SWC_ACTION_KIND_OUT_VOLTAGE "OUT_VOLTAGE"
#define SWC_ACTION_KIND_OUT_RELEASE "OUT_RELEASE"
#define SWC_ACTION_KIND_APP_LAUNCH "APP_LAUNCH"
#define SWC_ACTION_KIND_APP_INTENT "APP_INTENT"
#define SWC_ACTION_KIND_KEYCODE "KEYCODE"
#define SWC_ACTION_KIND_MEDIA "MEDIA"
#define SWC_ACTION_KIND_VOLUME "VOLUME"
#define SWC_ACTION_KIND_SYSTEM "SYSTEM"
#define SWC_ACTION_KIND_BUZZ "BUZZ"
#define SWC_ACTION_KIND_APP_RAW "APP_RAW"

// The JSON key each kind uses for its required string parameter, or NULL when
// the kind has none. This mirrors `kParamKeys` in lib/Config/ConfigCodec.cpp;
// APP_INTENT is the only kind with a second (`data`) slot.
#define SWC_ACTION_PARAM_NONE NULL
#define SWC_ACTION_PARAM_OUT_VOLTAGE NULL
#define SWC_ACTION_PARAM_OUT_RELEASE NULL
#define SWC_ACTION_PARAM_APP_LAUNCH "package"
#define SWC_ACTION_PARAM_APP_INTENT "action"
#define SWC_ACTION_PARAM_KEYCODE "keycode"
#define SWC_ACTION_PARAM_MEDIA "command"
#define SWC_ACTION_PARAM_VOLUME "target"
#define SWC_ACTION_PARAM_SYSTEM "command"
#define SWC_ACTION_PARAM_BUZZ "pattern"
#define SWC_ACTION_PARAM_APP_RAW "command"

// The second, optional string slot each kind may carry, or NULL when it has
// none. Only APP_INTENT uses it (`action` + `data`, spec 3.6's stated example);
// every other kind has at most one string parameter.
#define SWC_ACTION_PAYLOAD_NONE NULL
#define SWC_ACTION_PAYLOAD_OUT_VOLTAGE NULL
#define SWC_ACTION_PAYLOAD_OUT_RELEASE NULL
#define SWC_ACTION_PAYLOAD_APP_LAUNCH NULL
#define SWC_ACTION_PAYLOAD_APP_INTENT "data"
#define SWC_ACTION_PAYLOAD_KEYCODE NULL
#define SWC_ACTION_PAYLOAD_MEDIA NULL
#define SWC_ACTION_PAYLOAD_VOLUME NULL
#define SWC_ACTION_PAYLOAD_SYSTEM NULL
#define SWC_ACTION_PAYLOAD_BUZZ NULL
#define SWC_ACTION_PAYLOAD_APP_RAW NULL

// Which kinds require that parameter to be non-empty. Mirrors
// `ActionTakesPayload` (lib/Config/ConfigModel.h). OUT_VOLTAGE is the exception:
// it requires `key_mv`, a NUMBER, not a string.
#define SWC_ACTION_NEEDS_PARAM_NONE 0
#define SWC_ACTION_NEEDS_PARAM_OUT_VOLTAGE 0
#define SWC_ACTION_NEEDS_PARAM_OUT_RELEASE 0
#define SWC_ACTION_NEEDS_PARAM_APP_LAUNCH 1
#define SWC_ACTION_NEEDS_PARAM_APP_INTENT 1
#define SWC_ACTION_NEEDS_PARAM_KEYCODE 1
#define SWC_ACTION_NEEDS_PARAM_MEDIA 1
#define SWC_ACTION_NEEDS_PARAM_VOLUME 1
#define SWC_ACTION_NEEDS_PARAM_SYSTEM 1
#define SWC_ACTION_NEEDS_PARAM_BUZZ 1
#define SWC_ACTION_NEEDS_PARAM_APP_RAW 1

// The frame types (spec 4.3). A typo in a frame name is a nack at
// runtime; referencing these makes it a build failure instead.
#define SWC_FRAME_HELLO "hello"
#define SWC_FRAME_EVENT "event"
#define SWC_FRAME_STATUS "status"
#define SWC_FRAME_LADDER_SAMPLE "ladder_sample"
#define SWC_FRAME_MAINTENANCE "maintenance"
#define SWC_FRAME_ACK "ack"
#define SWC_FRAME_NACK "nack"
#define SWC_FRAME_LOG "log"
#define SWC_FRAME_LINK_GAP "link_gap"
#define SWC_FRAME_CONFIG_GET "config_get"
#define SWC_FRAME_CONFIG_BEGIN "config_begin"
#define SWC_FRAME_CONFIG_CHUNK "config_chunk"
#define SWC_FRAME_CONFIG_END "config_end"
#define SWC_FRAME_CONFIG_PATCH "config_patch"
#define SWC_FRAME_LEARN_START "learn_start"
#define SWC_FRAME_LEARN_STOP "learn_stop"
#define SWC_FRAME_LEARN_COMMIT "learn_commit"
#define SWC_FRAME_MAINTENANCE_ENTER "maintenance_enter"
#define SWC_FRAME_MAINTENANCE_EXIT "maintenance_exit"
#define SWC_FRAME_TEST_KEY "test_key"
#define SWC_FRAME_IDENTIFY "identify"
#define SWC_FRAME_REBOOT "reboot"
#define SWC_FRAME_PING "ping"
#define SWC_FRAME_TIME_SYNC "time_sync"
#define SWC_FRAME_OTA_BEGIN "ota_begin"
#define SWC_FRAME_OTA_CHUNK "ota_chunk"
#define SWC_FRAME_OTA_END "ota_end"
