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
| `CONFIG_ESP_ADC_CAL_USE_EFUSE_CALIBRATION` | **not defined** | inert. No such symbol; the real IDF 5.5.5 ADC-cal keys are `ADC_CALI_EFUSE_TP_ENABLE` / `ADC_CALI_EFUSE_VREF_ENABLE` / `ADC_CALI_LUT_ENABLE` (esp_adc Kconfig), and they are `default y` on the relevant targets. **Spec §2.3's per-chip accuracy intent is likely already satisfied by defaults, but this must be confirmed, not assumed, before Task 13's ADC work.** |
| `CONFIG_ESP_ADC_CAL_DEFAULT_ATTENUATION_12` | **not defined** | inert. No such symbol; attenuation is a runtime `adc_oneshot` argument in IDF 5.x, not a Kconfig key. The 12 dB choice belongs in the HAL. |

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
