#!/usr/bin/env python3
"""Prove FR-34: the REAL Espressif provisioning handshake completes on the DUT.

**Why this is a script and not a unit test.** The PoP derivation, the advertised
short name and the Sec0 gate are host-testable and ARE tested. What FR-34 asks
for — "the real Espressif provisioning app completes provisioning against this
device" — is the one thing the host cannot reach: it is the NimBLE GAP stack, the
`wifi_provisioning` endpoints and the Sec1 (SRP6a) handshake on real silicon.

This does not run the phone app. It runs **`esp_prov.py`**, Espressif's own
reference provisioning client, which is the implementation the app wraps and the
protocol the device must speak. On Linux that tool drives `hci0`; on macOS its
BLE client is `bleak` (CoreBluetooth), so the same reference client runs here
unchanged — the `iface='hci0'` argument is unused by the bleak backend.

**What the handshake proves, in order:**

  1. The BLE service advertises and is discoverable as `SWC-<short-id>`.
  2. A Sec1 session establishes ONLY with the correct PoP — the derived
     `<PoP>`-style secret the app shows over USB — and a WRONG PoP is refused
     (the security property: an unauthenticated provisioning window would be a
     radio-range takeover).
  3. `CmdSetConfig` returns status 0 (credentials accepted).
  4. `CmdApplyConfig` returns status 0 (applied).

**What it does NOT prove, and why:** the WiFi *join*. That needs a real AP, and
is FR-35's dependency, not FR-34's. By default this drives a deliberately
non-existent SSID so the handshake is exercised without needing the bench to
offer a network; the tool then shows, from the device's own status poll, that the
join is the only thing left. Pass `--ssid`/`--passphrase` for a real AP and
`--expect-join` to require it.

Exit codes: 0 the handshake completed (and the join, if required), 1 a step
failed (a real defect), 2 usage / port / tooling error.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)


def find_esp_prov():
    """Locate Espressif's reference client. Prefer IDF_PATH, else the pinned
    PlatformIO framework package (which is where this bench's IDF lives)."""
    cands = []
    idf = os.environ.get("IDF_PATH")
    if idf:
        cands.append(os.path.join(idf, "tools", "esp_prov", "esp_prov.py"))
    for pkg in glob.glob(os.path.expanduser(
            "~/.platformio/packages/framework-espidf@*/tools/esp_prov/esp_prov.py")):
        cands.append(pkg)
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.25)
        self.ser.dtr = True
        self.ser.rts = True
        self.seq = 0
        self.pending = b""
        # Every frame decoded, kept for `collect`. A reply the device emits just
        # after an ack (the `maintenance` frame follows `maintenance_enter`'s ack
        # immediately) can land in the SAME read as the ack; a `request` that
        # discarded the frames it scanned while hunting for the ack would drop it.
        self.seen = []
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _write(self, **frame):
        self.seq += 1
        frame = {"v": 1, "seq": self.seq, **frame}
        self.ser.write((json.dumps(frame, separators=(",", ":")) + "\n").encode())
        self.ser.flush()
        return self.seq

    def _pump(self, seconds):
        """Read for `seconds`, decode every line into `self.seen`."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                self.pending += self.ser.read(4096)
            except Exception:
                break
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    self.seen.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    def collect(self, seconds):
        self._pump(seconds)
        out, self.seen = self.seen, []
        return out

    def request(self, timeout=5.0, **frame):
        seq = self._write(**frame)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump(0.25)
            for msg in list(self.seen):
                if msg.get("for_seq") == seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None

    def uptime(self):
        self._write(type="ping")
        for f in self.collect(2.5):
            if f.get("type") == "status" and "uptime_ms" in f:
                return f["uptime_ms"]
        return None


def open_window(link, tries=3):
    """Exit then enter, so the `maintenance` frame is re-emitted with the session
    facts (it is emitted only on change, so an already-open window stays quiet)."""
    for _ in range(tries):
        link.request(type="maintenance_exit", timeout=4.0)
        link.collect(1.0)
        typ, _ = link.request(type="maintenance_enter", timeout=6.0)
        if typ != "ack":
            continue
        for f in link.collect(3.0):
            if f.get("type") == "maintenance" and f.get("active"):
                return "SWC-" + f["ble_name"], f["pop"]
    return None, None


def run_esp_prov(esp_prov, ble_name, pop, ssid, passphrase, timeout=120):
    """Run the reference client; return (returncode, combined output)."""
    idf_path = os.path.dirname(os.path.dirname(os.path.dirname(esp_prov)))
    env = dict(os.environ, IDF_PATH=idf_path)
    cmd = [sys.executable, esp_prov, "--transport", "ble",
           "--service_name", ble_name, "--sec_ver", "1", "--pop", pop,
           "--ssid", ssid, "--passphrase", passphrase, "-v"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, env=env, cwd=os.path.dirname(esp_prov))
    except subprocess.TimeoutExpired as e:
        return 124, (e.stdout or "") + (e.stderr or "")
    return p.returncode, p.stdout + p.stderr


def strip_ansi(s):
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/cu.usbmodem1234561",
                    help="DUT (product firmware) serial port")
    ap.add_argument("--ssid", default="SWC-Bench-NoSuchAP",
                    help="SSID to send (default: a non-existent one, to exercise "
                         "the handshake without needing a real AP)")
    ap.add_argument("--passphrase", default="bench-passphrase")
    ap.add_argument("--expect-join", action="store_true",
                    help="require the WiFi join to succeed (needs a real AP)")
    ap.add_argument("--ble-name", default=None,
                    help="override the advertised BLE name (skip the window read)")
    ap.add_argument("--pop", default=None, help="override the PoP")
    args = ap.parse_args()

    if not os.path.exists(args.port):
        print(f"ERROR: no such port: {args.port}", file=sys.stderr)
        return 2
    esp_prov = find_esp_prov()
    if esp_prov is None:
        print("ERROR: esp_prov.py not found (set IDF_PATH or install the "
              "pioarduino ESP-IDF framework package).", file=sys.stderr)
        return 2

    link = Link(args.port)
    try:
        up0 = link.uptime()
        ble_name, pop = args.ble_name, args.pop
        if not (ble_name and pop):
            ble_name, pop = open_window(link)
        if not ble_name or not pop:
            print("FAIL: could not open a maintenance window / read its facts.")
            return 1
        print(f"maintenance window: ble={ble_name} pop={pop}")

        # --- 1. the POSITIVE handshake -------------------------------------
        print("\n== positive: correct PoP ==")
        rc, out = run_esp_prov(esp_prov, ble_name, pop, args.ssid, args.passphrase)
        out = strip_ansi(out)
        ok_session = "Session Established" in out
        ok_set  = re.search(r"SetConfig status:\s*0x0\b", out) is not None
        ok_apply = re.search(r"ApplyConfig status:\s*0x0\b", out) is not None
        joined = "Provisioning was successful" in out
        print(f"   session established : {ok_session}")
        print(f"   CmdSetConfig  == 0x0 : {ok_set}")
        print(f"   CmdApplyConfig== 0x0 : {ok_apply}")
        print(f"   wifi joined         : {joined}")
        if not (ok_session and ok_set and ok_apply):
            print("\n--- esp_prov output ---")
            print(out[-2000:])
            print("FAIL: the provisioning handshake did not complete.")
            return 1
        if args.expect_join and not joined:
            print("FAIL: --expect-join was set but the WiFi join did not succeed.")
            return 1

        # --- 2. the NEGATIVE direction: a wrong PoP must be refused --------
        print("\n== negative: wrong PoP must NOT establish a session ==")
        wrong = "0000FF" if pop != "0000FF" else "1111EE"
        rc2, out2 = run_esp_prov(esp_prov, ble_name, wrong, args.ssid, args.passphrase)
        out2 = strip_ansi(out2)
        refused = "Session Established" not in out2
        print(f"   session refused     : {refused}")
        if not refused:
            print("FAIL: a WRONG PoP established a session -- the security "
                  "property is broken.")
            return 1

        # --- 3. the device survived the whole exchange ---------------------
        up1 = link.uptime()
        if up0 is not None and up1 is not None and up1 < up0:
            print(f"FAIL: uptime went backwards ({up0} -> {up1}); the device reset.")
            return 1
        print(f"\ndevice uptime across the exchange: {up0} -> {up1} ms (no reset)")

        print("\nPASS: FR-34's provisioning handshake completes on the device "
              "with the correct PoP and is refused with a wrong one.")
        if not args.expect_join:
            print("      (WiFi JOIN not required; that is FR-35's dependency. "
                  "Re-run with a real --ssid --passphrase --expect-join to close "
                  "it too.)")
        return 0
    finally:
        try:
            link.request(type="maintenance_exit", timeout=4.0)
        except Exception:
            pass
        link.close()


if __name__ == "__main__":
    sys.exit(main())
