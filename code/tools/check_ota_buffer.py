#!/usr/bin/env python3
"""Guard the OTA HTTP client buffers (N-92): every `esp_http_client_config_t` in
`OtaWifi.cpp` must size `buffer_size_tx` for a redirect to a signed CDN URL.

**Why this exists.** N-92 was "the WiFi OTA check works over a tunnel and fails
against the real GitHub release host". The mechanism: a GitHub `releases/...`
URL answers 302 and redirects to `release-assets.githubusercontent.com` with a
signed query string, so the request first line (`GET <path>?<sig> HTTP/1.1`) is
~900 bytes. `esp_http_client` renders that first line into `buffer_size_tx`,
whose default is 512, and `http_client_prepare_first_line` returns -1 ("Out of
buffer") when it does not fit -- surfacing as a bare `ESP_FAIL` on the redirect
hop. The IMAGE config set `buffer_size_tx = 1024`; the CHECK config set neither,
so only the check failed, and only against the real host (a quick tunnel serves
one short hop and never exercised it).

**The invariant, checked here:** every `esp_http_client_config_t` block in
`OtaWifi.cpp` sets `buffer_size_tx` to at least `MIN_TX`. A config that omits it
inherits the 512-byte default, which a real release redirect cannot fit. This is
source-level because `OtaWifi.cpp` is `ESP_PLATFORM`-gated: no native test
compiles it, and the failure needs the real host, so nothing else can see it.

Exit codes: 0 clean, 1 a real defect, 2 environment.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
OTA_CPP = REPO / "lib" / "Update" / "OtaWifi.cpp"

# A GitHub release redirect's first line is ~900 bytes; 1024 is the smallest round
# value with margin. 512 (IDF's default) cannot fit it.
MIN_TX = 1024


def config_blocks(text):
    """Return the text of each `esp_http_client_config_t ... = {};` ... assignment
    region in the file, i.e. from the declaration to the next `esp_http_client_init`
    (or end of function)."""
    blocks = []
    for m in re.finditer(r"esp_http_client_config_t\s+\w+\s*=\s*\{\s*\};", text):
        start = m.end()
        nxt = text.find("esp_http_client_init", start)
        blocks.append(text[start: nxt if nxt != -1 else len(text)])
    return blocks


def check_http_client_buffers(text):
    """Problems with each HTTP client config's TX buffer sizing."""
    problems = []
    blocks = config_blocks(text)
    if not blocks:
        problems.append("OtaWifi.cpp: found no esp_http_client_config_t blocks -- "
                        "the check is looking at the wrong file or shape")
        return problems
    for i, b in enumerate(blocks):
        m = re.search(r"\.?buffer_size_tx\s*=\s*(\d+)", b)
        if not m:
            problems.append(
                f"OtaWifi.cpp http config #{i + 1} does not set buffer_size_tx: it "
                f"inherits IDF's {512}-byte default, which cannot fit the ~900-byte "
                f"first line of a GitHub release redirect (N-92). Set it >= {MIN_TX}.")
            continue
        val = int(m.group(1))
        if val < MIN_TX:
            problems.append(
                f"OtaWifi.cpp http config #{i + 1} sets buffer_size_tx={val}, below "
                f"{MIN_TX}: a release redirect's first line will not fit (N-92).")
    return problems


def main() -> int:
    if not OTA_CPP.exists():
        print(f"ERROR: {OTA_CPP} does not exist", file=sys.stderr)
        return 2
    problems = check_http_client_buffers(OTA_CPP.read_text(encoding="utf-8"))
    if problems:
        print("OTA-buffer gate: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    n = len(config_blocks(OTA_CPP.read_text(encoding="utf-8")))
    print(f"OTA-buffer gate: OK -- {n} HTTP client config(s) size buffer_size_tx "
          f">= {MIN_TX}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
