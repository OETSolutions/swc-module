#!/usr/bin/env python3
"""The required-Kconfig gate.

Fails the build if a Kconfig key that a spec requirement depends on is absent
from `sdkconfig.defaults`, or carries the wrong value.

**Why this is a gate and not a review step.** A missing Kconfig key is the one
defect class this project has no other way to catch. It is not a compile error
(an unset symbol is simply an unset symbol), not a test failure (no host test
can see the bootloader's build options), and not a runtime symptom until the
exact path that depends on it runs -- which for the rollback flag means *the
boot loop a user would hit right after an update*.

It is also a defect that has already shipped once. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
was absent: `partitions.csv` had the A/B slots, `main.cpp` called
`esp_ota_mark_app_valid_cancel_rollback()`, and the call was a documented no-op
without the bootloader option. FR-37's rollback did not exist, and *nothing
failed*. Grep for the call site looked like proof it worked.

**Why `sdkconfig.defaults` and not the generated `sdkconfig.esp32s3`.** The
generated file is a gitignored build artifact (`code/.gitignore:27`), so a key
set only there does not survive a clean build -- `rm sdkconfig.esp32s3` and the
requirement is gone again. `defaults` is the only file that is authoritative and
tracked. When the generated file IS present, its value is checked too, because a
defaults key that did not take effect is the same defect wearing a disguise.

Exit codes: 0 all present and correct, 1 a key is missing or wrong, 2 the
defaults file itself is missing.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULTS = REPO / "sdkconfig.defaults"
GENERATED = REPO / "sdkconfig.esp32s3"

# (key, required value, the requirement it carries)
REQUIRED = [
    ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", "y",
     "4 MB flash, in choice form (the string form is silently ignored)"),
    ("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG", "y",
     "console on the ROM USB-JTAG peripheral (spec 10.3)"),
    ("CONFIG_TINYUSB_CDC_ENABLED", "y",
     "the app link; without it tinyusb_cdc_acm.h refuses to compile"),
    ("CONFIG_TINYUSB_CDC_RX_BUFSIZE", "1024",
     "RX FIFO >= the 1024-byte frame cap, so one frame arrives in one pass"),
    ("CONFIG_BT_ENABLED", "y", "NimBLE BLE transport (spec 9.2)"),
    ("CONFIG_BT_NIMBLE_ENABLED", "y", "NimBLE, because Bluedroid does not fit"),
    ("CONFIG_MBEDTLS_HARDWARE_AES", "n",
     "the mbedTLS GCM/HARDWARE_AES crash (espressif/esp-idf#14298) -- the "
     "single reason this firmware is not on the stock platform"),
    ("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH", "y",
     "a coredump partition exists (spec 9.2) for post-mortem"),
    ("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE", "y",
     "FR-37 A/B rollback: without it a freshly-updated app that crash-loops "
     "is never reverted, and main.cpp's mark-valid call is a no-op"),
    ("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", "n",
     "the stock Mozilla cert BUNDLE is NOT used: the WiFi OTA trust anchor is a "
     "PINNED CA (lib/Update/ReleaseCa.h, N-62 fixed), so the ~200-root bundle is "
     "disabled and its flash is reclaimed"),
]

FORBIDDEN = [
    ("CONFIG_BT_BLUEDROID_ENABLED", "y",
     "Bluedroid does not fit 4 MB/no-PSRAM beside WiFi + OTA (spec 9.2)"),
]


def parse(path: pathlib.Path) -> dict:
    """Return {KEY: value} for a Kconfig file.

    Two forms carry a value and BOTH must be read:

      CONFIG_FOO=y            -> "y"
      # CONFIG_FOO is not set -> "n"     (how the generated sdkconfig writes n)

    The second form is a comment, so a parser that skips comments sees an `=n`
    key as absent. That is not a cosmetic difference: it makes every `=n`
    requirement fail its INERT check with a false positive -- caught by the
    guard's own mutation test, which is why this is spelled out here.
    """
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s*#\s*(CONFIG_[A-Za-z0-9_]+) is not set\s*$", line)
        if m:
            out[m.group(1)] = "n"
            continue
        line = line.split("#", 1)[0].strip()
        m = re.match(r"(CONFIG_[A-Za-z0-9_]+)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip().strip('"')
    return out


def value_of(cfg: dict, key: str) -> str | None:
    """A `# KEY is not set` line is an explicit n in the generated file."""
    if key in cfg:
        return cfg[key]
    return None


def main() -> int:
    if not DEFAULTS.exists():
        print(f"FAIL  {DEFAULTS} does not exist", file=sys.stderr)
        return 2

    defaults = parse(DEFAULTS)
    generated = parse(GENERATED) if GENERATED.exists() else None

    problems = []

    for key, want, why in REQUIRED:
        got = value_of(defaults, key)
        if got is None:
            problems.append(
                f"  MISSING  {key}={want}\n"
                f"           carries: {why}\n"
                f"           absent from sdkconfig.defaults, so the requirement "
                f"does not exist")
            continue
        if got != want:
            problems.append(
                f"  WRONG    {key}={got}, want {want}\n"
                f"           carries: {why}")
            continue
        if generated is not None:
            gen = value_of(generated, key)
            if gen != want:
                problems.append(
                    f"  INERT    {key}={want} in defaults but "
                    f"{gen if gen is not None else 'unset'} in the generated "
                    f"sdkconfig\n"
                    f"           carries: {why}\n"
                    f"           a defaults key that did not take effect is the "
                    f"same defect as a missing one")

    for key, bad, why in FORBIDDEN:
        if value_of(defaults, key) == bad:
            problems.append(
                f"  FORBIDDEN {key}={bad}\n"
                f"           {why}")

    if problems:
        print("Required-Kconfig gate FAILED:\n", file=sys.stderr)
        for p in problems:
            print(p, file=sys.stderr)
        print(
            "\nEach key above is the sole carrier of a spec requirement. Add or "
            "correct it in sdkconfig.defaults (NOT the generated sdkconfig, which "
            "is gitignored and does not survive a clean build).",
            file=sys.stderr)
        return 1

    print(
        f"Required-Kconfig gate: {len(REQUIRED)} keys present and correct"
        + ("" if generated is None else " in both defaults and the generated "
           "sdkconfig")
        + f", {len(FORBIDDEN)} forbidden key(s) absent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
