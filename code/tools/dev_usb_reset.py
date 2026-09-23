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

## When the reset is NOT the fix, and how a bus reset WEDGES the device

**This script killed the DUT once (2026-09-23), and the cause is a race.** The
`bootloader` frame restarts the chip into the ROM's download stub, and the stub
then enumerates its own USB peripheral. Issuing a bus reset *while that
enumeration is still in flight* collides with the stub's endpoint bring-up: the
reset control transfer never completes (`[Errno 60] Operation timed out`), and
the stub's USB peripheral is left in a state from which it never enumerates
again. The board was off the bus entirely, and no host-side action could recover
it -- not esptool in any `--before` mode, not repeated pyusb resets, not
`uhubctl` hub power cycles. Only a manual reset brought it back.

Two things made that worse than a normal failure and are fixed here:

* **A timed-out reset was reported as success.** The old code caught every
  exception and printed "device re-enumerated; continuing", which is true for a
  benign `Entity not found` (the handle died because the device re-enumerated out
  from under it) but FALSE for a timeout. A timeout means the reset never
  happened AND the device may now be off the bus. That is now a loud hard
  failure, never a reassurance.
* **The reset ran on the happy path.** It is needed only when the loader fails to
  appear on its own, but the old code reset first and seeked second. This version
  POLLS for the seek target first (a settle window) and resets only if it has
  not shown up. The loader usually does come up unaided; in that case the bus is
  never touched.

**How to apply:** treat a non-zero exit from this script as "the DUT may be off
the bus -- stop, do not retry, power-cycle it by hand or use the OTA route".
Never combine this with `uhubctl` power cycles: a hub port power-cut during the
stub's enumeration is the same race by another name.

Exit codes: 0 the seek target is up (with or without a reset), 1 the seek target
did not appear, 2 the reset itself failed (the device may be off the bus).
"""

import argparse
import errno
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


# The three ways a reset can end. Only the first two are recoverable; the third
# is the wedge and must be reported as a hard failure.
RESET_OK = "ok"
RESET_REENUMERATED = "reenumerated"
RESET_FAILED = "failed"

# macOS reports the device-re-enumerated-out-from-under-the-handle case as one of
# these, depending on the libusb build. Anything else is NOT benign.
_GONE_ERRNOS = {errno.ENODEV, errno.ENXIO, errno.ENOENT}
_GONE_TEXT = ("entity not found", "no such device", "device not found",
              "no such file or directory")


def reset_device(dev):
    """Reset `dev` and classify the outcome. Fail closed: only the two clear
    "the device went away because the reset worked" signatures count as success.
    """
    try:
        dev.reset()
        return RESET_OK
    except usb.core.USBError as e:
        text = str(e).lower()
        if getattr(e, "errno", None) in _GONE_ERRNOS or any(t in text for t in _GONE_TEXT):
            return RESET_REENUMERATED
        return RESET_FAILED
    except Exception:
        return RESET_FAILED


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
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to wait for --seek to come up on its own BEFORE "
                         "resetting. The reset is only needed when the host kept "
                         "the stale node; polling first keeps the bus reset -- the "
                         "step that can wedge the device -- off the happy path.")
    args = ap.parse_args()

    vid, pid = _vid_pid(args.vid_pid)

    # --- poll first: the reset is a fallback, not a step ---------------------
    if args.seek:
        port = find_loader_port(args.seek, args.settle)
        if port:
            print(f"loader port already up (no bus reset needed): {port}")
            return 0

    dev = usb.core.find(idVendor=vid, idProduct=pid)

    # If the node we would reset is already gone, the device has re-enumerated
    # on its own -- reset nothing and go straight to seeking. This is the common
    # case on a re-run, and reporting it as an error would be wrong.
    if dev is None:
        print(f"{args.vid_pid} not present; nothing to reset")
    else:
        outcome = reset_device(dev)
        if outcome == RESET_FAILED:
            print(
                f"ERROR: the USB bus reset of {args.vid_pid} did NOT complete "
                f"(timed out).\n"
                "The control transfer never finished, so the reset did not "
                "happen AND the device may now be off the bus entirely -- the "
                "ROM stub's USB peripheral can wedge if a reset lands while it "
                "is still enumerating (this killed the DUT once, 2026-09-23).\n"
                "DO NOT retry this script and DO NOT power-cycle a hub port: a "
                "repeat reset or a power cut in the same window is the same race "
                "again. Reset the board by hand, then prefer the OTA route:\n"
                "  tools/dev_flash.sh            # USB OTA, cannot wedge the bus\n",
                file=sys.stderr)
            return 2
        if outcome == RESET_REENUMERATED:
            # The handle died because the device re-enumerated out from under it.
            print(f"USB reset issued to {args.vid_pid} (device re-enumerated)")
        else:
            print(f"USB reset issued to {args.vid_pid}")

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
