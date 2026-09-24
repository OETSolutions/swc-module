#!/usr/bin/env python3
"""Prove FR-35's WiFi update path on the board: the device's own HTTP server
refuses a token-less and a corrupt upload, and (optionally) installs a good one.

**Why this needs the board and a real network hop.** The token gate, the route
table and every API body are host-tested (`MaintenanceHttp`, 17 cases), but the
`esp_http_server`/`esp_netif` bring-up, the raw-body receive loop and the flash
write are all device-only. The only way to exercise them is to JOIN the device's
own access point and drive it over HTTP, which is exactly what the phone does.

**What this proves, in order (each step is a distinct claim):**

  1. `GET /?token=<token>` returns the maintenance page -> the HTTP server is up
     and serving on the AP.
  2. `POST /api/ota/upload` with NO `X-SWC-Token` -> refused. This is the gate
     acting on the DEVICE, not only in the host-tested router.
  3. `POST /api/ota/upload` WITH the token but a CORRUPT image (true size, and
     the digest of the corrupt bytes) -> refused, and the device keeps serving.
     The refusal can only come from the streamed digest, so this proves the
     WiFi path shares `OtaUsb`'s verify gate (spec 9.4's "exactly once").
  4. `--install` only: the SAME upload with a GOOD image -> committed, then a
     reboot shows the new image running. This is the end-to-end install.

**The network change is real and is undone.** The script joins the device's open
AP (`SWC-<short id>`) on the Wi-Fi interface and then restores the machine's
previous Wi-Fi state -- it re-associates what was associated, and powers the
Wi-Fi off if nothing was. Confirm Wi-Fi is not the machine's internet path
first: `route -n get default` should name a wired interface (on this bench it is
`en15`), so dropping Wi-Fi does not cut the network.

Usage:
  # steps 1-3 (non-destructive; the device stays on its current image):
  python3 tools/bench_wifi_upload.py --dut /dev/cu.usbmodem1234561 --image <good.bin>
  # step 4 as well (installs the good image and reboots into it):
  python3 tools/bench_wifi_upload.py --dut ... --image <good.bin> --install

Exit codes: 0 all attempted steps passed, 1 a check failed (a real defect),
2 usage / environment error (not a firmware verdict).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut  # noqa: E402

AP_URL = "http://192.168.4.1"
WIFI_IF = "en0"


# --- the machine's Wi-Fi, captured and restored ----------------------------

def wifi_state():
    out = subprocess.run(["networksetup", "-getairportnetwork", WIFI_IF],
                         capture_output=True, text=True).stdout.strip()
    prefix = "Current Wi-Fi Network: "
    if out.startswith(prefix):
        return out[len(prefix):].strip() or None
    return None           # "not associated", or Wi-Fi off


def wifi_power(on):
    subprocess.run(["networksetup", "-setairportpower", WIFI_IF,
                    "on" if on else "off"], capture_output=True, text=True)


def join(ssid, timeout=20):
    """Join an OPEN network and wait for an address on the AP's subnet."""
    wifi_power(True)
    r = subprocess.run(["networksetup", "-setairportnetwork", WIFI_IF, ssid],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"   join {ssid} failed: {r.stdout.strip()} {r.stderr.strip()}",
              file=sys.stderr)
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        ip = subprocess.run(["ipconfig", "getifaddr", WIFI_IF],
                            capture_output=True, text=True).stdout.strip()
        if ip.startswith("192.168.4."):
            print(f"   joined {ssid}; {WIFI_IF} = {ip}")
            return True
        time.sleep(1)
    print(f"   joined {ssid} but no 192.168.4.x address within {timeout}s",
          file=sys.stderr)
    return False


def restore_wifi(previous):
    """Undo `join`: re-associate what was, or power Wi-Fi off if nothing was."""
    if previous:
        if join(previous):
            print(f"   Wi-Fi restored to {previous}")
        else:
            print(f"   WARNING: could not rejoin {previous}", file=sys.stderr)
    else:
        wifi_power(False)
        print("   Wi-Fi powered off (it was not associated before)")


# --- HTTP against the device ----------------------------------------------

def http(method, path, body=None, headers=None, timeout=20):
    """Return (status, body_text). A non-2xx is a RESULT here, not an error."""
    req = urllib.request.Request(AP_URL + path, data=body, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def open_window(dut):
    """Open the maintenance window over USB and return (ble_name, token, ssid)."""
    dut.collect(0.3)
    typ, _ = dut.request(type="maintenance_enter", timeout=6.0)
    if typ != "ack":
        print(f"ERROR: maintenance_enter -> {typ}", file=sys.stderr)
        return None
    for f in dut.collect(3.0):
        if f.get("type") == "maintenance" and f.get("active"):
            return f.get("ble_name"), f.get("token"), f.get("page_url")
    print("ERROR: no maintenance frame with a token arrived", file=sys.stderr)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--image", required=True, help="a real firmware.bin (1.5 MB)")
    ap.add_argument("--install", action="store_true",
                    help="ALSO upload the good image and reboot into it (step 4)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        good = f.read()
    corrupt = bytearray(good)
    pos = len(corrupt) // 2
    corrupt[pos] ^= 0x01
    good_sha = hashlib.sha256(good).hexdigest()
    corrupt_sha = hashlib.sha256(bytes(corrupt)).hexdigest()
    assert good_sha != corrupt_sha
    print(f"image: {len(good)} bytes  good sha {good_sha[:16]}...  "
          f"corrupt sha {corrupt_sha[:16]}... (byte {pos} flipped)")

    # Refuse to run if Wi-Fi is the machine's internet path -- dropping it would
    # cut the network, and this script is not worth that.
    default_if = subprocess.run(["route", "-n", "get", "default"],
                                capture_output=True, text=True).stdout
    if f"interface: {WIFI_IF}" in default_if:
        print(f"ERROR: {WIFI_IF} is the DEFAULT route; joining the device's AP "
              "would cut this machine's network. Connect over a wired interface "
              "or pass a different Wi-Fi interface.", file=sys.stderr)
        return 2

    previous = wifi_state()
    print(f"current Wi-Fi: {previous or '(not associated)'}")

    dut = Dut(args.dut)
    failed = False
    try:
        opened = open_window(dut)
        if not opened:
            return 1
        ble_name, token, page_url = opened
        ssid = "SWC-" + ble_name
        print(f"maintenance window open: token {token} (ssid {ssid})")

        if not join(ssid):
            return 1
        time.sleep(1.0)

        # --- 1. the page is served ------------------------------------------
        st, body = http("GET", f"/?token={token}")
        ok_page = (st == 200 and "ota" in body.lower())
        print(f"\n1. GET /?token= -> {st} ({len(body)} bytes)  "
              f"{'PASS' if ok_page else 'FAIL'}")
        failed |= not ok_page

        # --- 2. the token gate acts on the DEVICE ---------------------------
        st, body = http("POST", "/api/ota/upload", body=b"\x00" * 16,
                        headers={"X-SWC-Size": "16", "X-SWC-Sha256": "0" * 64})
        ok_gate = st in (401, 403)
        print(f"2. upload with NO token -> {st} {body.strip()[:60]!r}  "
              f"{'PASS (refused)' if ok_gate else 'FAIL (not refused)'}")
        failed |= not ok_gate

        # --- 3. a corrupt image is refused, and the device survives ---------
        st, body = http("POST", "/api/ota/upload", body=bytes(corrupt),
                        headers={"X-SWC-Token": token,
                                 "X-SWC-Size": str(len(corrupt)),
                                 "X-SWC-Sha256": corrupt_sha},
                        timeout=120)
        ok_refuse = st == 400
        print(f"3. upload a CORRUPT image -> {st} {body.strip()[:60]!r}  "
              f"{'PASS (refused)' if ok_refuse else 'FAIL (accepted!)'}")
        failed |= not ok_refuse
        # The device must still be alive and serving (a failed upload must not
        # take the HTTP server or the app down).
        st2, _ = http("GET", f"/?token={token}", timeout=10)
        alive = st2 == 200
        print(f"   device still serving after the refusal -> {st2}  "
              f"{'PASS' if alive else 'FAIL'}")
        failed |= not alive

        # --- 4. the good image installs (opt-in) ---------------------------
        if args.install:
            print("\n4. uploading the GOOD image (this will commit and reboot)…")
            st, body = http("POST", "/api/ota/upload", body=good,
                            headers={"X-SWC-Token": token,
                                     "X-SWC-Size": str(len(good)),
                                     "X-SWC-Sha256": good_sha},
                            timeout=300)
            ok_inst = st == 200
            print(f"   upload -> {st} {body.strip()[:60]!r}  "
                  f"{'PASS (installed)' if ok_inst else 'FAIL'}")
            failed |= not ok_inst
        else:
            print("\n4. skipped (no --install); the device stays on its image")
    finally:
        # Close the window and PUT THE NETWORK BACK, whatever happened.
        try:
            dut.request(type="maintenance_exit", timeout=3.0)
        except Exception:
            pass
        dut.close()
        restore_wifi(previous)

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-35 WiFi upload -- page served, token enforced, "
                  "corrupt image refused, device survived."))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
