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
line. **With the fix the enabled series should be no worse than the disabled
one**, and its repeat spread should stay in the low millivolts (no noise
injection).

**The precondition that makes or breaks this measurement.** The rig must be
presenting a wheel-like IDLE and the DUT must be REBOOTED while it holds that
idle, so the DUT seeds a sub-ceiling `idle_reference_mv`. If the DUT boots with
the line floating, the reference seeds ABOVE the 2900 mV ADC ceiling and
`DriveBoundLevelMv` refuses every target -- the pulse acks but never drives, and
the loopback reads the released level for every sweep point (a "no drive" result
that looks like a firmware fault and is not one). `prepare()` below reproduces the
car's power-on condition deliberately. This is the same trap `bench_ladder.py`'s
`check_fr31_headless_learn` documents.

Usage: flash the trim build, then
  ~/.platformio/penv/bin/python tools/bench_trim.py --dut <port> --rig <port> [--hold-ms N]
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)

# The canonical rig helpers (Dut, Rig, the idle constant) live in bench_ladder.py;
# reuse them rather than re-deriving the rig protocol, so a fix there (the status
# read that RELEASES the rig's DAC, the ramp wait) lands here too.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut, Rig, IDLE_CMD_MV  # noqa: E402

# Sweep points inside the output envelope. Low enough to stay reachable below the
# DUT's released (pull-up) level, which on this bench is ~3167 mV -- above it the
# sink FET is off and the line just floats, so those points measure the release,
# not the drive.
DEFAULT_TARGETS = (1900, 2200, 2500, 2800, 3100)

# The ADC's calibrated ceiling at 12 dB. A DUT idle at or above this seeds a
# reference that makes every command unreachable (see the module docstring).
ADC_CEILING_MV = 2900


class LoopbackRig(Rig):
    """`bench_ladder.Rig` plus the one thing this measurement needs: the loopback."""

    def watch_min(self, seconds=1.5):
        """Watch the loopback and return ch1's (min, mean) mV."""
        import re
        self._drain()
        self.s.reset_input_buffer()
        self.s.write(b"w\n")
        t = time.time()
        buf = ""
        while time.time() - t < seconds + 3.0:
            line = self.s.readline()
            if line:
                buf += line.decode("utf-8", "replace")
                if buf.count("min") >= 2:
                    break
        m = re.search(r"ch1:\s*min\s+(-?\d+)\s+max\s+(-?\d+)\s+mean\s+(-?\d+)", buf)
        if not m:
            return None, None
        return int(m.group(1)), int(m.group(3))


def prepare(dut, rig):
    """Put the DUT in a state where a command is actually reachable.

    Drives a wheel-like idle from the rig, reboots the DUT while it holds it, and
    confirms the DUT's own live ladder reference is sub-ceiling. Returns True on
    success. Without this the sweep measures the release for every point.
    """
    dut.request(type="maintenance_exit", timeout=2.0)
    time.sleep(0.3)
    rig.set_level(IDLE_CMD_MV)
    rig.drive_now()
    time.sleep(0.4)
    dut.request(type="reboot", boot_target="app", timeout=1.0)
    dut.close()
    time.sleep(4.5)
    try:
        dut.reopen()
    except OSError as e:
        print(f"ERROR: the DUT did not come back after the idle-seeding reboot: {e}",
              file=sys.stderr)
        return False
    time.sleep(1.0)

    for _ in range(8):
        time.sleep(0.3)
        dut.collect(0.2)
        dut._write(type="learn_start", channel=0)
        frames = dut.collect(0.5)
        dut._write(type="learn_stop", channel=0)
        levels = [f.get("level_mv") for f in frames if f.get("type") == "ladder_sample"]
        if levels and any(0 < l < ADC_CEILING_MV for l in levels):
            print(f"== DUT idle seeded sub-ceiling (ladder reads {sorted(set(levels))[0]} mV)")
            return True
    print("ERROR: the DUT's ladder idle is still >= the 2900 mV ceiling after a boot "
          "with the rig holding an idle; a command cannot be reached. Check the rig's "
          "J2/J3 wiring and that it is driving (see bench_ladder.py).", file=sys.stderr)
    return False


def measure_one(dut, rig, target, hold_ms):
    """Measure one `test_key` pulse's settled level.

    **Re-seed per measurement, and that is not optional.** This rig's DUT only
    drives while the rig is presenting a WHEEL-LIKE idle: the DUT's command-band
    ceiling is its own live idle minus the headroom, so with the line floating
    (released) every target sits above the reachable band and the pulse acks
    without driving -- the loopback then reads the released level for every sweep
    point, which reads as "nothing drives" and is a harness artifact, not a
    firmware fault. Driving the idle from the rig and rebooting the DUT while it
    holds it is the state `bench_output.py` measures in, and the only one in which
    this sweep means anything. It costs a ~6 s reboot per measurement; there are
    `len(DEFAULT_TARGETS) * repeats` of them.
    """
    if not prepare(dut, rig):
        return None, None
    # Start the watch, THEN fire, so the window definitely covers the drive.
    rig.s.reset_input_buffer()
    rig.s.write(b"w\n")
    time.sleep(0.15)
    typ, _ = dut.request(type="test_key", channel=0, key_mv=target, hold_ms=hold_ms)
    if typ != "ack":
        return None, None
    return rig.watch_min()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--rig", default="/dev/cu.usbmodem1121101")
    ap.add_argument("--hold-ms", type=int, default=700,
                    help="test_key hold (max 1000 per the router's bound)")
    ap.add_argument("--repeats", type=int, default=3,
                    help="repeats per target, for the noise/spread column")
    args = ap.parse_args()

    dut = Dut(args.dut)
    rig = LoopbackRig(args.rig)
    try:
        if not prepare(dut, rig):
            return 1
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
        print("           the N-84 wrong-direction step; the fix makes enabled no")
        print("           worse than disabled. A large +err on every row means")
        print("           nothing was driven -- a seeding failure, not a firmware one.")
        print("  spread   low single-digit millivolts => no ADC-noise injection;")
        print("           a large/jumpy spread => the loop is ringing")
        return 0
    finally:
        dut.close()
        rig.close()


if __name__ == "__main__":
    sys.exit(main())
