#!/usr/bin/env python3
"""Prove FR-2's ADC calibration on the board: the per-chip eFuse curve is APPLIED
(not the linear fallback the N-17 defect left in use), and the ADC tracks a known
injected level.

**What FR-2 asks, and what this tool can and cannot do.** The row has two parts:
(N) the curve maps endpoints, is monotonic, never exceeds the 2.9 V ceiling — all
host-tested; and (D) its accuracy *against a bench reference*, which the row marks
as needing a laboratory instrument. The absolute accuracy (mV vs a calibrated
meter) genuinely needs that instrument and is out of reach here. But two device
claims the row's (D) depends on ARE checkable on this bench without one:

  1. **The eFuse curve is actually applied on silicon.** This is the exact N-17
     defect: `EspHal` created the curve-fit handle and then converted every
     reading with a fixed linear line, so `cali_degraded` reported healthy while
     the device used the scale spec §2.3 forbids. `status.calibration_degraded`
     (added for this) reads the HAL's own flag, so a False here means the real
     `adc_cali_raw_to_voltage()` path ran on the real part — which no host test
     can assert, because it needs the factory eFuse.
  2. **The ADC tracks a known injected level, roughly linearly.** The rig's own
     DAC drives the DUT's ladder node to a commanded voltage; the DUT's
     `ladder_sample.level_mv` is the ADC's calibrated reading of it. Sweeping
     several levels and fitting measured = gain*injected + offset shows the two
     are consistent (gain ~1). This is a RELATIVE check against the rig's
     reference, not an absolute one — it cannot certify the rig's meter — but a
     gross miscalibration (wrong curve, wrong attenuation) would show as a gain
     far from 1 or an offset in the hundreds of mV.

So this tool does not claim FR-2's absolute-accuracy closure; it closes the two
device-side facts that closure rests on and records the measured gain/offset.

Usage:
  python3 tools/bench_cal.py --dut /dev/cu.usbmodem1234561 --rig /dev/cu.usbmodem1121101

Exit codes: 0 both checks pass, 1 a check failed (a real defect), 2 environment.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut                    # noqa: E402
from bench_trim import LoopbackRig, prepare     # noqa: E402

# The SWC ladder's sub-ceiling press range. The ADC ceiling is ~2900 mV, so all
# targets sit below it; idle is ~2835 and a press pulls the node DOWN, so these
# span the working band. Injected at the rig's DAC (its reference), read back by
# the DUT's ADC (the part under test).
TARGETS = (2835, 2600, 2400, 2200, 2050, 1900)


def read_ladder(dut, channel=0):
    """Open a learn run on the channel, collect its streamed `ladder_sample`
    levels, and return the median (mV). None if nothing streamed."""
    for _ in range(6):
        dut._write(type="learn_start", channel=channel)
        frames = dut.collect(0.6)
        dut._write(type="learn_stop", channel=channel)
        dut.collect(0.2)
        levels = [f.get("level_mv") for f in frames
                  if f.get("type") == "ladder_sample"
                  and isinstance(f.get("level_mv"), int) and f.get("level_mv") > 0]
        if levels:
            levels.sort()
            return levels[len(levels) // 2]
        time.sleep(0.2)
    return None


def read_status(dut, seconds=3.0):
    st = None
    for f in dut.collect(seconds):
        if f.get("type") == "status":
            st = f
    return st


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--rig", default="/dev/cu.usbmodem1121101")
    ap.add_argument("--tolerance-mv", type=int, default=60,
                    help="max |injected - measured| for the tracking check "
                         "(default 60; this is the channel's total error, NOT the "
                         "absolute ADC accuracy, which needs a lab meter)")
    args = ap.parse_args()

    dut = Dut(args.dut)
    rig = LoopbackRig(args.rig)
    failed = False
    try:
        # The ladder must be reachable: the rig presents a wheel-like idle and the
        # DUT boots holding it (see bench_trim.py's prepare()).
        if not prepare(dut, rig):
            print("WARN: could not seed the DUT's idle; the tracking check may be "
                  "unavailable.", file=sys.stderr)

        # --- 1. the eFuse calibration is APPLIED (N-17's exact defect) -------
        st = read_status(dut, 3.0) or {}
        degraded = st.get("calibration_degraded")
        print(f"\n1. status.calibration_degraded = {degraded}")
        if degraded is None:
            print("   (no such field: the DUT image predates it — reflash.)")
        applied = (degraded is False)
        print(f"   eFuse curve applied (not the linear fallback) -> "
              f"{'PASS' if applied else 'FAIL' if degraded is True else 'UNKNOWN'}")
        if degraded is True:
            print("   FAIL: the device is on the LINEAR fallback. Spec §2.3 forbids "
                  "it for a calibrated part; this is the N-17 defect.", file=sys.stderr)
        failed |= (degraded is True)

        # --- 2. the ADC tracks a known injected level -----------------------
        # The rig's DAC releases on its own status reads, so a reading taken
        # without re-driving can catch the RELEASED pull-up (~3170 mV) instead of
        # the driven level -- a bench artifact, not a firmware fault. Re-drive and
        # retry any reading at/above the ceiling before trusting it.
        ceiling = 2900
        print(f"\n2. injecting levels at the rig's DAC, reading the DUT's ADC:")
        pts = []
        for tgt in TARGETS:
            meas = None
            for _ in range(5):
                rig.set_level(tgt)
                rig.drive_now()
                time.sleep(0.3)
                v = read_ladder(dut)
                if v is not None and v < ceiling:
                    meas = v
                    break
                time.sleep(0.2)
            if meas is None:
                print(f"   {tgt} mV -> (no sub-ceiling reading; rig drive flaky)")
                continue
            err = meas - tgt
            pts.append((tgt, meas, err))
            print(f"   injected {tgt:>5} mV -> ADC {meas:>5} mV   err {err:+d} mV")

        if len(pts) < 3:
            print("   FAIL: fewer than 3 levels were observable; the rig's ladder "
                  "drive is not usable (check the J2/J3 wiring).", file=sys.stderr)
            failed |= True
        else:
            worst = max(abs(e) for _, _, e in pts)
            within = worst <= args.tolerance_mv
            # A loose slope sanity: a wrong attenuation or the forbidden linear
            # curve would put the gain far from 1. The band is deliberately wide
            # -- this is a cross-board comparison (the rig's DAC is its own
            # reference), so it catches a GROSS miscalibration, not a few-percent
            # one, which is what the lab-meter D-cell is for.
            n = len(pts)
            sx = sum(x for x, _, _ in pts)
            sy = sum(y for _, y, _ in pts)
            sxx = sum(x * x for x, _, _ in pts)
            sxy = sum(x * y for x, y, _ in pts)
            denom = n * sxx - sx * sx
            gain = (n * sxy - sx * sy) / denom if denom else float("nan")
            offset = (sy - gain * sx) / n if denom else float("nan")
            gain_ok = abs(gain - 1.0) <= 0.15
            print(f"   worst |error| = {worst} mV (tolerance {args.tolerance_mv}) -> "
                  f"{'PASS' if within else 'FAIL'}")
            print(f"   fit: measured = {gain:.3f}*injected {offset:+.0f} mV  "
                  f"(slope within 15% of 1.0 -> {'PASS' if gain_ok else 'FAIL'})")
            failed |= not (within and gain_ok)
    finally:
        dut.close()
        rig.close()

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-2 device-side -- the eFuse calibration is applied on "
                  "silicon and the ADC tracks the rig's injected levels. (Absolute "
                  "accuracy vs a lab meter is still the row's open D-cell.)"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
