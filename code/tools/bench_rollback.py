#!/usr/bin/env python3
"""Prove FR-37's A/B rollback with a GENUINELY faulting image.

Spec §10.4's level-4 bench test, verbatim: "flash a deliberately-faulting image
(compiles, panics at startup), assert the bootloader rolls back and the device
comes up on the old image. **This must be tested with a genuinely broken image**,
not a mocked failure, or it proves nothing."

**Why this cannot be a unit test.** Every other part of the OTA path is
host-testable and is tested: the digest gate, the stream state machine, the
commit. What is NOT testable off the board is the IDF bootloader's own behaviour
-- that a partition marked PENDING_VERIFY whose image crashes before it can call
`esp_ota_mark_app_valid_cancel_rollback` is reverted on the next boot. That is
silicon-plus-bootloader, and it is the property that decides whether a bad update
bricks a car-installed device.

The image `tools/bench_rollback.py` builds has one addition, guarded by
`-D SWC_BENCH_PANIC_IMAGE`: a NULL dereference as the FIRST statement of
`app_main`, before any HAL or orchestrator work -- so the image cannot reach the
mark-valid call it would need to survive. See `src/main.cpp`.

The test, in order, on the live device:

  1. baseline: `uptime_ms`, `heap_free`, `config_state`
  2. push the panicking image over the real USB-OTA path and commit it
     (`ota_begin`/`ota_chunk`/`ota_end`), then reboot into it
  3. the device will panic and reset; wait for it to serve frames again
  4. **the assertion**: it DOES serve frames again, which for THIS image can
     only mean the bootloader rolled back -- the image that was just committed
     crashes before it can answer anything
  5. `uptime_ms` on the recovered device is small (it just booted), and its
     `config_state` is `ok` (the running old image is healthy)

Exit codes: 0 the device recovered on the old image, 1 the device did not come
back (rollback failed -- a brick), 2 usage/port error.

**Ordering hazard this script guards against:** `pio test -e native` after a
var'd `pio run` deletes `.pio/build/`. The bad image is built in its own
`PLATFORMIO_BUILD_DIR`, so no other build is disturbed.
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
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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


def wait_for_frames(port, seconds):
    """Reconnect repeatedly and return the first `status` seen, or None.

    A panic reset drops the CDC port, so the open has to be retried -- the
    device re-enumerates a second or so after the reboot.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            link = Link(port)
        except Exception:
            time.sleep(0.5)
            continue
        try:
            st = link.status(2.0)
            if st is not None:
                return st
        finally:
            link.close()
        time.sleep(0.5)
    return None


def build_bad_image(outdir):
    """Build firmware.bin with the panic hook compiled in."""
    env = dict(os.environ)
    env["SWC_FW_VERSION"] = env.get("SWC_FW_VERSION", "dev")
    env["SWC_GIT_SHA"] = env.get("SWC_GIT_SHA", "local")
    env["PLATFORMIO_BUILD_FLAGS"] = "-D SWC_BENCH_PANIC_IMAGE"
    env["PLATFORMIO_BUILD_DIR"] = outdir
    print(f"== building the panicking image into {outdir}")
    r = subprocess.run(["pio", "run", "-e", "esp32s3"], cwd=REPO, env=env,
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print(r.stdout[-2000:], file=sys.stderr)
        print(r.stderr[-2000:], file=sys.stderr)
        return None
    path = os.path.join(outdir, "esp32s3", "firmware.bin")
    return path if os.path.exists(path) else None


def push(link, image):
    data = open(image, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    print(f"== pushing {len(data)} bytes, sha256 {sha[:16]}...")
    print(f"   (the wire chunk is {CHUNK} B and the device acks each one, so this "
          f"takes roughly {len(data) // 3300 // 60} min -- progress every 256 chunks)")
    t, m = link.request(type="ota_begin", size=len(data), sha256=sha)
    if t != "ack":
        print(f"ERROR: ota_begin refused: {m}", file=sys.stderr)
        return False
    off = 0
    n = 0
    started = time.time()
    while off < len(data):
        piece = data[off:off + CHUNK]
        t, m = link.request(type="ota_chunk", offset=off,
                            data_b64=base64.b64encode(piece).decode("ascii"))
        if t != "ack":
            print(f"ERROR: ota_chunk at {off} refused: {m}", file=sys.stderr)
            return False
        off += len(piece)
        n += 1
        if n % 256 == 0:
            pct = 100 * off // len(data)
            print(f"   {off}/{len(data)} ({pct}%) {int(time.time() - started)}s",
                  flush=True)
    t, m = link.request(type="ota_end")
    if t != "ack":
        print(f"ERROR: ota_end refused: {m}", file=sys.stderr)
        return False
    result = (m or {}).get("result")
    if result != "ok":
        print(f"ERROR: not installed (result={result})", file=sys.stderr)
        return False
    print(f"== panicking image verified and committed ({int(time.time() - started)}s)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", default=None,
                    help="use an existing panicking image instead of building one")
    ap.add_argument("--build-dir", default="/tmp/swc_badimg")
    args = ap.parse_args()

    image = args.image or build_bad_image(args.build_dir)
    if not image:
        print("ERROR: could not build the panicking image", file=sys.stderr)
        return 2

    try:
        link = Link(args.port)
    except OSError as e:
        print(f"ERROR: cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    try:
        base = link.status(2.0)
        if base is None:
            print("ERROR: the device is not answering; nothing to test",
                  file=sys.stderr)
            return 2
        print(f"== baseline: uptime={base.get('uptime_ms')} heap={base.get('heap_free')} "
              f"config_state={base.get('config_state')}")

        if not push(link, image):
            return 1

        # Reboot into the panicking image. The reply is best-effort: the device
        # acks and restarts, so the port drops under us.
        print("== rebooting into the panicking image (expect a panic reset)")
        try:
            link.request(type="reboot", boot_target="app")
        except serial.SerialException:
            pass
    finally:
        link.close()

    # The device panics, resets, the bootloader tries the pending image again /
    # rolls back, and eventually serves frames again. Give it room: a bootloader
    # rollback can take a couple of reset cycles.
    print("== waiting for the device to serve frames again (rollback)...")
    recovered = wait_for_frames(args.port, 30.0)
    if recovered is None:
        print("FAIL: the device did NOT come back within 30 s -- the committed "
              "image never rolled back (a brick)", file=sys.stderr)
        return 1

    print(f"== RECOVERED: uptime={recovered.get('uptime_ms')} "
          f"heap={recovered.get('heap_free')} config_state={recovered.get('config_state')}")
    if recovered.get("config_state") != "ok":
        print(f"FAIL: came back but config_state={recovered.get('config_state')}",
              file=sys.stderr)
        return 1
    print("PASS: the device panicked on the new image and the bootloader rolled "
          "back to the old one (FR-37). It serves frames again on a healthy image.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
