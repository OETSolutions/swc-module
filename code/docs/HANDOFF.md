# SWC firmware + Android app — session handoff

Written 2026-09-24, at the point the **assembled PCB is in hand and the next
session is bring-up on real hardware**. This is a starting point for a new
session: read it top to bottom, then open the two governing documents.

## 0. Orientation — where everything is

| Thing | Path |
| --- | --- |
| Repo root | `<repo-root>` |
| Branch | `feat/swc-firmware-android-app` (uncommitted, see §5) |
| Software root | `code/` — ESP-IDF firmware (`lib/`, `src/`) + Android app (`code/android/`) |
| **Spec** (authority) | `docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md` |
| **Plan** (task briefs) | `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md` |
| Agent instructions | `AGENTS.md` (repo root — the PCB doc split; also covers the PCB half) |
| Bring-up log | `code/docs/bring-up-log.md` (toolchain fixes, measured numbers) |
| SDD progress + history | `.superpowers/sdd/2026-09-18-swc-firmware-android-app/progress.md` |
| Static gates | `code/tools/` (see §4) |

**The spec/plan live under the repo root's `docs/`, not `code/docs/`.** The
harness resets the shell cwd to `code/` between Bash calls, so spec reads need an
absolute path or an explicit `cd` to the repo root.

## 1. What this product is (the one non-negotiable design fact)

An adapter between a factory steering-wheel resistor ladder and an aftermarket
Android head unit. Firmware on an **ESP32-S3** (pioarduino / IDF 5.5.5) decodes
the ladder, resolves a gesture (single/double/long), and drives an **MCP4728**
I²C DAC → servo → sink FET to present the head unit the KEY level it expects.
An Android app (Kotlin/Compose) talks to it over USB serial.

**The app has NO learning screen. The AUX1 button is the ONLY production learn
path** — hold AUX1 ~1.5 s to enter the learn wizard; a longer ~3 s hold opens the
maintenance window. It must work **with no app and no host**. An unbound gesture's
default is **passed through** to the head unit. Do not add a learning screen.

Two channels, SWC1 and SWC2. `DacChannel` → A=KEY1, B=ADJ1, C=KEY2, D=ADJ2.

## 2. Verify loop — run this before and after every change

```bash
cd code

# Host suite (503 tests) under ASan+UBSan, in an ISOLATED build dir so a
# concurrent session cannot clobber it:
PLATFORMIO_BUILD_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -Wno-deprecated-declarations" \
PLATFORMIO_BUILD_DIR=/tmp/swc_asan_iso pio test -e native

# Device image + size gate, also isolated (SWC_FW_VERSION/SHA must be set or the
# version string is stale):
SWC_FW_VERSION=dev SWC_GIT_SHA=local PLATFORMIO_BUILD_DIR=/tmp/swc_size_iso pio run -e esp32s3
python3 tools/check_size.py --bin /tmp/swc_size_iso/esp32s3/firmware.bin

# Static gates (each is also run by CI; all live in code/tools/):
python3 -m pytest tools/ -q          # gate self-tests
python3 tools/check_hal_contracts.py
python3 tools/check_frame_handlers.py
python3 tools/check_app_limits.py
python3 tools/check_task_ownership.py
python3 tools/check_sdkconfig_keys.py
python3 tools/check_spec_example.py
python3 tools/check_stack_usage.py
bash   tools/crosscheck_config.sh
```

Last verified: **503/503 native tests green, zero sanitizer reports**; device
build succeeds; `firmware.bin` **387,120 bytes = 19.7 %** of the 1,966,080-byte
app slot.

**Always use isolated build dirs.** Peer sessions share `code/.pio/build/` and
will race you (`PLATFORMIO_BUILD_DIR=…` per the commands above).

**Mutation-test every new assertion**: revert the fix, confirm the test *fails*,
restore. Several real defects were found by a guard that was itself untested.

## 3. Device-only blind spots — the important list for a real-board session

Two files are **excluded from the host build** (`build_src_filter`):
`lib/HAL/EspHal.cpp` and `lib/Link/UsbLink.cpp`. **The host suite cannot compile
them**, so every defect in them is invisible until the board is flashed. This is
where the remaining risk concentrates. The history already includes three such
clusters (NVS mount/write/read, the DAC I²C frame, the ADC calibration path).

When you flash, watch specifically for:
- **NVS** — `HalNvsSet` return contract and `ReadSlot` chunk-0 width (both were
  broken on-device while the host suite was green).
- **ADC** — `adc_read_mv` returns **-1 on error**, and **0 mV is a LEGAL value**.
  Every raw read must test `>= 0`; a failed read HOLDS state, never counts as a
  press. Three sites had this wrong (N-43).
- **USB DTR** — the `SET_CONTROL_LINE_STATE` wIndex must name the CDC **COMM**
  interface, not the DATA interface (TinyUSB matches the COMM one).
- **cJSON nesting** — `CJSON_NESTING_LIMIT` (default 1000) vs a **3,584-byte**
  `app_main` stack (~64 B/level on xtensa). A ~1 KB line of balanced brackets
  crashes the parser, and the stack gate is recursion-blind (N-71). The only
  valid test uses BALANCED brackets.

## 4. Static gates

All in `code/tools/`; the repo-root `.github/workflows/firmware.yml` invokes
them by name and CI runs from `code/`. Two notes worth having up front:

- **Most gates are currently UNTRACKED** (`??` in git): `check_app_limits.py`,
  `check_frame_handlers.py`, `check_hal_contracts.py`, `check_sdkconfig_keys.py`,
  `check_stack_usage.py`, `check_task_ownership.py`, and the matching
  `test_check_*.py`. They are NOT gitignored, so they will be committed with an
  explicit `git add` (see §5). **Do that before any push**, or CI fails on a
  clean checkout.
- `check_stack_usage.py` reads `sizeof(Config)` from the `static_assert` in
  `lib/Config/ConfigModel.h` (added this session) rather than carrying its own
  copy; deleting that assertion fails the gate loudly instead of silently
  reverting to a remembered number.

## 5. Uncommitted state — READ BEFORE TOUCHING GIT

**110+ files are modified/untracked on `feat/swc-firmware-android-app`, nothing
committed.** This is the single biggest hazard for a new session.

### Hard rules (a mistake here loses work or leaks secrets)

1. **`code/.env` contains LIVE credentials** (WiFi SSID + password, OpenWeatherMap
   API key, a Home Assistant long-lived token, location values) and belongs to a
   **different project**. It must **never be committed**. It is in
   `code/.gitignore` line 14.
2. **Never `git add code/` or `git add -A`.** Only explicit file lists. The
   untracked-tools situation (§4) and Drive's `.~*.insyncdl` placeholders inside
   source dirs make a directory-wide add unsafe.
3. `code/scratch.txt` (gitignored) and `code/.kilo/` are **not part of this
   project** — never stage.
4. `plastic_case/SWC_Enclosure.3mf` has a working-tree modification — **do not
   stage or commit it**.
5. `SWC.kicad_pcb` is modified in the working tree (PCB half) — leave it alone
   unless the task is the PCB.
6. **`git push` needs explicit user confirmation** — shared remote side effect.

Before committing anything, `git status` and read what you staged. The tools
under `code/tools/` (including the untracked gates) are the first explicit
`git add` batch to make CI honest.

## 6. Known-open items (prioritized for a real-board session)

These are **recorded gaps, not regressions** — the firmware is honest about
them (a router nack that says `not_implemented` is telling the truth). Full
detail is in the auto-memory index (`MEMORY.md`) under the N-numbers.

**Wire these as part of bring-up, using the board to verify:**

| Item | What's missing |
| --- | --- |
| **OTA (both paths)** | `ota_begin`/`ota_chunk`/`ota_end` are in the spec's frame table and nacked `not_implemented`. `OtaUsb`/`OtaWifi` are stubs on host; `ImageVerify` + the single `OtaCommit` gate exist and are tested — **the caller is what's missing.** |
| **Maintenance radio** | `BleProvisioning`, `WebPage`, `WebPageAssets` are implemented, tested, and **never started**. FR-32's whole point is the radio exists only inside the maintenance window. A user entering maintenance today gets a window with no radio behind it. |
| **Release check** | `ReleaseCheck` is pure logic, tested, no caller. Nothing compares a release against the running version. `hello`'s `caps` correctly omits `"ota"` (it *is* a promise). |
| **DAC fault path** | FR-13's read-back, §6.8's retry/backoff/latch, and a `FAULT_DAC` emitter are absent (`kFaultDac` has no caller). |
| **NTC temperature** | FR-1's "sample the NTC continuously" is unimplemented — the ADC temp channel is read by nothing, no B3380 conversion, so `temp_c_at_learn` is always 0 (N-67). |
| **AUX bindings are inert** | AUX1–3 are declared bindable inputs (spec §3.5, model, validator, app) but **no firmware path services a binding on them** (AUX1 is only the learn/maintenance trigger). An AUX binding is acked and never fires. |
| **Multi-action bindings** | Firmware executes only `actions[0]` — `BindingResolve` returns just the first (spec allows 2). |
| **`learn_channel_` is a constant 0** | Only channel 0 is learnable by any shipping means (the app has no learn screen), so SWC2 is not learnable on a two-channel install (N-23). |
| **`status` rail/temp/heap** | `rail_mv`, `temp_c`, `heap_free` have no producer (N-22); `gain_mode` reports channel 0 only (N-60). |

**Documentation-accuracy leftovers (low severity, comment-only):** a handful of
comment-vs-code drifts remain unrecorded from the last audit passes. None are
logic or safety bugs. Do **not** spend a bring-up session on these.

## 7. What was actually fixed recently (context, not TODO)

The audit history (~78 numbered findings) includes real, load-bearing fixes:
the `sizeof(Config)` by-value stack overflow (8912 B on 3.5–4 KB task stacks);
the NVS write/read contract failures; `-1` ADC sentinel consumed as a reading;
`uint16` wrap in the ratio mapping; cast-before-bounds UB on `1e999` (cJSON
parses to `+inf`); semver digit comparison dropping a component and overflowing a
`long` on xtensa; CI workflows at a path Actions never reads; learn-path
validity and config-write collapse; the unreachable pass-through. **This session
added** the `sizeof(Config)` assertion (§4) and made the stack gate read it.

## 8. Environment notes

- **PlatformIO needs the pioarduino fork** (stock stops at IDF 4.6.1). Pinned in
  `code/platformio.ini`; see `code/docs/bring-up-log.md` for the toolchain-deleted-
  on-every-build fix (three stacked defects — read that section before
  "reinstalling the toolchain").
- ESP32-S3 has **no DAC** and the calibrated ADC ceiling is **2.9 V at 12 dB**,
  not 3.1 V. Envelope floor 1800 mV, guard low 2600 mV, gain 1.82/1.00.
- Android: `compileSdk=36`, `targetSdk=34`, `minSdk=26`, Robolectric 4.14.1,
  JDK 17.

## 9. First moves for the new session

1. `git status` and read §5 — protect the uncommitted tree and `.env` first.
2. Read the spec's bring-up / maintenance sections (§8, §12) and the tail of
   `code/docs/bring-up-log.md`.
3. Wire **one** device-only path end-to-end against the real board (start with
   OTA or the maintenance radio — each has a tested seam waiting for a caller)
   and verify it **on hardware**, not just in the host suite.
4. Only then decide whether to commit. Explicit file lists; commit the untracked
   `code/tools/` gates so CI is honest; never the PCB or enclosure 3MF.
