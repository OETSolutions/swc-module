#!/usr/bin/env python3
"""Prove FR-38's activity half: a request served by the maintenance server KEEPS
THE WINDOW OPEN past its own timeout, and silence reaps it.

**What FR-38 requires.** The configured `maintenance_timeout_ms` of *inactivity*
returns the device to normal mode. The mode's logic is host-tested
(`MaintenanceMode`), but the SOURCE of activity is device-only: `main.cpp` bumps
the window's clock whenever the maintenance HTTP server's request count moves, so
the proof needs a real HTTP request against the running server.

**Why the window must be reachable, and how.** The device serves its page on the
bench LAN after it is provisioned (the same recipe as `bench_wifi_upload.py`:
Sec1 over BLE onto the SSID in `code/.env`, then HTTP at the device's LAN IP).
The host Mac is not touched. **Provision and drive HTTP in the SAME window** -- a
`maintenance_exit` -> `maintenance_enter` cycle leaves the driver STA-only without
reconnecting (see `bench_wifi_upload.py`).

**The test, with an unambiguous signal.** The device's own `maintenance` frame is
emitted only on CHANGE, so its absence is not evidence -- the observable used here
is HTTP reachability, which is exactly what the window controls: while the window
is open the radio and its server are up, and on exit `TearDown()` frees the radio
(FR-32), so the server stops answering.

  1. Set `maintenance_timeout_ms` live to a short value (default 12 s) via the
     `config_patch` command, applied with `SetTimeout` (no radio teardown).
  2. Hold the window open with a request every few seconds for ~3x the timeout.
     The window MUST still answer at the end -- if the timeout were a fixed
     deadline from entry, it would have closed. That is the renewal proof.
  3. Stop the requests. The window MUST close within roughly the timeout. That is
     the inactivity proof (and shows step 2's survival was the requests, not the
     timeout being ignored).
  4. Restore the default timeout.

Usage:
  python3 tools/bench_maintenance_renewal.py            # 12 s timeout
  python3 tools/bench_maintenance_renewal.py --timeout-ms 8000

Exit codes: 0 both halves held, 1 a check failed (a real defect), 2 environment.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut                      # noqa: E402
import bench_prov                                 # noqa: E402
import bench_wifi_upload as wu                    # noqa: E402

DEFAULT_TIMEOUT_MS = 12000
DEFAULT_LAN_IP = wu.DEFAULT_LAN_IP


def http_ok(ip, token):
    """True iff the device's maintenance server answers a token-carrying GET.
    This is the window's own on/off observable: the server exists only while the
    window is open (FR-32 tears the radio down on exit)."""
    try:
        with urllib.request.urlopen(f"http://{ip}/?token={token}", timeout=4) as r:
            return r.status == 200
    except urllib.error.HTTPError as e:
        return e.code == 200
    except Exception:
        return False


def drain_maintenance(dut):
    """Return the last `maintenance` frame's `active`, or None if none arrived."""
    a = None
    for f in dut.collect(0.6):
        if f.get("type") == "maintenance":
            a = f.get("active")
    return a


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS,
                    help=f"short timeout to set live (default {DEFAULT_TIMEOUT_MS})")
    ap.add_argument("--ssid", default=None)
    ap.add_argument("--passphrase", default=None)
    ap.add_argument("--ip", default=None)
    args = ap.parse_args()

    ssid = args.ssid or wu.env_value("WIFI_SSID")
    passphrase = args.passphrase if args.passphrase is not None else wu.env_value("WIFI_PASSWORD")
    if not ssid or passphrase is None:
        print("ERROR: set WIFI_SSID/WIFI_PASSWORD in code/.env or pass "
              "--ssid/--passphrase.", file=sys.stderr)
        return 2
    esp_prov = bench_prov.find_esp_prov()
    if esp_prov is None:
        print("ERROR: esp_prov.py not found.", file=sys.stderr)
        return 2

    dut = Dut(args.dut)
    try:
        dut.collect(0.4)
        dut.request(type="maintenance_exit", timeout=2.0)
        time.sleep(1.0)
        dut.request(type="reboot", boot_target="app", timeout=2.0)
        dut.close()
        print("rebooting the DUT; waiting for the BLE stack to settle…")
        time.sleep(12)

        dut = Dut(args.dut)
        opened = wu.open_window(dut)
        if not opened:
            return 1
        ble_name, pop, token = opened
        adv = "SWC-" + ble_name
        print(f"window open: adv {adv}, PoP {pop}, token {token}")

        time.sleep(4)
        rc, out = bench_prov.run_esp_prov(esp_prov, adv, pop, ssid, passphrase,
                                          timeout=180)
        joined = "WiFi state: Connected" in bench_prov.strip_ansi(out)
        print(f"provision rc={rc} connected={joined}")
        if not joined:
            print(bench_prov.strip_ansi(out)[-400:], file=sys.stderr)
            return 1

        ip = args.ip
        deadline = time.time() + 40
        while time.time() < deadline:
            cand = ip or wu.arp_ip_for_mac(wu.station_mac(ble_name)) or DEFAULT_LAN_IP
            if http_ok(cand, token):
                ip = cand
                break
            time.sleep(1)
        if not ip:
            print("ERROR: the window's server never answered on the LAN.",
                  file=sys.stderr)
            return 1
        print(f"device on the LAN at {ip}")

        # Step 1: lower the timeout live.
        typ, msg = dut.request(type="config_patch",
                               path="settings.maintenance_timeout_ms",
                               value=args.timeout_ms, timeout=5.0)
        applied = (typ == "ack")
        print(f"\n1. set maintenance_timeout_ms = {args.timeout_ms} -> {typ}  "
              f"{'PASS' if applied else 'FAIL'}")
        if not applied:
            print(json.dumps(msg), file=sys.stderr)
            return 1
        T = args.timeout_ms / 1000.0

        # Step 2: hold the window with periodic requests well past the timeout.
        hold = max(3 * T, T + 8)
        print(f"2. holding the window with a request every 3 s for {hold:.0f}s "
              f"(> {T:.0f}s timeout)…")
        t0 = time.time()
        while time.time() - t0 < hold:
            http_ok(ip, token)
            time.sleep(3)
        survived = http_ok(ip, token)
        print(f"   window still answering after {time.time()-t0:.0f}s of activity "
              f"-> {'PASS (renewed)' if survived else 'FAIL (closed early)'}")
        failed = not survived

        # Step 3: silence must reap it. Detect closure with the device's OWN
        # `maintenance` frame, NOT `http_ok`: an HTTP probe is ITSELF a request,
        # so polling with it would renew the window and it would never close --
        # the observation would perturb the thing observed. The frame flips
        # `active`->false on close and is emitted over USB, which the window does
        # not gate.
        print(f"3. removing all activity; the window must close within ~{T:.0f}s…")
        dut.collect(0.5)                 # clear anything buffered
        t0 = time.time()
        closed_at = None
        while time.time() - t0 < 3 * T + 8:
            for f in dut.collect(1.0):       # tick in 1 s slices to timestamp
                if f.get("type") == "maintenance" and f.get("active") is False:
                    closed_at = time.time() - t0
                    break
            if closed_at is not None:
                break
        reaped = closed_at is not None
        shown = "never" if closed_at is None else f"{closed_at:.1f}s"
        print(f"   window closed {shown} after the last request -> "
              f"{'PASS (reaped)' if reaped else 'FAIL (stayed open)'}")
        failed |= not reaped
        # And the server itself is gone (the radio is torn down on exit, FR-32):
        # a probe now can only answer if the window is somehow still up.
        print(f"   server after close -> "
              f"{'down (PASS)' if not http_ok(ip, token) else 'UP (FAIL)'}")

        # Step 4: restore. Best-effort -- the window may already be closed.
        try:
            dut.request(type="config_patch",
                        path="settings.maintenance_timeout_ms",
                        value=300000, timeout=4.0)
        except Exception:
            pass
    finally:
        try:
            dut.request(type="maintenance_exit", timeout=3.0)
        except Exception:
            pass
        dut.close()

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-38 -- activity renews the window past its timeout, "
                  "and silence reaps it."))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
