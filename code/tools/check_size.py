#!/usr/bin/env python3
"""The app-slot size gate (spec 10.5, mitigation R-1).

Fails the build if the firmware image would not fit its OTA slot.

**Why this is a gate and not a review step.** The `app0`/`app1` slots are
1,920 KB each (`partitions.csv`; 0x1E0000, the largest 64 KB-aligned size that
fits beside a 64 KB coredump). Exceeding one means the device cannot be
OTA-updated -- and the failure appears when a user tries to update, not when the
code is written. Catching it on every push is the R-1 mitigation: the app not
fitting is discovered on day one rather than the week the boards land.

**What is measured, and why it is `firmware.bin`.** That is the file actually
written to the slot. The ELF's `text`+`data` is a different number (it excludes
the ELF's own overhead and any padding), and `-t size`'s output is not
machine-readable in this PlatformIO -- there is no `--json-output`, which an
earlier revision of the plan assumed. Measuring the artifact removes every
conversion in between.

**The gate must not measure a TEST image.** A device build (`pio run -e esp32s3`)
and a device TEST build (`pio test -e esp32s3`) write to the SAME
`.pio/build/esp32s3/firmware.bin`, because they share one env and therefore one
build directory. Whichever ran last is what this script reads. CI ran the build,
then the device-suite link, then this gate -- so it measured the TEST image
(~287 KB) instead of the production one (~386 KB), under-reporting by ~99 KB and
ready to pass a production image ~99 KB over its slot. That is worse than no gate:
it is a gate that reports a confident number for the wrong file. The images are
distinguishable (the test image links the Unity runner and none of the app's own
symbols), so this script now REFUSES to report unless the artifact it read is a
production image -- see `is_unity_test_image`. The CI step order is also fixed so
the production build is the last word before the measurement, but the check here
is what makes the mistake impossible to make silently rather than merely unlikely.

Exit codes: 0 fits, 1 over budget, 2 the artifact is missing or is not a
production image.
"""

import argparse
import csv
import pathlib
import sys

# The default is the real number, not a round one. It is the app slot from
# partitions.csv, and it is deliberately NOT 4 MB (the flash size) -- the image
# must fit ONE slot for OTA to work, and the two slots plus otadata plus the
# coredump are why the usable ceiling is 1,920 KB.
DEFAULT_MAX = 1_966_080

# Symbol names that exist ONLY in a Unity test image. `UnityDefaultTestRun` and
# `UnityBegin` are the runner's own entry points, and no production translation
# unit references them. Matching the NAME (not a raw byte pattern) is what makes
# this a fact about the symbols rather than about string-table layout.
UNITY_SYMBOLS = (b"UnityDefaultTestRun", b"UnityBegin", b"UnityConcludeTest")


def is_unity_test_image(elf: pathlib.Path) -> bool:
    """Does this ELF link Unity's test runner, i.e. is it a device TEST image?

    Returns False when there is no ELF to inspect. The primary product of the
    build is `firmware.bin`, and refusing to report a size merely because a
    sibling ELF is absent would break a legitimate size-only invocation; the
    check exists to catch the shared-path clobber, and in the CI flow (and in
    every `pio run`/`pio test` flow) the ELF is always present beside the bin.
    """
    if not elf.is_file():
        return False
    blob = elf.read_bytes()
    return any(sym in blob for sym in UNITY_SYMBOLS)


def app_slot_bytes(partitions_csv: pathlib.Path) -> int | None:
    """Read the app slot size from the partition table, if there is one.

    Reading it rather than hardcoding is what keeps the gate honest when the
    table changes: a partition edit that shrinks the slot must not leave a gate
    that still passes at the old size.
    """
    if not partitions_csv.is_file():
        return None
    sizes = []
    with partitions_csv.open() as f:
        for row in csv.reader(
            line for line in f if line.strip() and not line.lstrip().startswith("#")
        ):
            if len(row) < 5:
                continue
            name, ptype, _subtype, _offset, size = (c.strip() for c in row[:5])
            if ptype == "app" and name.startswith("app"):
                sizes.append(int(size, 0))
    # All app slots must be equal for an A/B update to work, so the SMALLEST is
    # the real budget. A larger slot elsewhere must not raise the ceiling.
    return min(sizes) if sizes else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default=".pio/build/esp32s3/firmware.bin",
                    help="the image that is written to the slot")
    ap.add_argument("--max-bytes", type=int, default=None,
                    help="override the budget (default: the app slot size)")
    ap.add_argument("--partitions", default="partitions.csv")
    args = ap.parse_args()

    image = pathlib.Path(args.bin)
    if not image.is_file():
        print(f"size gate: {image} not found -- build first", file=sys.stderr)
        return 2

    # A TEST image means the device-suite link ran after the build and clobbered
    # the shared artifact. Refuse rather than report a number for the wrong file.
    elf = image.with_suffix(".elf")
    if is_unity_test_image(elf):
        print(
            f"size gate: {image} is a TEST image (it links Unity's runner), not "
            "the production firmware.\n"
            "`pio test -e esp32s3` and `pio run -e esp32s3` share "
            f"{image.parent}, so the test link overwrote the build output.\n"
            "Re-run `pio run -e esp32s3` after the test link so the measured "
            "artifact is the production image.",
            file=sys.stderr,
        )
        return 2

    size = image.stat().st_size
    slot = args.max_bytes or app_slot_bytes(pathlib.Path(args.partitions)) or DEFAULT_MAX

    pct = 100.0 * size / slot
    verdict = "OK" if size <= slot else "OVER BUDGET"
    print(f"size gate: {image} is {size:,} bytes")
    print(f"           slot budget {slot:,} bytes -> {pct:.1f}% [{verdict}]")

    if size > slot:
        print(
            f"\nFAIL: the image is {size - slot:,} bytes over the app slot.\n"
            "The device could not be OTA-updated. Per spec 9.6's fallback, reduce\n"
            "the feature set rather than raising this threshold -- the slot size is\n"
            "a hardware/partition constraint, and raising it means a smaller\n"
            "coredump or no A/B slot at all.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
