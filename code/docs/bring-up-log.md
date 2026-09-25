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

## The device-only NVS cluster (three defects, none visible on the host)

Found in the audit passes after Tasks 1–14c. All three are in `EspHal` — the ONE
`lib/` file the host build excludes — so the native suite never compiles them and
`MockHal` is the only HAL the tests exercise. Each passed on the host and failed
totally on a real device.

1. **`nvs_flash_init()` was called NOWHERE.** NVS was never mounted, so every
   `nvs_open` returned `ESP_ERR_NVS_NOT_INITIALIZED`. Config save, config load,
   and headless-learn persistence each silently did nothing. Fix: mount in
   `EspHalInit` (with an erase-and-retry on `NO_FREE_PAGES`/`NEW_VERSION_FOUND`).
   `MockHal` has no mount step, so nothing on the host could see it. Guard:
   `tools/check_hal_contracts.py` now scans `lib/` and `src/` for a project call
   to `nvs_flash_init`.
2. **`HalNvsSet` returned `len` on success.** Every consumer treats nonzero as a
   write failure (`ConfigStore` tests `!= 0`), so on hardware EVERY save read as
   failed: `Save` always returned false, `cfg_seq` never advanced, nothing
   persisted. `MockHal::NvsSet` returned 0, so the host suite was green. Fix:
   return 0 on success.
3. **`HalNvsGet` was gated on a boot-time flag.** `nvs_open(READONLY)` returns
   `ESP_ERR_NVS_NOT_FOUND` on a factory-fresh board (the namespace is created by
   the first READWRITE open), so the cached flag stayed false for the whole first
   power cycle and every read reported "absent" even right after a successful
   save. Fix: open per call, no cached flag.

The common shape is worth naming: **a HAL contract the two implementations agree
on only by convention, with no host test able to compile the real one.** The
static guard in `tools/check_hal_contracts.py` exists because a behavioral test
cannot reach `EspHal` at all.

## The device-only stack overflow (the same blind spot, one layer up)

Found 2026-09-22, in the audit pass after the NVS cluster. `sizeof(Config)` is
**8,912 bytes**, and several functions held one BY VALUE — as a local, or as the
return value of the `ConfigDefault()` factory. The compiler's own `-fstack-usage`
(`-Og`) measured:

| function | frame | runs on |
|---|---:|---|
| `SystemOrchestrator::Boot` | 18,704 B | main task, 3,584 B |
| `CommandRouter::HandleLearnCommit` | 18,096 B | TinyUSB task, 4,096 B |
| `CommandRouter::HandleConfigPatch` | 17,920 B | TinyUSB task |
| `CommandRouter::BeginConfigReplyRun` | 17,856 B | TinyUSB task |
| `CommandRouter::HandleLearnStart` | 9,728 B | TinyUSB task |
| `CommandRouter::HandleConfigEnd` | 9,088 B | TinyUSB task |
| `ConfigStore::Load` | 8,960 B | both |

The task stacks are `CONFIG_ESP_MAIN_TASK_STACK_SIZE = 3584` (the generated
`sdkconfig.h`; the defaults file does not raise it) and `tusb_cfg.task.size =
4096` (`UsbLink.cpp`). Summed along the real chains, `Boot -> ConfigStore::Load
-> ConfigDecodeBlob -> ConfigDecodeJson` reached **~36 KB**, and
`HandleConfigPatch`'s chain ~35.8 KB. That is a **guaranteed stack overflow on
every device boot and on the first `config_get`** — an immediate panic, before a
single key could be driven.

Invisible to everything that runs today, for the same reason as the NVS cluster:
the host suite's threads have megabytes, so a frame of any size is harmless
there, and **the board has never been flashed** (§"What is NOT verified"). The
host build also excludes nothing here — these are all host-compiled files — so
this was never a compile-visibility problem; it is a *runtime-resource* problem
that no native test can express.

**Fix.** `ConfigDefault` takes an out-parameter (`void ConfigDefault(Config*)`)
instead of returning by value, and every site that needs a whole config works in
a `static` or a member rather than a local — the same place the class's other
scratch already lives (`CommandRouter::staging_`, `reply_buf_`,
`ConfigStore`'s `g_blob`). `ConfigDecodeJson` keeps its "never partially applied"
guarantee by decoding into a file-local scratch and copying to `*out` only after
both the fields and the validator pass. After the fix the largest frame in the
project is 1,072 B and the deepest chain is ~1.5 KB.

**A SECOND overflow, found by the same check.** `CommandRouter::Process` held two
1 KB scratch buffers as locals (2,352 B frame), and `Process` runs three frames
below `Emit` (1,072 B) and `Nack` (544 B) on the main task:
`app_main -> UsbLinkService -> Process -> Nack -> Emit -> NdjsonWriter::Write`
summed to **4,080 B against the 3,584-byte stack**. That path is hit the first
time a `config_get` reply is chunked out. Same fix: those buffers are file-local
statics.

**Guard:** `tools/check_stack_usage.py` (wired into the build-and-size-gate CI
job). It recompiles the project with `-fstack-usage` from the device build's own
flags, reads the task stacks from where they are actually set, builds the call
graph from the objects' `ASM_EXPAND` relocations, and takes the EXACT longest
path per task root. The exactness matters: the first version used a DFS that
skipped already-visited nodes, and a mutation test (a 2 KB frame added to
`EstablishSafeIdle`, reachable only through `Boot`) went **unreported** — a false
negative, the one direction a stack gate must not fail in. The DP version
catches it.

## The cross-task race the host suite structurally cannot see

Found 2026-09-23, audit pass after the stack work (same device-only blind spot,
a different axis: concurrency rather than resources).

**The topology.** `UsbLinkStart` installs TinyUSB with `tusb_cfg.task.size =
4096`, `priority = 5`, `xCoreID = 0`. The device's own loop is `app_main` — main
task, `priority = 1` (`ESP_TASK_MAIN_PRIO = ESP_TASK_PRIO_MIN + 1`), `xCoreID =
0`. TinyUSB's driver task runs `tud_task()` forever, and `tud_task` is what
invokes `tud_cdc_rx_cb` → `CdcRxCallback`, and the line-state callback → 
`CdcLineStateCallback`. **So both callbacks run on the TinyUSB task, and it
preempts the poll loop at any instruction** — same core, higher priority.

**What they touched.** `CdcRxCallback` fed the bytes straight into
`UsbCdc::FeedBytes`, which *parsed* them and called the sink — i.e. the entire
command protocol ran on the TinyUSB task: `config_end`, `config_patch`,
`learn_commit` reassigning `config_` and rebuilding classifiers; `identify`,
`enter`/`exit_maintenance`, `test_key` driving a channel. Meanwhile
`SystemOrchestrator::Tick` — classification, the gesture machines, the output
derivation — ran on `app_main`. The DTR callback was worse in kind, however
small: it called `SetUsbConnected`, mutating the orchestrator, and `hello`/the
config reply run on the router.

**Nothing takes a lock.** Verified by grep across `lib/` and `src/`: no
`xSemaphore*`, no `portENTER_CRITICAL`, no `std::mutex`, no `std::atomic`
outside the transport's own ring. So every field the two tasks share was an
unprotected read/write pair. The dangerous one is `config_` (8,912 B, copied
field-by-field while `Tick` classifies against it) and the per-channel
classifiers: a torn read there is a **wrong key voltage driven at the head
unit**, which is the direction spec 6.2 calls the only dangerous one.

**Why no test caught it.** Every host test calls `OnLine` and `Tick` from one
thread, so mutual exclusion is free and the race simply does not exist there.
`UsbLink.cpp` is device-only (it names TinyUSB) and excluded from the host
build, and **the board has never been flashed** — so this is invisible to the
suite the same way the NVS cluster and the stack overflow were.

**Fix.** All link work moves to the poll task. The `UsbCdc` RX path becomes a
lock-free SPSC byte ring (`kRxCapacity = 4096`, well above the 1,024-byte CDC
FIFO, so a burst arriving between two 10 ms ticks fits): `FeedBytes` now only
*copies* bytes and publishes `rx_head_` with a release store; the new
`DrainRx` parses and delivers, and is called from `UsbLinkService` — the poll
task — next to `Tick`. The DTR callback no longer calls into the router or the
orchestrator at all; it publishes `g_pending_line_state`, and `ServiceLineState`
applies it on the poll task.

**Two more defects found while fixing the fix** (both in the new code, both
fixed, both now covered):

1. **The DTR transition was published as an EDGE against the APPLIED state.**
   `if (open != g_host_open) publish(...)` loses a transition: if the host opens
   and closes between two poll ticks, the close is compared against a
   `g_host_open` that is still `false` (the open has not been applied yet), is
   judged "no change", and is dropped — so the poll task then applies the *open*
   and the device latches a session the host already ended (`hello` sent to
   nobody, `LED_STAT` solid forever, a config run left open). Fixed by
   publishing the DTR **level** unconditionally and letting `ServiceLineState`
   reconcile; it is idempotent when the level has not moved. Guarded (clause 2b).
2. **A disconnect could not discard bytes a producer was in flight on.** The
   callback runs on the other task, so `NoteDisconnected` moving only the
   consumer-owned tail leaves a window: a producer that read its head *before*
   the reset and stores it *after* resurrects the ring, and a command from the
   ended session (`config_patch`, `test_key`) is then parsed as the first frame
   of the next one. Fixed with `rx_epoch_`: the consumer bumps it **before**
   reading the head, the producer re-checks it **after** storing the head and
   pulls that head back to the tail if it moved. The bump-before-read order is
   load-bearing and the leaking interleaving for the opposite order is written
   out in `NoteDisconnected`. Tested with an injectable seam that fires inside
   `FeedBytes` between staging and publishing — the only way to reach that
   interleaving; a test that disconnected *before* calling `FeedBytes` would be
   rescued by the disconnect's own tail reset and would pass with the epoch check
   deleted. (Deleting the check is caught; the bump-vs-reset ORDER is a reasoned
   invariant the seam cannot separate, and the comment says so rather than
   claiming a test.)

**A third defect, on the config-apply path (`ApplyConfig`).** Its first cut set
`buzzer_ = BuzzerGrammar(hal_, level)` / `leds_ = LedGrammar(hal_, level)` — the
same shape `Boot` uses, which is correct THERE because nothing is in flight yet.
On the apply path it is a bug: a freshly constructed grammar starts at
`kNone`/`kOff`, so the assignment **silently cancels whatever pattern is
showing**. The learn wizard owns both channels as its prompts while it runs, and
it sets each pattern **once on entry**, not per tick — so a config push landing
mid-learn (a `config_patch`, or an app push overlapping a headless AUX1 learn)
would blank the prompt the user is reading, with nothing to restore it. Fixed by
adding `BuzzerGrammar::SetLevel` / `LedGrammar::SetLevel`, which change the level
and leave the live pattern alone (same reasoning as `MaintenanceMode::SetTimeout`
in the same function). Both the pattern-cancellation and the repaint-over-the-
prompt cases were mutation-tested: restoring the assignment form fails two tests,
making `SetLevel` a no-op fails a third.

**Guard:** `tools/check_task_ownership.py` (wired into build-and-size-gate). It
pins the shapes that make the split safe and are easy to undo by
"simplifying" a callback: `CdcRxCallback` must not parse or reach the sink;
`CdcLineStateCallback` must not call `OnConnected`/`OnDisconnected`/
`SetUsbConnected` and must not read the applied `g_host_open`; `UsbLinkService`
must call `DrainRx` and `ServiceLineState`; and `UsbCdc::FeedBytes` must not call
`DrainRx` or touch the sink. Each was mutation-tested and is reported.

## The ratio mapping narrowed to uint16 BEFORE clamping (a wrap into range)

Found 2026-09-23, same audit pass. `PresentLevel` maps a wheel level onto the head
unit's range by ratio (spec 6.9): `head_unit_idle_mv * level_mv / wheel_idle_mv`.
The arithmetic was in `long` — and then **narrowed to `MilliVolt` (uint16_t)
before any clamp**:

```cpp
const MilliVolt target = static_cast<MilliVolt>(
    (static_cast<long>(head_unit_idle_mv) * level_mv) / wheel_idle_mv);
```

With a small `wheel_idle_mv` the product exceeds 65535, and the cast reduces it
**mod 65536**. The dangerous part is where it lands: about **half** of the wrapped
values fall back inside `[1800, 5200]` — the valid output envelope — so the value
is indistinguishable from a real target and the downstream clamp in
`GainPolicyCodeForTarget` sees nothing wrong. **The device drives a key voltage
nothing defined.**

Measured over the ranges the validators permit (`mv_center` and
`learned_idle_mv` each bounded only to a plausible ADC reading, independently;
`head_unit_idle_mv` to the envelope): **620,054 (level, wheel_idle) pairs wrap into
the envelope**, and `wheel_idle_mv <= 230 mV` is enough to reach them. A concrete
reachable case: `mv_center` = 2896 (a button "learned" against an unreadable input
reads near full scale), `learned_idle_mv` = 1, head-unit idle 4980 →
`4980 * 2896 / 1 = 14,422,080` → wraps to **4160 mV**, a perfectly plausible 5 V
target.

Invisible to the suite for a plain reason: no existing test presents a ratio whose
product exceeds 16 bits, so the cast never wrapped in a test — and the wrap is
silent by construction (it produces a *valid* value, not a crash).

**Fix.** The mapping moved to `GainPolicyMapWheelLevelToHeadUnit` in
`GainPolicy` — beside the other output arithmetic and directly unit-testable, which
is what the wrap needs — and it **saturates to `[0, kOutputCeilingMv]` before the
narrowing cast**. Saturation is the correct direction: the ceiling is where an
over-large ratio would have been clamped anyway, and spec 6.2's only dangerous
error is over-ranging a 3 V head unit, which the ceiling prevents. `0` is returned
for a non-positive `wheel_idle_mv` (no denominator, nothing to say).

**Guard:** two tests in `test_native/test_output/GainPolicyTest.cpp` —
`AnExtremeRatioSaturatesInsteadOfWrappingIntoAValidTarget` (asserts the premise,
that the product exceeds the uint16 range, and then that the result is the ceiling
rather than the wrapped value) and `TheRatioMappingStillMatchesTheOrdinaryCase`
(pins the normal arithmetic and the zero-denominator cases). Removing the
saturation fails both.

## size-after-tasks-1-14c: TinyUSB arrives, and the size jump is real

`size-after-tasks-1-14c: text=278681, data=89012, bss=779541, dec=1147234 (0x118162) bytes — firmware.elf, pio run -e esp32s3 -t size, 2026-09-19`

`firmware.bin` is **367,808 bytes** — 18.7 % of the 1,966,080-byte slot (RAM
92,692 B, 28.3 %). The last recorded `firmware.bin` was **306,325 bytes** at the
end of Task 18, so this task adds **61,483 bytes**.

**Almost all of it is third-party code that did not exist in the image before.**
Measured from the object files rather than inferred:

| component | objects | text bytes |
|---|---:|---:|
| `espressif__tinyusb` (TinyUSB core + DCD) | 22 | 23,067 |
| `espressif__esp_tinyusb` | 8 | 15,368 |
| **subtotal** | **30** | **38,435** |

The balance is this task's own code (`UsbCdc`, `UsbLink`) plus the linker's
alignment and the descriptor/string tables.

**`bss` is the more interesting number, and it is NOT mostly TinyUSB.** The
firmware's bss is 779,541 B. `nm --size-sort` attributes the two largest symbols
to this task, and they are deliberately large:

| symbol | bytes | what it is |
|---|---:|---|
| `UsbLinkStart()::router` | 44,880 | `CommandRouter`: two `ConfigMaxSerializedSize()` buffers (11,203 each) + the 22,407-byte staging buffer + members |
| `g_cdc` | 3,116 | `UsbCdc`: the two-maximum-frame TX buffer + `NdjsonReader` |

Both are function-local `static`s inside `UsbLinkStart`, which is why they appear
under its mangled name. The 44,880 figure is exact and worth keeping in view: the
staging buffer is `ConfigMaxSerializedSize()` = 22,407 B **because spec 4.2
requires that bound be a compile-time constant** — a heap buffer sized from the
peer's `total_len` would be an overflow primitive driven from the other end of
the wire. That is a deliberate trade of 22 KB of committed RAM for a bound that
cannot be exceeded, and it is the kind of trade the no-PSRAM budget has to keep
paying for.

These figures are **committed BSS, not peak**: claimed at boot, never returned.

**The Defect 49 check is what makes the +61 KB mean anything.** `UsbCdc.cpp.o` and
`UsbLink.cpp.o` exist, `tinyusb_cdc_acm.c.o` exists, and `nm firmware.elf` finds
`UsbLinkStart`, `UsbLinkService` and `tinyusb_cdcacm_write_queue`. A size that grew
while the symbols were absent would be the 215 → 289 trap again, one directory
over.

### Defect 62 recurrence — and a correction to the recorded fix

Building at the **previous commit in a git worktree** tripped Defect 62 again and
destroyed the toolchain (and briefly `tool-ninja` / `tool-esp-rom-elfs`, which
PlatformIO repopulated on the next run).

**The recorded fix was incomplete, and the incompleteness is the interesting
part.** It said to place the toolchain under the name `toolchain-xtensa-esp-elf`
and label its `package.json` version `14.2.0+20260121`. What actually works is
different in one respect: the package must carry a **`.piopm` whose `spec.name` is
`toolchain-xtensa-esp-elf`**, which is how PlatformIO resolves
`get_package_dir("toolchain-xtensa-esp-elf")`. Copying the directory to that name
while leaving the original in place produces **two** packages claiming the same
spec name, and PlatformIO then reports `Missing toolchain directory 'None'` —
`get_package_dir` returns `None` rather than raising, and the message names the
*absence*, which reads like a deleted toolchain rather than a duplicated one.

**The state that is stable has exactly one candidate:** the directory
`~/.platformio/packages/toolchain-xtensa-esp-elf` (extracted from
`~/.platformio/dist/xtensa-esp-elf-14.2.0_20260121-aarch64-apple-darwin.tar.xz`,
116 entries in `bin/`) with `package.json` **and** `.piopm` both reporting
`14.2.0+20260121`. Verified with consecutive builds: `bin/` stays at 116 entries
and PlatformIO reports `toolchain-xtensa-esp-elf @ 14.2.0+20260121`.

Two facts that made this harder than it should have been, both recorded because
they will recur:

1. **The registry file and the manifest are two homes for one version.** When
   `.piopm` said `14.2.0+20251107` and `package.json` said `14.2.0+20260121`, the
   build ran fine — so the mismatch is not itself fatal — but the banner printed
   the stale one and the platform's `_check_tool_version` compared against the
   other. That is the project's dominant defect class appearing in the toolchain
   cache. Both now agree.
2. **`~/.platformio/tools/` is NOT a backup of `~/.platformio/packages/`.** It is
   a separate install root with its own copies; the `dist/` tarball is the reliable
   source. Restoring the package **directory** from `tools/` gives a tree whose
   top-level `bin/` is real but whose layout otherwise differs.

## The on-device test suite did not link (the whole "D" column was unrunnable)

Found 2026-09-23, same audit pass. Every "D" cell of the spec's coverage matrix
(§11) claims a device test on real silicon. Nothing in CI ever built
`test/test_hw` — the workflow ran `pio run -e esp32s3` and `pio test -e native`
and stopped, so the device suite was never compiled by anything. Compiling it
for the first time:

```
ld: .pio/build/esp32s3/test/test/test_hw/TestEspHal.o: multiple definition of `setUp';
    TestBleProvisioning.o: first defined here
```

The cause is Unity's own runner. `UnityDefaultTestRun` (unity.c:2201) calls the
GLOBAL `setUp()` and `tearDown()` around every test, and `test_hw` is three
translation units — `TestEspHal.c`, `TestBleProvisioning.cpp`, `TestUsbCdc.cpp`
— each of which defined its own to bring up the hardware it needs. Three
definitions of one symbol is a multiple-definition error, so the suite could not
link and the spec's device test plan was, in fact, empty.

**The fix is per-file hooks.** `unity_config.h`'s `TEST` macro now passes the
file's `swc_setup`/`swc_teardown` (both `static`) into the registry at
registration, and `app_main` calls them around each test. The global
`setUp`/`tearDown` still exist in `test_main.c` as empty no-ops, because Unity's
runner insists on them. The names must NOT be `setUp`/`tearDown`: unity.h
declares those non-static, and a `static` redefinition of a non-static
declaration is its own error.

**A second defect in the same file, found by the warning the fix surfaced.**
`esp_hal_clock_advances` asserted with `TEST_ASSERT_GREATER_OR_EQUAL_UINT64`,
which is a STUB on this target: `UNITY_SUPPORT_64` turns itself on only when
`UNITY_LONG_WIDTH` or `UNITY_POINTER_WIDTH` is 64, and both are 32 on xtensa, so
the macro expands to `UNITY_TEST_FAIL("not supported")`. The test could never
pass AND asserted nothing — the compiler said so, quietly, by reporting `t0`
unused, since the operand was discarded. It now compares the low 32 bits with
the UINT32 form. It was the only 64-bit Unity assert in the device tree.

**The guard.** CI now compiles the device suite with
`pio test -e esp32s3 --without-uploading --without-testing`. With the defect
reintroduced it exits 1 (`ERRORED`); clean, it exits 0 (`SKIPPED`). Verifying
that exit status mattered — a step that printed the error and exited 0 would
have been the same false confidence in a new costume.

## Five CommandRouter frame-handler defects (found by the 2026-09-23 audit pass)

All five are in `lib/Link/CommandRouter.cpp`, each invisible to the native suite
until a test drives the exact frame shape it needs.

**An empty `button_id` persisted a config the next boot refuses.** `Str()` returns
the item for `""` — a valid JSON string with a non-null `valuestring` — so a
present-but-EMPTY id passed the "required" guard and was copied into the stored
button. Neither `LearnSession::Commit` nor `ConfigStore::Save` validates it, so it
persisted. `ConfigDecodeJson` refuses the empty string (`ReadStr`), so the next
boot's `Load` returns `kFellBackToDefaults` and the user loses every binding, both
channels and all settings, reported only as a corrupt config. The id/name are now
checked for non-empty AND width. Every prior `learn_commit` test sent a real id,
which is why none caught it.

**`send_duration_ms` had no upper bound.** A `config_patch` of `1e10` — which
`NumToU32` had already wrapped into range — reached
`key_released_at_ms = now + send_duration_ms` and drove the KEY line for ~49.7
days: the phantom press FR-15/FR-39 exist to prevent. `ConfigValidate` bounded only
that it was NONZERO; the codec's magnitude check refuses values PAST u32, not the
u32 maximum. It now has a RANGE ceiling (`kSendDurationMaxMs`, 10 s), the same rule
`maintenance_timeout_ms` already followed, and the spec's §3.4 paragraph says so.
`test_key`'s `hold_ms` was already bounded at 1 s for the identical reason.

**`learn_stop` defeated the channel guard.** The guard keyed on `learn_open_`, but
`learn_stop` is the SPECIFIED flow (spec 4.3: start → stream → stop → commit) and
clears it. So `learn_start(0) → stream → learn_stop → learn_commit(1)` wrote
channel 0's measurement onto channel 1's ladder — the exact wrong-channel write the
guard was added to prevent. It now keys on `learn_channel_`, the SESSION's channel,
which survives a stop.

**`learn_commit` never closed the stream.** It left `learn_open_` true, so the
device kept emitting `ladder_sample` and kept feeding `session_` post-press idle
readings; a duplicate commit re-ran `Commit` over idle samples, and a client that
committed and went quiet left the device streaming into the link forever.

**`identify` collapsed its two patterns.** Both `"flash"` and `"buzz"` mapped to one
`Identify()` that did both the LED double-flash and the buzz — the "accepts a field
and ignores it" class. `Identify(bool flash, bool buzz)` separates them; only
`flash` arms the LED_STAT borrow window. A buzz-only request no longer flashes.

Each fix has a test that fails on the reverted code (mutation-tested), and the
spec rows for `send_duration_ms` and `identify` were corrected to match.

## AUX1–AUX3 are declared bindable and nothing services them (N-26)

Found 2026-09-23, continuing audit. `BindingChannel` is `SWC1 | SWC2 | AUX1 |
AUX2 | AUX3 | ANY` (spec §3.5/§3.1), `Config` carries an `aux[3]` table, §3.7's
worked example binds `{"channel":"AUX1","button":"aux1","gesture":"SINGLE"}`, the
codec and `ConfigValidate` accept such a binding, and the Android model exposes
AUX1/2/3 as first-class channels. **No firmware path reads any of it**:

- `SystemOrchestrator` services only `channels_[0..channel_count_-1]` — the two
  SWC ladders. There is no per-AUX servicing loop.
- `ADC_CH_AUX2`/`ADC_CH_AUX3` are named only by `EspHal`'s channel map, never read.
- `config.aux[]` is read only inside the codec.
- `BindingResolve` consults `aux[i].id` only to decide whether a binding names a
  real input; it is never invoked for an `AUX*` channel because nothing produces a
  gesture event from an AUX input.

An app that binds a gesture to AUX1 gets an `ack` and a binding that never fires.
The one AUX behaviour that IS wired is AUX1's learn role (the 1.5 s / 3 s holds),
a separate mechanism. Recorded as N-26 with a cheap v1 decision (declare AUX1–3
bindings accepted-and-inert, drop them from the app picker); the full fix is a
spec decision plus a servicing loop plus a hardware measurement, so it is not a
local patch.

## The app set VersionMismatch and kept talking (§4.5)

Spec §4.5: on a version mismatch the app "must show an explicit ... state rather
than attempting to talk. Silent partial compatibility is how a config gets
corrupted." `SwcClient` set the state — off the envelope `v` and off a
`version_mismatch` nack — but nothing gated outbound frames: after a mismatched
`hello`, `connect()` still wrote `ping` and `getConfig()` still wrote
`config_get`, onto a peer whose vocabulary disagrees. Fixed at the single choke
point every write passes through (`sendLocked`), with a short-circuit in
`sendAndAwait` so a caller does not park on a full 15 s timeout over the
deliberate silence. Test `a version mismatch stops the app talking`, mutation-
tested. The app's other §4.4 gap — no 5 s idle-ping, no 10 s silence watchdog, and
`LinkState.Disconnected` assigned nowhere so a dead transport leaves the UI showing
`Connected` — is recorded as N-27 (an app-plan decision, not a local patch).

## The app's numeric limits drifted from the firmware's (`send_duration_ms`)

Found 2026-09-23, continuing audit — and it was a drift the PREVIOUS pass
introduced. That pass added a key-hold ceiling, `kSendDurationMaxMs` (10 s), to
`ConfigValidate` and `ConfigModel.h` and updated the spec, but did NOT add the
mirror to the app. `ConfigJson.problems()` is documented in its own comment as
"Mirrors `ConfigValidate`"; it is the gate `SwcClient.setConfig` runs before
sending. So a config carrying `send_duration_ms = 60000` passed the app's local
check, reached the device, and was nacked at decode — losing the whole save with
the offending field UNNAMED. The `maintenance_timeout_ms` ceiling had been mirrored
by hand (third-audit finding 1 recorded the parity gap and that nothing guarded
it); this is the same shape, one field later.

Fixed by adding `K_SEND_DURATION_MAX_MS` and the range check to the app, plus a
mutation-tested assertion in `ConfigCodecTest` (`the app refuses the fields the
firmware refuses`). **The class of defect is now guarded**: `tools/check_app_limits.py`
reads every count, string width and timing ceiling from BOTH `ConfigModel.h` and
the app's `Config.kt` and fails on any divergence — nine limits, wired into the
firmware workflow. Mutation-tested: changing the firmware ceiling to 20000 fails
the guard with the field named.

## `getConfig` parked on its full timeout under a version mismatch

Found 2026-09-23, continuing audit — the sibling the previous pass's §4.5 fix
missed. `sendLocked` (the choke point) correctly refuses to put `config_get` on the
wire once the versions disagree, and `sendAndAwait` short-circuits so a
reply-bearing request does not wait for a reply that deliberate silence will never
bring. But `getConfig` does not use `sendAndAwait`: it registers a waiter that only
a config RUN can end, and with no frame out no run can come — so it parked the
caller for its full 15 s timeout on a silence the app chose, which is exactly what
`connect()` hits on every launch against a mismatched device. Fixed with an early
return of the unchanged local model, matching `sendAndAwait`. Test `getConfig does
not wait out its timeout once the versions disagree`, mutation-tested.

## The contract's `ack` row named a field no ack emits — and the field lists had no guard

Found 2026-09-23, continuing audit. `contract_schema.py` had no test comparing a
frame's declared `fields` to what the router actually puts on the wire, and it had
drifted the same way spec N-22's `status` row did:

- `ack` declared `err`, which **no ack ever emits** — every ack is `ok:true` and
  every failure is a `nack`, so `err` is a field the app would read and never
  receive. It also **omitted `mv_center`/`mv_tolerance`**, which `learn_commit`'s
  ack genuinely carries (`HandleLearnCommit`) — the derived window that IS the
  answer the learn flow exists to return, so the omission hid the one ack field a
  client needs.
- `status` was already corrected to `vbus_present,gain_mode,uptime_ms,config_state,output_safe`
  (N-22), but nothing pinned it.

Fixed the schema and the spec §4.3 row, and added
`test_frame_field_lists_match_the_router`, which reads the router's emit sites
(each `Emit("X", body)` attributed to the `snprintf` that built `body`) and asserts
**both** directions: no field emitted but undeclared, and no field declared without
a producer (`for_seq` excepted — it is reply-only by spec). Mutation-tested three
ways: restoring `ack`'s `err`, restoring `status`'s phantom `rail_mv`, and adding a
producerless field each fail with the field named. The `fields` column is unused by
the generators, so this whole change produces **no diff** in the generated
`swc_contract.h`/`Contract.kt` — it is a vocabulary guard, exactly as the field
lists were meant to be.

`time_sync`'s `epoch_ms`/`tz_offset_min` and `ota_begin`'s `size` are declared with
no reader, but both are documented deferrals rather than live defects (the firmware
has no RTC — `time_sync` is acked and discarded on purpose — and every `ota_*` is
nacked `not_implemented`, N-14), and the app sends neither. Left as-is; the new test
exempts them by reading only the fw→app direction, which is where the N-22 class of
phantom lived.

## The spec claimed `hello` is sent "on request" — it is sent only on connect

Found 2026-09-23, continuing audit. Spec §4.3's `hello` row read "Sent on connect
**and on request**", but the firmware has exactly ONE `hello` producer:
`CommandRouter::OnConnected()`, reached from the DTR callback when the host opens
the port (`UsbLink`). No request frame yields a `hello` — `ping` answers with a
`status` (§4.3's own reply row says so), and §4.5's negotiation reads the envelope
`v` that every frame carries, never `hello.protocol_v`. This is the spec-overclaims
shape: a reader implementing a client could wait for a `hello` in reply to a
`ping` and hang, or assume a re-`hello` recovers a link that has gone stale (it
does not). Corrected the row to "Sent on connect" with the single producer named.
Doc-only; no code change.

## Spec §3.1's entity map named a `created_at` field that does not exist

Found 2026-09-23, continuing audit. §3.1's entity map listed the top-level
`Config` fields as `schema_version, device_id, created_at, updated_at`, but the
model (`ConfigModel.h`), the codec (`ConfigCodec.cpp`) and the decoder-verified
§3.7 worked example carry exactly ONE stamp: `updated_at_ms`. There is no
`created_at` anywhere in `lib/`, `android/`, or `contract/`. A reader taking §3.1
as the field list would emit a `created_at` the codec ignores and omit
`updated_at_ms`'s real name. Corrected the map to `schema_version, device_id,
updated_at_ms`.

Investigating it surfaced the real underlying gap, recorded as **N-28**:
`updated_at_ms` is itself declared, round-tripped and validated, but no production
path gives it a value — the firmware never assigns it (only a test sets a fixed
`1700000000000`), and the app writes back whatever it decoded (or `0`). It has the
N-22 shape: a field carried and decoded but never meaningful. Not repaired here
because a producer is a decision (no RTC without `time_sync`, so a device stamp
would be uptime/boot-count, not wall-clock), and nothing reads it, so the only
cost today is bytes.

## The ADC ceiling was spelled three times (one fact, three homes)

Found 2026-09-23, continuing audit. The calibrated ADC ceiling at 12 dB -- "the
largest pin reading possible", 2900 mV, spec §3.2's `MilliVolt` bound -- existed as
THREE independent constants with nothing comparing them:

- `kAdcFullScaleMv12dB = 2900` in `lib/Analog/CalibrationCurve.h` (the real one --
  it is the `mv_high` endpoint of the calibration curve, fed to `AdcRawToMilliVolts`);
- an anonymous-namespace `kAdcCeilingMv = 2900` in `lib/Analog/LadderDecode.cpp`
  (`LadderProfileIsValid`, the rebase clamp);
- an anonymous-namespace `kAdcCeilingMv = 2900` in `lib/Learning/LearnSession.cpp`
  (the sample-range gate).

This is the project's most-repeated defect class (two homes for one fact, one of
them stale) -- the same shape as `kIdleMarginPermille` before it was exported, and
`kNominalRailMv`, both of which carry a comment saying exactly why they live in a
header. A re-tune of any one (say the 12 dB endpoint, if a board revision changed
the divider) would have silently disagreed with the ADC's own scaling.

Fixed: `LadderDecode.h` now includes `Analog/CalibrationCurve.h`, and both `.cpp`
files read `kAdcCeilingMv = kAdcFullScaleMv12dB` from that one home rather than a
literal. Mutation-tested by setting the shared constant to 2500: 56 native tests
across `test_analog`, `test_learning` and `test_system` fail, proving both
consumers genuinely read the shared name and no local literal survives. Restored;
450/450 pass, device build succeeds.

## The firmware runs only a binding's FIRST action; §3.5 and the app run the list

Found 2026-09-23, continuing audit. Spec §3.5 makes a binding's `actions` an
**ordered list executed in order, each independently failable**, and names the
product's core case as one binding carrying both halves: "emit the factory key
press **and** tell the app". The plan agrees — "the multi-action runner is Task
13's job". The APP implements its half: `AppViewModel.runAppSideAction` iterates
`binding.actions` and runs every app-owned one. The firmware does not:

- `BindingResolve` returns only `actions[0]` (`out.action = b.actions[0]`);
- `SystemOrchestrator` executes that one action and only in its `OUT_VOLTAGE`
  branch — every other kind, and every action past the first, falls to the `else`
  that just `ReleaseKey`s.

**Confirmed by probe, not by reading.** A temporary `BindingResolver` test built a
legal two-action binding (`[APP_INTENT, OUT_VOLTAGE]`, `kMaxActionsPerBinding` is
2, so `ConfigValidate` accepts it) and asserted the resolver returns the
`OUT_VOLTAGE`. It FAILED: the resolver returned `APP_INTENT`. So on a binding whose
app-side action is FIRST, the firmware drives NO key at all (the `else` releases
the line) while the app still fires its own half from the `event` — a press that
silently does nothing on the wire. Probe removed after confirming; 450/450 pass.

**Reachability is narrow:** the app's editor always builds single-action bindings
(`actions = listOf(action)`) and `ConfigDefault` ships none, so this needs a
hand-authored or `config_patch`-written config. Recorded as **N-29** with the v1
alternative (refuse a multi-action binding in `ConfigValidate` until the runner
exists, turning silent partial execution into a named rejection).

## `config_patch` omitted the one settings scalar it claimed to cover

Found 2026-09-23, continuing audit. `HandleConfigPatch`'s own comment scopes it to
"`settings.*` scalars only", and it carried a path branch for every one EXCEPT
`maintenance_timeout_ms` — the single numeric settings scalar missing from an
otherwise-complete table. A client could patch all four timings and both feedback
levels but not the maintenance window; the path was refused `unknown_path` as if it
did not exist.

Inert today (the app and the web page send no `config_patch`), but it is a real
inconsistency between the handler's stated scope and the table, and the fix is the
same shape as everything else there. Added the branch (`NumToU32`, so the same
range rule as the others) and extended the spec §4.3 row to NAME the path
vocabulary rather than saying "single field" generically — the vague wording is
what let the omission hide.

Two tests added and mutation-tested: `APatchCanSetEverySettingsScalarIncludingTheMaintenanceWindow`
(asserts the whole declared set is patchable, so a future added scalar fails here
rather than going silently unpatchable) and `APatchOfTheMaintenanceWindowIsRangeChecked`
(zero and `> kMaintenanceTimeoutMaxMs` are refused, stored config untouched).
Reverting the branch fails the first test. Native suite 450 → 452.

## A recovered config was reported as DEFAULTS over the link

Found 2026-09-23, continuing audit. `ConfigStore::Load` reports a config that came
from the BACKUP slot (the newest slot was torn) as `kRecoveredFromBackup` — a
distinct result precisely because that config is real and authoritative: spec 6.8
makes the other slot THE config after a tear. `SystemOrchestrator::Boot` accepts
both `kLoaded` and `kRecoveredFromBackup` and runs the recovered config.

Two other call sites wrote the narrower test `== kLoaded`:

- `CommandRouter::BeginConfigReplyRun` (the `config_get` / connect reply). On a
  recovered device it answered with `ConfigDefault()` while the device was RUNNING
  the recovered config. The app drew an empty grid over a configured device, and
  its next save then wrote those defaults back — wiping the user's bindings, both
  learned ladders and every setting, reported as a successful save.
- `CommandRouter::HandleLearnStart`'s neighbour seed. Reading only `kLoaded` left
  the neighbour set EMPTY, so a re-learn on a recovered device could not see the
  buttons already there and could commit a second window on top of an existing one.

**The fix is one home, not two patches.** `ConfigLoadResultIsUsable()` in
`ConfigStore.h` now answers "did `Load` put a real, user-authored config in the
out-parameter", and all three sites (Boot included, where the inline `||` was the
same fact spelled a second time) call it. A predicate is the right shape here
because the rule is about the RESULT's meaning, and every consumer of `Load` asks
that same question.

Two tests added and mutation-tested independently: `AConfigGetReportsARecoveredBackupRatherThanDefaults`
(reassembles the chunked reply and asserts it carries the RECOVERED device id, not
the default one) and `ALearnStartOnARecoveredDeviceSeedsTheRecoveredLadder` (a
measurement inside a recovered button's window is refused `too_close_to_existing`,
which is only true if the recovered ladder seeded the neighbour set). Reverting
either call site fails exactly its own test and not the other. Native suite
452 → 454.

## The config-fault state and LED were latched at boot, and never cleared by a commit

`status`'s `config_state` is spec'd (§4.3) as **the config's state** — one of
`ok` / `none` / `recovered` / `defaults` — and `status` is emitted "Periodic **+
on change**". The firmware set it once, in `Boot`, from the `ConfigLoadResult`,
and `ApplyConfig` never touched it. So the field went on describing the BOOT LOAD
after the device had committed — and was RUNNING — a different config.

The reachable failure is the app's own wording. `LinkScreen.configWarning` for
`defaults` tells the user *"Your learned buttons and bindings are gone. Program
it again from the Bindings screen."* The user does exactly that — a `config_end`,
`config_patch` or `learn_commit` — and the device keeps reporting `defaults`
every 2 s forever. The app re-asserts a fault the device is no longer in: the
report contradicting the reality, which is the exact class of lie §6.8 and the
`config_state` field both exist to prevent. `none` had the same shape (a fresh
device that has just been programmed is no longer a pass-through device), and
`recovered` likewise (the newest slot was lost but the user has since re-saved).

The LED had the matching half. `Boot` latches `LED_STAT` to `kBlink` on a
`kFellBackToDefaults`, and nothing cleared it — so a remedied device blinked
"not OK" forever. Spec §7.3's "`blink` clears only on a reboot" is stated for **"a
wiring fault or a collapsed rail ... a hardware condition that does not fix
itself"**; a corrupt config is not one. A commit writes a valid config, which is
precisely the remedy.

**The fix splits the one flag into the two facts it was conflating.** `faulted_`
became `hw_faulted_` (a hardware condition, latched to reboot, raised by
`ReportFault`) and `config_faulted_` (the spec 6.8 fallback, cleared by a commit);
`Faulted()` ORs them, because the lamp is the single fault channel. `config_state_`
(renamed from `boot_config_state_`, a name that invited the boot-only behaviour)
is set by `Boot` and reset by the new `NoteConfigCommitted()`, the one home every
commit path already funnels through — `ApplyConfig` (`config_end`, `config_patch`,
the app's `learn_commit`) and `ApplyLearnedProfile` (the headless AUX1 learn).

Spec updated first, in both places: §4.3 now states the field follows the config
through a commit, and §7.3 states the config-fault exception to the reboot-only
latch and why the two fault kinds share a lamp but have different lifetimes.

Two tests, mutation-tested independently. `ApplyConfigClearsTheConfigFaultAndReportsOk`
(boots a corrupt config, asserts `defaults` + a latched fault, commits, asserts
`ok` + no fault) and `ApplyConfigKeepsAHardwareFaultLatched` (a collapsed rail
stays latched across a commit, while `config_state` still moves to `ok` — the two
facts are independent and the lamp shows either). Reverting the `NoteConfigCommitted()`
call fails both; making it also clear `hw_faulted_` fails exactly the hardware test.
Native suite 454 → 455.

The learn wizard's `Exit` handback was checked as a possible third case — it
stamps `LED_STAT` solid directly, bypassing `RestatLeds` — and is **correct**: the
maintenance-deferral branch holds its state flag inverted while the wizard is
active, so the first tick after it deactivates fires a `RestatLeds` that restores
the fault blink. Verified by probe (10 toggles/s over the second after a learn on
a faulted device), so no change was needed there.

**A second half found while testing the fix.** `ApplyLearnedProfile` performs its
OWN `Save` (the wizard holds no store), so the obvious placement of
`NoteConfigCommitted()` — right after the `pass_through_ = false` — would report
`ok` and stand the config-fault blink down BEFORE the write, and a FAILED write
would leave the app told the config was safe while nothing reached NVS: the
"reported success for a failed write" lie `persisted_` exists to prevent, one
layer up. The call is therefore ordered AFTER the save and **gated on
`persisted_`**. The other two commit paths (`config_end`, `config_patch`) already
gate `ApplyConfig` on a successful `Save`, so they need no change; only the
headless learn owns its save. Third test:
`AFailedLearnSaveDoesNotStandDownTheConfigFault` (corrupt boot → `defaults` +
latched, arm `FailNextNvsWrite`, learn, assert the word STAYS `defaults` and the
fault STAYS latched) — removing the `persisted_` gate fails it. Native suite
455 → 456.

## The size gate measured the TEST image, not the production one (CI R-1 was blind)

`pio run -e esp32s3` (production) and `pio test -e esp32s3` (the device Unity
suite) are ONE PlatformIO env, so they share ONE build directory and write the
SAME `.pio/build/esp32s3/firmware.bin`. Whichever ran last is what a reader sees.
CI ran the production build, then the device-suite link, then `check_size.py` — so
the gate read the **test** image.

Measured on this machine: production `firmware.bin` = **386,064 bytes** (19.6 % of
the 1,966,080-byte slot); the image left behind by `pio test -e esp32s3` =
**287,424 bytes** (14.6 %). The gate was under-reporting the production image by
**~99 KB** — ready to pass a production image ~99 KB over its slot, which is the
exact R-1 failure (an image that cannot be OTA-updated) the gate exists to catch.
A gate that reports a confident number for the wrong file is worse than no gate.

The two images are distinguishable without running anything: the test image links
Unity's runner (`UnityDefaultTestRun`, `UnityBegin`, `UnityConcludeTest`) and none
of the app's own symbols, and the production image is the reverse. `check_size.py`
now reads the artifact's sibling `.elf` and **refuses to report** (exit 2) when it
is a Unity image, naming the shared-path cause and the fix. The CI step order is
also corrected — the device-suite link runs BEFORE the production build, so the
build is the last writer and the measured file is the real one — but the refusal
is what makes the mistake impossible to make silently rather than merely unlikely;
the ordering could be reordered again by a future edit.

Mutation-checked: disabling the `is_unity_test_image` guard makes the gate report
`14.6 % [OK]` for the test image (the original bug), and restoring it reports the
true `19.6 % [OK]`. Four tests added in `tools/test_check_size.py`, and the CI
`pytest` step broadened from `pytest test_gen_contract.py` to `pytest .` — a
named file silently skipped every later test file, which is how a new gate test
would have been added and never run. tools pytest 20 → 24.

## A blank eFuse booted silently — the BOOT_DEGRADED the comments promised was never played

Spec §3.2 requires that when `adc_cali_create_scheme_curve_fitting()` returns
`ESP_ERR_NOT_SUPPORTED` (blank eFuses — some third-party module batches), the
firmware "fall back to a documented linear approximation **and report that it
did**, rather than silently mis-scaling every reading". The fallback exists. The
**report** did not: `EspHal` logged it to the console and set `cali_degraded`, but
nothing ever announced it, and both `src/main.cpp` and `EspHal.cpp` carried a
comment claiming "the orchestrator also plays `BOOT_DEGRADED` for this class of
condition".

It did not. `IHAL` carries no calibration accessor, so the orchestrator had no way
to know; its boot pattern came purely from the `ConfigLoadResult`, and
`kBootDegraded` fired only for a recovered config (SystemOrchestrator.cpp:420). A
blank-eFuse device therefore booted with the clean `BOOT_OK` beep — silent about
the one thing the spec says must not be silent. The device console log that does
name it is unreadable in production anyway (N-16: TinyUSB moves the shared USB PHY
and the console goes dark), so the buzzer is the only signal a user at the bench
actually hears.

> **N-16 corrected 2026-09-25 — the console is no longer dark.** The paragraph
> above is right about the buzzer being the bench signal, but the console claim is
> now obsolete: the console PRIMARY was moved to UART0 (the fabricated board already
> pads it out at TP7/TP8), which is OFF the USB PHY, so `ESP_LOG*` survives TinyUSB
> taking the PHY. The `BOOT_DEGRADED` beep is still worth playing — it is what a user
> with no console hears — but the log is now readable too. See the N-16 entry below.

The plan's own step for this (`EspHal.c`) specified **two** mechanisms — "a `log`
frame (§4.3) at init, and a `BOOT_DEGRADED` boot (§7.2)". Neither was reachable:
the `log` frame cannot exist because the log sink is registered by `UsbLinkStart`,
which runs *after* `SystemOrchestratorCreate`, so there is no link at boot; and the
boot pattern was never wired.

**Fix.** `SystemOrchestratorCreate(hal, calibration_degraded)` takes the flag —
it is an ARGUMENT, not a call into `EspHalCalibrationIsDegraded()`, because
`SystemOrchestrator.cpp` is HOST-compiled and `EspHal.cpp` is the one translation
unit the host build excludes, so naming it here would fail the native link. The
device caller (`src/main.cpp`) already holds the answer. `Boot` folds it into the
pattern with an explicit precedence — a config fallback (`FAULT_CONFIG`) outranks a
degraded calibration, which outranks a clean boot — because `Play` REPLACES, so
playing-then-overwriting would lose the louder signal. Spec §3.2, the plan step,
and open item N-17 corrected to match.

Two tests, mutation-checked: `ADegradedAdcCalibrationBootsAsDegradedNotSilent`
(3 pulses = `BOOT_DEGRADED`) and `AHealthyAdcCalibrationStillBootsClean` (1 pulse =
`BOOT_OK`, so the first cannot pass by the pattern always being degraded). Dropping
`|| calibration_degraded_` from `Boot` fails exactly the first. Native suite
456 → 458. Device build and the device-suite link both succeed with the changed
`Create` signature.

## The app's DTR went to the data interface — so `hello` was never sent

Found 2026-09-23, continuing audit. `UsbSerialTransport.open()` issues CDC
`SET_CONTROL_LINE_STATE` itself (Android's `UsbDeviceConnection` has no `setDtr`),
and it addressed the request to the **data** interface — `conn.setControlLineState(
iface.id, ...)`, where `iface` is the `USB_CLASS_CDC_DATA` interface it had just
claimed. That is the wrong interface, and the failure is total and silent.

CDC 1.2 §6.3.12 puts `SET_CONTROL_LINE_STATE` on the **communication** interface,
and the device side enforces it: TinyUSB's `cdcd_control_xfer_cb` walks its CDC
instances and matches `request->wIndex` against `p_cdc->itf_num`
(`managed_components/espressif__tinyusb/src/class/cdc/cdc_device.c:388`), and
`itf_num` is set from the **communication** interface descriptor
(`cdcd_open`, `cdc_device.c:307`). `TUD_CDC_DESCRIPTOR` emits the pair as
`(comm_id, comm_id + 1)` (`usbd.h:262`), so the data interface number matches no
instance: the loop falls out, `TU_VERIFY(itf < CFG_TUD_CDC)` fails, the class
handler returns false, and the core stalls EP0 (`process_setup_received`'s
"Returns false if unable to complete the request, causing caller to stall control
endpoints").

The chain from there is every part of the link:

1. The request STALLs, so `tud_cdc_line_state_cb` never fires.
2. `UsbLink`'s `CdcLineStateCallback` therefore never records an open, so
   `ServiceLineState` never calls `CommandRouter::OnConnected()`.
3. No `hello` is emitted — `OnConnected` is its only producer — and no config reply
   run is started.
4. The app sets `LinkState.Connected` only on `hello`, so the app sits at
   `Disconnected` forever. `getConfig` still asks, but the app's own `config_get`
   is answered by nothing.

Nothing errors anywhere. Enumeration succeeds, the interface claims, the bulk
endpoints read and write, and no frame ever arrives — which reads as a dead
adapter. The class's own comment already warned about exactly this ("getting it
wrong is silent"), which is why it is worth recording that it was wrong.

**Fixed** by resolving the communication interface rather than assuming the data
one: `selectControlLineInterface` (a top-level `internal` function so it is
JVM-testable, since the failure it guards is invisible without a device) prefers the
communication interface immediately preceding the claimed data interface and falls
back to the first one. A device with no communication interface yields null and is
reported `NotOurDevice` rather than sent a request that would stall.
`ControlLineInterfaceTest` covers the single-instance pair, a reversed enumeration
order, a two-instance composite device (where picking the *first* comm interface
would address the wrong CDC), the no-comm case, and the non-adjacent fallback.
Mutation-tested: returning `dataId` (the old behaviour) fails all five.

## A driven key tripped its own head-unit-gone check, flooding the link with events

Found 2026-09-24, on hardware — by the two-board bench rig (`code_driver_board/`:
a second SWC board presenting ladder stimuli into this board's SWC inputs, with the
DUT's own KEY output looped back into the driver's sensor inputs). This is the
defect the host suite could not see because it needs the analog loop closed by a
real sense node.

**Symptom.** A learned-only button — no binding, and no head unit to map onto —
re-emitted its gesture every poll tick while held. Measured: a steady 2 s hold at
the learned level produced **26–177 identical `event{…,"gesture":"SINGLE"}` frames**
~70 ms apart, where `SINGLE` means exactly one frame. A *higher*-level button
(the bound `OUT_VOLTAGE` path) emitted exactly one, which is the tell: the fault
depends on the commanded level, not on the press.

**Mechanism.** Spec §6.2 step 2's envelope check is on `V_KEY_idle` — the line's
**resting** level — but the check ran every tick, including while this device was
**driving** a pulse. While driving, the sense node reads **this device's own
output**; there is no other actor on the line. A learned-only button (`ConfigDefault`
ships `binding_count == 0`) has `head_unit_idle_mv_ == 0`, so it presents the
button's ratio onto the command band's **1800 mV floor** — which is *at* the
envelope's low edge. The reading therefore fell (just) outside the 1.80–5.20 V
envelope, `head_unit_gone` went true, and the tail did `ReleaseKey` +
`gestures.Reset()` **every tick**. The line floated back up, the still-held button
re-classified on the next tick, and re-emitted. The `Reset()` also discarded the
press state, so a held button could never reach `LONG`.

The user's own read of the signal was right and is the design intent: `/SENSEn` is
"only relevant while things stabilize on power up, … to determine the gain we
need". It is a **boot/gain** input (§6.2 step 1–5), not a per-tick supervision
input — and the one per-tick use it had was reading the device's own output back.

**Fix.** `SystemOrchestrator::HeadUnitGone(index, sense_mv, now_ms)` centralises
the test (both call sites — the enabled path and the disabled-channel path — had
their own copy of the raw comparison). It does not ask "is the line driven"; it
asks "is the reading **consistent with what we are driving**":

1. **A driven line is judged only on a deep sag.** Every command clamps to the
   band floor, so a driven reading at or above `kFaultSagMaxMv` (1600 mV = the
   1800 mV floor less a 200 mV margin, covering the servo's undershoot and the
   ADC's calibration of the commanded level — measured ~1790 mV for a 1800 mV
   command) is one we produced, not a fault. A **rail collapse** is the other
   out-of-envelope case while driving, and it drives the line far *below* that, so
   FR-39's phantom-key release still fires.
2. **A released line is judged on the whole envelope** — the spec's own reading —
   and the verdict must **persist** for `kHeadUnitGoneSettleMs` (250 ms), so a line
   settling after a release, or a rail coming up at boot, is not read as an absent
   head unit. A `-1` conversion failure holds the settle clock rather than being
   treated as out-of-envelope (the N-43 sentinel — `0 mV` is legal).

Pinned by `ADrivenLineAtTheCommandFloorIsNotAHeadUnitGoneFault`, mutation-checked:
disabling the sag guard fails exactly that test. `ARailSagDuringAPressReleasesTheKey`
still covers the deep-sag release, so the fix's two halves are each held by a test
that fails without it. Native suite 503 → 504. Device build clean (RAM 44.0%,
Flash 19.7%).

## USB OTA was unreachable — the router refused the frames it was built to carry

Found/fixed 2026-09-24 (spec open item N-14). `OtaUsb` implemented the whole
run — the verify gate, the chunk accumulation, the single commit point — and
`CommandRouter` nacked every `ota_begin`/`ota_chunk`/`ota_end` as
`not_implemented`, so no host could start one. `hello` had **correctly** stopped
advertising `"ota"` (a capability the router could not honour), which meant the
product's flagship in-car update path — the one the user asked for first, the one
that needs no WiFi and no maintenance page — was unreachable in a shipped-looking
tree. A green host suite did not notice, because the missing edge was the router
dispatch, not a library.

**This was deliberately left for the board, and the board is now here.** The
earlier note on the item said "Do NOT wire the router to `OtaUsb` before the board
exists" — the wiring is small, and it switches on the only path in this project
that can brick a device. With the boards in hand the wiring is done, and the
remaining risk (the `esp_ota_*` flash write) is left where it belongs: on the
bench, to be proved with a deliberately corrupt image before the write is trusted.

**The fix is a thin adapter, not a second implementation.** The three handlers
call the same `OtaBegin`/`OtaChunk`/`OtaEnd` the WiFi path calls, because spec 9's
rule is that the checksum, slot-writing and commit logic exists exactly once and
takes a byte stream — a second implementation of the verification path is how one
route ends up less safe than the others. The router layer only parses the frame,
applies the same range-first ordering the config transport already uses (cJSON
ignores `ERANGE`, so `size: 1e999` arrives as `+inf` and a bare cast of it is UB),
and turns the result into an ack or a nack.

Two requirements spec 9.3 states are now enforced and were not before: an image
larger than the slot is refused **before any byte is written**, and any chunk
whose offset is not the next expected byte is refused as a `gap` (and the run
aborted) — a spliced image would otherwise fail its digest at the end for a reason
that points nowhere near the cause. `kAppSlotBytes` (1920 KiB) is now the one home
for the slot bound, shared by the router, `OtaWifiInstall` and `check_size.py`, and
pinned against the app's mirror by `check_app_limits.py`.

`hello` advertises `"ota"` again, and the guard against re-drifting is
`HelloAdvertisesOtaOnlyIfTheDispatcherImplementsIt` — it asserts the caps string
against the dispatcher's real behaviour rather than a second hardcoded copy, so
the two cannot disagree silently.

Nine router tests cover begin/chunk/end, the gap, a bad hash, an oversize image, an
`inf` size, a chunk with no run, and a truncated run. The host cannot run the
`ESP_PLATFORM` flash write, so `OtaEnd` reports `result:not_supported` rather than
claiming an install — asserted, so a host test cannot build on a lie that nothing
was installed.

**The app half** is `SwcClient.pushFirmware` plus the Update screen: a file picker
(read off the UI thread), a progress bar driven by the per-chunk acks, and a
`PushResult` that says whether the image installed, was refused, or failed — every
non-install outcome stating that the device kept its old image, which is the
screen's core promise (spec 9). Six client tests and eight screen/view-model tests.
Android 120 → 133; native 504 → 514.


## The console was dark in production — and it needed no respin to fix (N-16)

Spec §4.1 has long said the ESP32-S3's ONE internal USB PHY is shared by
USB-Serial-JTAG and USB-OTG, and that `UsbLinkStart` installing TinyUSB moves the
PHY to USB-OTG — so a console left on USB-Serial-JTAG goes dark the moment the app
link comes up. That part is correct and was verified against Espressif's docs. What
was never true is the REMEDY the spec recorded:

> The console can be restored with a UART0 console on unused GPIO43/44 (needs header
> pins or test pads **on a respin**), or an external PHY (≥6 GPIOs).

**The fabricated board already pads out that UART.** `SWC.kicad_pcb` routes the
module's `/TXD0` and `/RXD0` — GPIO43/44, the S3's `U0TXD`/`U0RXD` (SOC
`uart_channel.h`: `UART_NUM_0_TXD_DIRECT_GPIO_NUM 43`, `U0RXD_GPIO_NUM 44`) — to
test pads **TP7** and **TP8**, bare THT pads with nothing else on those nets
(verified against the PCB netlist 2026-09-25). So "needs a respin" was false, and
the decision N-16 asked to make "before the board is finalised" was already made in
copper: keep `TP7`/`TP8`, they are the console.

The consequence of not noticing was concrete. Every `ESP_LOG*` line the firmware
emits after the link comes up was unreadable — including `UsbLinkStart`'s OWN
`"tinyusb driver install failed"`, the one message that reports the link failing.
A dark console is why FR-40's `status.reset_reason` field exists at all (there was
no other way for a peer to see a watchdog reset) and why the blank-eFuse
`BOOT_DEGRADED` path leaned entirely on the buzzer.

**Fixed** in `sdkconfig.defaults`:

- `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` — console PRIMARY on UART0, which is **off
  the USB PHY**, so it survives TinyUSB taking the PHY.
- `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` — output is DUPLICATED to the
  USB port (the S3 sets `ESP_ROM_CONSOLE_OUTPUT_SECONDARY = 1`, so both get every
  byte), so `pio test -e esp32s3` and a serial monitor still read the log off USB
  until TinyUSB installs; the durable copy is on the pads.
- `CONFIG_ESP_CONSOLE_USB_CDC` stays forbidden — a console on the TinyUSB CDC port
  would let a debug `printf` be parsed as a protocol frame (spec 4.1's HARD
  requirement, unchanged).

The old `sdkconfig.defaults` comment asserted the OPPOSITE — "Both can be up at
once on the S3 because USB-Serial-JTAG is a ROM peripheral independent of the OTG
controller TinyUSB drives" — which is the exact false claim N-16 exists to refute;
it is corrected in place.

**Gated, not merely changed.** `tools/check_sdkconfig_keys.py` now REQUIRES
`CONFIG_ESP_CONSOLE_UART_DEFAULT` + the secondary and FORBIDS
`CONFIG_ESP_CONSOLE_USB_CDC`, so a later "simplify the console" edit cannot
silently reintroduce the dark-console defect; the on-device assertion
`TestUsbCdc.cpp::TheConsoleIsNotConfiguredOntoTheCdcPort` checks the same two facts
(it used to assert `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, the safe-but-dark primary).
Device build clean (RAM 53.4%, Flash 71.7%); native suite unaffected. A CP2102 UART
bridge is on the bench, so the restored console is directly verifiable on TP7/TP8.

**N-16 is fully resolved:** the download-mode half by N-80 (a reset enters the ROM
USB loader with no BOOT press), the console half here.

## The on-device suite ran on silicon for the first time: 6 failures of 22 (N-87)

`test/test_hw` had never executed on hardware. It was written "BLOCKED until the
board exists" (plan Tasks 14c/23), and CI's own comment says it is "NEVER run in
CI — it needs the device"; the green build step only proves it *compiles*
(`pio test -e esp32s3 --without-uploading --without-testing`). So the first real
run, on the DUT, reported **22 Tests / 6 Failures** — six latent defects, five in
the tests and one in the firmware.

**Five were tests that could not pass or could not mean anything.**

- **Reversed Unity arguments** (`esp_hal_dac_write_reaches_the_sense_divider`,
  `esp_hal_sense_pins_read_as_analog_not_driven`). `TEST_ASSERT_GREATER_THAN_INT_MESSAGE(threshold,
  actual, msg)` asserts `actual > threshold`, but both tests passed `(reading, bound)`.
  The readings were fine (505 mV, 1625 mV — valid driven levels); the CONSTANT was
  being compared as the actual, so the report read "Expected -1 to be greater than
  505" and "Expected 100 to be greater than 1625". Every assertion in both tests was
  backwards and could only ever fail.
- **`PendingTx() == 0` with no host** (`ATransportAcceptsAMaximumFrameWithNoHostAttached`,
  `ShortWritesAgainstTheRealFifoStillSendTheWholeFrame`). The suite deliberately
  does not install TinyUSB — that is what keeps the console readable over USB
  (spec 4.1 / N-16) — so `tinyusb_cdcacm_write_queue` returns 0 for every call and
  the TX buffer can never drain: "Expected 0 Was 1500 / 2046". Even *with* a host,
  two maximum frames (2046 B) cannot drain into a 512-byte FIFO nobody reads, so the
  no-host claim was self-contradictory. Reworked to assert what is true and
  valuable on device (the pair is buffered whole and never dropped; the real
  function returns bytes-ACCEPTED-not-requested, 0 with the link down), leaving the
  partial-acceptance retry path to the host suite's FIFO double.
- **`EspHalInit` was not idempotent** (`DeviceMacIsReadableAndNotAllZeroes`). Every
  test file's per-test `setup` calls `EspHalInit()`; the second call's
  `adc_oneshot_new_unit` fails with "adc1 is already in use" (`ESP_ERR_NOT_FOUND`),
  so `EspHalInit` returned NULL and every test after the first file reported
  "Expected Non-NULL". The same call also `memset`s `g_state`, wiping live I2C/ADC
  handles — a real hazard, not just a test artifact. Fixed with an idempotent latch
  that returns the SAME interface on a repeat call.

**One was a real firmware defect.**

- **`esp_hal_ldac_idles_high_so_the_pulldown_cannot_latch` read back 0.** IDF says
  it outright (driver/gpio.h:146): "If the pad is not configured for input (or input
  and output) the returned value is always 0." `InitGpio` configured
  `SWC_PIN_DAC_LDAC_B` as `GPIO_MODE_OUTPUT`, so the pin R13's 10 k pulldown makes
  safety-critical — ~LDAC, active LOW, which must *idle HIGH* — could never be read
  back as high. Fixed by configuring it `GPIO_MODE_INPUT_OUTPUT`: the drive is
  unchanged (push-pull, never floating), but its driven level is now observable.
  The pin's *behaviour* was always correct; only its observability was wrong, so no
  key ever mis-fired.

**Verified on the DUT 2026-09-25: 22 Tests 0 Failures 0 Ignored, OK.** Host suite
unaffected (597/597), tools 53/53, all 10 static gates green. The production image
was restored to BOTH app slots afterwards (`output_safe: true`, `config_state: ok`,
`heap_free: 139136`).

**A bench note, because it cost a cycle.** `tools/dev_flash.sh --loader` ends with
`--after watchdog_reset`, and on the S3 that does NOT clear
`RTC_CNTL_FORCE_DOWNLOAD_BOOT` — only esptool's `hard_reset` path does
(`esptool/targets/esp32s3.py:345`). So a device flashed through the download-mode
frame boots straight back into the ROM loader, looking wedged (`303A:0009`, silent,
esptool "No serial data received"). `esptool ... --after hard-reset` clears the bit
and boots the app. The script's OTA route (the default) does not hit this.

## §10.4 Level-4 repeatability: 200 presses each, zero misclassifications (2026-09-25)

The §10.4 stress battery's repeatability half is now EXECUTED on the two-board
rig: `tools/bench_ladder.py --only stress --presses 100` presents each learned
button's own centre 100 times and requires the DUT to resolve THAT button every
time. Two instrumented runs: **0 misclassifications across 2 buttons × 100
presses, twice** (400 consecutive clean presses at the board's own rail).

The check now **attributes a miss before counting it**, because the first version
of this check reported phantom failures that were entirely the rig's:

- **A press cycle whose settle is shorter than the rig's DAC ramp misses the
  press.** `set_level` reads the rig's status, which releases its DAC
  (`KeyLine::FloatMv` → `Dac::Release`); the next `drive_now()` then RAMPS for
  ~1 s. A cycle that drove the centre with a 0.4 s settle presented the *previous*
  level, so the DUT saw no press and the check reported "got no event" — 6 of 6
  presses, which reads exactly like a broken classifier. Driving with
  `settle_s=1.4` (and `drive_now`'s own default is 1.6) fixed it.
- **A miss is attributed: `set_level` returns `None` when the rig never converged
  to the centre**, so a `None` is a rig non-convergence, counted separately and
  excluded. A genuine miss (rig DID converge) is re-presented once at the same
  centre to separate a one-off from a repeatable failure; the retry outcome is in
  the error string.

Across the two instrumented runs there were also **0 rig non-convergences** — so
on a 1.4 s cadence the rig's +/-nudge loop reached the centre every single time.

**What this does NOT cover.** The rail-voltage sweep (3.14/3.30/3.47 V) needs the
+3V3 net driven by a bench supply, which the two-board rig cannot command — the
rail is the DUT's own regulator — so that leg is bench-gated and is not claimed
here. The 72-hour soak and the DAC 4096-code monotonicity sweep (which needs the
KEY-out loopback wire, absent on this bench) are likewise not run here.

## FR-1's NTC clause: the channel was never converted, so `temp_c_at_learn` was a constant

Found 2026-09-24, continuing audit (N-67). FR-1 requires the firmware to "sample
both ladder channels **and the NTC** continuously". The ladder half was real —
every poll tick converts each channel through `AdcReader`. The NTC half had no
implementation at all:

- `ADC_CH_TEMP` was mapped to `ADC_CHANNEL_6` in `EspHal`'s `AdcPinFor` and read
  by **nothing** — every `adc_read_mv` call site named SWC1/SWC2, AUX1 or
  KEY_SENSE1/2.
- There was no NTC-to-temperature conversion anywhere in the tree — no B3380
  routine, no Steinhart-Hart, no divider inversion — so the raw millivolts would
  not have been a temperature even if read.
- Both learn paths passed a literal 0 for `temp_tenths_c`, so the field a future
  compensation is meant to consume could not hold a measurement.

Three separate surfaces said otherwise (spec 6.4's honesty paragraph, §11's FR-1
row, and a comment in `SystemOrchestrator.cpp` that quoted a sentence appearing
nowhere in the spec). Those were corrected in an earlier pass; this closes the
feature they had described.

**Fixed.** The divider values came off the schematic rather than needing the
board: `SWC.kicad_sch` carries `RT1` (`Device:Thermistor_NTC`, value `10k B3380`)
from the `TEMP_ADC` node to `GND`, and `R29` (`Device:R`, `10k`) from `+3V3` to
that node. So the part is on the **low side** — the node RISES with temperature —
and `R = R_series * V / (VDD - V)`.

`lib/Analog/NtcConvert.h` holds the inversion and the B-constant model
(`1/T = 1/T0 + ln(R/R0)/B`) in integer maths, because the config carries no
floating point. `ln` is the expansion `2*(y + y³/3 + y⁵/5 + …)` with
`y = (R-R0)/(R+R0)` in 2^16 fixed point. **Thirty series terms, not the
conventional eight**: at the hot end `y` approaches -0.9 and eight terms are
+0.8 °C wrong; thirty bring the error under 0.25 °C across -40 to +120 °C. A
tenfold-wider fixed-point scale does not help — the truncation is the error term,
not the scale.

`SystemOrchestrator::SampleNtcTenthsC` reads `ADC_CH_TEMP` and converts, and
**both** learn paths now record the result in `temp_c_at_learn`: the headless
wizard (every tick of a prompt) and `CommandRouter::RecordLearnSample` (the
app-driven session). A failed read **holds** the last good value rather than
reporting the sentinel, because the ADC returns -1 on error (N-43) and a learn
that stored "0 C" from a transient would record a temperature nothing measured.

Pinned by 8 `NtcConvert` tests whose expectations come from the **datasheet
model applied to the schematic's divider**, not from the implementation — so a
swapped divider side or a sign error fails rather than cancelling out — plus one
orchestrator test that drives a whole headless learn and checks the committed
profile's `temp_c_at_learn` is the converted value. Mutation-tested by reverting
the headless call site to the sentinel, which fails the suite.

**Still board-gated:** the conversion is validated against the datasheet's
B-constant table, not against a thermometer. Reading one room temperature and
comparing is the bring-up step that would close even that.

## The DAC fault path was three-quarters absent — and two comments asserted otherwise

Found 2026-09-24, continuing audit (N-21). Spec §6.8's I²C row promises four
things for a DAC failure: *retry with backoff*, *latch*, *release the line*,
*never drive a guessed code*. Only the last held, and two doc-comments claimed the
first two were implemented.

- **No read-back.** FR-13 step 3b and §6.1's startup sequence say "VERIFY the DAC
  is in the safe state (read back)", and `EspHal` called no `i2c_master_receive` at
  all. The firmware believed its own write.
- **No retry, no backoff.** `IHAL.h` and `EspHal.cpp` both said `dac_set_code`
  "retries with backoff internally and latches a fault on persistent failure".
  Neither was true: one synchronous `i2c_master_transmit`, one `ESP_LOGE`.
- **No `FAULT_DAC` emitter.** §7.2 gives the pattern the I²C/DAC meaning and
  nothing played it, because `dac_set_code` returns `void` so no caller could learn
  of a failure.

**Fixed.** `IHAL` gains `dac_read_code`; `EspHal` issues the MCP4728 Read Command
and `DacFrame::DecodeReadCode` (pure, host-tested) owns the byte layout — 24
sequential bytes, 3 of input register then 3 of EEPROM per channel A→D, code =
`buf[6n+2] | ((buf[6n+1] & 0x0F) << 8)`. That layout is from DS22187E Figure
5-15, cross-checked against Adafruit's driver, and pinned by a round-trip test
through the existing encoder. `DacRetry.h` holds the retry policy — 3 attempts,
1 ms then 2 ms, short on purpose because the whole sequence must fit inside the
200 ms key pulse — applied to both the code and the power-mode writes.

`SystemOrchestrator::VerifySafeIdleIdleCodes` runs the read-back per channel at
the end of `EstablishSafeIdle` and compares against the code just written.

**A mismatch releases rather than driving the read value.** The read value is
exactly what the check just declared untrustworthy, so driving it is the
"guessed code" §6.8 forbids; the firmware re-asserts the idle code, which *is*
the released state (§6.7).

**A wrong value needed a second latch.** The HAL's `dac_faulted` is set by a
failed I²C *transaction* — but a mismatch is a transaction that succeeded, so the
HAL cannot see it. `OutputVerified()` therefore folds in the orchestrator's own
`dac_verify_failed_` as well; without it FR-37's rollback gate could not see the
one failure the read-back exists to find. `ReportDacFault` plays `FAULT_DAC` once
per boot (an edge — the latch is permanent, so a per-tick report would `Play`
it every tick forever) and `Tick` checks the HAL latch, so a write that fails on
the key path is reported too, not only the boot one.

Pinned by 4 `DacFrame` read/round-trip tests, 4 `DacRetry` tests and 5
orchestrator tests, mutation-tested three ways: dropping the read-back call,
driving the read value instead of the written one, and removing
`dac_verify_failed_` from the gate — each fails the suite.

One contract drift was caught on the way: the `ack` row in `contract_schema.py`
did not declare `result`, which `ota_end`'s ack had started emitting in the
previous session's OTA work. The gate self-test (`test_gen_contract.py`) found
it; the row now declares it.

## The firmware dropped a binding's second action — and its first, if the app's came first

Found 2026-09-24, continuing audit (N-29). §3.5 is explicit that a binding's
`actions` are "executed in order, each independently failable" and names the
product's core case as one binding carrying *both* halves — "emit the factory key
press **and** tell the app". The app implements its half; the firmware did not.

`BindingResolve` returned a single `ResolvedAction` built from `b.actions[0]` only,
and `SystemOrchestrator`'s execute path tested that one action and only in its
`kOutVoltage` branch — every other kind, and every action past the first, fell to an
`else` that merely `ReleaseKey`d. Two distinct failures follow:

- `[OUT_VOLTAGE, APP_LAUNCH]` (the exact combination §3.5 says the list exists for)
  drove the key correctly but never played the bound `BUZZ`, if present.
- `[APP_INTENT, OUT_VOLTAGE]` — an app action FIRST — drove **no key at all**,
  because `actions[0]` was not `OUT_VOLTAGE` and the `else` released the line. The
  press silently did nothing on the wire the user was watching, while the app still
  fired its own half from the `event` frame.

Reachability was narrow but real: the app's editor only ever builds single-action
bindings and `ConfigDefault` ships none, so this needs a hand-authored or
`config_patch`-written config with two actions — legal, accepted by `ConfigValidate`
(`kMaxActionsPerBinding` is 2), and decoded without complaint.

**Fixed** by making the resolver carry the whole ordered list and the orchestrator
run it. `BindingResolve` now returns a `ResolvedBinding` (`found`, `action_count`,
`actions[]`), refusing the binding if *any* action is un-executable rather than
checking only the first. A new `SystemOrchestrator::RunBindingActions` walks the
list: the `OUT_` family and `BUZZ` are the firmware's column (§3.6's "Executed by"
table) and execute, while an app-owned kind is **skipped** — neither executed nor
treated as a release — which is what §3.5's "a failed app-side action must never
prevent the hardware key press" requires.

The order-dependency the open item flagged is resolved by the list's own order: the
`OUT_VOLTAGE` pulse is set on `key_released_at_ms` and the runner does **not** issue
a release, so a later action cannot cut it short; only an explicit `OUT_RELEASE` or
`NONE` releases. `BUZZ` still REPLACES the default `KEY_ACCEPTED` (one buzzer, `Play`
replaces rather than queues, §7.2), and an empty command band still suppresses the
acknowledgement and plays `KEY_UNKNOWN`.

Pinned by three orchestrator tests — `ABindingWithTwoActionsRunsBothInOrder`,
`AnAppActionFirstStillDrivesTheKeyAfterIt`, and
`AnAppActionAfterTheLevelDoesNotReleaseTheKey` — plus two resolver tests for the
ordered list and the any-position refusal. Mutation-tested twice: collapsing the
runner loop to one action and turning the app-kind skip back into a `ReleaseKey`
each fail the suite.


## Committed HEAD re-verified on the bench (2026-09-25, post-CI-fix)

After the CI-fix and doc-consolidation commits, the committed tip was re-verified
against the live DUT (`/dev/cu.usbmodem1234561`) with the two-board rig
(`/dev/cu.usbmodem1121101`). The firmware BINARY is unchanged from the earlier
bench runs (only `.github/workflows/` and docs moved since), so these re-run the
same proofs against the current tree; all pass:

- **FR-2** (`bench_cal.py`): the eFuse curve is applied on silicon
  (`calibration_degraded=False`, so the N-17 linear-fallback defect is absent);
  the ADC tracks the rig's injected 1900–2835 mV with worst error 41 mV
  (tolerance 60) and a least-squares slope of 1.024.
- **FR-16 / FR-39 / FR-40** (`bench_output.py`): a driven channel reaches its
  target and releases back to the pull-up level (the FET is off, the line is
  high-Z, not held down); a reboot leaves the line idle (`output_safe=True`) and
  safe idle is re-established before the next key.
- **FR-9 / FR-12 / FR-31 / FR-31b / FR-42** (`bench_ladder.py --only
  fr9,fr12,fr31,fr42`): ch0 classifies the rig-presented button while ch1
  correctly emits nothing; an out-of-band level yields `event{button:null}`;
  three learned buttons all class-match on replay, including a freshly taught
  one; the device serves its link with no host attached.

DUT on entry: `fw_version` `dev`, `hw_id` `SWC-S3`, `protocol_v` 1, caps
`["config","learn","ota"]`, `config_state` ok, `output_safe` true.
