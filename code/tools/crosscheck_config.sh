#!/usr/bin/env bash
# Prove the Kotlin codec and the C codec agree on the wire.
#
# The two sides have their own round-trip tests, and NEITHER can see this: a
# Kotlin test compares Kotlin to Kotlin, and the C tests compare C to C. A field
# name that differs between them round-trips perfectly on both sides and produces
# a config the device refuses -- the failure surfaces as "corrupt config", which
# names nothing. This script decodes the app's own output with the firmware's own
# decoder, validates it, re-encodes it, and requires the bytes to be identical.
#
# Requires: a host C++ compiler and the cJSON the firmware uses.
set -euo pipefail
cd "$(dirname "$0")/.."

FIXTURE="${1:-/tmp/swc_sample_config.json}"
CJSON_DIR="$(find .pio/libdeps/native -maxdepth 2 -type d -name 'cJSON*' 2>/dev/null | head -1)"
if [ -z "$CJSON_DIR" ]; then
  CJSON_DIR="$(find .pio/libdeps -maxdepth 2 -type d -name 'cJSON*' 2>/dev/null | head -1)"
fi
: "${CJSON_DIR:?cJSON not found; run 'pio test -e native' once to fetch it}"
CJSON_INC="$CJSON_DIR"

if [ ! -f "$FIXTURE" ]; then
  echo "fixture $FIXTURE missing; run EmitFixtureTest first" >&2
  exit 2
fi

c++ -std=gnu++17 -I lib -I "$CJSON_INC" -DSWC_NATIVE_TEST \
    -o /tmp/decode_probe /tmp/decode_probe.cpp \
    lib/Config/ConfigCodec.cpp lib/Analog/LadderDecode.cpp lib/Gesture/GestureStateMachine.cpp lib/Output/GainPolicy.cpp "$CJSON_INC/cJSON.c" 2>&1 | tail -20

/tmp/decode_probe "$FIXTURE"
