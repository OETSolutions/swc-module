#!/usr/bin/env python3
"""Push a firmware image to the device over the app link (spec 9.3's USB OTA).

**Why this exists alongside `dev_flash.sh`.** The bootloader-target route needs
the device to ALREADY run firmware that implements `boot_target: "bootloader"`,
which is a chicken-and-egg: getting that firmware on in the first place would
otherwise need the physical BOOT press this whole exercise is about avoiding.
USB OTA closes that loop -- it is a spec'd capability the current firmware
already has, it runs over the same CDC port, and it needs no loader at all.

So the button-free development loop is:

  * this script, or `dev_flash.sh --ota`, to put an image on over the app link
  * `dev_flash.sh` (the bootloader route) as the alternative when you want a
    raw esptool write, or when the running image is too broken to OTA

Both end with the device running the new image.

Frame flow (spec 9.3, matching `CommandRouter::HandleOta*`):
  ota_begin {size, sha256}                 -> ack
  ota_chunk {offset, data_b64}  x N        -> ack   (offset must be exactly
                                                     the running byte count)
  ota_end {}                               -> ack {result: "ok"}
  reboot    {boot_target: "app"}           -> ack   (the device does NOT reboot
                                                     on its own; spec 9.5)

Exit codes: 0 pushed and acked, 1 refused, 2 usage/port error.
"""

import argparse
import base64
import hashlib
import json
import os
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run with the PlatformIO python "
          "(~/.platformio/penv/bin/python) or `pip install pyserial`.", file=sys.stderr)
    sys.exit(2)

# Must match `kConfigWireChunkBytes` (lib/Util/Base64.h): the firmware decodes
# each chunk into a buffer of exactly this size and REFUSES anything larger, so a
# bigger chunk here would be nacked rather than truncated -- but a smaller one
# keeps the two in step without a second place to remember the number.
CHUNK = 512


class Link:
    """One open CDC connection, with a sequence counter and a read buffer."""

    def __init__(self, port, timeout=2.0):
        self.ser = serial.Serial(port, 115200, timeout=0.25)
        self.timeout = timeout
        self.seq = 0
        self.pending = b""
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _next_seq(self):
        self.seq += 1
        return self.seq

    def request(self, **frame):
        """Send a frame and return (type, message) for its reply, or (None, None).

        The device interleaves status/event frames and its startup `config_get`
        reply on the same link, so replies are matched on `for_seq` rather than
        on arrival order.
        """
        seq = self._next_seq()
        return self._request_seq(seq, frame)

    def _request_seq(self, seq, frame):
        frame = {"v": 1, "seq": seq, **frame}
        self.ser.write((json.dumps(frame, separators=(",", ":")) + "\n").encode())
        self.ser.flush()

        deadline = time.time() + self.timeout
        while time.time() < deadline:
            self.pending += self.ser.read(4096)
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("for_seq") == seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", required=True, help="the firmware.bin to push")
    ap.add_argument("--no-reboot", action="store_true",
                    help="stop after the commit; do not send the reboot frame")
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="per-frame reply timeout (seconds)")
    args = ap.parse_args()

    if not os.path.exists(args.image):
        print(f"ERROR: no such image: {args.image}", file=sys.stderr)
        return 2
    data = open(args.image, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    print(f"== image {args.image}: {len(data)} bytes, sha256 {sha[:16]}...")

    try:
        link = Link(args.port, timeout=args.timeout)
    except OSError as e:
        print(f"ERROR: cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    try:
        t, m = link.request(type="ota_begin", size=len(data), sha256=sha)
        if t != "ack" and (m or {}).get("err") == "run_open":
            # A previous attempt was interrupted (a Ctrl-C, a killed script) and
            # left the device's OTA run open. The device refuses a new `ota_begin`
            # while a run is open -- correctly, since two overlapping writes would
            # corrupt the slot -- so clear it the one way the protocol offers: a
            # chunk whose offset is not the next expected byte, which is refused
            # AND aborts the run (`HandleOtaChunk`'s `gap` path). Doing this
            # automatically is what makes a killed push recoverable rather than
            # requiring the operator to know the trick.
            print("== a previous run was left open; aborting it and retrying")
            link.request(type="ota_chunk", offset=1 << 30, data_b64="AAAA")
            t, m = link.request(type="ota_begin", size=len(data), sha256=sha)
        if t != "ack":
            print(f"ERROR: ota_begin refused: {m}", file=sys.stderr)
            return 1

        off = 0
        n = 0
        total = len(data)
        while off < total:
            piece = data[off:off + CHUNK]
            b64 = base64.b64encode(piece).decode("ascii")
            t, m = link.request(type="ota_chunk", offset=off, data_b64=b64)
            if t != "ack":
                print(f"ERROR: ota_chunk at offset {off} refused: {m}", file=sys.stderr)
                return 1
            off += len(piece)
            n += 1
            if n % 64 == 0 or off == total:
                print(f"   {off}/{total} bytes ({100 * off // total}%)", flush=True)

        t, m = link.request(type="ota_end")
        if t != "ack":
            print(f"ERROR: ota_end refused: {m}", file=sys.stderr)
            return 1
        result = (m or {}).get("result")
        if result != "ok":
            # `not_supported` is the host build's honest answer; anything else here
            # is a real refusal (verify failed, could not set the boot partition).
            print(f"ERROR: the device did not install the image (result={result})", file=sys.stderr)
            return 1
        print("== image verified and committed")

        if args.no_reboot:
            print("== committed; not rebooting (--no-reboot). Send a reboot frame to run it.")
            return 0

        # The device does NOT reboot on its own after a commit -- spec 9.5: it
        # offers a reboot, because the active image must keep the key line safe
        # until the operator chooses. So drive it explicitly.
        #
        # The reboot reply is BEST-EFFORT and its absence is not a failure. The
        # device acks and restarts, and the CDC port drops with the reset, so a
        # read racing that teardown raises `SerialException: Device not
        # configured` -- which says the reboot happened, not that it failed. A
        # commit that has already been acked is the thing being reported here.
        print("== rebooting into the new image")
        try:
            link.request(type="reboot", boot_target="app")
        except serial.SerialException:
            pass
        return 0
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
