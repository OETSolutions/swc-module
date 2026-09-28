#!/usr/bin/env python3
"""Generate the release manifest the device's OTA check consumes (spec 9.5).

**Why a committed script rather than inline `python -c` in the workflow.** The
manifest shape is parsed by BOTH `code/lib/Update/ReleaseCheck.cpp` (the device)
and `code/android/.../update/ReleaseManifest.kt` (the app), and a shape mismatch
is a silent permanent failure: `ReleaseCheckParse` returns `kMalformed` for a
missing field and the device reports "the release manifest could not be read",
naming neither the field nor the file. The rules it enforces, restated here so a
reader of one is not missing the other:

  * `latest_version` is REQUIRED and must EQUAL `firmware.version` -- a
    self-contradictory manifest is refused, not resolved.
  * `firmware.sha256`, `firmware.size_bytes`, `firmware.url` are REQUIRED.
  * `size_bytes` must be a WHOLE number (a fraction is refused, not truncated).
  * `url` must be `https://` -- a plain-http URL is a downgrade, refused.
  * `min_from_version` and `channel` are OPTIONAL.

The output is byte-stable (sorted keys, fixed separators, one trailing newline)
so a regeneration diff means a REAL change, not formatting.

Usage:
  gen_release_manifest.py --version 1.0.0 --firmware path/to/firmware.bin \\
      --url https://github.com/OWNER/REPO/releases/download/v1.0.0/firmware.bin \\
      [--min-from-version 1.0.0] [--channel stable] [--out version_manifest.json]

Writes to stdout when --out is omitted.
"""

import argparse
import hashlib
import json
import os
import sys


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True,
                    help="the release version, WITHOUT a leading 'v' (e.g. 1.0.0)")
    ap.add_argument("--firmware", required=True, help="path to firmware.bin")
    ap.add_argument("--url", required=True,
                    help="the https URL the device will download firmware.bin from")
    ap.add_argument("--min-from-version", default=None,
                    help="optional floor: a device older than this is not offered the update")
    ap.add_argument("--channel", default="stable", help="optional channel label (default stable)")
    ap.add_argument("--out", default=None, help="output path (default: stdout)")
    args = ap.parse_args()

    version = args.version.lstrip("v")
    if not version or not all(part.isdigit() for part in version.split(".")):
        print(f"ERROR: --version must look like 1.2.3 (got {args.version!r})",
              file=sys.stderr)
        return 2
    if not args.url.startswith("https://"):
        # The device refuses a non-https URL outright (ReleaseCheck.cpp's
        # IsHttpsUrl), so producing one here would be a manifest that can never
        # be installed. Fail where the mistake is made.
        print(f"ERROR: --url must be https:// (got {args.url!r})", file=sys.stderr)
        return 2
    if not os.path.isfile(args.firmware):
        print(f"ERROR: --firmware not found: {args.firmware}", file=sys.stderr)
        return 2

    size = os.path.getsize(args.firmware)
    if size <= 0 or size > 4294967295:
        # The device refuses an out-of-range size (ReadSize). 4 GiB is the ceiling.
        print(f"ERROR: firmware size {size} is out of the device's accepted range",
              file=sys.stderr)
        return 2

    manifest = {
        "latest_version": version,
        "channel": args.channel,
        "firmware": {
            "version": version,
            "url": args.url,
            "size_bytes": size,
            "sha256": sha256_file(args.firmware),
        },
    }
    if args.min_from_version:
        manifest["min_from_version"] = args.min_from_version.lstrip("v")

    text = json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
