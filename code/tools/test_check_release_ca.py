"""The release-CA gate must FAIL on each shape it claims to catch.

A guard that only ever prints "OK" is indistinguishable from one whose checks
never ran -- the `swc-stack-gate-silent-undertcount` failure, where a gate summed
zero chains and printed the same green line as a real pass. So each check is
driven here against a MUTATED copy of the real source: the mutation is the defect
the check exists for, and the assertion is that the check names it.

The mutations are textual, applied to the file as it exists, so this test breaks
-- loudly -- if the code it reads is renamed or restructured, rather than
silently passing against a stub.

The N-92 shape is the important one: a pin holding only ONE of the two roots the
release chain traverses. That is exactly the state the tree was in when the DUT
reported "the release manifest could not be read" while the repo was public.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_release_ca as crc  # noqa: E402

REAL_CA = crc.CA_H.read_text(encoding="utf-8")
REAL_OTA = crc.OTA_CPP.read_text(encoding="utf-8")


def _mutate(text, old, new):
    assert old in text, f"the mutation target is gone: {old!r}"
    return text.replace(old, new, 1)


def test_the_real_source_passes():
    # The baseline, without which every mutation below proves nothing.
    assert crc.check_pin_shape(REAL_CA) == []
    assert crc.check_ota_uses_the_pin(REAL_OTA) == []
    pems, n = crc.extract_pems(REAL_CA)
    assert n == 2
    problems, _ = crc.check_identities(pems)
    assert problems == []


# --- (1) the pin must hold BOTH roots (N-92) --------------------------------

def test_a_single_root_pin_is_caught():
    """N-92: dropping the ISRG (hop-2) certificate leaves the redirect hop unable
    to build a chain -- the exact production failure."""
    # Remove the second PEM (ISRG Root X1) and its comment marker.
    start = REAL_CA.index("// --- ISRG Root X1")
    end = REAL_CA.index("-----END CERTIFICATE-----\\n\"", start) + len(
        '-----END CERTIFICATE-----\\n"')
    mutated = REAL_CA[:start] + REAL_CA[end:]
    problems = crc.check_pin_shape(mutated)
    assert problems, "a one-root pin must be reported"
    assert "exactly 2" in problems[0]


def test_a_third_certificate_is_caught():
    """A third cert drifts back toward the N-62 bundle."""
    mutated = _mutate(REAL_CA, 'static const char kReleaseCaPem[] =',
                      'static const char kReleaseCaPem[] =\n'
                      '    "-----BEGIN CERTIFICATE-----\\n"\n'
                      '    "QUJD\\n"\n'
                      '    "-----END CERTIFICATE-----\\n"')
    problems = crc.check_pin_shape(mutated)
    assert problems, "a three-cert pin must be reported"


def test_the_symbol_missing_is_caught():
    mutated = _mutate(REAL_CA, "kReleaseCaPem", "kSomethingElse")
    assert crc.check_pin_shape(mutated)


# --- (2) OtaWifi must select the pin, not the bundle ------------------------

def test_the_default_bundle_is_caught():
    mutated = _mutate(REAL_OTA, "cfg.cert_pem = kReleaseCaPem;",
                      "cfg.crt_bundle_attach = esp_crt_bundle_attach;")
    problems = crc.check_ota_uses_the_pin(mutated)
    assert any("crt_bundle_attach" in p for p in problems)


def test_an_ota_that_ignores_the_pin_is_caught():
    mutated = REAL_OTA.replace("kReleaseCaPem", "kBundledPem")
    problems = crc.check_ota_uses_the_pin(mutated)
    assert any("does not reference kReleaseCaPem" in p for p in problems)


# --- (3) the identities must cover both hops --------------------------------

def test_missing_hop_two_subject_is_caught():
    """If the second PEM is a real cert but NOT the redirect root, the gate names
    the absent subject rather than passing on count alone."""
    # Swap the ISRG cert for an unrelated valid one: use the Sectigo cert twice.
    sectigo = REAL_CA[REAL_CA.index("-----BEGIN CERTIFICATE-----"):
                      REAL_CA.index('-----END CERTIFICATE-----\\n"') +
                      len('-----END CERTIFICATE-----\\n"')]
    start = REAL_CA.index("// --- ISRG Root X1")
    body_start = REAL_CA.index("-----BEGIN CERTIFICATE-----", start)
    body_end = REAL_CA.index('-----END CERTIFICATE-----\\n"', body_start) + len(
        '-----END CERTIFICATE-----\\n"')
    mutated = REAL_CA[:body_start] + sectigo + REAL_CA[body_end:]
    pems, n = crc.extract_pems(mutated)
    assert n == 2, "still two certs, so the count check alone would pass"
    problems, _ = crc.check_identities(pems)
    assert any("ISRG Root X1" in p for p in problems), \
        "the absent hop-2 root must be named"
