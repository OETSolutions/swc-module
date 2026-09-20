"""Tests for the generated contract.

Run: `cd code/tools && python3 -m pytest test_gen_contract.py -v`

The point of these is not that the generators run. It is that every fact with a
SECOND home is asserted to still agree with its first, because that is how this
project's defects have actually happened: two spellings of one list, one of them
stale, with nothing comparing them.
"""

import pathlib
import re
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
CODE = HERE.parent

import contract_schema  # noqa: E402


def _read(rel):
    return (CODE / rel).read_text()


def test_action_kinds_are_contiguous_from_zero():
    # A kind's ordinal is its in-memory value on each side, so a gap would be a
    # value nothing defines. (Action ids 1-63 are a different thing and do not
    # exist at all; the wire carries the kind NAME, per spec 3.6.)
    ordinals = sorted(k.ordinal for k in contract_schema.ACTION_KINDS)
    assert ordinals == list(range(len(ordinals))), "kind ordinals must have no gaps"


def test_there_are_exactly_the_eleven_spec_kinds():
    # Spec 3.6's table. A kind added here without a spec entry is a name the app
    # can send and the firmware has no behavior for.
    assert len(contract_schema.ACTION_KINDS) == 11
    assert contract_schema.ACTION_KINDS[0].name == "NONE"


def _firmware_action_kind_enumerators():
    """Parsed from ConfigModel.h -- the firmware's own declaration order."""
    text = _read("lib/Config/ConfigModel.h")
    body = text.split("enum class ActionKind : uint8_t {", 1)[1].split("};", 1)[0]
    return [w.strip() for w in body.replace("\n", " ").split(",") if w.strip()]


def test_generated_kinds_match_the_firmware_enum_order():
    # The schema's order and ConfigModel.h's ActionKind declaration order are ONE
    # fact with two spellings. Both sides use their own enum ordinal internally;
    # the wire carries the NAME (spec 3.7), so a drift here is a defect in the
    # generated header, not a misread config. Asserted anyway, because two
    # spellings of one list is how this plan's defects start.
    firmware = _firmware_action_kind_enumerators()
    generated = [k.c_enum for k in contract_schema.ACTION_KINDS]
    assert generated == firmware, (
        "regenerate the contract after changing ActionKind's declaration order")


def test_generated_kinds_are_the_same_ELEVEN_as_the_firmware_enum():
    # The order test above would pass with a shorter prefix, so the COUNT is
    # asserted separately. Otherwise dropping APP_RAW from the schema entirely
    # leaves a passing order test over ten kinds.
    assert len(_firmware_action_kind_enumerators()) == len(
        contract_schema.ACTION_KINDS)


def test_wire_names_match_the_firmware_codec_table():
    # ConfigCodec.cpp's `kActionKindNames` is what actually encodes and decodes
    # stored configs. If the schema's wire name differs from the codec's, the app
    # writes a config the firmware cannot read -- and the failure is a mysterious
    # "corrupt config" rather than anything naming the mismatch.
    text = _read("lib/Config/ConfigCodec.cpp")
    body = text.split("constexpr EnumName kActionKindNames[] = {", 1)[1].split("};", 1)[0]
    # The entries arrive as `{static_cast<int>(ActionKind::kNone), "NONE"},`, so
    # the cast sits between the enumerator and the string. Match across it
    # deliberately rather than assuming adjacency.
    pairs = re.findall(r'ActionKind::(\w+)\s*\)\s*,\s*"([^"]+)"', body)
    assert pairs, "could not parse kActionKindNames"

    firmware = {enum: wire for enum, wire in pairs}
    for k in contract_schema.ACTION_KINDS:
        assert k.c_enum in firmware, f"{k.c_enum} missing from kActionKindNames"
        assert firmware[k.c_enum] == k.name, (
            f"{k.c_enum}: codec says {firmware[k.c_enum]!r}, schema says {k.name!r}")


def test_needs_param_matches_the_firmware_function():
    # `ActionTakesPayload` (ConfigModel.h) is the firmware's authority for which
    # kinds require a non-empty string parameter. The schema restates it so the
    # APP can grey out an empty field without duplicating the rule -- which is
    # only safe if the two are asserted equal.
    text = _read("lib/Config/ConfigModel.h")
    body = text.split("inline bool ActionTakesPayload(ActionKind k) {", 1)[1]
    body = body.split("}", 1)[0]
    takes = set(re.findall(r"ActionKind::(\w+)", body))

    for k in contract_schema.ACTION_KINDS:
        expected = k.c_enum in takes
        assert k.needs_target == expected, (
            f"{k.name}: schema says needs_target={k.needs_target}, "
            f"ActionTakesPayload says {expected}")


def test_param_keys_match_the_firmware_codec_table():
    # The per-kind JSON key the app must use. `kParamKeys` is the encoder's and
    # the DECODER's shared table; an app using a different key writes a config
    # that round-trips wrong.
    text = _read("lib/Config/ConfigCodec.cpp")
    body = text.split("constexpr ParamKeys kParamKeys[] = {", 1)[1].split("};", 1)[0]
    rows = re.findall(
        r"ActionKind::(\w+)\s*,\s*(nullptr|\"[^\"]*\")\s*,\s*(nullptr|\"[^\"]*\")", body)
    assert rows, "could not parse kParamKeys"

    def lit(s):
        return None if s == "nullptr" else s.strip('"')

    firmware = {enum: (lit(t), lit(p)) for enum, t, p in rows}
    for k in contract_schema.ACTION_KINDS:
        assert k.c_enum in firmware, f"{k.c_enum} missing from kParamKeys"
        assert firmware[k.c_enum] == contract_schema.PARAM_KEYS[k.name], (
            f"{k.name}: codec says {firmware[k.c_enum]}, "
            f"schema says {contract_schema.PARAM_KEYS[k.name]}")


def test_frame_types_are_unique():
    names = [f.name for f in contract_schema.FRAMES]
    assert len(names) == len(set(names)), "a duplicated frame name is a nack waiting"


def test_every_spec_4_3_frame_is_present():
    # Spec 4.3's table, including `link_gap` and the maintenance pair, which the
    # router emits/handles and the table omitted until this task corrected it.
    required = {
        "hello", "event", "status", "ladder_sample", "ack", "nack", "log",
        "link_gap",
        "config_get", "config_begin", "config_chunk", "config_end",
        "config_patch", "learn_start", "learn_stop", "learn_commit",
        "maintenance_enter", "maintenance_exit",
        "test_key", "identify", "reboot", "ping", "time_sync",
        "ota_begin", "ota_chunk", "ota_end",
    }
    assert {f.name for f in contract_schema.FRAMES} == required


def test_router_outbound_frames_are_all_in_the_contract():
    # The router is the firmware's voice. Any frame it EMITS must be in the
    # contract, or the app has a frame it cannot name -- and the schema would be
    # describing a protocol the device does not speak.
    text = _read("lib/Link/CommandRouter.cpp")
    emitted = set(re.findall(r'Emit\("(\w+)"', text))
    contract = {f.name for f in contract_schema.FRAMES
                if f.direction in ("fw2app", "both")}
    missing = emitted - contract
    assert not missing, f"router emits frames absent from the contract: {sorted(missing)}"


def test_router_inbound_commands_are_all_in_the_contract():
    # The inverse direction, and the more dangerous one: a command the router
    # ACCEPTS but the contract omits is a command the app cannot send.
    text = _read("lib/Link/CommandRouter.cpp")
    body = text.split("static const char *kCmds[] = {", 1)[1].split("};", 1)[0]
    accepted = set(re.findall(r'"(\w+)"', body))
    contract = {f.name for f in contract_schema.FRAMES
                if f.direction in ("app2fw", "both")}
    missing = accepted - contract
    assert not missing, (
        f"router accepts commands absent from the contract: {sorted(missing)}")


def test_generated_header_matches_the_checked_in_copy(tmp_path):
    out = tmp_path / "swc_contract.h"
    subprocess.run([sys.executable, str(HERE / "gen_contract.py"), "--out", str(out)],
                   check=True, cwd=HERE)
    assert out.read_text() == _read("contract/swc_contract.h"), (
        "regenerate and commit contract/swc_contract.h")


def test_generated_kotlin_matches_the_checked_in_copy(tmp_path):
    out = tmp_path / "Contract.kt"
    subprocess.run([sys.executable, str(HERE / "gen_contract_kotlin.py"), "--out", str(out)],
                   check=True, cwd=HERE)
    assert out.read_text() == _read(
        "android/app/src/main/java/com/oetsolutions/swc/contract/Contract.kt"), (
        "regenerate and commit Contract.kt")


def test_generators_are_deterministic(tmp_path):
    # Two runs must agree, or the checked-in comparison above fails at random and
    # the whole gate gets ignored. This is the property that makes it usable.
    a = tmp_path / "a.h"
    b = tmp_path / "b.h"
    for out in (a, b):
        subprocess.run([sys.executable, str(HERE / "gen_contract.py"), "--out", str(out)],
                       check=True, cwd=HERE)
    assert a.read_text() == b.read_text()


@pytest.mark.parametrize("suffix,compiler_var", [(".c", "CC"), (".cpp", "CXX")])
def test_the_generated_header_actually_compiles(tmp_path, suffix, compiler_var):
    # Nothing in lib/ or src/ includes this header YET, so the device build never
    # compiles it -- which means a syntax error in the generated output would ship
    # silently and be discovered when the first consumer is added. This compiles it
    # directly, as BOTH C and C++, because the consumer's language is not fixed: a
    # `src/` file is C and a `lib/` file is C++.
    #
    # This is the same principle as the device build's "prove the object exists"
    # check: an artifact nothing compiles is an artifact nobody has checked.
    import os
    import shutil

    compiler = os.environ.get(compiler_var) or shutil.which(
        "c++" if suffix == ".cpp" else "cc")
    if not compiler:
        pytest.skip(f"no {compiler_var} compiler available")

    src = tmp_path / f"probe{suffix}"
    # Reference every macro family, so a missing or malformed one is a compile
    # error rather than an unused-define that nobody notices.
    src.write_text(
        '#include "contract/swc_contract.h"\n'
        + ('#include <assert.h>\n#include <string.h>\n'
           'int main(void) {\n'
           '  _Static_assert(SWC_KIND_APP_RAW == 10, "kind ordinals are contiguous");\n'
           '  assert(strcmp(SWC_ACTION_KIND_OUT_VOLTAGE, "OUT_VOLTAGE") == 0);\n'
           '  assert(strcmp(SWC_FRAME_HELLO, "hello") == 0);\n'
           '  assert(SWC_ACTION_NEEDS_PARAM_APP_INTENT == 1);\n'
           '  assert(SWC_ACTION_PARAM_NONE == 0);\n'
           '  assert(SWC_ACTION_PAYLOAD_NONE == 0);\n'
           '  assert(SWC_PROTOCOL_VERSION >= 1);\n'
           '  return 0;\n}\n'
           if suffix == ".c" else
           '#include <cassert>\n#include <cstring>\n'
           'int main() {\n'
           '  static_assert(SWC_KIND_APP_RAW == 10, "kind ordinals are contiguous");\n'
           '  assert(std::strcmp(SWC_ACTION_KIND_BUZZ, "BUZZ") == 0);\n'
           '  assert(std::strcmp(SWC_FRAME_NACK, "nack") == 0);\n'
           '  assert(SWC_ACTION_NEEDS_PARAM_NONE == 0);\n'
           '  assert(SWC_ACTION_PARAM_NONE == nullptr);\n'
           '  assert(SWC_ACTION_PAYLOAD_APP_INTENT != nullptr);\n'
           '  return 0;\n}\n'))
    exe = tmp_path / "probe"
    subprocess.run([compiler, f"-I{CODE}", "-o", str(exe), str(src)],
                   check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)


def test_the_header_has_the_protocol_version():
    text = _read("contract/swc_contract.h")
    assert f"#define SWC_PROTOCOL_VERSION {contract_schema.PROTOCOL_VERSION}" in text


def test_the_firmware_protocol_version_agrees_with_the_contract():
    # The version is DEFINED once, in lib/Link/Ndjson.h beside `NdjsonWriter`,
    # which is what stamps `"v"` into every frame. The generator does not define
    # it -- a consumer (Task 15, CommandRouter) must compile before this task
    # exists, so the generated header cannot be its home. What this asserts is
    # that the two AGREE, which is the part that actually catches drift. A
    # generated value nothing compares, or a hand-written value nothing checks,
    # each drifts exactly as freely as the other.
    hdr = _read("lib/Link/Ndjson.h")
    m = re.search(r"constexpr uint8_t kNdjsonProtocolVersion\s*=\s*(\d+)", hdr)
    assert m, "kNdjsonProtocolVersion must be defined in lib/Link/Ndjson.h"
    assert int(m.group(1)) == contract_schema.PROTOCOL_VERSION, (
        f"firmware says {m.group(1)}, contract says {contract_schema.PROTOCOL_VERSION}")


def test_the_header_defines_no_action_ids():
    # Spec 3.6: "a generated contract must not synthesize them". This is the
    # assertion that keeps a future revision from reintroducing the 1-63 table an
    # earlier revision of this task had.
    #
    # The pattern is `SWC_ACTION_<NAME>` WITHOUT `KIND_` or `NEEDS_PARAM_` or
    # `PARAM_` in it, because those are the legitimate forms: an action-id macro
    # would be named for the kind itself (`SWC_ACTION_VOL_UP 7`). Matching the
    # bare prefix flagged `SWC_ACTION_NEEDS_PARAM_NONE 0` -- a boolean, not an id
    # -- which is an over-broad test rather than a real defect.
    text = _read("contract/swc_contract.h")
    bad = re.findall(
        r"#define\s+(SWC_ACTION_(?!KIND_)(?!NEEDS_PARAM_)(?!PARAM_)(?!PAYLOAD_)\w+)"
        r"\s+\d+", text)
    assert not bad, (
        f"an action ID macro reappeared: {bad} -- spec 3.6 forbids numeric action ids")
    assert "SWC_ACTION_KIND_" in text, "the wire-name macros are the correct form"
