"""The OTA-buffer gate must FAIL on each shape it claims to catch.

A guard that only ever prints "OK" is indistinguishable from one whose checks
never ran -- the `swc-stack-gate-silent-undertcount` failure, where a gate summed
zero chains and printed the same green line as a real pass. So each check is
driven here against a MUTATED copy of the real source: the mutation is the defect
the check exists for, and the assertion is that the check names it.

The N-92 shape is the important one: a config that does NOT set `buffer_size_tx`
(the check path's original state, which inherits IDF's 512-byte default and cannot
fit a GitHub release redirect's ~900-byte first line).
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_ota_buffer as cob  # noqa: E402

REAL = cob.OTA_CPP.read_text(encoding="utf-8")


def _mutate(old, new):
    assert old in REAL, f"the mutation target is gone: {old!r}"
    return REAL.replace(old, new, 1)


def test_the_real_source_passes():
    # The baseline, without which every mutation below proves nothing.
    assert cob.check_http_client_buffers(REAL) == []
    assert len(cob.config_blocks(REAL)) == 2


def test_a_config_missing_buffer_size_tx_is_caught():
    """Exactly N-92: the check config with no `buffer_size_tx` line."""
    mutated = _mutate("    cfg.buffer_size_tx = 1024;\n", "")
    problems = cob.check_http_client_buffers(mutated)
    assert problems, "a config with no buffer_size_tx must be reported"
    assert "buffer_size_tx" in problems[0]


def test_a_too_small_buffer_size_tx_is_caught():
    mutated = _mutate("    cfg.buffer_size_tx = 1024;",
                      "    cfg.buffer_size_tx = 512;")
    problems = cob.check_http_client_buffers(mutated)
    assert any("below" in p for p in problems)


def test_a_file_with_no_http_configs_is_caught():
    """If the file changes shape so the scan finds nothing, that is a failure, not
    a silent pass."""
    problems = cob.check_http_client_buffers("int x;\n")
    assert any("found no esp_http_client_config_t" in p for p in problems)
