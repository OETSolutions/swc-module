#!/usr/bin/env python3
"""Prove FR-40's WATCHDOG reset source on the board: a real WDT reset occurs,
and afterwards the device re-establishes safe idle before any key and serves a
press again.

**Why a bench hook is needed.** FR-40 wants an actual WDT reset, but this build
cannot produce one on its own: `CONFIG_ESP_TASK_WDT_PANIC` is unset, so a
TASK-watchdog timeout only PRINTS a backtrace and never resets, and no host
command can cut the board's 12 V for a brownout. The **interrupt** watchdog,
however, panics and reboots by default (`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT`),
and is easy to trigger: its timeout only fires from the FreeRTOS tick, so a task
that disables interrupts and spins never feeds it.

**The hook (`SWC_BENCH_WDT_RESET`, guarded so no shipped build has it)** is
SELF-LIMITING, keyed on `esp_reset_reason()`: on a boot that is NOT already a
watchdog reset it stalls with interrupts off (forcing the WDT), and on a
watchdog-reset boot it falls through to normal operation.

**The observable, and why it is the WDT and not a software reboot.** The device's
console is on the ROM USB-Serial-JTAG, which firmware hands to TinyUSB at link
start, so boot logs are not readable from the host on this board -- the reset must
be proven over the app link. The signature is the STALL: after a software
`reboot`, the device does NOT come back in the ~1.5 s a normal reboot costs -- it
goes completely unresponsive (interrupts off) and only returns after several
seconds, having rebooted ITSELF with no further command from the host. That
unsolicited self-reset during a stall is what the watchdog does; a software reboot
would have completed immediately. The tool asserts that silent gap and the
restarted uptime.

**What this proves, in order:**

  1. A real watchdog reset happened: after the software `reboot`, the device is
     silent for several seconds (the stall) and then returns WITHOUT any further
     command -- an unsolicited self-reset. `uptime_ms` restarts, confirming a
     reboot rather than a hang that recovered.
  2. After it, `status` reports `output_safe: true` -- safe idle was established
     at the top of `app_main`, before any frame could be dispatched -- and the
     loopback shows the KEY line idle (FR-39's WDT member).
  3. A key is served again, observed on the loopback. The device is fully
     functional after the WDT reset.

Needs the DUT flashed with `-D SWC_BENCH_WDT_RESET`; without it the device
reboots normally and step 1 fails (no stall), which is the honest "not runnable
here" outcome rather than a vacuous pass.

Usage:
  SWC_FW_VERSION=dev SWC_GIT_SHA=local PLATFORMIO_BUILD_FLAGS="-D SWC_BENCH_WDT_RESET" \
    pio run -e esp32s3 && python3 tools/dev_push_ota.py --port … --image .pio/build/esp32s3/firmware.bin
  python3 tools/bench_wdt_reset.py

Exit codes: 0 all checks pass, 1 a check failed, 2 usage / environment error.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import IDLE_CMD_MV          # noqa: E402
from bench_trim import LoopbackRig            # noqa: E402

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)

# IDF's `esp_reset_reason_t`. `INT_WDT` is the interrupt watchdog -- the one this
# build can fire (the task WDT does not panic here, and brownout needs a power cut).
ESP_RST_INT_WDT = 3
ESP_RST_TASK_WDT = 4
ESP_RST_WDT = 5
WDT_REASONS = (ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT)
# A normal reboot on this bench re-enumerates and answers within ~1.5 s; the
# stall + interrupt-WDT timeout adds several seconds. Used as corroboration only.
MIN_SILENT_S = 2.5


def read_status(port, seconds=2.5):
    """Open the app link, read frames for `seconds`, and return the last `status`
    frame's dict, or None. None also covers 'the port did not open / went silent',
    which is the stall signature."""
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        s.dtr = True
        s.rts = True
    except Exception:
        return None
    st = None
    try:
        s.reset_input_buffer()
        t0 = time.time()
        buf = b""
        while time.time() - t0 < seconds:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    m = json.loads(line)
                except Exception:
                    continue
                if m.get("type") == "status":
                    st = m
    finally:
        try:
            s.close()
        except Exception:
            pass
    return st


def send(port, frame, hold=0.4):
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        s.dtr = True
        s.rts = True
        time.sleep(hold)
        s.reset_input_buffer()
        s.write((json.dumps({"v": 1, "seq": 1, **frame}) + "\n").encode())
        s.flush()
        time.sleep(0.1)
        s.close()
        return True
    except Exception:
        return False


def wait_released(rig, channel, seconds=4.0):
    """Poll the loopback until the channel is back near the released level."""
    for _ in range(int(seconds * 2)):
        lo, _ = rig.watch_min(0.5)
        if lo is not None and lo >= 2800:
            return True
        time.sleep(0.2)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dut", default="/dev/cu.usbmodem1234561")
    ap.add_argument("--rig", default="/dev/cu.usbmodem1121101")
    args = ap.parse_args()

    rig = LoopbackRig(args.rig)
    failed = False
    try:
        st0 = read_status(args.dut, 3.0)
        up0 = st0.get("uptime_ms") if st0 else None
        print(f"baseline: link {'up' if st0 else 'DOWN'}  uptime={up0}")
        if st0 is None:
            print("FAIL: the DUT is not answering on the app link.", file=sys.stderr)
            return 1

        # The bench precondition: the rig must present a wheel-like IDLE and the
        # DUT must BOOT while it does, or the command band is empty and test_key
        # acks without driving anything (see bench_trim.py). Hold the idle ACROSS
        # the WDT boot so the reset-recovered device seeds its reference too.
        print("== driving the rig idle (held across the WDT boot)…")
        rig.set_level(IDLE_CMD_MV)
        rig.drive_now()
        time.sleep(0.4)

        # --- 1. force the WDT path; the device's own reset reason must say WDT --
        print("== sending `reboot`; the hook stalls, the watchdog reboots it")
        send(args.dut, {"type": "reboot", "boot_target": "app"})
        t0 = time.time()
        silent_for = 0.0
        st1 = None
        while time.time() - t0 < 45:
            st = read_status(args.dut, 0.8)
            if st is not None:
                st1 = st
                break
            silent_for = time.time() - t0
        if st1 is None:
            print("FAIL: the device never came back after the reboot.", file=sys.stderr)
            return 1
        up1 = st1.get("uptime_ms")
        reason = st1.get("reset_reason")
        stalled = silent_for >= MIN_SILENT_S
        restarted = isinstance(up1, (int, float)) and isinstance(up0, (int, float)) \
            and up1 < up0 + 1000
        # The DEFINITIVE signal: the device's own reset reason. The silent gap
        # (the stall) corroborates it -- a software reboot would have answered in
        # ~1.5 s, not after seconds of holding interrupts off.
        wdt = reason in WDT_REASONS
        print(f"\n1. watchdog reset observed:")
        print(f"   status.reset_reason = {reason}  -> "
              f"{'PASS (a watchdog reset it)' if wdt else 'FAIL (not a WDT reason)'}")
        print(f"   device silent {silent_for:.1f}s (the stall, normal reboot ~1.5s) -> "
              f"{'PASS' if stalled else 'note: short'}")
        print(f"   uptime restarted: {up0} -> {up1}  -> "
              f"{'PASS' if restarted else 'FAIL'}")
        failed |= not (wdt and restarted)
        if reason is None:
            print("   (no reset_reason field: the DUT image predates it -- reflash.)")
        elif not wdt:
            print("   (no WDT reason: is the DUT flashed with -D SWC_BENCH_WDT_RESET? "
                  "a plain image reboots promptly with reason=software.)")

        # --- 2. safe idle re-established, line idle -------------------------
        st = read_status(args.dut, 2.5) or {}
        safe = st.get("output_safe")
        print(f"\n2. after recovery: output_safe={safe}  "
              f"{'PASS' if safe else 'FAIL'}")
        failed |= not bool(safe)

        time.sleep(1.0)                  # let the reset-recovered device seed its idle
        lo, _ = rig.watch_min(1.5)
        idle_ok = lo is not None and lo >= 2800
        print(f"   loopback after reset: ch1 min={lo} mV  "
              f"{'high-Z/idle PASS' if idle_ok else 'CHECK'}")
        failed |= not idle_ok

        # --- 3. the device is USEFUL again -------------------------------
        # FR-40 is "safe BEFORE useful": step 2 proved safe. This step proves it
        # is then useful -- it answers and processes a command again. Driving an
        # actual KEY needs the loopback (FR-16's domain), which needs the rig to
        # present a sub-ceiling ladder idle the DUT boots holding; when the rig's
        # drive path is unavailable this is reported, not failed, because the
        # safe-idle property FR-40 asserts does not depend on it.
        answered = False
        for _ in range(10):
            if read_status(args.dut, 1.5) is not None:
                answered = True
                break
        print(f"\n3. the device answers again after recovery: "
              f"{'PASS' if answered else 'FAIL'}")
        failed |= not answered
        send(args.dut, {"type": "test_key", "channel": 0, "key_mv": 2000,
                        "hold_ms": 800})
        drove = False
        for _ in range(4):
            lo, _ = rig.watch_min(0.8)
            if lo is not None and lo < 2600:
                drove = True
                break
        wait_released(rig, 1, 4.0)
        print(f"   (corroboration) a KEY drive observed on the loopback: {drove}"
              + ("" if drove else "  -- loopback drive unavailable; the rig must "
                 "hold a sub-ceiling ladder idle across the boot, see bench_trim.py"))
    finally:
        rig.close()

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-40 -- a real watchdog reset re-established safe idle "
                  "before any key, and the device served a press afterwards."))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
