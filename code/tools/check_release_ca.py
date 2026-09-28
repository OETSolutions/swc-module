#!/usr/bin/env python3
"""Guard the pinned release CAs (N-62, N-92): they are real certificates, they are
the ones the code uses, and their expiries are visible.

**Why this exists.** N-62 was "the code says pinned CA, but uses the ~200-root
default bundle". The fix pins the release chain's roots in `lib/Update/ReleaseCa.h`.
That pin has a maintenance cost a bundle does not: it does NOT self-update, so if a
root is renewed or the release host changes CA, the fetch breaks and nothing else
notices until a user tries to update. This gate makes the pin checkable:

  1. `lib/Update/ReleaseCa.h` defines `kReleaseCaPem` and contains **exactly the two
     roots the GitHub release chain traverses**: the Sectigo root that signs
     `github.com` (hop 1) and the ISRG root that signs the `release-assets.`
     redirect target (hop 2). A single cert is the N-92 defect (the fetch dies on
     the redirect); a third cert drifts back toward a bundle, which is N-62.
  2. `OtaWifi.cpp` selects that header (not the default bundle): it references
     `kReleaseCaPem` and does NOT set `crt_bundle_attach`.
  3. Each certificate parses and its `notAfter` is far enough out that expiry is a
     planned event, not a surprise: **fails if any has expired, warns within 180
     days.** A pinned CA expiring silently is the failure this gate is for.

The identities are asserted by SUBJECT (the pin must actually be the root each hop
uses), so swapping in an unrelated-but-valid root is caught. The pure checks are
functions of the source TEXT so `test_check_release_ca.py` can drive them against
mutated copies -- a gate that only ever prints OK is indistinguishable from one
whose checks never ran.

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

# The two roots the release chain must be able to build, matched by a substring of
# the certificate SUBJECT. Hop 1 is github.com (Sectigo); hop 2 is the 302 target
# release-assets.githubusercontent.com (Let's Encrypt / ISRG).
EXPECTED_SUBJECTS = [
    "Sectigo Public Server Authentication Root E46",   # hop 1: github.com
    "ISRG Root X1",                                     # hop 2: release-assets.*
]


def extract_pems(text):
    """Return (list-of-PEM-strings, block-count) from the C string literals."""
    blocks = re.findall(r"BEGIN CERTIFICATE(.*?)END CERTIFICATE", text, re.S)
    pems = []
    for body in blocks:
        b64 = "".join(re.findall(r'([A-Za-z0-9+/=]+)\\n', body))
        if not b64:
            return [], len(blocks)
        lines = [b64[i:i + 64] for i in range(0, len(b64), 64)]
        pems.append("-----BEGIN CERTIFICATE-----\n" + "\n".join(lines) +
                    "\n-----END CERTIFICATE-----\n")
    return pems, len(blocks)


def check_pin_shape(ca_text):
    """Problems with the header's pin COUNT and parseability (no openssl needed)."""
    problems = []
    if "kReleaseCaPem" not in ca_text:
        problems.append("ReleaseCa.h does not define kReleaseCaPem")
    pems, n = extract_pems(ca_text)
    if n != len(EXPECTED_SUBJECTS):
        problems.append(
            f"ReleaseCa.h must contain exactly {len(EXPECTED_SUBJECTS)} "
            f"certificates (the two roots the release chain traverses: hop 1 "
            f"github.com, hop 2 the redirect target); found {n}. One is the N-92 "
            f"defect (the redirect hop fails TLS); more drifts toward the N-62 "
            f"bundle.")
    elif len(pems) != n:
        problems.append("ReleaseCa.h: could not parse every PEM body")
    return problems


def check_ota_uses_the_pin(ota_text):
    """Problems with how OtaWifi.cpp selects its trust anchor (no openssl needed)."""
    problems = []
    if "kReleaseCaPem" not in ota_text:
        problems.append("OtaWifi.cpp does not reference kReleaseCaPem -- the pin is "
                        "not the anchor the fetch uses")
    if "crt_bundle_attach" in ota_text:
        problems.append("OtaWifi.cpp still sets crt_bundle_attach -- the default "
                        "bundle is back, which is the N-62 defect")
    return problems


def subject_of(pem):
    out = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                         input=pem, capture_output=True, text=True)
    return out.stdout.strip()


def enddate_of(pem):
    out = subprocess.run(["openssl", "x509", "-noout", "-enddate"],
                         input=pem, capture_output=True, text=True)
    m = re.search(r"notAfter=(.+)", out.stdout)
    if not m:
        return None, None
    end = datetime.datetime.strptime(m.group(1).strip(),
                                     "%b %d %H:%M:%S %Y %Z").replace(
        tzinfo=datetime.timezone.utc)
    return end, (end - datetime.datetime.now(datetime.timezone.utc)).days


def check_identities(pems):
    """Problems with each cert's SUBJECT (must include both hops' roots) and its
    expiry. Returns (problems, notes). Needs openssl."""
    problems = []
    notes = []
    seen_subjects = []
    for pem in pems:
        subj = subject_of(pem)
        seen_subjects.append(subj)
        end, days_left = enddate_of(pem)
        if end is None:
            problems.append(f"could not parse a certificate's dates: {subj}")
            continue
        notes.append(f"{subj} expires in {days_left} days")
        if days_left < 0:
            problems.append(f"a pinned CA EXPIRED {abs(days_left)} days ago "
                            f"({end:%Y-%m-%d}) -- the release fetch will fail: {subj}")
        elif days_left < WARN_DAYS:
            notes[-1] += "  WARNING: renew the pin soon"
    for want in EXPECTED_SUBJECTS:
        if not any(want in s for s in seen_subjects):
            problems.append(f"the pinned CAs do not include '{want}' -- the release "
                            f"chain cannot be built without it")
    return problems, notes


def main() -> int:
    if not CA_H.exists():
        print(f"ERROR: {CA_H} does not exist", file=sys.stderr)
        return 1

    ca_text = CA_H.read_text()
    ota_text = OTA_CPP.read_text()
    problems = (check_pin_shape(ca_text) + check_ota_uses_the_pin(ota_text))

    pems, _ = extract_pems(ca_text)
    notes = []
    if pems and not problems:
        id_problems, notes = check_identities(pems)
        problems += id_problems

    if problems:
        print("release-CA gate: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"release-CA gate: OK -- {len(pems)} pinned roots (github.com + redirect "
          f"target), used by OtaWifi, no bundle")
    for note in notes:
        print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
