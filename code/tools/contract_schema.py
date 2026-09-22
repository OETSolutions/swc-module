"""The one source for the firmware/app contract.

Everything the two sides must agree on lives here: the protocol version, the
action-kind vocabulary, and the frame types. `gen_contract.py` and
`gen_contract_kotlin.py` both read THIS module and nothing else, and
`test_gen_contract.py` asserts the generated files still match it.

**What the contract deliberately does NOT contain: action ids.** Spec 3.6 is
explicit that an action has no id -- it is identified by its `kind` plus its
params -- and that "a generated contract must not synthesize them". An earlier
revision of this task emitted a `1-63` id table, which is exactly the thing the
spec forbids. The ordinal below is a position in a C enum, used for nothing on
the wire: the wire always carries the kind NAME (spec 3.7), because an ordinal
silently rebinds when a kind is inserted and a stored config from an older
firmware would then mean something else.

Two of these tables have a second home, and the tests assert they still agree
rather than pretending they have one:

- `ACTION_KINDS`' order mirrors `enum class ActionKind` in ConfigModel.h.
- `needs_target` is `ActionTakesPayload` in ConfigModel.h.

Both are read, never redefined. Reading them here and asserting equality is what
turns a drift into a failing test instead of a runtime parse failure in a car.
"""

PROTOCOL_VERSION = 1

# Spec 3.6's table, in ConfigModel.h's declaration order. `c_enum` is the C
# enumerator; `name` is the WIRE name (spec 3.7) -- these differ because the
# firmware uses a `k` prefix internally while the wire has none.
from collections import namedtuple

ActionKind = namedtuple("ActionKind", "name ordinal c_enum needs_target")
Frame = namedtuple("Frame", "name direction fields")

# `needs_target` mirrors ActionTakesPayload (ConfigModel.h:67). It is stated here
# for the APP's benefit -- so the app can refuse to save an action with an empty
# required field without duplicating the rule -- and the test asserts it matches
# the firmware's function. Note OUT_VOLTAGE is the exception on both sides: it
# needs `key_mv`, a NUMBER, not a string target.
ACTION_KINDS = [
    ActionKind("NONE",         0,  "kNone",        False),
    ActionKind("OUT_VOLTAGE",  1,  "kOutVoltage",  False),
    ActionKind("OUT_RELEASE",  2,  "kOutRelease",  False),
    ActionKind("APP_LAUNCH",   3,  "kAppLaunch",   True),
    ActionKind("APP_INTENT",   4,  "kAppIntent",   True),
    ActionKind("KEYCODE",      5,  "kKeycode",     True),
    ActionKind("MEDIA",        6,  "kMedia",       True),
    ActionKind("VOLUME",       7,  "kVolume",      True),
    ActionKind("SYSTEM",       8,  "kSystem",      True),
    ActionKind("BUZZ",         9,  "kBuzzer",      True),
    ActionKind("APP_RAW",      10, "kAppRaw",      True),
]

# The parameter names the firmware actually writes for each kind's two generic
# string slots (ConfigCodec.cpp's `kParamKeys`). This is what the app must use as
# the JSON key, and it is per-kind rather than generic on purpose: spec 3.6 says
# "each kind names them on the wire". APP_INTENT is the only kind using both
# slots (the user's stated example: an intent with a data payload).
PARAM_KEYS = {
    "NONE":        (None, None),
    "OUT_VOLTAGE": (None, None),          # uses `key_mv`, a number
    "OUT_RELEASE": (None, None),
    "APP_LAUNCH":  ("package", None),
    "APP_INTENT":  ("action", "data"),
    "KEYCODE":     ("keycode", None),
    "MEDIA":       ("command", None),
    "VOLUME":      ("target", None),
    "SYSTEM":      ("command", None),
    "BUZZ":        ("pattern", None),
    "APP_RAW":     ("command", None),
}

# Spec 4.3's frame table. `direction` is "fw2app" or "app2fw"; a frame in both
# directions is listed once per direction. Used for the generated string
# constants, so the app and the firmware both reference symbols instead of
# literals -- a typo in a frame name is then a compile error, not a nack.
FRAMES = [
    Frame("hello",         "fw2app", "fw_version,hw_id,protocol_v,caps"),
    # `level_mv` is the FR-3 FILTERED level and there is no `confidence`: the
    # field names the quantity the device actually decided on, and confidence is
    # a learned-button property rather than a classification output, so there is
    # nothing honest to send. See spec 4.3's notes on `event`.
    Frame("event",         "fw2app", "channel,button,gesture,t_ms,level_mv"),
    # `tx_dropped`/`rx_overflows` are the DEVICE transport's own loss counters
    # (N-24 and its inbound twin). They are declared because the app now reads
    # them; the router's emit site is asserted against this row in both
    # directions by test_frame_field_lists_match_the_router, so the row cannot
    # drift back to the N-22 phantom-field shape.
    Frame("status",        "fw2app", "vbus_present,gain_mode,uptime_ms,config_state,output_safe,tx_dropped,rx_overflows"),
    Frame("ladder_sample", "fw2app", "channel,level_mv,n"),
    # `ack` carries `for_seq` and `ok` -- and `mv_center`/`mv_tolerance` on the
    # ONE ack that has them, `learn_commit`'s (CommandRouter::HandleLearnCommit),
    # where the derived window is the answer the learn screen exists to read. It
    # does NOT carry `err`: every ack is `ok:true` and every failure is a `nack`,
    # so an `err` on this row names a field no firmware writes -- the same
    # phantom-field shape as N-22's `status` row. The list below is asserted
    # against the router's actual emit sites (test_frame_field_lists_match_the_router),
    # so it cannot drift back.
    #
    # `result` is the same shape on the OTA path: `ota_end`'s ack
    # (CommandRouter::HandleOtaEnd) reports WHICH outcome it reached -- `installed`
    # on a device, `not_supported` on a host build with no partitions. That
    # distinction is the point: spec 9.3's install is a device-only action, and an
    # ack that only said `ok:true` would let a bench run read an install that never
    # happened as a success.
    Frame("ack",           "fw2app", "for_seq,ok,mv_center,mv_tolerance,result"),
    Frame("nack",          "fw2app", "for_seq,err,detail"),
    Frame("log",           "fw2app", "level,msg"),
    Frame("link_gap",      "fw2app", "channel,button,gesture,expected_seq,got_seq"),

    Frame("config_get",    "app2fw", ""),
    # These three are "both", not "app2fw". Spec 4.3 lists them under the app
    # because that is the direction a user thinks of for config_set, but the
    # SAME three frame types carry a config_get REPLY from the firmware -- the
    # router emits them (CommandRouter::BeginConfigReplyRun). A one-directional
    # model here would say the firmware can answer a request the app cannot
    # parse, which is the shape of the defect this contract exists to prevent.
    Frame("config_begin",  "both", "total_len,crc32"),
    Frame("config_chunk",  "both", "offset,data_b64"),
    Frame("config_end",    "both", "sha256"),
    Frame("config_patch",  "app2fw", "path,value"),
    Frame("learn_start",   "app2fw", "channel,button_id"),
    Frame("learn_stop",    "app2fw", "channel,button_id"),
    Frame("learn_commit",  "app2fw", "channel,button_id,name"),
    Frame("maintenance_enter", "app2fw", ""),
    Frame("maintenance_exit",  "app2fw", ""),
    Frame("test_key",      "app2fw", "channel,key_mv,hold_ms"),
    Frame("identify",      "app2fw", "pattern"),
    Frame("reboot",        "app2fw", "boot_target"),
    Frame("ping",          "app2fw", ""),
    Frame("time_sync",     "app2fw", "epoch_ms,tz_offset_min"),
    Frame("ota_begin",     "app2fw", "size,sha256"),
    Frame("ota_chunk",     "app2fw", "offset,data_b64"),
    Frame("ota_end",       "app2fw", ""),
]


def action_kind(name):
    """Look up a kind by its wire name. Raises rather than returning None."""
    for k in ACTION_KINDS:
        if k.name == name:
            return k
    raise KeyError(f"unknown action kind {name!r}")
