# Bring-up log

Measurements taken on this machine, not invented values.

## Environment

- PlatformIO Core 6.2.0 (`/opt/homebrew/bin/pio`), Python 3.14.7.
- Platform: pioarduino pinned tag `55.03.311` (`PLATFORM: Espressif 32 (55.3.311)`).
- Framework resolved: `framework-espidf @ 3.50505.0` = **ESP-IDF 5.5.5**.
- Toolchain: `toolchain-xtensa-esp-elf @ 14.2.0+20260121`.
- Board: `SWC Adapter (ESP32-S3, 4MB, no PSRAM)`.

## size-baseline

**BLOCKED — no baseline can be recorded yet.** The brief's verbatim
`partitions.csv` does not build (see "Partition table" below), so the spec's
1952 KB app slot was never produced.

For reference only, a **non-spec** aligned table was built to prove the rest of
the skeleton is sound:

- Flash: **215,085 bytes** used of 1,966,080 (10.9 %) — this is against a
  0x1E0000 (1920 KB) slot from a diagnostic table, **not** the spec's 0x1E8000
  (1952 KB) slot. Do not compare later budgets against this number.
- RAM: 16,728 bytes of 327,680 (5.1 %).
- `Total image size: 215,341 bytes` (`firmware.bin` = 215,488 bytes).

## Partition table (BLOCKED)

The brief's verbatim `partitions.csv` fails IDF's own partition validator:

```
Partition app1 invalid: Offset 0x208000 is not aligned to 0x10000
*** [.pio/build/esp32s3/partitions.bin] Error 2
```

Root cause: ESP-IDF's `components/partition_table/gen_esp32part.py` sets
`ALIGNMENT[APP_TYPE] = 0x10000`, so every `app` partition offset must be
64 KiB-aligned. The spec §9.2 table places `app1` at `0x208000`, and
`0x208000 % 0x10000 = 0x8000` — misaligned. `app0` at `0x20000` is aligned;
`app1` is not. §9.2's "arithmetic, checked exactly" passage checks the *sum* but
never the 64 KiB alignment rule.

This is a spec/plan defect, not a toolchain defect: substituting a 64 KiB-aligned
table (app0 `0x20000`/`0x1E0000`, app1 `0x200000`/`0x1E0000`, coredump
`0x3E0000`) builds to `SUCCESS` on the same pinned platform. Fixing it requires a
spec-level decision (it moves the OTA layout), so it was **not** changed here.

## Flash size mismatch (finding)

Every build prints:

```
Warning! Flash memory size mismatch detected. Expected 4MB, found 2MB!
```

The generated `sdkconfig.esp32s3` contains `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"`
and `CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y`, not 4 MB. `board_build.flash_size = 4MB`
in `platformio.ini` and `upload.flash_size = "4MB"` in the board JSON are
**not** reaching the generated sdkconfig. This matters because `src/main.c`
aborts at runtime when `esp_flash_get_size()` != 4 MiB. The builder's own
suggested fix is to set the key explicitly in `sdkconfig.defaults`:

```ini
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

Not applied — the brief specifies `sdkconfig.defaults` verbatim.

## sdkconfig keys (checked after configure)

- `CONFIG_MBEDTLS_HARDWARE_AES` is present and **`is not set`** (i.e. `n`) in the
  generated `sdkconfig.esp32s3` — the required mbedTLS safety knob took effect.
- No IDF 5.5 rejection/rename of any key in `sdkconfig.defaults` was observed.

## CMake configure (evidence both CMakeLists.txt were used)

- `project_description.json` → `"project_name": "code"` (from `project(code)` in
  `code/CMakeLists.txt`); `CMAKE_PROJECT_NAME:STATIC=code` in `CMakeCache.txt`.
- `code/src` is registered as an IDF component: `"src"` appears in
  `build_components` and `.../code/src` in `build_component_paths`.
- `code/src/main.c` compiles as
  `esp-idf/src/CMakeFiles/__idf_src.dir/main.c.obj` — the component that
  `code/src/CMakeLists.txt` registers. PlatformIO did **not** synthesize these.

## Toolchain note (environment, resolved)

`tool-scons` thrashed: the pinned platform requires `4.40801.0` while PlatformIO
Core requires `~4.41101.0`, and the pioarduino registry zip
(`scons-4.8.1.zip`) is metadata-only — it contains no SCons source. The platform
deleted `~/.platformio/packages/tool-scons` on every run, so SCons was absent and
the build died with `ModuleNotFoundError: No module named
'SCons.Tool.FortranCommon'`. Repaired locally by installing a real SCons source
tree at the required version; no platform pin was changed.
