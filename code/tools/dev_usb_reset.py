#!/usr/bin/env python3
"""Force a USB-level reset of a device so the host re-enumerates it.

**Why this is needed, and why it is not obvious.** When the firmware sets the
ESP32-S3's force-download bit and restarts, the ROM brings its download
peripheral up on the same physical USB port -- but macOS keeps the OLD device
node bound (the app's TinyUSB `303A:4001`), so the new ROM interface never
appears in `/dev`. The device then looks *wedged*: it is enumerated and silent,
and esptool cannot reach it, because the only port the host offers is the dead
app one. A USB bus reset makes the host tear that stale node down and enumerate
whatever is actually on the port now, which is the loader.

The bench sequence that found this (2026-09-24): the `bootloader` frame acked,
the device rebooted, `/dev/cu.usbmodem1234561` stayed as `303A:4001` and stopped
answering, and every esptool mode failed. After `Device.reset()` through pyusb
the device re-enumerated as `/dev/cu.usbmodem113101` (`303A:0009`, the S3 ROM's
USB-OTG download interface) and esptool connected immediately.

Exit codes: 0 reset issued (or nothing to reset), 2 a hard error.
"""

import argparse
import glob
import os
import sys
import time

try:
    import usb.core
except ImportError:  # pragma: no cover
    print("ERROR: pyusb not available; install it into the PlatformIO penv:\n"
          "  ~/.platformio/penv/bin/python -m pip install pyusb", file=sys.stderr)
    sys.exit(2)


def _vid_pid(spec: str):
    v, p = spec.split(":")
    return int(v, 16), int(p, 16)


def find_loader_port(specs, timeout):
    """Poll until a port matching any VID:PID spec in `specs` is listed.

    Returns the port path, or None. Used to confirm the ROM loader came up after
    a USB reset -- polling rather than a fixed sleep, because the re-enumeration
    time varies and a sleep would either be too short (flaky) or waste seconds.
    """
    import json
    import subprocess

    wanted = {s.upper() for s in specs}
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = subprocess.run(["pio", "device", "list", "--json-output"],
                             capture_output=True, text=True).stdout
        try:
            data = json.loads(out)
        except Exception:
            data = []
        for d in data:
            hwid = d.get("hwid", "").upper()
            for w in wanted:
                if w in hwid:
                    port = d.get("port", "")
                    # `cu.` is the non-blocking spelling on macOS; a `tty.` open
                    # waits for DCD and can hang.
                    if port.startswith("/dev/tty."):
                        port = "/dev/cu." + port[len("/dev/tty."):]
                    return port
        time.sleep(0.2)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vid-pid", required=True,
                    help="the device to reset, e.g. 303A:4001 (the app node)")
    ap.add_argument("--seek", action="append", default=[], metavar="VID:PID",
                    help="after resetting, wait for a port with this VID:PID "
                         "(repeatable). Without it, the reset is fire-and-forget.")
    ap.add_argument("--seek-timeout", type=float, default=20.0)
    args = ap.parse_args()

    vid, pid = _vid_pid(args.vid_pid)
    dev = usb.core.find(idVendor=vid, idProduct=pid)

    # If the node we would reset is already gone, the device has re-enumerated
    # on its own -- reset nothing and go straight to seeking. This is the common
    # case on a re-run, and reporting it as an error would be wrong.
    if dev is None:
        print(f"{args.vid_pid} not present; nothing to reset")
    else:
        try:
            dev.reset()
            print(f"USB reset issued to {args.vid_pid}")
        except Exception as e:
            # `Entity not found` means the reset WORKED: the device
            # re-enumerated out from under the handle mid-call. That is the
            # normal outcome, not a failure.
            print(f"USB reset: {e} (device re-enumerated; continuing)")

    if not args.seek:
        return 0

    port = find_loader_port(args.seek, args.seek_timeout)
    if port:
        print(f"loader port up: {port}")
        return 0
    print(f"no port matching {'/'.join(args.seek)} within {args.seek_timeout}s",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
