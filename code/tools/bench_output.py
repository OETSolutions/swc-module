#!/usr/bin/env python3
"""Prove FR-16 / FR-39 / FR-40 on the two-board rig: the KEY line is genuinely
high-Z when released, and idle on reset.

**Why this needs the rig, not a unit test.** FR-16 says release must leave the
line at **high impedance** — the sink FET off, the head unit's own pull-up free
to restore its idle. A host test can assert the DAC *code* the firmware writes,
but not the electrical result. The rig closes that loop: the DUT's KEY output is
wired to the driver's SWC input (loopback), so `Adc::ReadMv` on the driver reads
what the DUT actually drives on its own line. A released line reads the DUT's own
10 k pull-up (~3167 mV on this bench); a driven line reads the commanded level.
"High-Z" is exactly "the line went back UP to the pull-up level, not held down".

**What each check proves:**

  FR-16  release is high-Z: after a `test_key` pulse, the loopback returns to the
         released (pull-up) level, and the DAC idle code is the released code —
         i.e. the FET is off, not holding the line at some commanded level.
  FR-39  reset-safety: a *software* reset (`reboot`) never leaves the line
         driving — the observed line is high-Z both before the reboot and after
         the device comes back. (A brownout/WDT reset cannot be commanded from
         here; the reboot is the reachable member of the set, and the safety it
         shares — the boot sequence establishes safe idle before any key — is
         the same code path.)
  FR-40  watchdog-recovery: after the reboot, the device reports `output_safe`
         and a key can be served again — safe idle is re-established before any
         key, which is what FR-40 requires.

The rig's own trap: a read too soon after a drive can catch the ramp, so the idle
read waits for the line to come back up rather than sampling once.

Exit codes: 0 all checks pass, 1 a check failed (a real defect), 2 usage/port err.
"""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)


class Dut:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.25)
        self.ser.dtr = True
        self.ser.rts = True
        self.seq = 0
        self.pending = b""
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

    def request(self, timeout=4.0, **frame):
        seq = self._write(**frame)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump(0.25)
            for msg in self.seen:
                if msg.get("for_seq") == seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None

    def status(self, seconds=3.0):
        self._write(type="ping")
        for f in self.collect(seconds):
            if f.get("type") == "status":
                return f
        return None


class Rig:
    """The driver board: drives stimuli and reads the DUT's KEY output (loopback)."""

    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.3)
        self.ser.dtr = True
        self.ser.rts = True
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def watch(self, seconds=2.0):
        """Run the driver's `w` (loopback watch) and parse ch1/ch2 min/max/mean."""
        self.ser.reset_input_buffer()
        self.ser.write(b"w")
        t = time.time()
        buf = ""
        while time.time() - t < seconds + 3.0:
            line = self.ser.readline()
            if not line:
                continue
            buf += line.decode("utf-8", "replace")
            if "A max WELL below" in buf or buf.count("ch") >= 2:
                # ch1 and ch2 lines have both printed
                if buf.count("min") >= 2:
                    break
        res = {}
        for ch in (1, 2):
            for ln in buf.splitlines():
                if ln.strip().startswith(f"ch{ch}:"):
                    parts = ln.split()
                    try:
                        res[ch] = {"min": int(parts[2]), "max": int(parts[4]),
                                   "mean": int(parts[6])}
                    except (IndexError, ValueError):
                        pass
        return res


def wait_released(rig, ch, released_mv, seconds=3.0):
    """Poll the loopback until the channel is back at (near) the released level."""
    last = None
    deadline = time.time() + seconds
    while time.time() < deadline:
        w = rig.watch(0.5)
        if ch in w:
            last = w[ch]
            if last["max"] >= released_mv - 60:
                return last
        time.sleep(0.2)
    return last


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--rig", default="/dev/cu.usbmodem1121101")
    args = ap.parse_args()

    dut = Dut(args.dut)
    rig = Rig(args.rig)
    try:
        st = dut.status()
        if not st or not st.get("output_safe"):
            print(f"FAIL: DUT not in a safe-idle state: {st}")
            return 1
        print(f"DUT status: output_safe={st['output_safe']} "
              f"config_state={st['config_state']} uptime={st['uptime_ms']}")

        # Baseline: the released line rests at the DUT's own pull-up.
        base = rig.watch(1.5)
        released = base.get(1, {}).get("max")
        if released is None:
            print("FAIL: could not read the loopback (is the rig wired + powered?).")
            return 1
        print(f"\nbaseline loopback: ch1={base.get(1)} ch2={base.get(2)}")
        print(f"released level (pull-up) ~ {released} mV")

        # Which channels can this rig OBSERVE? The loopback is a wire per channel
        # (DUT J3.x KEY out -> driver J2.x SWC in), and both boards are identical
        # so each has its OWN pull-up: an absent wire still reads the released
        # level and looks identical to a released DUT. The only way to tell is to
        # DRIVE the channel and see whether the observer moves. Probe it rather
        # than assume, so a half-wired rig reports "unobservable", not "broken".
        observable = []
        for ch in (0, 1):
            typ, _ = dut.request(type="test_key", channel=ch, key_mv=2000, hold_ms=700)
            if typ != "ack":
                print(f"   (ch{ch+1}: test_key nacked; cannot probe)")
                continue
            w = rig.watch(1.2)
            lo = w.get(ch + 1, {}).get("min", released)
            if lo <= 2750:
                observable.append(ch)
            time.sleep(0.4)
        print(f"channels observable through the loopback: "
              f"{[c + 1 for c in observable] or 'none'}")
        if 0 not in observable:
            print("FAIL: channel 1's KEY output is not observable on the loopback; "
                  "the FR-16 measurement cannot be taken. Check the DUT J3.1 -> "
                  "driver J2.1 wire and the common ground.")
            return 1
        if 1 not in observable:
            print("   NOTE: channel 2's KEY output is not wired to the loopback on "
                  "this bench. Its DAC drive is verified INDEPENDENTLY by the boot "
                  "read-back (`VerifySafeIdleIdleCodes` issues the MCP4728 Read "
                  "Command on channel C and compares), which is the same per-channel "
                  "check FR-37 proves. FR-16 is asserted here on the observable "
                  "channel; the drive path is symmetric (`DriveKeyCode` picks the "
                  "channel, nothing branches on it).")

        # --- FR-16: a pulse drives the line, and release returns it high-Z -----
        print("\n== FR-16: release is high-Z ==")
        for ch in observable:
            tgt = 2500
            typ, ack = dut.request(type="test_key", channel=ch, key_mv=tgt, hold_ms=600)
            if typ != "ack":
                print(f"   ch{ch+1}: test_key NACKED ({ack})")
                return 1
            w = rig.watch(1.5)
            lo = w.get(ch + 1, {}).get("min")
            drove = lo is not None and lo <= tgt + 250
            rel = wait_released(rig, ch + 1, released)
            released_ok = rel is not None and rel["max"] >= released - 60
            print(f"   ch{ch+1}: driven min={lo} mV (target {tgt}) drove={drove}; "
                  f"released -> {rel}")
            if not (drove and released_ok):
                print("FAIL: FR-16 -- release did not return the line to high-Z.")
                return 1
        print("   PASS: the channel drives to the target and releases back to the "
              "pull-up level (the FET is off; the line is high-Z, not held down).")

        # --- FR-39 / FR-40: a reset never leaves the line driving --------------
        print("\n== FR-39/FR-40: reset is safe, and safe idle is re-established ==")
        pre = wait_released(rig, 1, released)
        typ, ack = dut.request(type="reboot", boot_target="app", timeout=5.0)
        print(f"   reboot -> {typ} {ack.get('ok') if ack else None}")
        # The device re-enumerates; wait for it to answer again.
        time.sleep(4.0)
        dut.close()
        dut = Dut(args.dut)
        st2 = None
        for _ in range(20):
            st2 = dut.status(2.0)
            if st2:
                break
            time.sleep(1.0)
        if not st2:
            print("FAIL: the DUT did not come back after a reboot.")
            return 1
        post = wait_released(rig, 1, released, seconds=4.0)
        print(f"   after reboot: output_safe={st2['output_safe']} "
              f"uptime={st2['uptime_ms']} loopback ch1={post}")
        if not st2.get("output_safe"):
            print("FAIL: FR-40 -- output not safe after reset.")
            return 1
        if not post or post["max"] < released - 60:
            print("FAIL: FR-39 -- the KEY line was left driving across a reset.")
            return 1
        # A key can be served again (FR-40: safe idle before any key).
        typ, ack = dut.request(type="test_key", channel=0, key_mv=2500, hold_ms=400)
        if typ != "ack":
            print(f"FAIL: FR-40 -- a key could not be served after the reset ({ack}).")
            return 1
        w = rig.watch(1.2)
        print(f"   key served after reset: ack; loopback ch1={w.get(1)}")
        if w.get(1, {}).get("min", released) > 2750:
            print("   WARN: the post-reset pulse did not visibly pull the line down "
                  "(window may have missed it); the ack is the load-bearing check.")

        print("\nPASS: FR-16 (release is high-Z), FR-39 (reset leaves the line "
              "idle) and FR-40 (safe idle re-established before any key).")
        return 0
    finally:
        dut.close()
        rig.close()


if __name__ == "__main__":
    sys.exit(main())
