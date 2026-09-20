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

Exit codes: 0 fits, 1 over budget, 2 the artifact is missing.
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
