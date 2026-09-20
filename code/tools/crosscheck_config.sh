#!/usr/bin/env bash
# Prove the Kotlin codec and the C codec agree on the wire.
#
# The two sides have their own round-trip tests, and NEITHER can see this: a
# Kotlin test compares Kotlin to Kotlin, and the C tests compare C to C. A field
# name that differs between them round-trips perfectly on both sides and produces
# a config the device refuses -- the failure surfaces as "corrupt config", which
# names nothing. This script decodes the fixture below with the firmware's own
# decoder, validates it, re-encodes it, and requires the bytes to be identical.
#
# **This used to be unrunnable from a clean checkout**, which is the defect it was
# meant to prevent: the probe source lived in /tmp, the fixture was generated to
# one path by the Kotlin side and read from a different one here, and the check was
# wired into no CI job. All three are fixed -- the probe is `tools/decode_probe.cpp`,
# the fixture is a checked-in artifact at `contract/swc_sample_config.json` produced
# by the app's own codec, and the android workflow regenerates and diffs it.
#
# Requires: a host C++ compiler and the cJSON the firmware uses.
set -euo pipefail
cd "$(dirname "$0")/.."

FIXTURE="${1:-contract/swc_sample_config.json}"

CJSON_DIR="$(find .pio/libdeps/native -maxdepth 2 -type d -name 'cJSON*' 2>/dev/null | head -1)"
if [ -z "$CJSON_DIR" ]; then
  CJSON_DIR="$(find .pio/libdeps -maxdepth 2 -type d -name 'cJSON*' 2>/dev/null | head -1)"
fi
: "${CJSON_DIR:?cJSON not found; run 'pio test -e native' once to fetch it}"

if [ ! -f "$FIXTURE" ]; then
  echo "fixture $FIXTURE is missing. It is checked in; regenerate it with:" >&2
  echo "  (cd android && ./gradlew :app:testDebugUnitTest --tests '*EmitFixtureTest')" >&2
  exit 2
fi

PROBE_BIN="$(mktemp -t swc_decode_probe.XXXXXX)"
trap 'rm -f "$PROBE_BIN"' EXIT

# -w: the firmware's own sources are built with -Wall -Wextra -Werror, but this
# probe compiles them against a bare host compiler whose warnings differ from the
# ESP toolchain's. The probe's EXIT CODE is the gate; an unrelated warning is not.
c++ -std=gnu++17 -w -I lib -I "$CJSON_DIR" -DSWC_NATIVE_TEST \
    -o "$PROBE_BIN" tools/decode_probe.cpp \
    lib/Config/ConfigCodec.cpp lib/Analog/LadderDecode.cpp \
    lib/Gesture/GestureStateMachine.cpp lib/Output/GainPolicy.cpp \
    "$CJSON_DIR/cJSON.c"

"$PROBE_BIN" "$FIXTURE"
