"""The maintenance-radio gate must FAIL on each shape it claims to catch.

A guard that only ever prints "OK" is indistinguishable from one whose checks
never ran -- the `swc-stack-gate-silent-undertcount` failure, where a gate summed
zero chains and printed the same green line as a real pass. So each check is
driven here against a MUTATED copy of the real source: the mutation is the
defect the check exists for, and the assertion is that the check names it.

The mutations are textual, applied to the file as it exists, so this test breaks
-- loudly -- if the code it reads is renamed or restructured, rather than
silently passing against a stub.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_maintenance_radio as cmr  # noqa: E402

REAL = cmr._strip_comments(cmr.RADIO.read_text(encoding="utf-8"))


def _mutate(old: str, new: str) -> str:
    """Apply one textual mutation to the real, comment-stripped source."""
    assert old in REAL, f"the mutation target is gone from MaintenanceRadio.cpp: {old!r}"
    return REAL.replace(old, new, 1)


def _mutate_all(old: str, new: str) -> str:
    """Apply a mutation to EVERY occurrence.

    Needed because the source records a failure on four separate paths; mutating
    only the first would leave the gate a correct one to find and the assertion
    would pass for the wrong reason.
    """
    assert old in REAL, f"the mutation target is gone from MaintenanceRadio.cpp: {old!r}"
    return REAL.replace(old, new)


def test_the_real_source_passes():
    # The baseline, without which every mutation below proves nothing.
    assert cmr.check_teardown_pairs_the_bringup(REAL) == []
    assert cmr.check_every_reply_sets_its_status(REAL) == []
    assert cmr.check_the_failure_count_is_window_scoped(REAL) == []


# --- (1) teardown pairs the bring-up ---------------------------------------

def test_a_resource_freed_by_an_unset_flag_is_caught():
    # A flag the teardown clears but the bring-up never sets: the resource it
    # guards is either leaked (FR-32) or the flag is dead.
    mutated = _mutate("g_netif_inited = true;", "/* removed */;")
    problems = cmr.check_teardown_pairs_the_bringup(mutated)
    assert any("g_netif_inited" in p for p in problems), problems


def test_a_teardown_with_no_flags_is_caught():
    # The flat "free everything" shape: no per-resource flags at all. Every
    # `<flag> = false;` in the file goes, so the teardown names nothing.
    mutated = REAL
    for flag in ("g_netif_inited", "g_event_loop", "g_wifi_inited", "g_prov_inited"):
        mutated = mutated.replace(f"{flag} = false;", "(void)0;")
    problems = cmr.check_teardown_pairs_the_bringup(mutated)
    assert any("no resource flag" in p for p in problems), problems


# --- (2) every reply sets its status ---------------------------------------

def test_a_missing_status_line_is_caught():
    # The "refusal reads as success" defect: drop the one status-line call.
    mutated = _mutate("httpd_resp_set_status(req, status);", "(void)status;")
    problems = cmr.check_every_reply_sets_its_status(mutated)
    assert any("httpd_resp_set_status" in p for p in problems), problems


# --- (3) the failure count is window-scoped --------------------------------

def test_a_sticky_failure_count_is_caught():
    # The cross-window bug found in this pass: nothing clears the count, so one
    # failed bring-up makes every later window claim its radio is down.
    mutated = _mutate("g_failures = 0;\n    if (!g_active", "if (!g_active")
    problems = cmr.check_the_failure_count_is_window_scoped(mutated)
    assert any("does not clear g_failures" in p for p in problems), problems


def test_an_accumulated_failure_count_is_caught():
    # `+=` makes it a running total, which answers a different question.
    mutated = _mutate_all("g_failures = 1;", "++g_failures;")
    problems = cmr.check_the_failure_count_is_window_scoped(mutated)
    assert any("not a running total" in p or "Assign 1" in p for p in problems), problems


def test_a_start_that_never_records_a_failure_is_caught():
    mutated = _mutate_all("g_failures = 1;", "(void)0;")
    problems = cmr.check_the_failure_count_is_window_scoped(mutated)
    assert any("never records a failure" in p for p in problems), problems
