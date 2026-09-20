#!/usr/bin/env python3
"""Decode the spec's worked example with the firmware's own decoder.

`docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md` §3.7 is
the spec's most concrete assertion about the wire format: a full config, written
out field by field. Nothing was checking it, and it had drifted from the codecs
in six independent ways at once -- settings nested under a `device` object that
does not exist, five settings names no code appears anywhere, feedback levels
written as words where the enum is numeric, required fields omitted, a channel
keyed `id` instead of `name`, and action params §3.6 had already removed.

A stale example is worse than no example, because it is the thing a reader
copies. So this extracts the block and decodes it with the firmware's decoder --
the same one `UsbLink` uses, reached through `ConfigDecodeJson`.

It is a GENERATED-CODE check in spirit: the spec is the source, and this proves
the source still parses. Run it in CI; a failure names the byte where agreement
ended.
"""

import json
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SPEC = (REPO.parent / "docs" / "superpowers" / "specs"
        / "2026-09-18-swc-firmware-android-app-design.md")


def extract_example(spec_text):
    """The ```jsonc block in §3.7, with its comments and trailing commas removed.

    jsonc is JSON with comments; cJSON has no comment support, so the comments are
    stripped here rather than sent to the decoder as if it could skip them.
    """
    m = re.search(r"### 3\.7 Full worked example.*?```jsonc\n(.*?)\n```",
                  spec_text, re.S)
    if not m:
        raise SystemExit("FAIL: could not find the jsonc block in §3.7")
    body = m.group(1)
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r",(\s*[}\]])", r"\1", body)
    return body


def build_probe():
    """Compile the firmware's decoder into a standalone probe.

    Reusing `ConfigDecodeJson` is the whole point: a Python re-implementation of
    the field rules would be a THIRD home for them, and the two that already exist
    are the ones that disagreed with the spec.
    """
    cjson = None
    for pattern in ("*.pio/libdeps/native/cJSON*", "*.pio/libdeps/*/cJSON*"):
        hits = list(REPO.glob(pattern))
        if hits:
            cjson = hits[0]
            break
    if cjson is None:
        raise SystemExit("FAIL: cJSON not found; run 'pio test -e native' once first")

    src = pathlib.Path(tempfile.mkdtemp()) / "probe.cpp"
    src.write_text(r'''
#include "Config/ConfigCodec.h"
#include <cstdio>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("FAIL: cannot open %s\n", argv[1]); return 2; }
    static char buf[262144];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    Config c{};
    if (!ConfigDecodeJson(buf, n, &c)) {
        printf("FAIL: the firmware decoder REJECTED the spec's example\n");
        return 1;
    }
    printf("OK: the spec's example decodes (%zu bytes, %u channels, %u bindings)\n",
           n, (unsigned)c.channel_count, (unsigned)c.binding_count);
    return 0;
}
''')
    out = src.with_suffix("")
    compile_cmd = [
        "c++", "-std=gnu++17", "-I", "lib", "-I", str(cjson), "-DSWC_NATIVE_TEST",
        "-o", str(out), str(src),
        "lib/Config/ConfigCodec.cpp", "lib/Analog/LadderDecode.cpp",
        "lib/Gesture/GestureStateMachine.cpp", "lib/Output/GainPolicy.cpp",
        str(cjson / "cJSON.c"),
    ]
    r = subprocess.run(compile_cmd, cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-2000:])
        raise SystemExit("FAIL: could not build the decode probe")
    return out


def main():
    spec = SPEC.read_text()
    example = extract_example(spec)
    try:
        json.loads(example)
    except json.JSONDecodeError as e:
        raise SystemExit(f"FAIL: the spec's example is not valid JSON: {e}")

    probe = build_probe()
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(example)
        path = f.name
    r = subprocess.run([str(probe), path], capture_output=True, text=True)
    print(r.stdout.strip())
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
