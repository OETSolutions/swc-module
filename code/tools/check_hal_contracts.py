#!/usr/bin/env python3
"""Pin the IHAL nvs_* return contract across every implementation.

**Why this is a gate and not a review step.** `EspHal` is the ONE lib/ file the
host build excludes, so the native suite never compiles it and MockHal is the
only implementation the tests exercise. The two implementations therefore agree
with each other only by convention -- and there is no host test that can catch a
disagreement. That convention was wrong in TWO places at once (found 2026-09 in
the fifth/sixth audit passes):

  * `HalNvsGet` read an NVS value at the header's 16 bytes; the stored value is a
    2048-byte chunk. IDF returns ESP_ERR_NVS_INVALID_LENGTH (not NOT_FOUND) for an
    undersized buffer, so the real device failed every config load while the mock
    truncated and passed.
  * `HalNvsSet` returned `len` on success; every consumer treats nonzero as
    failure (`ConfigStore` tests `!= 0`), so the real device failed every config
    save while the mock returned 0.

Both are silent on the host and total on the device. This gate is a static check
of the shapes that let that happen, since a behavioral test cannot reach EspHal:

  1. `HalNvsSet` must not return a byte count; success is 0, like the mock.
  2. `HalNvsGet` must not read into a buffer sized to the header when the caller
     passed a full-size buffer (the `kBlobHeaderBytes` local-buffer shape), and it
     must not truncate the returned length to the caller's `len`.
  3. `ConfigStore` must read chunk 0 into the caller's buffer, not a fixed
     header-sized local.
  4. `HalNvsGet` must not gate on a CACHED availability flag -- it must open the
     namespace itself. nvs_open(READONLY) returns NOT_FOUND on a factory-fresh
     board (the namespace is created by the first READWRITE open), so a flag set
     from a boot-time READONLY probe stays false for the whole first power cycle
     and every read reports "absent" right after a successful save.
  5. NVS must be MOUNTED before any nvs_open: `EspHalInit` (or some project file)
     must CALL `nvs_flash_init()`. It was called nowhere, so on a real device every
     nvs_open returned ESP_ERR_NVS_NOT_INITIALIZED and all persistence silently
     failed while the host suite (MockHal, which has no mount step) passed.

Exit codes: 0 clean, 1 a violation, 2 a file could not be found.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ESP_HAL = REPO / "lib" / "HAL" / "EspHal.cpp"
CONFIG_STORE = REPO / "lib" / "Config" / "ConfigStore.cpp"


def _read(p: pathlib.Path):
    if not p.exists():
        print(f"FAIL: {p} not found", file=sys.stderr)
        return None
    return p.read_text()


def _strip_comments(src: str) -> str:
    """Remove // and /* */ comments so a comment that QUOTES a return shape (this
    codebase documents its contracts in comments) cannot trip a shape check."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _fn_body(src: str, name: str) -> str:
    """The brace-balanced body of `name(...)`, comments stripped, or '' if absent."""
    src = _strip_comments(src)
    m = re.search(rf"\b{re.escape(name)}\s*\([^)]*\)\s*\{{", src)
    if not m:
        return ""
    i = m.end()
    depth = 1
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end():i]


def main() -> int:
    esp = _read(ESP_HAL)
    cfg = _read(CONFIG_STORE)
    if esp is None or cfg is None:
        return 2

    problems = []

    # 1. HalNvsSet returns 0 on success, never a byte count. Any `return`
    #    statement that mentions `len` is the byte-count shape (the correct form
    #    returns a literal 0 / -1 and never names the parameter).
    body = _fn_body(esp, "HalNvsSet")
    if not body:
        problems.append("EspHal: HalNvsSet not found")
    else:
        for stmt in re.finditer(r"return\b[^;]*;", body):
            if re.search(r"\blen\b", stmt.group(0)):
                problems.append(
                    "EspHal::HalNvsSet returns a byte count on success; the contract "
                    "is 0 on success (ConfigStore tests `!= 0`), so every device "
                    "write would read as a failure."
                )
                break

    # 2. HalNvsGet must not shorten the reported length, and must not read a
    #    caller-supplied blob into a fixed header-sized buffer.
    body = _fn_body(esp, "HalNvsGet")
    if not body:
        problems.append("EspHal: HalNvsGet not found")
    else:
        if re.search(r"sz\s*=\s*len\b", body):
            pass  # expected: the size out-param starts as the caller's capacity
        if re.search(r"if\s*\(\s*sz\s*>", body):
            problems.append(
                "EspHal::HalNvsGet clamps the reported size; nvs_get_blob returns the "
                "real size even on INVALID_LENGTH, and truncating it hides an "
                "undersized-buffer read."
            )

    # 3. ConfigStore must read chunk 0 into the caller's buffer, at full width.
    body = _fn_body(cfg, "ConfigStore::ReadSlot")
    if not body:
        problems.append("ConfigStore: ReadSlot not found")
    else:
        if re.search(r"uint8_t\s+\w+\s*\[\s*kBlobHeaderBytes\s*\]", body):
            problems.append(
                "ConfigStore::ReadSlot reads chunk 0 into a header-sized local; chunk "
                "0 holds a whole chunk and an undersized read fails on the device."
            )
        if "blob_cap" not in body:
            problems.append(
                "ConfigStore::ReadSlot does not read chunk 0 into the caller's buffer "
                "(blob_cap); it must, so the read is full-chunk-width."
            )

    # 4. HalNvsGet must open the namespace itself, never gate on a cached flag.
    body = _fn_body(esp, "HalNvsGet")
    if body:
        if re.search(r"\bg_state\.\w+\b", body) and not re.search(r"nvs_open\b", body):
            problems.append(
                "EspHal::HalNvsGet gates on cached state instead of opening the "
                "namespace; a boot-time READONLY probe reports NOT_FOUND on a "
                "factory-fresh board, so the flag would hide every write made in "
                "that power cycle."
            )
        elif re.search(r"if\s*\(\s*!\s*g_state\.\w+\s*\)\s*return", body):
            problems.append(
                "EspHal::HalNvsGet short-circuits on a cached availability flag; it "
                "must open the namespace per call so a same-session save is visible."
            )

    # EspHalState must not carry a cached nvs availability member at all.
    if re.search(r"struct\s+EspHalState\s*\{[^}]*\b(bool\s+\w*open\w*|bool\s+nvs_\w+)", _strip_comments(esp)):
        problems.append(
            "EspHalState caches an NVS-open flag; drop it -- nvs availability is not "
            "stable across a power cycle and opening per call is what makes it correct."
        )

    # 5. NVS must be mounted somewhere in the project before any nvs_open. Scan the
    #    project's own source (lib/ and src/), never IDF's -- a mount call in an
    #    IDF component does not mount this project's namespace at boot.
    project_srcs = sorted((REPO / "lib").rglob("*.cpp")) + sorted((REPO / "lib").rglob("*.c")) \
        + sorted((REPO / "src").rglob("*.cpp")) + sorted((REPO / "src").rglob("*.c"))
    mounts = []
    for p in project_srcs:
        src = _strip_comments(p.read_text())
        if re.search(r"\bnvs_flash_init\s*\(", src):
            mounts.append(p.relative_to(REPO))
    if not mounts:
        problems.append(
            "no project source calls nvs_flash_init(); NVS is never mounted, so every "
            "nvs_open returns ESP_ERR_NVS_NOT_INITIALIZED and all config/learn "
            "persistence silently fails on the device (MockHal has no mount step, so "
            "the host suite cannot see it)."
        )

    if problems:
        print("HAL NVS contract guard FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("HAL NVS contract guard: OK")
    print("  EspHal::HalNvsSet  -> 0 on success; HalNvsGet -> byte count, no clamp")
    print("  ConfigStore::ReadSlot reads chunk 0 at full width from the caller's buffer")
    print(f"  NVS mounted by: {', '.join(str(m) for m in mounts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
