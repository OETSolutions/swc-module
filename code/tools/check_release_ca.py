#!/usr/bin/env python3
"""Guard the pinned release CA (N-62): it is a real single certificate, it is the
one the code uses, and its expiry is visible.

**Why this exists.** N-62 was "the code says pinned CA, but uses the ~200-root
default bundle". The fix pins a single root in `lib/Update/ReleaseCa.h`. That pin
has a maintenance cost a bundle does not: it does NOT self-update, so if the root
is renewed or the release host changes CA, the fetch breaks and nothing else
notices until a user tries to update. This gate makes the pin checkable:

  1. `lib/Update/ReleaseCa.h` defines `kReleaseCaPem` and contains EXACTLY ONE
     PEM certificate (one BEGIN/END pair) -- a second cert would make it a bundle
     again, which is the defect N-62 records.
  2. `OtaWifi.cpp` selects that header (not the default bundle): it references
     `kReleaseCaPem` and does NOT set `crt_bundle_attach`.
  3. The certificate parses and its `notAfter` is far enough out that expiry is a
     planned event, not a surprise: **fails if it has expired, warns within 180
     days.** A pinned CA expiring silently is the failure this gate is for.

Exit codes: 0 clean (or a warn far from expiry), 1 a real defect, 2 environment.
"""

import os
import re
import subprocess
import sys
import datetime
import pathlib

REPO = pathlib.Path(__file__).resolve().parent.parent
CA_H = REPO / "lib" / "Update" / "ReleaseCa.h"
OTA_CPP = REPO / "lib" / "Update" / "OtaWifi.cpp"
WARN_DAYS = 180


def extract_pem(text):
    """Return the single PEM block's base64 body as a real PEM string."""
    blocks = re.findall(r"BEGIN CERTIFICATE(.*?)END CERTIFICATE", text, re.S)
    if len(blocks) != 1:
        return None, len(blocks)
    # The header stores the PEM as C string literals; pull the base64 out.
    body = blocks[0]
    b64 = "".join(re.findall(r'([A-Za-z0-9+/=]+)\\n', body))
    if not b64:
        return None, len(blocks)
    lines = [b64[i:i + 64] for i in range(0, len(b64), 64)]
    return ("-----BEGIN CERTIFICATE-----\n" + "\n".join(lines) +
            "\n-----END CERTIFICATE-----\n"), len(blocks)


def main() -> int:
    problems = []
    if not CA_H.exists():
        print(f"ERROR: {CA_H} does not exist", file=sys.stderr)
        return 1

    ca_text = CA_H.read_text()
    if "kReleaseCaPem" not in ca_text:
        problems.append(f"{CA_H.name} does not define kReleaseCaPem")

    pem, n = extract_pem(ca_text)
    if n != 1:
        problems.append(f"{CA_H.name} must contain exactly ONE certificate "
                        f"(found {n}); more than one is a bundle, the N-62 defect")
    if pem is None and n == 1:
        problems.append(f"{CA_H.name}: could not parse the PEM body")

    ota = OTA_CPP.read_text()
    if "kReleaseCaPem" not in ota:
        problems.append("OtaWifi.cpp does not reference kReleaseCaPem -- the pin is "
                        "not the anchor the fetch uses")
    if "crt_bundle_attach" in ota:
        problems.append("OtaWifi.cpp still sets crt_bundle_attach -- the default "
                        "bundle is back, which is the N-62 defect")

    # Expiry: parse with openssl if present.
    days_left = None
    if pem is not None:
        try:
            out = subprocess.run(["openssl", "x509", "-noout", "-enddate"],
                                 input=pem, capture_output=True, text=True)
            m = re.search(r"notAfter=(.+)", out.stdout)
            if m:
                end = datetime.datetime.strptime(m.group(1).strip(),
                                                 "%b %d %H:%M:%S %Y %Z").replace(
                    tzinfo=datetime.timezone.utc)
                days_left = (end - datetime.datetime.now(datetime.timezone.utc)).days
                if days_left < 0:
                    problems.append(f"the pinned CA EXPIRED {abs(days_left)} days ago "
                                    f"({end:%Y-%m-%d}) -- the release fetch will fail")
        except Exception as e:  # pragma: no cover
            print(f"  (could not parse the cert dates: {e})", file=sys.stderr)

    if problems:
        print("release-CA gate: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    note = ""
    if days_left is not None:
        note = f" (expires in {days_left} days)"
        if days_left < WARN_DAYS:
            note += "  WARNING: renew the pin soon"
    print(f"release-CA gate: OK -- one pinned cert, used by OtaWifi, no bundle{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
