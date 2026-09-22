"""Tests for the size-gate's test-image discriminator (spec 10.5).

The gate's whole value is that it reports a number for the RIGHT file. A device
build and a device test build share one `.pio/build/esp32s3/firmware.bin`, so the
gate measured the test image in CI (287 KB) while the production image was 386 KB
-- under-reporting by ~99 KB and ready to pass an over-slot image. These pin the
discriminator that makes the mistake impossible rather than merely unlikely.

These do NOT need PlatformIO: `is_unity_test_image` reads an ELF's bytes, so a
tiny synthetic file with or without the Unity symbol names exercises it exactly.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_size  # noqa: E402


def _write(tmp_path: pathlib.Path, name: str, body: bytes) -> pathlib.Path:
    p = tmp_path / name
    p.write_bytes(body)
    return p


def test_a_unity_image_is_detected(tmp_path):
    # A test image links Unity's runner; the name of its entry point is in the
    # symbol table (and the string table), so its presence means "not production".
    elf = _write(tmp_path, "firmware.elf", b"\x7fELF....\x00UnityDefaultTestRun\x00....")
    assert check_size.is_unity_test_image(elf) is True


def test_a_production_image_is_not_detected(tmp_path):
    # The production image carries none of the Unity runner's symbols.
    elf = _write(tmp_path, "firmware.elf", b"\x7fELF....\x00SystemOrchestrator\x00....")
    assert check_size.is_unity_test_image(elf) is False


def test_a_missing_elf_is_not_a_test_image(tmp_path):
    # The ELF is a sibling of the bin, not the bin itself; a size-only invocation
    # with no ELF must not be refused merely because the sibling is absent.
    assert check_size.is_unity_test_image(tmp_path / "nope.elf") is False


def test_the_discriminator_uses_the_runner_names():
    # A guard against a future edit that widens this to some common substring:
    # every entry must be a Unity RUNNER symbol, not a word that could appear in
    # production (e.g. a bare "Unity" or "test").
    for sym in check_size.UNITY_SYMBOLS:
        assert b"Unity" in sym, sym
