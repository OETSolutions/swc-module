#!/usr/bin/env python3
"""Every protocol handler must run on ONE task, and USB callbacks must not.

**Why this is a gate and not a review step.** `UsbLink.cpp` is the one TU the
host build excludes besides `EspHal.cpp`, and for the same reason: it names
TinyUSB. Its callbacks are reached by the **TinyUSB task** (priority 5), while
`SystemOrchestrator::Tick` runs on the poll loop in **app_main** (priority 1) --
both pinned to core 0, so the callback PREEMPTS the poll loop at any instruction.
Nothing in this firmware takes a lock (verified: no `xSemaphore*`,
`portENTER_CRITICAL`, `std::mutex` or `std::atomic` in `lib/` or `src/` outside
the transport's own ring), so any state both tasks touch is a data race.

That was true of the whole protocol, and the host suite cannot see it: the tests
call `OnLine` and `Tick` from ONE thread, so mutual exclusion is free there and
the race is invisible. The failure on a real device is a torn read of the
8,912-byte `Config` while `Tick` classifies against it -- a wrong key voltage on
the output, which is the direction the spec calls dangerous -- or a half-updated
classifier.

The shape this gate pins, which is what makes the split safe and is easy to
undo by "simplifying" a callback:

  1. `CdcRxCallback` must NOT parse: it must not call `UsbCdc::DrainRx`, and it
     must not reach the sink. It stages bytes and returns.
  2. `CdcLineStateCallback` must NOT call into the router or the orchestrator:
     no `OnConnected`/`OnDisconnected`, no `SetUsbConnected`, no `ResetSession`.
     It publishes a pending transition that `UsbLinkService` applies.
  2b. `CdcLineStateCallback` must NOT read the APPLIED state (`g_host_open`). An
     edge published against the applied state loses a transition: if the host
     opens and closes between two poll ticks, the close is compared against a
     `g_host_open` that is still false (the open has not been applied yet), is
     judged "no change", and is dropped -- the device then applies the open and
     latches a session the host already ended. The callback must publish the DTR
     LEVEL unconditionally and let `ServiceLineState` reconcile.
  3. `UsbLinkService` -- which runs on the poll task -- must call `DrainRx` and
     the line-state service, so the deferred work actually happens.
  4. `FeedBytes` must not call `DrainRx` (parsing in the producer is the original
     bug) and must not touch the sink.

Exit codes: 0 clean, 1 a violation, 2 a file could not be found.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
USB_LINK = REPO / "lib" / "Link" / "UsbLink.cpp"
USB_CDC = REPO / "lib" / "Link" / "UsbCdc.cpp"


def _read(p: pathlib.Path):
    if not p.exists():
        print(f"FAIL: {p} not found", file=sys.stderr)
        return None
    return p.read_text()


def _strip_comments(src: str) -> str:
    """Drop // and /* */ so a comment QUOTING a forbidden call (this codebase
    documents its invariants in prose, including the wrong version) cannot trip
    the check."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _fn_body(src: str, name: str, ret: str = "") -> str:
    """The brace-matched body of a function DEFINITION, or '' if not found."""
    m = re.search(
        r"^[A-Za-z_][\w:<>,\s\*&]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{",
        src,
        flags=re.MULTILINE,
    )
    if not m:
        return ""
    start = src.index("{", m.start())
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    return ""


def main() -> int:
    link = _read(USB_LINK)
    cdc = _read(USB_CDC)
    if link is None or cdc is None:
        return 2
    link = _strip_comments(link)
    cdc = _strip_comments(cdc)

    problems = []

    # 1. The RX callback stages bytes; it must not parse or reach the sink.
    body = _fn_body(link, "CdcRxCallback")
    if not body:
        problems.append("CdcRxCallback not found in UsbLink.cpp")
    else:
        if "DrainRx" in body:
            problems.append(
                "CdcRxCallback calls UsbCdc::DrainRx; parsing in the TinyUSB "
                "callback runs every protocol handler against state the poll task "
                "is concurrently mutating (Tick). It must only stage bytes."
            )
        if "FeedBytes" not in body:
            problems.append(
                "CdcRxCallback does not call FeedBytes; the staged ring is the only "
                "safe way to hand bytes from the USB task to the poll task."
            )

    # 2. The line-state callback must not reach the router or the orchestrator.
    body = _fn_body(link, "CdcLineStateCallback")
    if not body:
        problems.append("CdcLineStateCallback not found in UsbLink.cpp")
    else:
        for forbidden, why in (
            ("OnConnected",
             "emits `hello` and starts the config reply run, i.e. mutates router "
             "state the poll task reads"),
            ("OnDisconnected",
             "drops the run state the poll task reads"),
            ("SetUsbConnected",
             "mutates the orchestrator, which Tick is using"),
            ("ResetSession",
             "resets the transport's reader and TX, which DrainRx also owns -- the "
             "session reset must run on the poll task so it cannot land inside a "
             "producer call"),
        ):
            if forbidden in body:
                problems.append(
                    f"CdcLineStateCallback calls {forbidden}, which {why}. It runs on "
                    "the TinyUSB task; publish a pending transition and let "
                    "UsbLinkService apply it on the poll task."
                )
        if "g_host_open" in body:
            problems.append(
                "CdcLineStateCallback reads g_host_open, the APPLIED state. Publishing "
                "an edge against it loses a transition: an open+close between two poll "
                "ticks drops the close (g_host_open is still false), and the device "
                "then latches a session the host already ended. Publish the DTR LEVEL "
                "unconditionally; ServiceLineState reconciles and is idempotent."
            )

    # 3. The poll task must actually do the deferred work.
    body = _fn_body(link, "UsbLinkService")
    if not body:
        problems.append("UsbLinkService not found in UsbLink.cpp")
    else:
        if "DrainRx" not in body:
            problems.append(
                "UsbLinkService does not call UsbCdc::DrainRx, so staged frames are "
                "never parsed and no command is ever handled."
            )
        if "ServiceLineState" not in body:
            problems.append(
                "UsbLinkService does not call ServiceLineState, so the deferred DTR "
                "transition never happens and `hello` is never sent."
            )

    # 4. The transport's producer side must not parse either.
    body = _fn_body(cdc, "UsbCdc::FeedBytes") or _fn_body(cdc, "FeedBytes")
    if not body:
        problems.append("UsbCdc::FeedBytes not found in UsbCdc.cpp")
    else:
        if "DrainRx" in body:
            problems.append(
                "UsbCdc::FeedBytes calls DrainRx; the producer runs on the TinyUSB "
                "task and must not parse (that is the race this split removes)."
            )
        if "sink_" in body:
            problems.append(
                "UsbCdc::FeedBytes reaches the frame sink; delivery must happen in "
                "DrainRx, on the poll task."
            )

    if problems:
        print("task-ownership guard FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("task-ownership guard: OK")
    print("  USB callbacks stage bytes / publish DTR only; no protocol state touched")
    print("  UsbLinkService (poll task) drains the ring and applies the transition")
    return 0


if __name__ == "__main__":
    sys.exit(main())
