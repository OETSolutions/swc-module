#!/usr/bin/env python3
"""Measure the servo's static error with the trim loop DISABLED vs ENABLED (FR-19).

**Why this is a bench script.** Spec 6.5 ships the trim loop present-but-disabled
"until its gain is measured on hardware", and FR-19's remaining step is to enable
it and confirm it (a) converges and (b) injects no ADC noise. That is a
measurement of the real plant, so it needs the rig: the DUT is flashed with
`-D SWC_BENCH_TRIM_LOOP` and a `test_key` pulse is swept across the output
envelope while the driver watches the DUT's KEY output through the loopback.

**The defect this measurement found (N-84).** With the loop enabled, the firmware
fed `ServoLoop::Update` the sense reading taken *before* the command was written —
the line's IDLE level — so it applied one full `max_step` (8 codes = 11.7 mV at
gain 1.82) in the WRONG direction on every press. The tell is exactly what this
script measures: the enabled error is consistently ~11 mV WORSE than disabled,
with NO added noise. That is a fixed wrong-direction step, not a controller.

The fix services the loop from `Tick` after the servo settles, reading the DRIVEN
line. **Re-run this against the fixed build and the enabled series should be no
worse than the disabled one** and its repeat spread should stay in the low
millivolts (no noise injection).

Usage: flash the trim build, then
  ~/.platformio/penv/bin/python tools/bench_trim.py --dut <port> --rig <port> [--hold-ms N]
"""

import argparse
import statistics
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)

# Sweep points inside the output envelope. Low enough to stay reachable below the
# DUT's released (pull-up) level, which on this bench is ~3167 mV -- above it the
# sink FET is off and the line just floats, so those points measure the release,
# not the drive.
DEFAULT_TARGETS = (1900, 2200, 2500, 2800, 3100)


class Dut:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.25)
        self.ser.dtr = True
        self.ser.rts = True
        self.seq = 0
        self.pending = b""
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def request(self, timeout=3.0, **frame):
        import json
        self.seq += 1
        f = {"v": 1, "seq": self.seq, **frame}
        self.ser.write((json.dumps(f, separators=(",", ":")) + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pending += self.ser.read(4096)
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception:
                    continue
                if msg.get("for_seq") == self.seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None


class Rig:
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

    def watch_min(self, seconds=1.5):
        """Watch the loopback and return ch1's (min, mean) mV."""
        self.ser.reset_input_buffer()
        self.ser.write(b"w")
        t = time.time()
        buf = ""
        while time.time() - t < seconds + 3.0:
            line = self.ser.readline()
            if line:
                buf += line.decode("utf-8", "replace")
                if buf.count("min") >= 2:
                    break
        for ln in buf.splitlines():
            if ln.strip().startswith("ch1:"):
                p = ln.split()
                try:
                    return int(p[2]), int(p[6])
                except (IndexError, ValueError):
                    pass
        return None, None


def measure_one(dut, rig, target, hold_ms):
    # Start the watch, THEN fire, so the window definitely covers the drive.
    rig.ser.reset_input_buffer()
    rig.ser.write(b"w")
    time.sleep(0.15)
    typ, _ = dut.request(type="test_key", channel=0, key_mv=target, hold_ms=hold_ms)
    if typ != "ack":
        return None
    return rig.watch_min()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--rig", default="/dev/cu.usbmodem1121101")
    ap.add_argument("--hold-ms", type=int, default=700,
                    help="test_key hold (max 1000 per the router's bound)")
    ap.add_argument("--repeats", type=int, default=5,
                    help="repeats per target, for the noise/spread column")
    args = ap.parse_args()

    dut = Dut(args.dut)
    rig = Rig(args.rig)
    try:
        print(f"{'target':>7} {'min':>6} {'err':>6} {'spread':>7}  (min mV vs target; "
              f"spread = max-min over {args.repeats} repeats)")
        for tgt in DEFAULT_TARGETS:
            mins = []
            for _ in range(args.repeats):
                lo, _ = measure_one(dut, rig, tgt, args.hold_ms)
                if lo is not None:
                    mins.append(lo)
                time.sleep(0.25)
            if not mins:
                print(f"{tgt:>7}  (no reading / nack)")
                continue
            spread = max(mins) - min(mins)
            print(f"{tgt:>7} {min(mins):>6} {min(mins) - tgt:>+6} {spread:>7}")
        print("\nRead the columns:")
        print("  err      consistently ~-11 mV with the loop ENABLED vs disabled =>")
        print("           the N-84 wrong-direction step; the fix should make enabled")
        print("           no worse than disabled")
        print("  spread   low single-digit millivolts => no ADC-noise injection;")
        print("           a large/jumpy spread => the loop is ringing")
        return 0
    finally:
        dut.close()
        rig.close()


if __name__ == "__main__":
    sys.exit(main())
