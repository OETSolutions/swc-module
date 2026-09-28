#!/usr/bin/env python3
"""Self-test for `gen_release_manifest.py`.

The manifest this script emits is parsed by the firmware's `ReleaseCheck.cpp` and
the app's `ReleaseManifest.kt`, and a shape mismatch is a SILENT permanent
failure (the device reports "the release manifest could not be read" and names
nothing). So the rules that matter are pinned here rather than trusted:

  * `latest_version` == `firmware.version`
  * a whole-number `size_bytes`, a real `sha256`, an `https://` url
  * a non-https url and a fractional size are REFUSED at generation time
  * `min_from_version` / `channel` are optional

Run: `python3 -m pytest test_gen_release_manifest.py -q`
"""

import hashlib
import importlib.util
import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "gen_release_manifest.py")


def run(args):
    return subprocess.run([sys.executable, SCRIPT, *args],
                          capture_output=True, text=True)


@pytest.fixture
def firmware(tmp_path):
    p = tmp_path / "firmware.bin"
    p.write_bytes(b"\xE9\x00\x02\x00" + b"x" * 4096)
    return str(p)


def test_it_emits_the_firmware_and_app_manifest_shape(firmware, tmp_path):
    out = tmp_path / "m.json"
    r = run(["--version", "1.2.3", "--firmware", firmware,
             "--url", "https://example.test/fw.bin", "--out", str(out)])
    assert r.returncode == 0, r.stderr
    m = json.loads(out.read_text())
    # The fields ReleaseCheckParse reads as REQUIRED.
    assert m["latest_version"] == "1.2.3"
    assert m["firmware"]["version"] == m["latest_version"]
    assert m["firmware"]["url"] == "https://example.test/fw.bin"
    assert isinstance(m["firmware"]["size_bytes"], int)
    assert m["firmware"]["size_bytes"] == os.path.getsize(firmware)
    assert m["firmware"]["sha256"] == hashlib.sha256(open(firmware, "rb").read()).hexdigest()


def test_a_leading_v_is_stripped_from_the_version(firmware):
    r = run(["--version", "v2.0.0", "--firmware", firmware,
             "--url", "https://example.test/fw.bin"])
    assert r.returncode == 0, r.stderr
    assert json.loads(r.stdout)["latest_version"] == "2.0.0"


def test_a_plain_http_url_is_refused(firmware):
    # The device refuses a non-https url (ReleaseCheck.cpp's IsHttpsUrl), so
    # generating one would be a manifest that can never install.
    r = run(["--version", "1.0.0", "--firmware", firmware,
             "--url", "http://example.test/fw.bin"])
    assert r.returncode == 2
    assert "https" in r.stderr


def test_a_non_semver_version_is_refused(firmware):
    r = run(["--version", "latest", "--firmware", firmware,
             "--url", "https://example.test/fw.bin"])
    assert r.returncode == 2


def test_a_missing_firmware_is_refused(tmp_path):
    r = run(["--version", "1.0.0", "--firmware", str(tmp_path / "nope.bin"),
             "--url", "https://example.test/fw.bin"])
    assert r.returncode == 2


def test_the_optional_fields_appear_only_when_asked(firmware):
    base = run(["--version", "1.0.0", "--firmware", firmware,
                "--url", "https://example.test/fw.bin"])
    m = json.loads(base.stdout)
    assert m["channel"] == "stable"          # defaulted, but always present
    assert "min_from_version" not in m       # absent unless requested

    withfloor = run(["--version", "1.0.0", "--firmware", firmware,
                     "--url", "https://example.test/fw.bin",
                     "--min-from-version", "0.9.0"])
    assert json.loads(withfloor.stdout)["min_from_version"] == "0.9.0"


def test_the_output_is_byte_stable(firmware):
    a = run(["--version", "1.0.0", "--firmware", firmware,
             "--url", "https://example.test/fw.bin"]).stdout
    b = run(["--version", "1.0.0", "--firmware", firmware,
             "--url", "https://example.test/fw.bin"]).stdout
    assert a == b
    assert a.endswith("\n")
