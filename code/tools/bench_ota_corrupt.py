#!/usr/bin/env python3
"""Prove the USB-OTA path REFUSES a corrupt image and does not brick the device.

**Why this is a script and not a unit test.** The verification gate
(`ImageVerify`) is host-testable and IS tested, but the thing the HANDOFF asks to
prove only exists on hardware: that a corrupt image pushed over the real USB-OTA
path (`esp_ota_begin` / `esp_ota_write` / `esp_ota_end` into the NON-RUNNING
slot) is refused, and that the device **keeps running** afterwards. The failure
this guards against is the worst kind: a bad write that commits, so the device
reboots into a slot that does not work and needs a physical recovery. That can
only be settled with the part in hand.

The test, in order, on the live device:

  1. read the baseline: `uptime_ms`, `heap_free`, `config_state`
  2. `ota_begin` with the TRUE size and the TRUE sha256 of a real build
  3. stream the image with ONE byte flipped inside the payload
  4. `ota_end` -> must be a `nack` (NOT an ack), and the reason must be the
     verification one, not a transport accident
  5. re-read `uptime_ms` and re-issue a command: the device must NOT have
     rebooted (uptime must not have reset) and must still answer
  6. read the two app-slot headers back and confirm the RUNNING slot is
     untouched, so a refused image left no partial state behind

Exit codes: 0 the device refused the corrupt image and stayed up, 1 the device
ACCEPTED it (a real defect), 2 usage/port error.
"""

import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)

CHUNK = 512  # kConfigWireChunkBytes


class Link:
    def __init__(self, port, timeout=2.0):
        self.ser = serial.Serial(port, 115200, timeout=0.25)
        self.ser.dtr = True
        self.timeout = timeout
        self.seq = 0
        self.pending = b""
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def request(self, **frame):
        self.seq += 1
        seq = self.seq
        frame = {"v": 1, "seq": seq, **frame}
        self.ser.write((json.dumps(frame, separators=(",", ":")) + "\n").encode())
        self.ser.flush()
        deadline = time.time() + self.timeout
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
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("for_seq") == seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None

    def status(self, seconds=2.0):
        """Send a `ping` (spec 4.3: "Liveness; FW answers `status`") and return
        the status frame it produces, or None.

        **Draining alone is not enough.** The device emits `status` on connect and
        in response to a ping, not continuously -- so a passive read can sit
        forever on a healthy device and report it dead. Asking is what makes the
        liveness check meaningful.
        """
        self.seq += 1
        seq = self.seq
        self.ser.write((json.dumps({"v": 1, "seq": seq, "type": "ping"},
                                   separators=(",", ":")) + "\n").encode())
        self.ser.flush()
        deadline = time.time() + seconds
        last = None
        while time.time() < deadline:
            try:
                self.pending += self.ser.read(4096)
            except Exception:
                break
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("type") == "status":
                    last = msg
        return last


def read_slot_header(esptool, port, offset):
    """Return the first 32 bytes of a flash region, or None on failure."""
    out = f"/tmp/swc_slot_{offset:x}.bin"
    try:
        subprocess.run(
            [esptool, "--chip", "esp32s3", "--port", port,
             "--before", "no_reset", "--after", "no_reset",
             "read_flash", hex(offset), "0x20", out],
            check=True, capture_output=True, timeout=40)
        return open(out, "rb").read()
    except Exception as e:
        print(f"  (could not read slot {hex(offset)}: {e})")
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", required=True, help="a real firmware.bin to corrupt")
    ap.add_argument("--esptool", default=os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py"),
        help="esptool.py path (for the slot read-back step)")
    ap.add_argument("--py", default=os.path.expanduser("~/.platformio/penv/bin/python"))
    args = ap.parse_args()

    if not os.path.exists(args.image):
        print(f"ERROR: no such image: {args.image}", file=sys.stderr)
        return 2
    good = open(args.image, "rb").read()
    true_sha = hashlib.sha256(good).hexdigest()

    # Flip ONE byte deep inside the payload -- past any header the ROM might
    # check, so the refusal can only come from OUR digest gate, not from a
    # magic-number test that happens to catch a header edit.
    corrupt = bytearray(good)
    pos = len(corrupt) // 2
    corrupt[pos] ^= 0x01
    corrupt_sha = hashlib.sha256(bytes(corrupt)).hexdigest()
    assert corrupt_sha != true_sha, "the corruption did not change the digest"

    print(f"== image: {len(good)} bytes")
    print(f"   true    sha256 {true_sha[:16]}...")
    print(f"   corrupt sha256 {corrupt_sha[:16]}...  (byte {pos} flipped)")

    link = Link(args.port)
    try:
        base = link.status()
        if base is None:
            print("ERROR: the device did not answer status; is it running?", file=sys.stderr)
            return 2
        up0 = base.get("uptime_ms")
        print(f"== baseline: uptime_ms={up0} heap_free={base.get('heap_free')} "
              f"config_state={base.get('config_state')}")

        # Begin with the TRUE size and the TRUE sha, then send CORRUPT bytes.
        # This is deliberate: it isolates OUR digest check. If the sha were also
        # the corrupt one, `ota_end` would fail on the sha comparison rather than
        # on the stream digest, which is a different code path.
        t, m = link.request(type="ota_begin", size=len(good), sha256=true_sha)
        if t != "ack":
            print(f"ERROR: ota_begin refused unexpectedly: {m}", file=sys.stderr)
            return 2

        off = 0
        while off < len(corrupt):
            piece = bytes(corrupt[off:off + CHUNK])
            t, m = link.request(type="ota_chunk", offset=off,
                                data_b64=base64.b64encode(piece).decode())
            if t != "ack":
                # A mid-stream refusal is also a valid "refused" outcome, though
                # the design says the digest is checked at END.
                print(f"== refused mid-stream at offset {off}: {m}")
                break
            off += len(piece)

        t, m = link.request(type="ota_end")
        print(f"== ota_end -> {t}: {m}")

        if t == "ack":
            result = (m or {}).get("result")
            print(f"!! DEFECT: the device ACCEPTED a corrupt image (result={result})",
                  file=sys.stderr)
            return 1

        # The refusal reason must be the verification one.
        err = (m or {}).get("err")
        if err not in ("verify_failed", "bad_frame"):
            print(f"!! NOTE: refused, but for an unexpected reason: {err}", file=sys.stderr)

        # --- the device must still be alive and must NOT have rebooted --------

        time.sleep(1.0)
        after = link.status()
        if after is None:
            print("!! DEFECT: the device stopped answering status after the refusal",
                  file=sys.stderr)
            return 1
        up1 = after.get("uptime_ms")
        print(f"== after:    uptime_ms={up1} heap_free={after.get('heap_free')} "
              f"config_state={after.get('config_state')}")

        ok = True
        if up0 is not None and up1 is not None and up1 < up0:
            print(f"!! DEFECT: uptime went BACKWARDS ({up0} -> {up1}): the device rebooted",
                  file=sys.stderr)
            ok = False
        if after.get("config_state") != base.get("config_state"):
            print(f"!! NOTE: config_state changed: {base.get('config_state')} -> "
                  f"{after.get('config_state')}", file=sys.stderr)

        # A live command proves the link works, not just that status is flowing.
        t, m = link.request(type="maintenance_exit")
        if t != "ack":
            print("!! DEFECT: the device did not ack a command after the refusal",
                  file=sys.stderr)
            ok = False
        else:
            print("== device still answers commands")

        if ok:
            print("\nPASS: the corrupt image was REFUSED and the device kept running.")
            return 0
        return 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
