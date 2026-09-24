#!/usr/bin/env python3
"""Pin the firmware's numeric config limits to the app's mirrors.

The app's `ConfigJson.problems()` is a hand-written mirror of the firmware's
`ConfigValidate` + the fixed field widths in `ConfigModel.h`. Its own comment says
so. Nothing enforced that: `test_gen_contract.py` checks the enum/kind/param-key
tables, but the NUMERIC limits -- the counts, the string widths, and the two timing
ceilings -- had no cross-check at all.

That gap is not theoretical. The `send_duration_ms` ceiling (`kSendDurationMaxMs`)
was added to the firmware validator and to `ConfigModel.h` and NOT to the app: the
save would pass the app's local gate, reach the device, and be nacked at decode
with the offending field unnamed -- losing the whole save. It is the same shape as
the `maintenance_timeout_ms` ceiling that was caught and mirrored by hand, and the
same shape as the string widths (which the app documents as "the firmware's").

Every limit below is read from BOTH sides -- the C++ declaration and the Kotlin
`const val` -- and asserted equal. A rename or a re-tune on one side fails here
instead of surfacing as a config the device refuses.

Exit codes: 0 in agreement, 1 drift, 2 a source could not be read.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
CONFIG_MODEL = REPO / "lib" / "Config" / "ConfigModel.h"
LADDER_DECODE = REPO / "lib" / "Analog" / "LadderDecode.h"
BASE64_H = REPO / "lib" / "Util" / "Base64.h"
CONFIG_CODEC_H = REPO / "lib" / "Config" / "ConfigCodec.h"
CALIBRATION_H = REPO / "lib" / "Analog" / "CalibrationCurve.h"
OTA_USB_H = REPO / "lib" / "Update" / "OtaUsb.h"
NDJSON_H = REPO / "lib" / "Link" / "Ndjson.h"
CONFIG_KT = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "oetsolutions" / "swc" / "model" / "Config.kt"
SWC_CLIENT_KT = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "oetsolutions" / "swc" / "link" / "SwcClient.kt"
MANIFEST_FETCHER_KT = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "oetsolutions" / "swc" / "update" / "ManifestFetcher.kt"
LADDER_SCREEN_KT = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "oetsolutions" / "swc" / "ui" / "LadderScreen.kt"
LADDER_DECODE_CPP = REPO / "lib" / "Analog" / "LadderDecode.cpp"

# (label, C++ constant, Kotlin constant, C++ header). The header is explicit because
# not every width the app mirrors lives in ConfigModel.h: `LadderButton.id` is
# `kLadderIdLen` from LadderDecode.h, a SEPARATE constant from `Binding.id`'s
# `kBindingIdLen` even though both are 16 today.
#
# Every one is a value the app must refuse at the same boundary the firmware does.
# The string widths are checked as "under N chars" on the app side (Kotlin
# `length >= N`), matching `ReadStr`'s `n >= width` -- so the two are equal, not off
# by one.
PAIRS = [
    ("channel count",        "kMaxChannels",          "K_MAX_CHANNELS",          CONFIG_MODEL),
    ("binding count",        "kMaxBindings",          "K_MAX_BINDINGS",          CONFIG_MODEL),
    ("actions per binding",  "kMaxActionsPerBinding", "K_MAX_ACTIONS_PER_BINDING", CONFIG_MODEL),
    ("aux button count",     "kMaxAuxButtons",        "K_MAX_AUX_BUTTONS",       CONFIG_MODEL),
    ("action target width",  "kActionTargetLen",      "K_ACTION_TARGET_LEN",     CONFIG_MODEL),
    ("data payload width",   "kDataPayloadLen",       "K_DATA_PAYLOAD_LEN",      CONFIG_MODEL),
    # `Binding.id` and `Binding.button` are `char[kBindingIdLen]`. Both app check
    # sites -- the binding id and the ladder button id -- were a bare `16` until
    # 2026-09-23, so these two rows are what make the pairs visible and re-tunable:
    # the two drift directions are a gate that forecloses its own edit (a larger
    # firmware width -- the N-44 shape) and a send the device refuses (a smaller
    # one, which nacks the whole save with the field unnamed).
    ("binding id width",     "kBindingIdLen",         "K_BINDING_ID_LEN",        CONFIG_MODEL),
    # `LadderButton.id`, a different field with its own constant.
    ("ladder-button id width", "kLadderIdLen",        "K_LADDER_ID_LEN",         LADDER_DECODE),
    ("schema version",       "kConfigSchemaVersion",  "K_CONFIG_SCHEMA_VERSION", CONFIG_MODEL),
    ("maintenance ceiling",  "kMaintenanceTimeoutMaxMs", "K_MAINTENANCE_TIMEOUT_MAX_MS", CONFIG_MODEL),
    ("send-duration ceiling", "kSendDurationMaxMs",   "K_SEND_DURATION_MAX_MS",  CONFIG_MODEL),
    # The two WIRE constants, which live in `SwcClient.kt` rather than `Config.kt`
    # and so were invisible to this tool while it read one Kotlin file. Both are
    # hand-mirrored and both fail silently in one direction:
    #
    #   - `CHUNK_BYTES` too LARGE overruns the device's NDJSON line cap
    #     (`kNdjsonMaxFrame`, 1024 B): a 512-byte chunk base64s to 684 characters,
    #     and a bigger one stops fitting the frame the reader will accept, so the
    #     device drops the line. `kNdjsonMaxFrame` is read from `Ndjson.h` so the
    #     pair is pinned, not the lone app-side number.
    #   - `kWireConfigMaxBytes` too SMALL makes the app refuse a config the device
    #     would accept (`ConfigMaxSerializedSize()` is a function, so its literal
    #     is read from the `inline constexpr` body).
    ("wire config chunk",    "kConfigWireChunkBytes", "CHUNK_BYTES",             BASE64_H),
    ("wire config ceiling",  "ConfigMaxSerializedSize", "kWireConfigMaxBytes",   CONFIG_CODEC_H),
    # The firmware image slot, which the app's USB-OTA push mirrors (spec §9.3).
    # Too SMALL and the app refuses an image the device would accept; too LARGE and
    # it streams megabytes the device rejects at `ota_begin`. The firmware constant
    # moved to `OtaUsb.h` from a literal when USB OTA was wired (N-14).
    ("firmware slot",        "kAppSlotBytes",         "kWireFirmwareMaxBytes",   OTA_USB_H),
    # The inbound NDJSON line cap. The app's `lineCap` carries the comment
    # "// kNdjsonMaxFrame, spec 4.2" -- a hand-mirror with nothing comparing it, so
    # a firmware re-tune of the cap would leave the app refusing lines the device
    # still emits (or, worse, accumulating a partial line the device no longer
    # sends whole). A plain `val`, not a `const val`, so the matcher allows both.
    ("ndjson line cap",      "kNdjsonMaxFrame",       "lineCap",                 NDJSON_H),
    # The release-image download bound (spec §9.5 step 5, open item N-12). A SECOND
    # copy of the app slot: `HttpImageDownloader` caps the response it will hold in
    # the phone's memory at the same figure `SwcClient` refuses to push, so an
    # oversize response is stopped before it fills memory rather than after. Two
    # hand-written copies of the slot size with nothing comparing them is the exact
    # drift this tool exists for, so it is pinned here too.
    ("image download cap",   "kAppSlotBytes",         "kMaxImageBytes",          OTA_USB_H),
    # The ladder-geometry bounds the app's validator now mirrors (open item N-40).
    # The app's `ConfigJson.problems()` is a hand-written copy of `ConfigValidate`
    # + `LadderProfileIsValid`, and these four facts -- two string widths, the ADC
    # ceiling the geometry is measured against, and the button-count ceiling -- had
    # NO pair here and no app check at all, so the app accepted a config the device
    # refused at decode. `check_app_limits.py` cannot see the RULE level (that is
    # `ConfigCodecTest`'s job); it can see that the CONSTANTS the rules use agree,
    # which is what these rows pin.
    ("channel-name width",   "kChannelNameLen",       "K_CHANNEL_NAME_LEN",      CONFIG_MODEL),
    ("device-id width",      "kDeviceIdLen",          "K_DEVICE_ID_LEN",         CONFIG_MODEL),
    ("adc ceiling",          "kAdcFullScaleMv12dB",   "K_ADC_CEILING_MV",        CALIBRATION_H),
    ("ladder button max",    "kLadderMaxButtons",     "K_LADDER_MAX_BUTTONS",    LADDER_DECODE),
    # FR-30's rail-health floor. The app now MIRRORS the firmware's sag test so its
    # ladder screen can say "rail fault" (open item N-77), which makes this a
    # threshold with two homes: a re-tune on one side would let the screen report
    # "healthy" while the device is faulting, or the reverse. The C++ side is a
    # `constexpr` in the .cpp (not the header), so the pair reads the .cpp.
    ("rail-health floor",    "kRailHealthFloorPermille", "kRailHealthFloorPermille", LADDER_DECODE_CPP),
]


def _cpp_constant(text, name):
    """The integer value of `constexpr ... name = <value>;`.

    Also reads an `inline constexpr <type> Name() { return <value>; }` body, which
    is how `ConfigMaxSerializedSize()` is declared -- a function rather than a
    value, so a plain `name = value` scan would not find it.
    """
    m = re.search(rf"\b{re.escape(name)}\s*=\s*([0-9_]+)\s*[uUlL]*\s*;", text)
    if m:
        return int(m.group(1).replace("_", ""))
    m = re.search(rf"\b{re.escape(name)}\s*\(\s*\)\s*(?:const\s*)?\{{[^}}]*?return\s+([0-9_]+)",
                  text, re.S)
    if m:
        return int(m.group(1).replace("_", ""))
    return None


def _kt_constant(text, name):
    """The integer value of a Kotlin `const val`/`val name = <value>`.

    A `private companion object` member and a class-body `val` are both plain
    declarations in the source, so the `const` is optional -- `SwcClient.kt`'s
    `lineCap` is a `val`, its wire constants are `const val`.
    """
    m = re.search(
        rf"\b(?:const\s+)?val\s+{re.escape(name)}\s*(?::\s*[\w<>?]+\s*)?=\s*([0-9_]+)",
        text,
    )
    if not m:
        return None
    return int(m.group(1).replace("_", ""))


def main() -> int:
    for p in (CONFIG_MODEL, LADDER_DECODE, LADDER_DECODE_CPP, BASE64_H, CONFIG_CODEC_H,
              NDJSON_H, OTA_USB_H, CALIBRATION_H, CONFIG_KT, SWC_CLIENT_KT,
              MANIFEST_FETCHER_KT, LADDER_SCREEN_KT):
        if not p.is_file():
            print(f"FAIL: {p} not found", file=sys.stderr)
            return 2

    cpp_by_file = {
        CONFIG_MODEL: CONFIG_MODEL.read_text(),
        LADDER_DECODE: LADDER_DECODE.read_text(),
        BASE64_H: BASE64_H.read_text(),
        CONFIG_CODEC_H: CONFIG_CODEC_H.read_text(),
        NDJSON_H: NDJSON_H.read_text(),
        OTA_USB_H: OTA_USB_H.read_text(),
        CALIBRATION_H: CALIBRATION_H.read_text(),
        LADDER_DECODE_CPP: LADDER_DECODE_CPP.read_text(),
    }
    kt_by_file = {
        CONFIG_KT: CONFIG_KT.read_text(),
        SWC_CLIENT_KT: SWC_CLIENT_KT.read_text(),
        MANIFEST_FETCHER_KT: MANIFEST_FETCHER_KT.read_text(),
        LADDER_SCREEN_KT: LADDER_SCREEN_KT.read_text(),
    }

    def kt_find(name):
        """The Kotlin constant, searched across every app source this tool reads.

        Returned with the file it came from so a rename is reported against the
        right one -- the two wire constants live in `SwcClient.kt`, not `Config.kt`.
        """
        for path, text in kt_by_file.items():
            v = _kt_constant(text, name)
            if v is not None:
                return v, path.name
        return None, None

    drift = []
    for label, c_name, k_name, c_file in PAIRS:
        cpp = cpp_by_file[c_file]
        c_val = _cpp_constant(cpp, c_name)
        k_val, k_file = kt_find(k_name)
        if c_val is None:
            drift.append(f"{label}: {c_name} not found in {c_file.name}")
            continue
        if k_val is None:
            drift.append(f"{label}: {k_name} not found in any app source")
            continue
        if c_val != k_val:
            drift.append(
                f"{label}: firmware {c_name}={c_val} but app {k_name}={k_val} ({k_file})"
            )
            continue
        print(f"OK  {label:22s} {c_name} == {k_name} == {c_val}")

    if drift:
        print("\nFAIL: the app's numeric limits have drifted from the firmware's.\n"
              "The app is a MIRROR of ConfigValidate/ConfigModel.h; a limit that"
              " differs lets a config pass the app's gate and be nacked by the"
              " device at decode, losing the whole save with the field unnamed.\n",
              file=sys.stderr)
        for d in drift:
            print(f"  - {d}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
