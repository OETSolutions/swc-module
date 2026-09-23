#!/usr/bin/env python3
"""Send one frame to the device's app link and wait for its `ack`.

**Why this is a script and not inline `serial.write` in the shell script.** The
device speaks NDJSON over a TinyUSB CDC port, and the ACK RACES THE RESET: the
firmware emits the `ack`, FLUSHES it synchronously, and *then* restarts (see
`CommandRouter::HandleReboot` and `SetTxFlush`). A sender that writes the frame
and exits immediately can close the port before the flush lands, and the caller
then cannot tell "the device refused the target" from "the device rebooted before
I read the reply" -- which are the two outcomes this helper exists to separate.

So it holds the port open, reads until it sees the `ack` for the sequence it sent
(or a `nack`, or a timeout), and exits nonzero on anything but an ack.

The port is opened at 115200 for the CDC control lines only; TinyUSB CDC ignores
the baud rate for data, so the value is not a real speed.

Exit codes: 0 acked, 1 nacked or no reply, 2 usage/port error.
"""

import argparse
import json
import sys
import time

try:
    import serial  # pyserial, via the PlatformIO penv
except ImportError:  # pragma: no cover - environment problem, reported plainly
    print("ERROR: pyserial not available; run with the PlatformIO python "
          "(~/.platformio/penv/bin/python) or `pip install pyserial`.", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="the device's app-link serial port")
    ap.add_argument("--frame", required=True, help="one frame, as JSON, without the newline")
    ap.add_argument("--seq", type=int, default=None,
                    help="sequence to match against the ack; default: the frame's own seq")
    ap.add_argument("--expect-ack", action="store_true",
                    help="exit nonzero unless an ack for the sequence arrives")
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()

    try:
        frame = json.loads(args.frame)
    except json.JSONDecodeError as e:
        print(f"ERROR: --frame is not valid JSON: {e}", file=sys.stderr)
        return 2
    seq = args.seq if args.seq is not None else frame.get("seq")

    try:
        port = serial.Serial(args.port, 115200, timeout=0.25)
    except OSError as e:
        print(f"ERROR: cannot open {args.port}: {e}", file=sys.stderr)
        return 2

    with port:
        # The device may be mid-`config_get` reply when we attach (it starts one
        # on connect), so drain whatever is already buffered before we send --
        # otherwise a stale frame could be mistaken for our reply.
        time.sleep(0.3)
        port.reset_input_buffer()
        port.write((json.dumps(frame, separators=(",", ":")) + "\n").encode())
        port.flush()

        deadline = time.time() + args.timeout
        pending = b""
        while time.time() < deadline:
            chunk = port.read(4096)
            if chunk:
                pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                t = msg.get("type")
                # Only THIS sequence's ack/nack is ours; the device also emits
                # status/event/config frames on the same link.
                if t == "ack" and msg.get("for_seq") == seq:
                    print(f"acked: seq={seq}")
                    return 0
                if t == "nack" and msg.get("for_seq") == seq:
                    print(f"nacked: {msg.get('err')}: {msg.get('detail')}", file=sys.stderr)
                    return 1

    print(f"ERROR: no ack for seq={seq} within {args.timeout}s", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
