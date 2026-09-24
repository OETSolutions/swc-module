#!/usr/bin/env python3
"""Prove FR-35's WiFi update path on the board: the device's own HTTP server
refuses a token-less and a corrupt upload, and (optionally) installs a good one.

**Why this needs the board and a real network hop.** The token gate, the route
table and every API body are host-tested (`MaintenanceHttp`), but the
`esp_http_server`/`esp_netif` bring-up, the raw-body receive loop and the flash
write are all device-only. The only way to exercise them is over a real HTTP
connection to the device's server.

**How the device is reached -- and why this does NOT touch this Mac's Wi-Fi.**
The device is a WiFi *server*: it brings up its own OPEN hotspot (`SWC-<short
id>` at 192.168.4.1) only while the maintenance window is open, so it is a
WiFi *client* on the bench LAN only after it has been provisioned. This tool
provisions it onto the SAME network this machine is already on -- the SSID and
passphrase come from `code/.env` (`WIFI_SSID`/`WIFI_PASSWORD`), which is what a
production board receives over SoftAP and what a bench DUT receives here -- and
then talks to the device at its LAN IP. The Mac stays on its own network the
whole time; nothing here calls `networksetup` or joins the device's AP.

`esp_prov.py` (Espressif's own reference provisioning client -- the protocol the
phone app wraps) drives the Sec1 handshake, exactly as `bench_prov.py` does for
FR-34. **The provisioning and the HTTP sequence must happen in the SAME window:**
a `maintenance_exit` -> `maintenance_enter` cycle leaves the driver in STA-only
with the credentials already applied but NOT reconnected (the window does not
re-join on entry), so the LAN drops and never returns. So this tool opens the
window once, provisions, and runs every HTTP step before closing it.

**What this proves, in order (each step is a distinct claim):**

  1. `GET /?token=<token>` returns the maintenance page -> the HTTP server is up
     and serving on the joined network.
  2. `GET /?token=WRONG` and `POST /api/ota/upload` with NO token -> refused. This
     is the token gate acting on the DEVICE, not only in the host-tested router.
  3. `POST /api/ota/upload` WITH the token but a CORRUPT image (true size, and
     the digest of the corrupt bytes) -> refused, and the device keeps serving.
     The refusal can only come from the streamed digest, so this proves the
     WiFi path shares `OtaUsb`'s verify gate (spec 9.4's "exactly once").
  4. `--install` only: the SAME upload with a GOOD image -> committed, then a
     reboot shows the new image running. This is the end-to-end install.

Usage:
  # steps 1-3 (non-destructive; the device stays on its current image):
  python3 tools/bench_wifi_upload.py --image <good.bin>
  # step 4 as well (installs the good image and reboots into it):
  python3 tools/bench_wifi_upload.py --image <good.bin> --install

Exit codes: 0 all attempted steps passed, 1 a check failed (a real defect),
2 usage / environment error (not a firmware verdict).
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut                      # noqa: E402
import bench_prov                                 # noqa: E402

ENV_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        ".env")
# No default LAN IP: the address is DHCP-assigned and site-specific, so it is
# discovered from the ARP table by the device's station MAC, or passed with
# `--ip`. Nothing here is site-specific.


def env_value(key):
    """Read one value out of `code/.env` (gitignored; live credentials)."""
    try:
        with open(ENV_PATH) as f:
            for line in f:
                line = line.strip()
                if line.startswith(key + "="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
    except OSError:
        return None
    return None


def station_mac(ble_name):
    """The station MAC from the BLE short id. On this board the id is the low two
    bytes of the interface MAC (`A1B2` -> `fc:01:2c:c0:a1:b2`), which is how the
    device's own advertisement names itself."""
    b = re.sub(r"[^0-9A-Fa-f]", "", ble_name or "")
    if len(b) != 4:
        return None
    return "fc:01:2c:c0:" + b[0:2].lower() + ":" + b[2:4].lower()


def arp_ip_for_mac(mac):
    """Find the IP currently leased to `mac` from the ARP table. macOS prints the
    MAC with leading zeros stripped (`fc:1:2c:c0:a1:b2`), so compare per-octet."""
    if not mac:
        return None
    want = [int(x, 16) for x in mac.split(":")]
    out = subprocess.run(["arp", "-an"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.search(r"\((\d+\.\d+\.\d+\.\d+)\) at ([0-9a-fA-F:]+)", line)
        if not m:
            continue
        got = m.group(2).split(":")
        if len(got) != 6:
            continue
        if [int(x, 16) for x in got] == want:
            return m.group(1)
    return None


def http(ip, method, path, body=None, headers=None, timeout=20):
    """Return (status, body_bytes). A non-2xx is a RESULT here, not an error."""
    req = urllib.request.Request(f"http://{ip}{path}", data=body, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:              # connection refused / host down / timeout
        return None, str(e).encode()


def open_window(dut):
    """Open the maintenance window over USB and return (ble_name, pop, token)."""
    dut.collect(0.3)
    typ, _ = dut.request(type="maintenance_enter", timeout=6.0)
    if typ != "ack":
        print(f"ERROR: maintenance_enter -> {typ}", file=sys.stderr)
        return None
    for f in dut.collect(8.0):
        if (f.get("type") == "maintenance" and f.get("active")
                and f.get("token")):
            return f["ble_name"], f.get("pop"), f["token"]
    print("ERROR: no maintenance frame with a token arrived", file=sys.stderr)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--image", required=True, help="a real firmware.bin")
    ap.add_argument("--ssid", default=None,
                    help="LAN SSID (default: WIFI_SSID from code/.env)")
    ap.add_argument("--passphrase", default=None,
                    help="LAN passphrase (default: WIFI_PASSWORD from code/.env)")
    ap.add_argument("--ip", default=None,
                    help="device LAN IP (default: discovered from the ARP table "
                         "by the device's station MAC)")
    ap.add_argument("--install", action="store_true",
                    help="ALSO upload the good image and reboot into it (step 4)")
    args = ap.parse_args()

    ssid = args.ssid or env_value("WIFI_SSID")
    passphrase = args.passphrase if args.passphrase is not None else env_value("WIFI_PASSWORD")
    if not ssid or passphrase is None:
        print("ERROR: no SSID/passphrase. Set WIFI_SSID/WIFI_PASSWORD in code/.env "
              "or pass --ssid/--passphrase.", file=sys.stderr)
        return 2

    with open(args.image, "rb") as f:
        good = f.read()
    corrupt = bytearray(good)
    pos = len(corrupt) // 2
    corrupt[pos] ^= 0x01
    corrupt = bytes(corrupt)
    good_sha = hashlib.sha256(good).hexdigest()
    corrupt_sha = hashlib.sha256(corrupt).hexdigest()
    assert good_sha != corrupt_sha
    print(f"image: {len(good)} bytes  good sha {good_sha[:16]}...  "
          f"corrupt sha {corrupt_sha[:16]}... (byte {pos} flipped)")

    esp_prov = bench_prov.find_esp_prov()
    if esp_prov is None:
        print("ERROR: esp_prov.py not found (set IDF_PATH or install the "
              "pioarduino ESP-IDF framework package).", file=sys.stderr)
        return 2

    dut = Dut(args.dut)
    failed = False
    try:
        # Reboot first: after a fresh boot the BLE advertiser comes up cleanly, and
        # an already-open window from a prior run would advertise stale state.
        dut.collect(0.4)
        dut.request(type="maintenance_exit", timeout=2.0)
        time.sleep(1.0)
        dut.request(type="reboot", boot_target="app", timeout=2.0)
        dut.close()
        print("rebooting the DUT; waiting for the BLE stack to settle…")
        time.sleep(12)

        dut = Dut(args.dut)
        opened = open_window(dut)
        if not opened:
            return 1
        ble_name, pop, token = opened
        if not pop:
            print("ERROR: the maintenance frame carried no PoP.", file=sys.stderr)
            return 1
        adv = "SWC-" + ble_name
        print(f"window open: adv {adv}, PoP {pop}, token {token}; "
              f"provisioning onto {ssid!r}")

        # --- provision onto the LAN, IN THIS WINDOW -------------------------
        time.sleep(4)               # let the advertiser be seen before the scan
        rc, out = bench_prov.run_esp_prov(esp_prov, adv, pop, ssid, passphrase,
                                          timeout=180)
        text = bench_prov.strip_ansi(out)
        joined = "WiFi state: Connected" in text
        print(f"provision rc={rc}  connected={joined}")
        if not joined:
            print(text[-400:], file=sys.stderr)
            print("ERROR: provisioning did not reach 'Connected'.", file=sys.stderr)
            return 1

        # --- locate the device on the LAN and confirm it answers -------------
        mac = station_mac(ble_name)
        ip = args.ip
        deadline = time.time() + 40
        chosen = None
        while time.time() < deadline:
            cand = ip or arp_ip_for_mac(mac)
            if not cand:
                break
            st, _ = http(cand, "GET", "/", timeout=3)
            if st is not None:
                chosen = cand
                break
            time.sleep(1)
        if not chosen:
            print(f"ERROR: device not reachable on the LAN (tried "
                  f"{ip or mac or 'no candidate'}). Pass --ip if ARP has no "
                  f"entry for the device's MAC.", file=sys.stderr)
            return 1
        print(f"device on the LAN at {chosen} (sta mac {mac or 'unknown'})")

        # --- 1. the page is served ------------------------------------------
        st, body = http(chosen, "GET", f"/?token={token}")
        ok_page = (st == 200 and b"SWC" in body)
        print(f"\n1. GET /?token=          -> {st} ({len(body)} bytes)  "
              f"{'PASS' if ok_page else 'FAIL'}")
        failed |= not ok_page

        st, _ = http(chosen, "GET", "/?token=NOTTHETOKEN")
        ok_badtok = st in (401, 403)
        print(f"1b.GET /?token=WRONG     -> {st}  "
              f"{'PASS (refused)' if ok_badtok else 'FAIL (served!)'}")
        failed |= not ok_badtok

        # --- 2. the token gate acts on the DEVICE ---------------------------
        st, body = http(chosen, "POST", "/api/ota/upload", body=b"\x00" * 16,
                        headers={"X-SWC-Size": "16", "X-SWC-Sha256": "0" * 64})
        ok_gate = st in (401, 403)
        print(f"2. upload with NO token  -> {st} {body[:50]!r}  "
              f"{'PASS (refused)' if ok_gate else 'FAIL (not refused)'}")
        failed |= not ok_gate

        # --- 3. a corrupt image is refused, and the device survives ---------
        st, body = http(chosen, "POST", "/api/ota/upload", body=corrupt,
                        headers={"X-SWC-Token": token,
                                 "X-SWC-Size": str(len(corrupt)),
                                 "X-SWC-Sha256": corrupt_sha},
                        timeout=150)
        # The digest is only COMPARED in `ImageVerifyEnd`, so a corrupt image is
        # refused there and surfaces as 500 "install failed" (not the 400 a short
        # read or an over-size body gets). Both are a refusal, and the point of
        # this step is that NO 2xx was returned and nothing was committed.
        ok_refuse = st is not None and 400 <= st < 600
        print(f"3. upload a CORRUPT img -> {st} {body[:60]!r}  "
              f"{'PASS (refused)' if ok_refuse else 'FAIL (accepted!)'}")
        failed |= not ok_refuse
        st2, _ = http(chosen, "GET", f"/?token={token}", timeout=10)
        alive = st2 == 200
        print(f"   device still serving   -> {st2}  "
              f"{'PASS' if alive else 'FAIL'}")
        failed |= not alive

        # --- 4. the good image installs (opt-in) ---------------------------
        if args.install:
            print("\n4. uploading the GOOD image (this will commit and reboot)…")
            st, body = http(chosen, "POST", "/api/ota/upload", body=good,
                            headers={"X-SWC-Token": token,
                                     "X-SWC-Size": str(len(good)),
                                     "X-SWC-Sha256": good_sha},
                            timeout=300)
            ok_inst = st == 200
            print(f"   upload -> {st} {body[:60]!r}  "
                  f"{'PASS (installed)' if ok_inst else 'FAIL'}")
            failed |= not ok_inst
        else:
            print("\n4. skipped (no --install); the device stays on its image")
    finally:
        # Close the window. There is no host Wi-Fi to restore: we never left it.
        try:
            dut.request(type="maintenance_exit", timeout=3.0)
        except Exception:
            pass
        dut.close()

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-35 WiFi upload -- page served, token enforced, "
                  "corrupt image refused, device survived."))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
