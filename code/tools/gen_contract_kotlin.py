#!/usr/bin/env python3
"""Generate the Kotlin contract (`Contract.kt`) from `contract_schema.py`.

Same schema, same determinism rules as `gen_contract.py`, so the two generated
files are two spellings of one source rather than two sources.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import contract_schema as S  # noqa: E402

BANNER = """\
// GENERATED FILE -- do not edit by hand.
//
// Produced by code/tools/gen_contract_kotlin.py from code/tools/contract_schema.py.
// Regenerate with:
//     cd code/tools && python3 gen_contract_kotlin.py
// and commit the result. `test_gen_contract.py` fails if this file and the schema
// disagree.
//
// This is the app's half of the contract that keeps it from drifting from the
// firmware. Referencing these symbols rather than string literals means a frame
// or kind rename breaks the build instead of silently failing at runtime against
// a head unit in a car.
//
// There is deliberately NO `ActionIds` object. Spec 3.6: an action has no id.
"""


def render():
    out = [BANNER]
    out.append("package com.oetsolutions.swc.contract\n\n")

    out.append("""\
/**
 * The action kinds of spec 3.6, in the firmware's declaration order.
 *
 * [wireName] is what actually travels: spec 3.7 writes `"kind": "OUT_VOLTAGE"`.
 * The ordinal is in-memory only and must never be sent -- inserting a kind would
 * rebind every stored ordinal, so a config written by an older firmware would
 * silently mean something else.
 *
 * [paramKey] is the kind's required string parameter and [payloadKey] its
 * optional second one (spec 3.6: "each kind names them on the wire"). APP_INTENT
 * is the only kind that uses both -- the user's stated example, an intent with a
 * data payload.
 */
enum class ActionKind(
    val wireName: String,
    val paramKey: String?,
    val payloadKey: String?,
    val needsParam: Boolean,
) {
""")
    rows = []
    for k in S.ACTION_KINDS:
        key, payload = S.PARAM_KEYS[k.name]
        key_lit = "null" if key is None else f'"{key}"'
        payload_lit = "null" if payload is None else f'"{payload}"'
        rows.append(
            f'    {k.name}("{k.name}", {key_lit}, {payload_lit}, '
            f'{"true" if k.needs_target else "false"})'
        )
    out.append(",\n".join(rows))
    out.append(";\n\n")

    out.append("""\
    companion object {
        /**
         * Look up a kind by its wire name, or null if this app does not know it.
         *
         * Null rather than an exception: a newer firmware may legitimately send a
         * kind this app predates, and the caller must show that honestly (spec
         * 4.5 -- no silent partial compatibility) rather than crash.
         */
        fun fromWireName(name: String): ActionKind? = entries.firstOrNull { it.wireName == name }
    }
}
""")

    out.append("""\

/**
 * The frame `type` strings of spec 4.3.
 *
 * A single object of constants so every call site is a symbol reference. The
 * firmware's half is `SWC_FRAME_*` in `contract/swc_contract.h`.
 */
object Frames {
""")
    for f in S.FRAMES:
        out.append(f'    const val {f.name.upper()} = "{f.name}"\n')
    out.append("}\n")

    out.append(f"""
/**
 * The wire protocol version, which must match the firmware's `hello.protocol_v`.
 *
 * Spec 4.5: if the major version differs the app MUST show an explicit mismatch
 * state rather than attempting to talk. Silent partial compatibility is how a
 * config gets corrupted.
 */
const val PROTOCOL_VERSION = {S.PROTOCOL_VERSION}
""")

    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(
        pathlib.Path(__file__).resolve().parent.parent
        / "android" / "app" / "src" / "main" / "java" / "com" / "oetsolutions" / "swc"
        / "contract" / "Contract.kt"))
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
