#!/usr/bin/env python3
"""Pin the maintenance radio's window-scoped invariants (FR-32, N-15).

**Why this is a gate and not a review step.** `MaintenanceRadio.cpp` is the
THIRD translation unit the host build excludes (beside `EspHal.cpp` and
`UsbLink.cpp`), so no native test compiles it and `test/test_hw` needs the board.
Every property below is therefore asserted by nothing that runs, and each one is
a shape this project has already shipped once:

  1. **Start and stop must pair, and stop must free what start created.**
     FR-32 is a hard rule -- a leaked stack is a device with a live radio while
     it drives a car's steering wheel. `TearDown` therefore frees per-resource
     (one flag each), because the bring-up can bail out partway and a flat
     "free everything" would free something that never came up. Checked here is
     the weaker but checkable half: every flag `TearDown` reads is also SET
     somewhere in the bring-up, so a resource added later cannot be created
     without a corresponding teardown.

  2. **The HTTP handler must reply for EVERY outcome, and must not send a
     status line that can be read as success.** The router's `HttpOutcome`
     carries a status, and `SendOutcome` must call `httpd_resp_set_status` for a
     non-200 -- the app's script checks `r.ok`, so a 401 body sent with a 200
     status line is a refusal the page renders as success (the "reported as
     success" confusion the token gate exists to prevent).

  3. **The failure count must describe the CURRENT window.** It is cleared on
     stop and set on every failed start. The app branches on `ble_failures > 0`
     FIRST when writing the maintenance card, so a sticky count makes every
     later window claim its radio was down while the same frame carries a live
     `page_url` -- a user told there is no setup page with the page right there.

  4. **The failure count must be ASSIGNED, never accumulated.** `+=` makes it a
     running total, which answers a different question than "did this window's
     radio come up" and grows without bound.

Each check is structural, which is the point: none can tell a working bring-up
from a broken one (that is the bench's job, per the header), but each pins the
shape that silently rots -- a resource created with no teardown, a reply path
that forgets the status line, a counter that outlives its window.

Exit codes: 0 clean, 1 a violation, 2 a file could not be found.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
RADIO = REPO / "lib" / "Maintenance" / "MaintenanceRadio.cpp"

# Flags that describe this module's own STATE rather than a created resource, and
# are therefore not expected to guard a teardown step. `g_active` is "the radio
# is up"; there is nothing to free for it, and it is cleared for a different
# reason (so `MaintenanceRadioActive` reports down). Named here rather than
# inferred, because inferring "state" from a name is how a real resource quietly
# joins the exclusion list.
_STATE_FLAGS = frozenset({"g_active"})


def _strip_comments(src: str) -> str:
    """Drop // and /* */ so a comment QUOTING the wrong call cannot trip this.

    This codebase documents its invariants in prose, including the wrong version
    of them (that is how several defects were found), so a checker that read
    comments would fail on the documentation of a bug rather than the bug.
    """
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _fn_body(src: str, name: str) -> str:
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


def check_teardown_pairs_the_bringup(src: str):
    """(1) Every flag TearDown clears must be SET by the bring-up.

    A resource that is created and never torn down is FR-32's exact violation,
    and the host build cannot see it -- there is no test that compiles this file.
    The flag names are read from `TearDown` itself rather than a second list, so
    a resource added to the teardown is what drives the check.
    """
    problems = []
    teardown = _fn_body(src, "TearDown")
    if not teardown:
        return ["TearDown not found in MaintenanceRadio.cpp"]

    # `<flag> = false;` inside TearDown names the resources it owns. A state flag
    # is not one of them -- see `_STATE_FLAGS`.
    flags = sorted(
        set(re.findall(r"\b(g_\w+)\s*=\s*false\s*;", teardown)) - _STATE_FLAGS
    )
    if not flags:
        problems.append(
            "TearDown clears no resource flag; it frees by flag, so a flagless "
            "teardown is a flat 'free everything' that frees resources a partial "
            "bring-up never created."
        )

    # The pointer handles are the other half: they are nulled, not flag-cleared.
    handles = sorted(set(re.findall(r"\b(g_\w+)\s*=\s*nullptr\s*;", teardown)))

    creation = src
    for flag in flags:
        # A flag is "set" if it appears on the left of a true-assignment or is
        # declared true, anywhere OUTSIDE the teardown.
        outside = src.replace(teardown, " ")
        if not re.search(r"\b" + re.escape(flag) + r"\s*=\s*true\s*;", outside):
            problems.append(
                f"TearDown clears {flag}, but nothing sets it true outside the "
                f"teardown. Freeing by a flag that is never set means the "
                f"resource it guards is either never freed (leaked radio, FR-32) "
                f"or the flag is dead."
            )
    for h in handles:
        if not re.search(r"\b" + re.escape(h) + r"\s*=\s*[^=]", creation.replace(teardown, " ")):
            problems.append(
                f"TearDown nulls {h}, but nothing else assigns it. The handle it "
                f"guards is then never created or never released."
            )
    return problems


def check_every_reply_sets_its_status(src: str):
    """(2) A non-200 reply must set the status line."""
    problems = []
    body = _fn_body(src, "SendOutcome")
    if not body:
        return ["SendOutcome not found in MaintenanceRadio.cpp"]
    if "httpd_resp_set_status" not in body:
        problems.append(
            "SendOutcome never calls httpd_resp_set_status. The app's script "
            "checks `r.ok`, so a 401/404/500 body sent with the default 200 "
            "status line is a refusal the page renders as success -- the exact "
            "confusion the token gate exists to prevent."
        )
    if "o.status" not in body:
        problems.append(
            "SendOutcome does not read the outcome's status, so every reply goes "
            "out as the default response."
        )
    return problems


def check_the_failure_count_is_window_scoped(src: str):
    """(3)+(4) Cleared on stop, set (not accumulated) on a failed start."""
    problems = []

    stop = _fn_body(src, "MaintenanceRadioStop")
    if not stop:
        problems.append("MaintenanceRadioStop not found in MaintenanceRadio.cpp")
    elif not re.search(r"\bg_failures\s*=\s*0\s*;", stop):
        problems.append(
            "MaintenanceRadioStop does not clear g_failures. The count answers "
            "'did THIS window's radio come up', and the app branches on it FIRST "
            "when writing the maintenance card -- so a value left standing makes "
            "every later window say its radio was down while the same frame "
            "carries a live page_url."
        )

    start = _fn_body(src, "MaintenanceRadioStart")
    if not start:
        problems.append("MaintenanceRadioStart not found in MaintenanceRadio.cpp")
    else:
        # Any mutation of the count, prefix-increment included, so `++g_failures`
        # cannot slip past a check that only looked for a trailing operator.
        mutated = re.findall(r"\bg_failures\s*(\+\+|--|\+=|-=|=)", start)
        mutated += ["++"] * len(re.findall(r"(?:\+\+|--)\s*g_failures\b", start))
        if not mutated:
            problems.append(
                "MaintenanceRadioStart never records a failure, so a window whose "
                "radio did not come up is indistinguishable from a working one."
            )
        for op in mutated:
            if op in ("++", "--", "+=", "-="):
                problems.append(
                    f"MaintenanceRadioStart mutates g_failures with '{op}'. It is "
                    f"a state (0 = this window's radio is up), not a running "
                    f"total: accumulating answers a different question and grows "
                    f"without bound. Assign 1 on a failure."
                )
    return problems


def main() -> int:
    if not RADIO.exists():
        print(f"FAIL: {RADIO} not found", file=sys.stderr)
        return 2
    src = _strip_comments(RADIO.read_text(encoding="utf-8"))

    problems = []
    problems += check_teardown_pairs_the_bringup(src)
    problems += check_every_reply_sets_its_status(src)
    problems += check_the_failure_count_is_window_scoped(src)

    if problems:
        print("maintenance-radio guard FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("maintenance-radio guard: OK")
    print("  TearDown frees by a flag the bring-up sets (FR-32)")
    print("  every reply sets its status line, so no refusal reads as success")
    print("  the failure count is window-scoped and assigned, never accumulated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
