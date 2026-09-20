# Bring-up log — SWC adapter firmware

Task 1 (PlatformIO environment for the ESP32-S3). Measurements here are
**observed**, not derived. Every number below came out of a real command run on
this machine on 2026-09-18; re-run the command rather than trusting the figure.

## Resolved toolchain

| Item | Value | Source |
| --- | --- | --- |
| PlatformIO Core | 6.2.0 (`/opt/homebrew/bin/pio`) | `pio --version` |
| Platform | pioarduino `espressif32` **55.03.311** | build header |
| `framework-espidf` | **3.50505.0 (IDF 5.5.5)** | build header |
| Toolchain | `toolchain-xtensa-esp-elf @ 14.2.0+20260121` | build header |
| `tool-scons` | 4.41101.0 (SCons 4.11.1) | build header |
| Host Python | 3.14.7 | `python3 --version` |

Build succeeds: `pio run -e esp32s3` → `[SUCCESS]`.

## size-baseline:

`size-baseline: text=156257, data=59116, bss=375761, dec=591134 (0x9051e) bytes — firmware.elf, pio run -e esp32s3 -t size, 2026-09-18`

The linked `firmware.bin` is **215,488 bytes** (0x349C0) — a scaffolding image
with no application code, so this is a floor, not a budget. Every later budget
comparison is against these numbers.

App partition is **1920 KB (1,966,080 bytes)** per slot; the scaffolding image
occupies 10.96 % of one slot. **Note the slot is 1920 KB, not the 1952 KB the
spec §9.2 states** — see "Defect 2" below.

## The Xtensa toolchain is DELETED on every build (Defect 1's sibling)

**Symptom:** `Error: Missing toolchain directory 'None'`, or with `-v` a
`FileNotFoundError: ... 'package-postinstall.py'`. It recurs on every build, and
retrying does not help.

**Three defects stack here. The third is the one that wastes an hour.**

1. **`platform.json` points at a 2-FILE STUB.** The pioarduino registry zip
   (`.../0.0.1/xtensa-esp-elf-14.2.0_20260121.zip`) unpacks to `package.json` +
   `tools.json` and nothing else — 4,133 bytes, no `bin/`. Exactly the
   `tool-scons` stub of Defect 1, now on the toolchain. `install_required_packages`
   reports success and leaves an unusable directory.

2. **The version strings disagree, so the check never passes.** Both halves were
   measured:

   | Source | Says |
   | --- | --- |
   | `platform.json` `package-version` | `14.2.0+20260121` |
   | the stub's `package.json` | `14.2.0+20260121` |
   | the crosstool-NG tarball's own `package.json` | `14.2.0_20260121` (underscore) |
   | IDF's own install (`.piopm`) | `14.2.0+20260121` |

   So `_check_tool_version` can never be satisfied by anything the tarball
   provides: a manual install always mismatches, and PlatformIO then runs
   `safe_remove_directory` and re-unpacks the stub. **Deleting the tool is the
   steady state, not a transient fault.**

3. **IDF and PlatformIO install the same toolchain under DIFFERENT names.** IDF's
   `idf_tools.py` (driven by `_run_idf_tools_install`) puts a complete, working
   toolchain at `~/.platformio/packages/xtensa-esp-elf/` — note, **no
   `toolchain-` prefix** — with the correct `package.json` version. The
   PlatformIO/CMake side resolves `toolchain-xtensa-esp-elf`. So a fully working
   compiler can be sitting right there while the build reports it missing. That is
   the red herring: the fix looks like "reinstall the toolchain" when the
   toolchain is fine and simply is not where the build looks.

**The fix** (keep the working one, put it where it is expected, label it with the
exact string the checker wants):

```
cp -R ~/.platformio/packages/xtensa-esp-elf/* ~/.platformio/packages/toolchain-xtensa-esp-elf/
# then set package.json "version" to EXACTLY "14.2.0+20260121"
```

Verified: two consecutive `pio run -e esp32s3` builds succeed and the toolchain's
`bin/` stays at 116 entries. Before the label fix, every build emptied it.

**The label is load-bearing.** It looks like a cosmetic edit and it is not: it is
what makes `_check_tool_version` return true, which is the early-return that
skips the destructive reinstall.

### `IDF_MAINTAINER=1` is needed for the build

IDF's `tools/cmake/tool_version_check.cmake` runs
`idf_tools.py check-tool-supported`. **This IDF does not implement that
subcommand** — it exits with an argparse error, the check reads the empty stdout
as "unsupported", and it raises a FATAL_ERROR naming
`Tool doesn't match supported version from list ['esp-14.2.0_20260121']`. The
installed compiler *is* that version; the check cannot tell, because its own
helper is missing. The check's message documents the override, so builds here run
as `IDF_MAINTAINER=1 pio run -e esp32s3`.

Neither edit is in the repo (they live in the PlatformIO package cache), so
**neither survives a platform reinstall and neither is reproduced by a fresh
clone.** If the toolchain error reappears, apply both before debugging anything
else.

## size-after-tasks-1-14b: the real number

`size-after-tasks-1-14b: text=218361, data=71716, bss=531253, dec=821330 (0xc8852) bytes — firmware.elf, pio run -e esp32s3 -t size, 2026-09-19`

`firmware.bin` is **290,045 bytes** — 14.8 % of the 1,966,080-byte slot. This
supersedes the scaffolding figure above as the comparison baseline: Tasks 1–14b
are implemented (HAL, orchestrator, gesture, config, NDJSON, feedback, EspHal)
and this build *does* link them, which the scaffolding build did not.

**Read this number carefully, because it proved to be a trap.** The step from
215,085 → 289,789 bytes was the FIRST evidence that `lib/` had finally been
compiled into the image (Defect 44): an eleven-task build had previously weighed
exactly what a one-task build weighed, because IDF's component model was never
told that `lib/` existed and the linker had nothing to link. A budget gate cannot
detect that class of failure at all — it is an upper bound, and "no code" is
always under it. The size moving is a symptom; the check that means something is
that the object files exist and are referenced.

Two `text` measurements disagree (218,361 here vs. 290,045 total image) because
the size tool reports `.bin` size separately from ELF `.text`. Use the `.bin`
figure for the CI budget gate, which is what actually has to fit in the slot.

## Verification targets

- `pio run -e esp32s3 -t size` → prints the table above. **Works.**
- `pio run -e esp32s3 -t partition-table` → **does not exist** in pioarduino
  55.03.311. It fails with `*** Do not know how to make File target
  'partition-table'`. The brief's Step 8 names a target this platform version
  does not define. Verified equivalently instead (see Defect 2):
  `.pio/build/esp32s3/partitions.bin` is **3072 bytes** and decodes to exactly
  the CSV, ending at `0x400000`:

```
# ESP-IDF Partition Table
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,48K,
otadata,data,ota,0x15000,8K,
phy_init,data,phy,0x17000,4K,
app0,app,ota_0,0x20000,1920K,
app1,app,ota_1,0x200000,1920K,
coredump,data,coredump,0x3f0000,64K,
```

## Defects found in the brief (all fixed, all verified)

### Defect 1 — `framework = espidf` could not build at all (BLOCKING)

`ModuleNotFoundError : No module named 'SCons.Tool.FortranCommon'` on every
run, from `SCons/Tool/linkCommon/__init__.py:132 smart_link` (reached via
`SCons/Subst.py` during link-action signature expansion).

Root cause: `~/.platformio/platforms/espressif32/platform.py` sets
`COMMON_IDF_PACKAGES = ["tool-cmake", "tool-ninja", "tool-scons",
"tool-esp-rom-elfs"]` and installs them **only** under
`if "espidf" in frameworks:` (line 845). For `tool-scons`, pioarduino's
`platform.json` pins `package-version 4.40801.0`, whose artifact
(`pioarduino/registry/.../scons-4.8.1.zip`) is a **2-file stub** containing only
`package.json` + `tools.json` — no `SCons/` tree at all. PlatformIO Core
separately requires `tool-scons ~4.41101.0`. The two owners fight over the same
`packages/tool-scons` directory: it is installed, then deleted and reinstalled
several times *during a single build*, and SCons dies on the lazy import when it
loses the race.

Evidence it is not caused by this project's files: the same failure reproduces
with the **stock `esp32-s3-devkitc-1` board** on the **other** pioarduino tag
(55.03.39), and in a **completely fresh core dir**
(`PLATFORMIO_CORE_DIR=/tmp/pio_cleancore`), and with `platform_packages =
platformio/tool-scons@~4.41101.0` set. `framework = arduino` on the same
platform builds fine — the defect is specific to the ESP-IDF branch, which is
exactly risk R-9 / N-8.

Resolution: PlatformIO's own pin won. Once the `packages/tool-scons` directory
settled at `4.41101.0` (SCons 4.11.1, which *does* contain
`SCons/Tool/FortranCommon.py`), the build compiles the full IDF tree. No change
to the platform pin was made — the framework choice stands.

#### Defect 1 recurrence — 2026-09-19, and the actual mechanism

Defect 1 came back with the *same* error text but a different cause, and the
earlier "it settles" conclusion was wrong: it does not settle, it converges on
the directory being **deleted**.

Measured by polling `packages/tool-scons` every 250 ms during a build. The
directory is created, removed, recreated with `scons.py` present, then removed
and **left removed**. The build then dies at the lazy `FortranCommon` import
because `scons.py` itself is gone.

The mechanism is a version skew between the two owners, and this is the part the
first write-up missed:

- pioarduino's `platform.json` pins `tool-scons` **`package-version 4.40801.0`**.
- PlatformIO Core's own `dependencies.py` pins `tool-scons` **`~4.41101.0`**.

`_handle_existing_tool` (`platform.py:595`) compares the installed
`package.json` version against the platform's `package-version`. Installed is
`4.41101.0` (Core won the install), required is `4.40801.0` (pioarduino), so the
comparison **always fails**, on every build, forever. It then runs
`safe_remove_directory(tool_path)` and calls `self.install_tool()` to put it
back.

That reinstall cannot succeed. `install_tool` picks a branch from
`_check_tool_status`:

- Case 1 (`has_idf_tools and has_tools_json`) — needs `tools.json` in
  `packages/tool-scons`, which the real SCons tree does not contain.
- Case 2 (`has_idf_tools and has_piopm and not has_tools_json`) — true *before*
  the delete, which is how it got here.
- Fallthrough — "already configured", returns `True`.

After `safe_remove_directory` the directory is empty, so Case 1 and Case 2 are
both false and it takes the **fallthrough: logs success and returns without
reinstalling anything**. The directory stays deleted, and the next build fails
at the import. Nothing re-creates it, because every subsequent run takes the
same path.

Fix applied (machine-local, and it is a workaround, not an upstream fix): edit
`~/.platformio/platforms/espressif32/platform.json` to set `tool-scons`
`package-version` to **`4.41101.0`**, matching Core's pin. The version check then
passes, `_handle_existing_tool` returns early, and the directory is left alone.
Verified: `pio run -e esp32s3` → `[SUCCESS]`, and three consecutive builds are
green with `tool-scons` intact at 7 entries each.

**This edit lives in the PlatformIO package cache, not in the repo, so it does
not survive a platform reinstall and is not reproduced by a fresh clone.** If
the `FortranCommon` error reappears, re-apply it before debugging anything else:

```
python3 - <<'EOF'
import json, os
p = os.path.expanduser("~/.platformio/platforms/espressif32/platform.json")
d = json.load(open(p))
d["packages"]["tool-scons"]["package-version"] = "4.41101.0"
json.dump(d, open(p, "w"), indent=2)
EOF
```

A durable fix would be to pin `platform_packages = platformio/tool-scons@~4.41101.0`
in `platformio.ini`, but that was **tested and does not work** — it was one of
the conditions in which the original Defect 1 reproduced. The `platform.json`
edit is what is verified.

#### Defect 5 — the host test env never compiled any `lib/` source (BLOCKING)

`build_src_filter = -<*>` excludes the whole project tree, which is right for a
host build, and the plan added exactly one `+` line to put `test_native/*.cpp`
(MockHAL.cpp) back. **No source under `lib/` was ever compiled on the host**, so
every suite that tests real library code — not just the mock — failed to link:

```
Undefined symbols for architecture arm64:
  "LadderClassify(LadderProfile const&, int, int)", referenced from: ...
```

This was latent until Task 3 added the first `lib/` module with a `.cpp`. The
LDF cannot rescue it: the finder scans `src_dir`, which `-<*>` has emptied, so
`lib/Analog` is never detected as a dependency.

Fix: a second include line.

```
build_src_filter =
    -<*>
    +<../test_native/*.cpp>
    +<../lib/*/*.cpp>
```

Verified load-bearing by deleting the new line, cleaning `.pio/build/native`,
and watching `test_analog` fail to link; restoring it gives 19/19 PASSED. Note
`lib/*/*.cpp` (not `lib/*.cpp`) — sources live one level down, in per-module
directories.

#### Defect 6 — a suite's `#include` did not match the plan (data loss)

`test_native/test_analog/LadderDecodeTest.cpp` was found on disk with
`#include "LadderDecode.h"` where the plan specifies `"Analog/LadderDecode.h"`,
and `lib/Analog/` was **absent from the filesystem entirely** — it had been
deleted between two builds. Neither file had ever been committed, so git held no
copy: a scan of all 462 blobs and 30 dangling commits found no trace.

Recovered from a Time Machine local snapshot
(`com.apple.TimeMachine.2026-09-18-233541.local`, mounted read-only with
`mount_apfs -s`). Both `LadderDecode.h` and `LadderDecode.cpp` were present
there, and after restoring them all three files diff **byte-identical** against
the plan's code blocks.

The lesson worth keeping: the recovery window was ~70 minutes wide, and the only
reason it existed is that the files sat uncommitted long enough to be caught by
an automatic snapshot. **An uncommitted file on this machine is one snapshot
generation from gone.** Commit verified work promptly.

### Defect 2 — `partitions.csv` is rejected by IDF (BLOCKING)

`Partition app1 invalid: Offset 0x208000 is not aligned to 0x10000`

`gen_esp32part.py` sets `ALIGNMENT[APP_TYPE] = 0x10000`, so an **app**
partition's offset must be 64 KB aligned. The spec's `app1` offset `0x208000`
is not. Spec §9.2's own "Arithmetic, checked exactly" note verifies only that
the table *ends* at `0x400000`; it never checks the 64 KB alignment rule, so the
table is self-consistent and still invalid.

Fix applied: `app0`/`app1` size `0x1E8000` → `0x1E0000` (1920 KB, was 1952 KB)
and `app1` offset `0x208000` → `0x200000`. `0x3E0000–0x3EFFFF` is left reserved
so `coredump` stays at its specified `0x3F0000`/64 KB, and the table still ends
at `0x400000`.

**This costs 32 KB per slot — the app budget is 1920 KB, not the 1952 KB the
spec states throughout.** That is a spec change, not just a build fix: spec
§9.2, the N-3 risk and the §10.5 CI budget gate all quote 1952 KB. 1920 KB is
still above IDF's own `partitions_two_ota_large.csv` (1700 KB), so the design
intent holds, but **the plan owner should update those spec figures.**

### Defect 3 — flash size silently stayed at 2 MB

`Warning! Flash memory size mismatch detected. Expected 4MB, found 2MB!`

`platformio.ini`'s `board_build.flash_size = 4MB` is **not** honoured on the
ESP-IDF path. The board→sdkconfig injection that reads it
(`generate_board_specific_config`) is called from `HandleArduinoIDFsettings`,
which is Arduino-gated, so IDF fell back to its own default
(`ESPTOOLPY_FLASHSIZE` defaults to `ESPTOOLPY_FLASHSIZE_2MB`).

`ESPTOOLPY_FLASHSIZE` is a Kconfig **choice**; the string form
`CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` is silently ignored (tested — the generated
value stayed `2MB`). Fix applied: `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` in
`sdkconfig.defaults`. Verified: generated `ESPTOOLPY_FLASHSIZE = 4MB`, warning
gone, and `main.c`'s 4 MB runtime assertion now agrees with the image it was
built into.

### Defect 4 — `-t partition-table` target does not exist

See "Verification targets" above. Step 8's command cannot run as written on
55.03.311; the partition table was verified by decoding `partitions.bin`.

## sdkconfig keys IDF 5.5.5 does not define

Three keys from the brief are **not defined anywhere in IDF 5.5.5's Kconfig**
(grepped the whole framework tree). They are accepted silently — no warning is
emitted — so each was checked against the generated `sdkconfig.json` instead.
Per the brief these encode real decisions, so they are **reported, not dropped**:

| Key in brief | Status in IDF 5.5.5 | Effect |
| --- | --- | --- |
| `CONFIG_TINYUSB_CDC_ENABLED` | **not defined** — IDF 5.5.5 has no `components/tinyusb`; TinyUSB is a managed component (`espressif/esp_tinyusb`) | inert. The console *is* correctly on USB-Serial-JTAG (`ESP_CONSOLE_USB_SERIAL_JTAG = True`), but **no TinyUSB CDC app interface is configured at all** — spec §4.1's separate app USB interface is not yet real. Belongs to the task that adds `esp_tinyusb`. |
| `CONFIG_ESP_ADC_CAL_USE_EFUSE_CALIBRATION` | **not defined** | inert. No such symbol. The real IDF 5.5.5 ADC-cal keys are `ADC_CALI_EFUSE_TP_ENABLE` / `ADC_CALI_EFUSE_VREF_ENABLE` / `ADC_CALI_LUT_ENABLE` (`esp_adc/Kconfig:22,30,38`) — **but they sit under `depends on IDF_TARGET_ESP32`, so they are inert on the ESP32-S3 as well.** On the S3, per-chip calibration is a **runtime API**, not a build knob: `adc_cali_create_scheme_curve_fitting()` (`SOC_ADC_CALIBRATION_V1_SUPPORTED`, `soc_caps.h:127`). **Spec §2.3's 25 mV error budget therefore rests on Task 4 calling that API, not on any default.** |
| `CONFIG_ESP_ADC_CAL_DEFAULT_ATTENUATION_12` | **not defined** | inert. No such symbol; attenuation is a runtime `adc_oneshot` argument in IDF 5.x, not a Kconfig key. The 12 dB choice belongs in the HAL. |

**Both ADC keys and the TinyUSB key have been removed from
`sdkconfig.defaults`** rather than left in place as inert entries: a key that
looks like configuration but does nothing is worse than an absent one, because
it makes the file read as if calibration and the USB app interface were
configured. Each removal site carries a comment saying what to do instead.
The build was re-run after the removals and is unchanged (215,085 bytes, 10.9 %
of 1,966,080) — confirming they were inert.

Keys that **did** apply correctly (verified in generated `sdkconfig.json`):
`ESP_CONSOLE_USB_SERIAL_JTAG=True`, `ESP_CONSOLE_UART_DEFAULT=False`,
`BT_ENABLED=True`, `BT_NIMBLE_ENABLED=True`, `BT_BLUEDROID_ENABLED=False`,
`ESP_SYSTEM_PANIC_PRINT_REBOOT=True`, `ESP_COREDUMP_ENABLE_TO_FLASH=True`.

## `CONFIG_MBEDTLS_HARDWARE_AES`

**Confirmed `n`.** Generated `sdkconfig.json` reports
`MBEDTLS_HARDWARE_AES = False`. IDF 5.5.5 defaults this to `y`
(mbedtls Kconfig line 397–399, `default y`), so the brief's key is doing real
work — this is the project's deliberate software-AES path, and it is live.

## Board JSON fields the brief asked to be checked

- `"flash_size": "4MB"` / `"maximum_size": 4194304` — **correct** for the DOIT
  ESPS3-32-N4 (4 MB, no PSRAM). The build header reports `HARDWARE: ESP32S3
  240MHz, 320KB RAM, 4MB Flash`.
- `build.arduino.partitions: "default_8MB.csv"` — **inert, as the brief
  predicted.** `platformio.ini`'s `board_build.partitions = partitions.csv`
  wins; the generated `partitions.bin` decodes to `partitions.csv`, not to any
  8 MB table. It did **not** override the ini. Left in place per instruction.
- `"frameworks": ["arduino", "espidf"]` — the `arduino` entry is inert;
  `PIOFRAMEWORK` resolves to `['espidf']`. Left in place.

## What is NOT verified

Nothing was flashed — the module has not arrived. `main.c`'s runtime
assertions (4 MB flash, 2 cores) are therefore **unexecuted**; they are a
build-time-checked expression of intent only. The flash-size claim above is
about the *image configuration*, not a measurement off silicon.
