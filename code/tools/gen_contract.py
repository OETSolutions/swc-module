#!/usr/bin/env python3
"""Generate `contract/swc_contract.h` from `contract_schema.py`.

Deterministic: fixed iteration order, no timestamps, no absolute paths. That is
what makes the checked-in-vs-regenerated comparison in the test meaningful -- a
generator that embedded a date would fail its own test every day.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import contract_schema as S  # noqa: E402

BANNER = """\
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
"""


def render():
    out = [BANNER]
    out.append("#pragma once\n")
    out.append("// The wire protocol version. `lib/Link/Ndjson.h` DEFINES this value\n")
    out.append("// (as `kNdjsonProtocolVersion`); this macro is derived from the same\n")
    out.append("// schema and the test asserts the two agree. It is not this header's to\n")
    out.append("// define: the CommandRouter must compile before this task exists.\n")
    out.append(f"#define SWC_PROTOCOL_VERSION {S.PROTOCOL_VERSION}\n")

    out.append("""
// The action kinds, in `enum class ActionKind`'s declaration order
// (lib/Config/ConfigModel.h). The ORDINAL is an in-memory position only -- it is
// never written to the wire. A config carries the kind NAME (spec 3.7), because
// an ordinal silently rebinds if a kind is inserted and an older firmware's
// stored config would then mean something else.
enum SwcActionKind {
""")
    for k in S.ACTION_KINDS:
        out.append(f"    SWC_KIND_{k.name} = {k.ordinal},\n")
    out.append("};\n\n")

    out.append("// The WIRE form of each kind (spec 3.7's `\"kind\": \"OUT_VOLTAGE\"`).\n")
    out.append("// A config uses these strings, so these -- not the enum -- are what must\n")
    out.append("// stay stable across firmware versions.\n")
    for k in S.ACTION_KINDS:
        out.append(f'#define SWC_ACTION_KIND_{k.name} "{k.name}"\n')

    out.append("""
// The JSON key each kind uses for its required string parameter, or NULL when
// the kind has none. This mirrors `kParamKeys` in lib/Config/ConfigCodec.cpp;
// APP_INTENT is the only kind with a second (`data`) slot.
""")
    for k in S.ACTION_KINDS:
        target = S.PARAM_KEYS[k.name][0]
        if target is None:
            out.append(f"#define SWC_ACTION_PARAM_{k.name} NULL\n")
        else:
            out.append(f'#define SWC_ACTION_PARAM_{k.name} "{target}"\n')

    out.append("""
// The second, optional string slot each kind may carry, or NULL when it has
// none. Only APP_INTENT uses it (`action` + `data`, spec 3.6's stated example);
// every other kind has at most one string parameter.
""")
    for k in S.ACTION_KINDS:
        payload = S.PARAM_KEYS[k.name][1]
        if payload is None:
            out.append(f"#define SWC_ACTION_PAYLOAD_{k.name} NULL\n")
        else:
            out.append(f'#define SWC_ACTION_PAYLOAD_{k.name} "{payload}"\n')

    out.append("""
// Which kinds require that parameter to be non-empty. Mirrors
// `ActionTakesPayload` (lib/Config/ConfigModel.h). OUT_VOLTAGE is the exception:
// it requires `key_mv`, a NUMBER, not a string.
""")
    for k in S.ACTION_KINDS:
        out.append(f"#define SWC_ACTION_NEEDS_PARAM_{k.name} {1 if k.needs_target else 0}\n")

    out.append("\n// The frame types (spec 4.3). A typo in a frame name is a nack at\n")
    out.append("// runtime; referencing these makes it a build failure instead.\n")
    for f in S.FRAMES:
        out.append(f'#define SWC_FRAME_{f.name.upper()} "{f.name}"\n')

    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(
        pathlib.Path(__file__).resolve().parent.parent / "contract" / "swc_contract.h"))
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
