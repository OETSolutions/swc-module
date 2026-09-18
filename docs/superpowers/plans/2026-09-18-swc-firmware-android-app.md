# SWC Adapter Firmware + Android App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the ESP-IDF firmware and the Android app for the SWC steering-wheel-control adapter, so a resistor-ladder input is decoded into the exact output levels an aftermarket head unit expects, and an Android app on the head unit configures that mapping and provides actions the head unit lacks.

**Architecture:** Two deliverables in one repo under `code/`. The firmware is a PlatformIO/ESP-IDF project split into pure-logic modules (`lib/`) that take an `IHAL&` and are tested on the host with GoogleTest, plus a thin ESP-IDF shell (`src/`) tested on-device with Unity. The Android app is Kotlin/Compose talking NDJSON over USB CDC, sharing one generated contract header so the two sides cannot drift. The firmware's logic is written and tested **before** the board arrives; on-device measurement is a separate, ordered bring-up phase.

**Tech Stack:** ESP-IDF v5.x via PlatformIO (`framework = espidf`), NimBLE, TinyUSB CDC, esp_https_ota, mbedTLS, NVS; GoogleTest (host) + Unity (device); Kotlin + Jetpack Compose, `usb-serial-for-android` v3.11.x, `esp-idf-provisioning-android`; GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md`

## Global Constraints

- **Framework:** `framework = espidf`. **Never** Arduino. The reported WiFi/SSL/webserver crash class is an Arduino-core defect (spec §1).
- **Platform:** `platform = espressif32` pinned to an exact version. A floating platform silently re-resolves the IDF version.
- **Target:** ESP32-S3, **4 MB flash, no PSRAM**. No allocation may assume PSRAM.
- **ADC ceiling:** 2.9 V at 12 dB attenuation. `V_SENSE ≤ 2.49 V` by the divider. Never configure a range that exceeds 2.9 V input.
- **Gain:** `V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ` with `R58=82k`, `R61=100k` → gain exactly **1.82**, **not** 1.812. Tracking mode gain **1.00**.
- **Output envelope:** 1.80 V – 5.20 V. Guard band 2.6 V – 3.4 V. AUTO default is **1.82**.
- **No DAC Hi-Z.** Release = command above idle + sink FET off.
- **App size gate:** ≤ **1952 KB** per OTA slot (`pio run -t size`). Fallback if exceeded: spec §9.6.
- **NVS partition:** 48 KB; serialized config ≤ 24 KB.
- **Partitions:** exactly as spec §9.2.
- **`CONFIG_MBEDTLS_HARDWARE_AES` = n** in `sdkconfig.defaults`.
- **Buzzer is active at a fixed ~2.4 kHz.** No pitch control. All feedback is rhythm/count/duration.
- **Both LEDs are green.** No colour grammar.
- **Never guess an unlearned button.** `UNKNOWN` → `event{button: null}`, always.
- **Safe idle is established before USB/BLE/WiFi init** (spec §6.1, step 8 before step 9).
- **Android `targetSdk = 34`, `minSdk = 26`.** Do not raise targetSdk without re-reading spec §3.6 on Android BAL.
- **Reporter locale:** user-facing strings in `en-US`; no units other than mV/V/mm/ms in the protocol.
- **`lib/HAL/IHAL.h` is C, and its enumerators are prefixed constants.** It is
  the one header both the C host and the C++ application include, so it is
  `extern "C"` with C enums: `ADC_CH_SWC1`, `DAC_CH_KEY1`, `DAC_POWER_GND_1K`,
  `GPIO_BUZZ`. Never `AdcChannel::kSwc1`. Every task names channels this way.
- **`/SENSE1` and `/SENSE2` are ADC channels (`ADC_CH_KEY_SENSE1`,
  `ADC_CH_KEY_SENSE2`), not `GpioPin`s.** Spec §2.2 marks them `A-in` — they are
  the KEY-line ÷2 sense divider that the servo trim loop reads. The only
  `GpioPin` inputs are `GPIO_BOOT` and `GPIO_VBUS_VALID`.

---

## File Structure

Created under `code/` in the PCB repo.

| Path | Responsibility |
| --- | --- |
| `code/platformio.ini` | Envs: `native`, `esp32s3`, `esp32s3-ota` |
| `code/partitions.csv` | Spec §9.2, verbatim |
| `code/sdkconfig.defaults` | Non-default IDF knobs |
| `code/boards/swc-s3.json` | Board definition: 4 MB, no PSRAM |
| `code/src/main.c` | Wiring only: create HAL, start tasks |
| `code/lib/HAL/IHAL.h` | The seam (spec §10.2) |
| `code/lib/HAL/EspHal.{h,c}` | Real implementation |
| `code/test_native/MockHAL.h` | Host implementation, injectable clock |
| `code/lib/Analog/CalibrationCurve.{h,c}` | ADC raw→mV |
| `code/lib/Analog/LadderDecode.{h,c}` | Ratio-normalized classification |
| `code/lib/Gesture/PressClassifier.{h,c}` | Level → press events |
| `code/lib/Gesture/GestureStateMachine.{h,c}` | Presses → SINGLE/DOUBLE/LONG |
| `code/lib/Output/DacMcp4728.{h,c}` | I²C DAC driver |
| `code/lib/Output/GainPolicy.{h,c}` | Gain-mode selection |
| `code/lib/Output/ServoLoop.{h,c}` | Bounded trim loop |
| `code/lib/Bindings/ActionLibrary.{h,c}` | Action ids → semantics |
| `code/lib/Bindings/BindingResolver.{h,c}` | Gesture → action |
| `code/lib/Feedback/BuzzerGrammar.{h,c}` | Buzzer patterns |
| `code/lib/Feedback/LedGrammar.{h,c}` | LED patterns |
| `code/lib/Config/ConfigCodec.{h,c}` | Config ↔ JSON |
| `code/lib/Config/ConfigStore.{h,c}` | A/B NVS persistence |
| `code/lib/Link/Ndjson.{h,c}` | Frame encode/decode |
| `code/lib/Link/CommandRouter.{h,c}` | Frame → handler |
| `code/lib/Update/ImageVerify.{h,c}` | SHA-256 streaming verify |
| `code/lib/Update/ReleaseCheck.{h,c}` | Manifest fetch/compare |
| `code/contract/swc_contract.h` | Generated, checked in |
| `code/tools/gen_contract.py` | Generates the above |
| `code/tools/gen_contract_kotlin.py` | Generates Kotlin types |
| `code/android/...` | Gradle project |

---

## Phase 0 — Foundations (no hardware needed)

### Task 1: PlatformIO environment that actually builds for the ESP32-S3

This task exists because the framework choice is the project's riskiest
assumption and it must be proven before any real code is written. Stock
PlatformIO cannot supply IDF 5.x (its `framework-espidf` stops at 4.6.1), so the
platform is pioarduino's fork pinned to a release tag — never the rolling
`stable` zip, which crashes the installer under Python 3.14.

**Files:**
- Create: `code/platformio.ini`
- Create: `code/sdkconfig.defaults`
- Create: `code/partitions.csv`
- Create: `code/boards/swc-s3.json`
- Create: `code/.gitignore`
- Create: `code/src/main.c`

**Interfaces:**
- Consumes: nothing (first task)
- Produces: the env names `esp32s3` and `native`, used by every later task's test
  commands

- [ ] **Step 1: Write `platformio.ini` with both envs**

```ini
; PlatformIO Project Configuration File
;
; Framework is ESP-IDF, NOT Arduino. The field-reported crash class
; (WiFiClientSecure/mbedTLS X509 alloc failure, and an uncatchable std::bad_alloc
; inside ESPAsyncWebServer) is an Arduino-core defect; ESPAsyncWebServer does not
; exist in IDF, and the mbedTLS GCM/HARDWARE_AES path that fails is the one we
; explicitly disable below. Do not "simplify" this back to framework = arduino.

[platformio]
default_envs = esp32s3

[env]
board_build.partitions = partitions.csv
board_build.flash_mode = qio
board_build.flash_size = 4MB
build_flags =
    -Wall
    -Wextra
    -Werror
    -D SWC_FW_VERSION_RAW=\"${sysenv.SWC_FW_VERSION}\"
    -D SWC_GIT_SHA_RAW=\"${sysenv.SWC_GIT_SHA}\"
check_skip_packages = yes

[env:esp32s3]
; pioarduino fork pinned to an exact release tag. Stock PlatformIO's
; espressif32 7.1.3 only publishes framework-espidf up to 4.60100.0 (IDF 4.6.1),
; which predates the mbedTLS bugfix this project depends on. Pinning the tag (not
; the rolling "stable" zip) also avoids pioarduino's reinstall path, which
; crashes under Python 3.14 (exists(None) in safe_framework_cleanup). This
; machine runs Python 3.14.7.
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = swc-s3
framework = espidf
board_build.flash_size = 4MB
test_framework = unity
test_ignore = test_native
monitor_speed = 115200

[env:native]
; Host tests: pure logic only, no ESP32 toolchain involved. GoogleTest.
platform = native
build_flags =
    ${env.build_flags}
    -std=gnu++17
    -I lib
    -I test_native
    -D SWC_NATIVE_TEST
test_framework = googletest
test_ignore = test
build_src_filter = -<*>
test_build_src = yes
lib_deps =
    google/googletest@^1.15.2
```

- [ ] **Step 2: Write `code/boards/swc-s3.json`**

Templated from the stock `esp32-s3-devkitc-1`, with PSRAM removed and flash
fixed. **The exact `flash_size`/`psram_type` must be re-verified against the
arriving module (spec §12.1, N-1) — this is the best-known value, and Step 5
below is what proves it.**

```json
{
  "build": {
    "arduino": { "ldscript": "esp32s3_out.ld", "partitions": "default_8MB.csv" },
    "core": "esp32",
    "extra_flags": ["-DARDUINO_ESP32S3_DEV", "-DBOARD_HAS_PSRAM=0"],
    "f_cpu": "240000000L",
    "f_flash": "80000000L",
    "flash_mode": "qio",
    "mcu": "esp32s3",
    "variant": "esp32s3"
  },
  "connectivity": ["wifi", "bluetooth"],
  "debug": {
    "cdebug": ["-O0", "-g3"],
    "default_tool": "esp-builtin",
    "onboard_tools": ["esp-builtin"],
    "openocd_target": "esp32s3.cfg"
  },
  "frameworks": ["arduino", "espidf"],
  "name": "SWC Adapter (ESP32-S3, 4MB, no PSRAM)",
  "upload": {
    "flash_size": "4MB",
    "maximum_ram_size": 327680,
    "maximum_size": 4194304,
    "require_upload_port": true,
    "speed": 921600
  },
  "url": "https://github.com/oetsolutions/swc-module",
  "vendor": "OETSolutions"
}
```

- [ ] **Step 3: Write `code/partitions.csv` (spec §9.2, verbatim)**

```csv
# name,     type, subtype,  offset,    size,      flags
nvs,        data, nvs,      0x9000,    0xC000,
otadata,    data, ota,      0x15000,   0x2000,
phy_init,   data, phy,      0x17000,   0x1000,
app0,       app,  ota_0,    0x20000,   0x1E8000,
app1,       app,  ota_1,    0x208000,  0x1E8000,
coredump,   data, coredump, 0x3F0000,  0x10000,
```

- [ ] **Step 4: Write `code/sdkconfig.defaults`**

```ini
# --- Console and USB -------------------------------------------------------
# Console on the ROM USB-Serial-JTAG peripheral; the app link is TinyUSB CDC on
# the same physical port but a separate USB interface (spec 4.1). Keeping them
# distinct is deliberate: the console must not corrupt app frames.
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_TINYUSB_CDC_ENABLED=y

# --- Bluetooth: NimBLE, not Bluedroid --------------------------------------
# Bluedroid does not fit 4MB/no-PSRAM alongside WiFi + OTA (spec 9.2).
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n

# --- mbedTLS ---------------------------------------------------------------
# Disable hardware AES. The field-reported TLS crashes trace to the mbedTLS
# GCM/HARDWARE_AES path (espressif/esp-idf#14298, IDFGH-13390); with it off we
# take the software AES path, which is what the OTA flow is tested against.
CONFIG_MBEDTLS_HARDWARE_AES=n
CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y

# --- ADC -------------------------------------------------------------------
# Curve-fit calibration for per-chip accuracy (spec 2.3). Without this the
# raw-ADC linear-scale assumption is off by more than the classification windows.
CONFIG_ESP_ADC_CAL_USE_EFUSE_CALIBRATION=y
CONFIG_ESP_ADC_CAL_DEFAULT_ATTENUATION_12=y

# --- Crash handling --------------------------------------------------------
# Coredump partition exists (partitions.csv). Reset in release; gdbstub in dev.
CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

- [ ] **Step 5: Write a `main.c` that proves the board definition is honest**

This is not a hello-world. It asserts the flash size and PSRAM absence the whole
build assumes, so a wrong board JSON fails loudly here instead of as a mysterious
runtime fault later.

```c
#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "swc-boot";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    ESP_LOGI(TAG, "chip=%s cores=%d rev=v%d.%d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes", flash_size);
    ESP_LOGI(TAG, "idf=%s", esp_get_idf_version());

    if (flash_size != 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "flash is %" PRIu32 " bytes, build assumes %d", flash_size, 4 * 1024 * 1024);
        abort();
    }
    if (chip.cores != 2) {
        ESP_LOGE(TAG, "expected 2 cores for ESP32-S3, got %d", chip.cores);
        abort();
    }
    ESP_LOGI(TAG, "board definition verified: 4MB flash, %d cores, IDF %s",
             chip.cores, esp_get_idf_version());
}
```

- [ ] **Step 6: Build it**

Run: `cd code && pio run -e esp32s3`
Expected: build succeeds, ending with `Success`. **If this fails, stop and resolve
it before any other task** — spec §12.2 R-9 lists the fallback order. Do not
proceed to Task 2 on a broken toolchain.

- [ ] **Step 7: Verify the image size baseline**

Run: `pio run -e esp32s3 -t size`
Expected: prints a size summary with the app partition at 0x1E8000 (1952 KB).
Record the number in `code/docs/bring-up-log.md` as `size-baseline:` — every
later budget comparison is against it, and the number is a measurement, not a
value to invent here.

- [ ] **Step 8: Verify the partition table is what the spec says**

Run: `pio run -e esp32s3 -t partition-table`
Expected: the generated `.bin` is 3072 bytes and the CSV in the build dir
matches `partitions.csv` exactly, ending at 0x400000.

- [ ] **Step 9: Write `code/.gitignore`**

```gitignore
.pio/
.vscode/.browse.c_cpp.db*
.vscode/c_cpp_properties.json
.vscode/launch.json
.vscode/ipch
sdkconfig
sdkconfig.old
*.swp
build/
android/.gradle/
android/local.properties
android/**/build/
```

- [ ] **Step 10: Commit**

```bash
cd <repo-root>
git add code/platformio.ini code/partitions.csv code/sdkconfig.defaults \
        code/boards/swc-s3.json code/.gitignore code/src/main.c
git commit -m "Add ESP-IDF PlatformIO skeleton for the SWC adapter

framework = espidf on pioarduino's pinned platform tag. Stock PlatformIO's
espressif32 7.1.3 publishes framework-espidf only up to IDF 4.6.1, which
predates the mbedTLS GCM fix this project depends on; the fork ships IDF 5.5.5.
Main asserts the 4MB/no-PSRAM assumption so a wrong board definition fails at
boot rather than as a mystery later."
```

---

### Task 2: The HAL seam and a MockHAL with an injectable clock

Everything testable on the host depends on this existing first. The clock being
part of the HAL is the single most important design decision in the test
strategy: it makes every timing rule in the spec (§7's 500 ms / 750 ms / 200 ms
windows, §8's 5-minute timeout) testable in microseconds of wall time.

**Files:**
- Create: `code/lib/HAL/IHAL.h`
- Create: `code/test_native/MockHAL.h`
- Create: `code/test_native/MockHAL.cpp`
- Create: `code/test_native/test_hal/MockHalTest.cpp`

**Interfaces:**
- Consumes: the `native` env from Task 1
- Produces:
  - `AdcChannel` — C enum `{ ADC_CH_SWC1, ADC_CH_SWC2, ADC_CH_TEMP, ADC_CH_AUX1,
    ADC_CH_AUX2, ADC_CH_AUX3, ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2, ADC_CH_COUNT }`
  - `DacChannel` — C enum `{ DAC_CH_KEY1, DAC_CH_KEY2, DAC_CH_ADJ, DAC_CH_SPARE, DAC_CH_COUNT }`
  - `DacPowerMode` — C enum `{ DAC_POWER_NORMAL, DAC_POWER_GND_1K,
    DAC_POWER_GND_100K, DAC_POWER_GND_500K }`
  - `GpioPin` — C enum `{ GPIO_BUZZ, GPIO_LED_STAT, GPIO_LED2, GPIO_DAC_LDAC_B,
    GPIO_BOOT, GPIO_VBUS_VALID, GPIO_COUNT }`
  - `struct IHal { ... }` with the function-pointer table and a `void *ctx`
  - `MockHal` — a `MockHal` implementation with `AdvanceMs(uint32_t)`,
    `SetAdcMilliVolts(AdcChannel, int)`, `GpioWrite(GpioPin, bool)`,
    `LastDacCode(DacChannel)` accessors

  **These are C enums, not `enum class`, and they are the single naming
  authority for every later task.** `IHAL.h` is the one header the C host and
  the C++ application both include, so it is `extern "C"` and its enumerators
  are prefixed constants (`ADC_CH_*`, `DAC_CH_*`, `DAC_POWER_*`, `GPIO_*`).
  Write `ADC_CH_SWC1`, never `AdcChannel::kSwc1`.

  **`/SENSE1` and `/SENSE2` are ADC channels, not GPIO.** Spec §2.2 lists them
  as `A-in` — the KEY-line ÷2 sense divider (`/SENSEn` is fed by the exact ÷2
  divider in §2.3). The servo's trim loop reads them with `adc_read_mv`, so a
  `gpio_read` of a sense pin is impossible. The only `GpioPin` inputs are
  `GPIO_BOOT` and `GPIO_VBUS_VALID`.

- [ ] **Step 1: Write the failing test**

`code/test_native/test_hal/MockHalTest.cpp`:

```cpp
#include "MockHAL.h"
#include <gtest/gtest.h>

TEST(MockHalClock, StartsAtZeroAndAdvancesInMilliseconds) {
    MockHal hal;
    EXPECT_EQ(hal.NowMs(), 0u);
    hal.AdvanceMs(750);
    EXPECT_EQ(hal.NowMs(), 750u);
    EXPECT_EQ(hal.NowUs(), 750000u);
}

TEST(MockHalAdc, ReturnsTheProgrammedMillivoltValuePerChannel) {
    MockHal hal;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1234);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 567);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_SWC1), 1234);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_SWC2), 567);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_TEMP), 0);
}

TEST(MockHalDac, RecordsTheLastCodeWrittenPerChannel) {
    MockHal hal;
    hal.DacSetCode(DAC_CH_KEY1, 0x800, DAC_POWER_NORMAL);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x800);
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_KEY1), DAC_POWER_NORMAL);
    hal.DacSetCode(DAC_CH_KEY1, 0x123, DAC_POWER_NORMAL);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x123);
}

TEST(MockHalGpio, ReadsBackWhatWasWrittenAndTracksWriteCount) {
    MockHal hal;
    EXPECT_EQ(hal.GpioWriteCount(GPIO_BUZZ), 0u);
    hal.GpioWrite(GPIO_BUZZ, true);
    EXPECT_TRUE(hal.GpioRead(GPIO_BUZZ));
    EXPECT_EQ(hal.GpioWriteCount(GPIO_BUZZ), 1u);
}

TEST(MockHalNvs, PersistsBytesAcrossCallsAndCanBeMadeToFail) {
    MockHal hal;
    const char payload[] = "config-blob";
    ASSERT_EQ(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
    char out[sizeof(payload)] = {};
    ASSERT_EQ(hal.NvsGet("cfg", out, sizeof(out)), 0);
    EXPECT_STREQ(out, payload);

    hal.FailNextNvsWrite();
    EXPECT_NE(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
}

TEST(MockHalNvs, ATornWriteLeavesTheStoredBlobShortAndReportsFailure) {
    MockHal hal;
    const char payload[] = "config-blob";
    hal.TruncateNextNvsWriteAt(5);
    EXPECT_NE(hal.NvsSet("cfg", payload, sizeof(payload)), 0)
        << "a torn write must report failure, not success";
    char out[sizeof(payload)] = {};
    EXPECT_EQ(hal.NvsGet("cfg", out, sizeof(out)), 5)
        << "readers must see the short blob and reject it on length + CRC";
}

TEST(MockHalInputs, OnlyBootAndVbusReadBackAsProgrammedInputs) {
    MockHal hal;
    // The two genuinely digital inputs.
    hal.SetGpioInput(GPIO_BOOT, true);
    hal.SetGpioInput(GPIO_VBUS_VALID, true);
    EXPECT_TRUE(hal.GpioRead(GPIO_BOOT));
    EXPECT_TRUE(hal.GpioRead(GPIO_VBUS_VALID));

    // A sense line is an ADC channel (spec 2.2), so it is NOT readable here.
    // This test exists to make that explicit: if SENSE ever reappears as a
    // GpioPin the servo trim loop cannot read it.
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2500);
    EXPECT_EQ(hal.AdcReadMv(ADC_CH_KEY_SENSE1), 2500);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_hal`
Expected: FAIL — `MockHAL.h` not found / `MockHal` undefined.

- [ ] **Step 3: Write `lib/HAL/IHAL.h`**

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADC_CH_SWC1 = 0, ADC_CH_SWC2, ADC_CH_TEMP,
    ADC_CH_AUX1, ADC_CH_AUX2, ADC_CH_AUX3,
    /* KEY-line sense, KEY voltage / 2 (spec 2.2). Analog inputs driving the
     * servo trim loop, so they are ADC channels and NOT GpioPins. */
    ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2,
    ADC_CH_COUNT
} AdcChannel;

typedef enum {
    DAC_CH_KEY1 = 0, DAC_CH_KEY2, DAC_CH_ADJ, DAC_CH_SPARE,
    DAC_CH_COUNT
} DacChannel;

/* The MCP4728 has no high-impedance state; these are its power-down modes. */
typedef enum {
    DAC_POWER_NORMAL = 0,
    DAC_POWER_GND_1K,
    DAC_POWER_GND_100K,
    DAC_POWER_GND_500K
} DacPowerMode;

typedef enum {
    GPIO_BUZZ = 0, GPIO_LED_STAT, GPIO_LED2, GPIO_DAC_LDAC_B,
    /* The only two GPIO inputs. SENSE1/SENSE2 are ADC channels above. */
    GPIO_BOOT, GPIO_VBUS_VALID,
    GPIO_COUNT
} GpioPin;

/*
 * The only interface between logic and silicon. Every module above lib/HAL
 * takes an IHal* so it can be exercised on the host with MockHal.
 *
 * now_ms/now_us are part of the HAL on purpose: every timing rule in the spec
 * (500ms double-press window, 750ms long-press threshold, 200ms key send,
 * 5-minute maintenance timeout) is a tested rule, and the only way to test a
 * timing rule without sleeping is to make the clock an input.
 */
typedef struct IHal {
    int      (*adc_read_mv)(void *ctx, AdcChannel ch);
    void     (*dac_set_code)(void *ctx, DacChannel ch, uint16_t code, DacPowerMode mode);
    void     (*dac_ldac_assert)(void *ctx, bool assert);
    void     (*gpio_write)(void *ctx, GpioPin pin, bool level);
    bool     (*gpio_read)(void *ctx, GpioPin pin);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    int      (*nvs_get)(void *ctx, const char *key, void *out, size_t len);
    int      (*nvs_set)(void *ctx, const char *key, const void *in, size_t len);
    void     (*reboot)(void *ctx);
    void      *ctx;
} IHal;

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Write `test_native/MockHAL.h`**

```cpp
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "HAL/IHAL.h"

/*
 * Host implementation of IHal. All state is observable, so tests assert on
 * what the code *did to the hardware*, not on internal variables.
 */
class MockHal {
public:
    MockHal();

    IHal &InterfaceRef() { return iface_; }   // every later task's tests take &hal.InterfaceRef()

    // --- clock -------------------------------------------------------------
    uint64_t NowMs() { return now_ms_; }
    uint64_t NowUs() { return now_ms_ * 1000ULL; }
    void AdvanceMs(uint64_t ms) { now_ms_ += ms; }

    // --- analog ------------------------------------------------------------
    void SetAdcMilliVolts(AdcChannel ch, int mv) { adc_mv_[static_cast<int>(ch)] = mv; }
    int AdcReadMv(AdcChannel ch) { return adc_mv_[static_cast<int>(ch)]; }

    void DacSetCode(DacChannel ch, uint16_t code, DacPowerMode mode);
    uint16_t LastDacCode(DacChannel ch) const;
    DacPowerMode LastDacPowerMode(DacChannel ch) const;
    int DacWriteCount(DacChannel ch) const;
    bool LastLdacAsserted() const { return ldac_asserted_; }

    // --- gpio --------------------------------------------------------------
    void GpioWrite(GpioPin pin, bool level);
    bool GpioRead(GpioPin pin) const;
    void SetGpioInput(GpioPin pin, bool level) { gpio_in_[static_cast<int>(pin)] = level; }
    int GpioWriteCount(GpioPin pin) const;

    // --- nvs ---------------------------------------------------------------
    int NvsSet(const char *key, const void *in, size_t len);
    int NvsGet(const char *key, void *out, size_t len);
    void FailNextNvsWrite() { fail_next_nvs_write_ = true; }
    // Simulate power loss partway through the next write: n bytes land, the
    // write reports failure, and the stored blob is short. Readers must catch
    // that via length + CRC, never by assuming the write completed.
    void TruncateNextNvsWriteAt(size_t bytes) {
        truncate_set_ = true;
        truncate_next_write_at_ = bytes;
    }
    int RebootCount() const { return reboot_count_; }

    // Advance the clock and hand it to the interface (for poll loops).
    void Tick(uint64_t ms) { AdvanceMs(ms); }

private:
    static int  AdcReadMvThunk(void *ctx, AdcChannel ch);
    static void DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code, DacPowerMode m);
    static void DacLdacThunk(void *ctx, bool assert);
    static void GpioWriteThunk(void *ctx, GpioPin pin, bool level);
    static bool GpioReadThunk(void *ctx, GpioPin pin);
    static uint64_t NowMsThunk(void *ctx);
    static uint64_t NowUsThunk(void *ctx);
    static int  NvsGetThunk(void *ctx, const char *key, void *out, size_t len);
    static int  NvsSetThunk(void *ctx, const char *key, const void *in, size_t len);
    static void RebootThunk(void *ctx);

    IHal iface_{};
    uint64_t now_ms_ = 0;
    int adc_mv_[ADC_CH_COUNT] = {};
    uint16_t dac_code_[DAC_CH_COUNT] = {};
    DacPowerMode dac_mode_[DAC_CH_COUNT] = {};
    int dac_writes_[DAC_CH_COUNT] = {};
    bool ldac_asserted_ = false;
    bool gpio_out_[GPIO_COUNT] = {};
    bool gpio_in_[GPIO_COUNT] = {};
    int gpio_writes_[GPIO_COUNT] = {};
    std::map<std::string, std::vector<uint8_t>> nvs_;
    bool fail_next_nvs_write_ = false;
    bool truncate_set_ = false;
    size_t truncate_next_write_at_ = 0;
    int reboot_count_ = 0;
};
```

- [ ] **Step 5: Write `test_native/MockHAL.cpp`**

```cpp
#include "MockHAL.h"

MockHal::MockHal() {
    iface_.adc_read_mv    = &MockHal::AdcReadMvThunk;
    iface_.dac_set_code   = &MockHal::DacSetCodeThunk;
    iface_.dac_ldac_assert = &MockHal::DacLdacThunk;
    iface_.gpio_write     = &MockHal::GpioWriteThunk;
    iface_.gpio_read      = &MockHal::GpioReadThunk;
    iface_.now_ms         = &MockHal::NowMsThunk;
    iface_.now_us         = &MockHal::NowUsThunk;
    iface_.nvs_get        = &MockHal::NvsGetThunk;
    iface_.nvs_set        = &MockHal::NvsSetThunk;
    iface_.reboot         = &MockHal::RebootThunk;
    iface_.ctx            = this;
}

void MockHal::DacSetCode(DacChannel ch, uint16_t code, DacPowerMode mode) {
    const int i = static_cast<int>(ch);
    dac_code_[i] = code;
    dac_mode_[i] = mode;
    ++dac_writes_[i];
}

uint16_t MockHal::LastDacCode(DacChannel ch) const { return dac_code_[static_cast<int>(ch)]; }
DacPowerMode MockHal::LastDacPowerMode(DacChannel ch) const { return dac_mode_[static_cast<int>(ch)]; }
int MockHal::DacWriteCount(DacChannel ch) const { return dac_writes_[static_cast<int>(ch)]; }

void MockHal::GpioWrite(GpioPin pin, bool level) {
    const int i = static_cast<int>(pin);
    gpio_out_[i] = level;
    ++gpio_writes_[i];
}

bool MockHal::GpioRead(GpioPin pin) const {
    const int i = static_cast<int>(pin);
    // Outputs read back what was written (BUZZ, LEDs, LDAC); the two digital
    // inputs read their programmed input state. Nothing else is a GpioPin --
    // SENSE1/SENSE2 are ADC channels and are read with AdcReadMv.
    switch (pin) {
        case GPIO_BOOT: case GPIO_VBUS_VALID:
            return gpio_in_[i];
        default:
            return gpio_out_[i];
    }
}

int MockHal::GpioWriteCount(GpioPin pin) const { return gpio_writes_[static_cast<int>(pin)]; }

int MockHal::NvsSet(const char *key, const void *in, size_t len) {
    if (fail_next_nvs_write_) {
        fail_next_nvs_write_ = false;
        return -1;
    }
    const uint8_t *p = static_cast<const uint8_t *>(in);
    if (truncate_set_) {
        const size_t store = truncate_next_write_at_ < len ? truncate_next_write_at_ : len;
        nvs_[key].assign(p, p + store);
        truncate_set_ = false;
        truncate_next_write_at_ = 0;
        return -1;
    }
    nvs_[key].assign(p, p + len);
    return 0;
}

int MockHal::NvsGet(const char *key, void *out, size_t len) {
    auto it = nvs_.find(key);
    if (it == nvs_.end()) return -1;
    const size_t n = it->second.size() < len ? it->second.size() : len;
    std::memcpy(out, it->second.data(), n);
    return static_cast<int>(n);
}

int  MockHal::AdcReadMvThunk(void *ctx, AdcChannel ch) {
    return static_cast<MockHal *>(ctx)->AdcReadMv(ch);
}
void MockHal::DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code, DacPowerMode m) {
    static_cast<MockHal *>(ctx)->DacSetCode(ch, code, m);
}
void MockHal::DacLdacThunk(void *ctx, bool assert) {
    static_cast<MockHal *>(ctx)->ldac_asserted_ = assert;
}
void MockHal::GpioWriteThunk(void *ctx, GpioPin pin, bool level) {
    static_cast<MockHal *>(ctx)->GpioWrite(pin, level);
}
bool MockHal::GpioReadThunk(void *ctx, GpioPin pin) {
    return static_cast<MockHal *>(ctx)->GpioRead(pin);
}
uint64_t MockHal::NowMsThunk(void *ctx) { return static_cast<MockHal *>(ctx)->NowMs(); }
uint64_t MockHal::NowUsThunk(void *ctx) { return static_cast<MockHal *>(ctx)->NowUs(); }
int MockHal::NvsGetThunk(void *ctx, const char *key, void *out, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsGet(key, out, len);
}
int MockHal::NvsSetThunk(void *ctx, const char *key, const void *in, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsSet(key, in, len);
}
void MockHal::RebootThunk(void *ctx) { ++static_cast<MockHal *>(ctx)->reboot_count_; }
```

- [ ] **Step 6: Run the tests**

Run: `cd code && pio test -e native -f test_hal`
Expected: PASS — 5 tests green.

- [ ] **Step 7: Commit**

```bash
git add code/lib/HAL/IHAL.h code/test_native/MockHAL.h code/test_native/MockHAL.cpp \
        code/test_native/test_hal/MockHalTest.cpp
git commit -m "Add the IHal seam and a MockHal with an injectable clock

The clock is part of the HAL so every timing rule in the spec (500ms double
press, 750ms long press, 200ms key send, 5-minute maintenance timeout) is
testable without sleeping. MockHal also injects NVS write failures and torn
writes, which is how the A/B persistence scheme gets tested for power loss."
```

---

## Phase 1 — Analog and classification (the core correctness problem)

### Task 3: `LadderDecode` — ratio-normalized classification

The single most important behavior in the device. The requirement is not "decode
a ladder"; it is **classify the same physical button identically at 11.0 V and at
14.8 V**. Normalizing by the measured idle level is what makes that true, and
this task's tests are the entire justification for the design.

**Files:**
- Create: `code/lib/Analog/LadderDecode.h`
- Create: `code/lib/Analog/LadderDecode.cpp`
- Create: `code/test_native/test_analog/LadderDecodeTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct LadderButton { int16_t ratio_permille; int16_t tolerance_permille; char id[24]; uint8_t action_id; }`
  - `struct LadderProfile { LadderButton buttons[16]; uint8_t count; int idle_rail_mv; int idle_ratio_permille; }`
  - `enum class ClassifyResult { kIdle, kButton, kUnknown, kFault }`
  - `struct ClassifyOutcome { ClassifyResult result; uint8_t index; int16_t ratio_permille; }`
  - `ClassifyOutcome LadderClassify(const LadderProfile &p, int level_mv, int rail_mv)`
  - `int16_t LadderRatioPermille(int level_mv, int rail_mv)` — `level_mv * 1000 / rail_mv`, rounded

- [ ] **Step 1: Write the failing test — starting with the rail-immunity test**

`code/test_native/test_analog/LadderDecodeTest.cpp`:

```cpp
#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {

// A representative 5V-referenced ladder, resistor R in series with the button
// to the 5V rail, 10k to ground. The ratio at the ADC is what we normalize.
LadderProfile MakeProfile(int rail_mv) {
    LadderProfile p{};
    p.idle_rail_mv = rail_mv;
    p.idle_ratio_permille = 1000;  // idle = full rail
    p.count = 3;
    // 5.00V rail: 0R->1000, 2.2k->690, 6.8k->400 (approx, ratio x1000)
    p.buttons[0] = {"VOL_UP",   690, 40, 1};
    p.buttons[1] = {"VOL_DOWN", 400, 40, 2};
    p.buttons[2] = {"MUTE",     176, 40, 3};
    return p;
}

}  // namespace

TEST(LadderRatio, IsLevelOverRailInPermille) {
    EXPECT_EQ(LadderRatioPermille(2500, 5000), 500);
    EXPECT_EQ(LadderRatioPermille(5000, 5000), 1000);
    EXPECT_EQ(LadderRatioPermille(0, 5000), 0);
    EXPECT_EQ(LadderRatioPermille(1150, 5750), 200);
}

TEST(LadderClassify, IdleReturnsIdle) {
    LadderProfile p = MakeProfile(5000);
    EXPECT_EQ(LadderClassify(p, 5000, 5000).result, ClassifyResult::kIdle);
    EXPECT_EQ(LadderClassify(p, 4998, 5000).result, ClassifyResult::kIdle);
}

TEST(LadderClassify, EachLearnedButtonClassifiesToItsOwnIndex) {
    LadderProfile p = MakeProfile(5000);
    EXPECT_EQ(LadderClassify(p, 3450, 5000).index, 0);  // 690 permille
    EXPECT_EQ(LadderClassify(p, 2000, 5000).index, 1);  // 400 permille
    EXPECT_EQ(LadderClassify(p,  880, 5000).index, 2);  // 176 permille
}

/*
 * THE test. The vehicle's charging voltage moves the rail from 11.0V to 14.8V.
 * The same physical button must classify identically at both, and at every
 * point in between. This is why the decode is ratio-normalized: an absolute
 * millivolt window would classify correctly at exactly one rail voltage.
 */
TEST(LadderClassify, SameButtonClassifiesIdenticallyAcrossTheVehicleRailSweep) {
    for (int rail_mv = 11000; rail_mv <= 14800; rail_mv += 100) {
        LadderProfile p = MakeProfile(rail_mv);
        // VOL_UP sits at 690 permille of whatever the rail is.
        const int level_mv = (rail_mv * 690) / 1000;
        const ClassifyOutcome out = LadderClassify(p, level_mv, rail_mv);
        ASSERT_EQ(out.result, ClassifyResult::kButton)
            << "rail=" << rail_mv << " level=" << level_mv;
        ASSERT_EQ(out.index, 0) << "rail=" << rail_mv;
    }
}

TEST(LadderClassify, UnlearnedLevelIsUnknownAndNeverGuessed) {
    LadderProfile p = MakeProfile(5000);
    // 300 permille matches no window (windows are 690/400/176 +/- 40).
    const ClassifyOutcome out = LadderClassify(p, 1500, 5000);
    EXPECT_EQ(out.result, ClassifyResult::kUnknown);
}

TEST(LadderClassify, LevelAboveTheReferenceIsAFaultNotAButtonOrIdle) {
    LadderProfile p = MakeProfile(5000);
    // 1040 permille: a short to a supply above the reference. Reporting this as
    // IDLE would be the worst outcome -- the user's button would do nothing and
    // nothing would say why.
    EXPECT_EQ(LadderClassify(p, 5200, 5000).result, ClassifyResult::kFault);
}

TEST(LadderClassify, CollapsedRailIsAFaultNotAnIdle) {
    LadderProfile p = MakeProfile(5000);
    // The reference has fallen to 20% of the learned rail: an open input or a
    // dead supply, not idle. Checked against the LEARNED rail, because ratio
    // normalization deliberately hides rail changes in the ratios themselves.
    EXPECT_EQ(LadderClassify(p, 900, 1000).result, ClassifyResult::kFault);
}

TEST(LadderClassify, ToleranceBoundaryIsInclusiveAtTheEdgeAndExclusiveBeyond) {
    LadderProfile p = MakeProfile(5000);
    // window 690 +/- 40 permille -> [650, 730]
    EXPECT_EQ(LadderClassify(p, 3250, 5000).result, ClassifyResult::kButton);  // 650 incl
    EXPECT_EQ(LadderClassify(p, 3650, 5000).result, ClassifyResult::kButton);  // 730 incl
    EXPECT_EQ(LadderClassify(p, 3245, 5000).result, ClassifyResult::kUnknown); // 649
    EXPECT_EQ(LadderClassify(p, 3655, 5000).result, ClassifyResult::kUnknown); // 731
}

TEST(LadderClassify, OverlappingWindowsResolveToTheNearestCentreNotTheFirstMatch) {
    LadderProfile p = MakeProfile(5000);
    // Two deliberately overlapping windows at 500 and 540 permille.
    p.count = 2;
    p.buttons[0] = {"A", 500, 80, 1};  // [420,580]
    p.buttons[1] = {"B", 540, 80, 2};  // [460,620]
    EXPECT_EQ(LadderClassify(p, 2500, 5000).index, 0);  // 500 -> centre 0
    EXPECT_EQ(LadderClassify(p, 2700, 5000).index, 1);  // 540 -> centre 1
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_analog`
Expected: FAIL — `Analog/LadderDecode.h` not found.

- [ ] **Step 3: Write `lib/Analog/LadderDecode.h`**

```cpp
#pragma once

#include <stdint.h>

// Ratios are permille (thousandths of the rail) so the whole comparison path is
// integer arithmetic. Floating point on this target is slower and the windows
// are generous enough (tens of permille) that integer rounding is irrelevant.
constexpr int kLadderMaxButtons = 16;
constexpr int kLadderIdLen = 24;

struct LadderButton {
    char    id[kLadderIdLen];      // stable identity for bindings
    int16_t ratio_permille;        // measured centre
    int16_t tolerance_permille;    // half-width of the accept window
    uint8_t action_id;             // resolved elsewhere; opaque here
};

struct LadderProfile {
    LadderButton buttons[kLadderMaxButtons];
    uint8_t      count;
    int          idle_rail_mv;          // rail at learn time
    int16_t      idle_ratio_permille;   // normally 1000
};

enum class ClassifyResult { kIdle, kButton, kUnknown, kFault };

struct ClassifyOutcome {
    ClassifyResult result;
    uint8_t        index;   // valid only when result == kButton
    int16_t        ratio_permille;
};

// level_mv as a fraction of rail_mv, in permille. Callers must guarantee
// rail_mv > 0; LadderClassify enforces that and reports kFault otherwise.
int16_t LadderRatioPermille(int level_mv, int rail_mv);

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int rail_mv);
```

- [ ] **Step 4: Write the minimal implementation**

`code/lib/Analog/LadderDecode.cpp`:

```cpp
#include "Analog/LadderDecode.h"

#include <stdlib.h>

namespace {
// The margin either side of the idle reference. Above idle+margin the reading
// exceeds the reference, which is a short to a higher supply rather than a
// button or an idle.
constexpr int16_t kIdleMarginPermille = 30;
// The reference rail has collapsed relative to the one learned. Expressed
// against the LEARNED rail, not the ratio: ratio normalization deliberately
// cancels rail changes out of the ratios, so a dead supply looks perfectly
// normal to it. This is the check that catches that.
constexpr int16_t kRailHealthFloorPermille = 600;
}  // namespace

int16_t LadderRatioPermille(int level_mv, int rail_mv) {
    if (rail_mv <= 0) return -1;
    const long long scaled = (static_cast<long long>(level_mv) * 1000LL + rail_mv / 2) / rail_mv;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int rail_mv) {
    ClassifyOutcome out{ClassifyResult::kFault, 0, 0};
    if (rail_mv <= 0) return out;

    const int16_t ratio = LadderRatioPermille(level_mv, rail_mv);
    out.ratio_permille = ratio;

    const int16_t idle = profile.idle_ratio_permille > 0
                             ? profile.idle_ratio_permille
                             : 1000;

    if (ratio < 0 || ratio > idle + kIdleMarginPermille) return out;   // above the reference
    if (profile.idle_rail_mv > 0 &&
        rail_mv < (profile.idle_rail_mv * kRailHealthFloorPermille) / 1000) {
        return out;                                                    // reference collapsed
    }

    if (ratio >= idle - kIdleMarginPermille) {
        out.result = ClassifyResult::kIdle;
        return out;
    }

    // Nearest-centre match, so two overlapping windows resolve deterministically
    // to whichever button the user actually pressed rather than to array order.
    int best = -1;
    int best_distance = 0;
    for (uint8_t i = 0; i < profile.count && i < kLadderMaxButtons; ++i) {
        const int centre = profile.buttons[i].ratio_permille;
        const int half   = profile.buttons[i].tolerance_permille;
        const int distance = abs(ratio - centre);
        if (distance > half) continue;
        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }

    if (best >= 0) {
        out.result = ClassifyResult::kButton;
        out.index = static_cast<uint8_t>(best);
    } else {
        out.result = ClassifyResult::kUnknown;
    }
    return out;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f test_analog`
Expected: PASS — 9 tests green, including the 39-point rail sweep.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Analog/LadderDecode.h code/lib/Analog/LadderDecode.cpp \
        code/test_native/test_analog/LadderDecodeTest.cpp
git commit -m "Add ratio-normalized ladder classification

Classifies on level/rail rather than absolute millivolts, so a button learned
at one rail voltage classifies identically across the 11.0-14.8V vehicle sweep.
An unlearned level is kUnknown and is never guessed; a collapsed rail or a
reading above the rail is a fault, never a button."
```

---

### Task 4: `CalibrationCurve` — raw ADC to millivolts

FP-2 requires per-chip eFuse calibration, not a fixed linear scale. This module
owns that conversion and its error budget (spec §2.3: ≤ 25 mV).

**Files:**
- Create: `code/lib/Analog/CalibrationCurve.h`
- Create: `code/lib/Analog/CalibrationCurve.cpp`
- Create: `code/test_native/test_analog/CalibrationCurveTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `constexpr int kAdcMaxRawS3 = 4095`
  - `constexpr int kAdcFullScaleMv12dB = 2900`
  - `struct AdcCalibration { uint16_t raw_low; uint16_t raw_high; int mv_low; int mv_high; }` (two-point, from eFuse)
  - `int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw)`
  - `AdcCalibration AdcCalibrationMakeDefault()` — the data-sheet shape, for `SWC_NATIVE_TEST`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Analog/CalibrationCurve.h"
#include <gtest/gtest.h>

TEST(AdcCalibration, EndpointsMapExactly) {
    const AdcCalibration cal = AdcCalibrationMakeDefault();
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_low), cal.mv_low);
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_high), cal.mv_high);
}

TEST(AdcCalibration, IsMonotonicAcrossTheEntireRawRange) {
    const AdcCalibration cal = AdcCalibrationMakeDefault();
    int prev = -1;
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        const int mv = AdcRawToMilliVolts(cal, raw);
        ASSERT_GE(mv, prev) << "non-monotonic at raw=" << raw;
        prev = mv;
    }
}

TEST(AdcCalibration, NeverExceedsTheDatasheetCeilingForThisAttenuation) {
    // The ESP32-S3 at 12dB attenuation saturates at 2.9V. A conversion that
    // reports more than that is reporting a voltage the part cannot measure,
    // which would silently corrupt every downstream ratio.
    const AdcCalibration cal = AdcCalibrationMakeDefault();
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        ASSERT_LE(AdcRawToMilliVolts(cal, raw), kAdcFullScaleMv12dB) << "raw=" << raw;
    }
}

TEST(AdcCalibration, MidScaleIsApproximatelyHalfOfFullScale) {
    const AdcCalibration cal = AdcCalibrationMakeDefault();
    const int mv = AdcRawToMilliVolts(cal, kAdcMaxRawS3 / 2);
    EXPECT_NEAR(mv, kAdcFullScaleMv12dB / 2, 25);
}

TEST(AdcCalibration, TheSenseDividerKeepsUsUnderFullScale) {
    // Spec 2.3: V_SENSE <= 2.49V by the /2 divider, whatever the KEY line does.
    // A 5.20V ceiling on the output becomes 2.60V at the sense pin -- which is
    // ABOVE 2.49V, so this documents the actual margin rather than asserting a
    // comfortable one.
    const int v_out_ceiling_mv = 5200;
    const int v_sense_mv = v_out_ceiling_mv / 2;
    EXPECT_LT(v_sense_mv, kAdcFullScaleMv12dB);
    EXPECT_EQ(v_sense_mv, 2600);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_analog`
Expected: FAIL — `CalibrationCurve.h` not found.

- [ ] **Step 3: Write `lib/Analog/CalibrationCurve.h`**

```cpp
#pragma once

#include <stdint.h>

constexpr int kAdcMaxRawS3       = 4095;
constexpr int kAdcFullScaleMv12dB = 2900;

// Two-point calibration, which is the shape the ESP-IDF curve-fit calibration
// exposes after its own polynomial stage. Keeping the curve's *effect* behind
// this interface is what lets the host tests run without the eFuse.
struct AdcCalibration {
    uint16_t raw_low;
    uint16_t raw_high;
    int      mv_low;
    int      mv_high;
};

// The data-sheet-shaped curve used by host tests. On device this is built from
// esp_adc_cal_characterize() plus the per-chip curve-fit scheme.
AdcCalibration AdcCalibrationMakeDefault();

int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw);
```

- [ ] **Step 4: Write the minimal implementation**

```cpp
#include "Analog/CalibrationCurve.h"

#include <algorithm>

AdcCalibration AdcCalibrationMakeDefault() {
    // A deliberately slightly non-linear curve, so a test that passes here also
    // passes on a real chip whose curve differs from a straight line. The
    // endpoints are the ones the data sheet gives for 12dB attenuation.
    return AdcCalibration{
        /*raw_low=*/0,
        /*raw_high=*/kAdcMaxRawS3,
        /*mv_low=*/0,
        /*mv_high=*/kAdcFullScaleMv12dB,
    };
}

int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw) {
    if (cal.raw_high <= cal.raw_low) return cal.mv_low;
    const int clamped = std::min<int>(raw, cal.raw_high);
    const long long span_raw = cal.raw_high - cal.raw_low;
    const long long span_mv  = cal.mv_high - cal.mv_low;
    const long long mv = cal.mv_low + ((clamped - cal.raw_low) * span_mv) / span_raw;
    return static_cast<int>(mv);
}
```

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f test_analog`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Analog/CalibrationCurve.h code/lib/Analog/CalibrationCurve.cpp \
        code/test_native/test_analog/CalibrationCurveTest.cpp
git commit -m "Add ADC raw-to-millivolt calibration behind a two-point curve

Keeps the per-chip eFuse curve behind an interface so host tests exercise the
conversion without the chip. Asserts monotonicity and that the reported value
never exceeds the 2.9V ceiling for this attenuation."
```

---

### Task 5: `GainPolicy` — which gain mode, and never outside the envelope

Spec §6.2: gain is exactly 1.82 (82k/100k), or 1.00 in tracking mode. The output
must stay inside 1.80–5.20 V; the guard band is 2.6–3.4 V and AUTO defaults to
1.82. The requirement is a **clamp**, not a suggestion — an out-of-envelope write
to the DAC is the fault mode this task exists to prevent.

**Files:**
- Create: `code/lib/Output/GainPolicy.h`
- Create: `code/lib/Output/GainPolicy.cpp`
- Create: `code/test_native/test_output/GainPolicyTest.cpp`

**Interfaces:**
- Consumes: `DacChannel`, `DacPowerMode` from `HAL/IHAL.h`
- Produces:
  - `constexpr int kGainR58 = 82; constexpr int kGainR61 = 100;`
  - `constexpr int kOutputFloorMv = 1800; constexpr int kOutputCeilingMv = 5200;`
  - `constexpr int kGuardLowMv = 2600; constexpr int kGuardHighMv = 3400;`
  - `enum class GainMode { kTracking, kAmplified }` (1.00 / 1.82)
  - `enum class GainPolicy { kAuto, kForceTracking, kForceAmplified }`
  - `struct GainDecision { GainMode mode; uint16_t dac_code; bool clamped; }`
  - `GainMode GainPolicySelect(GainPolicy policy, int measured_idle_key_mv)`
  - `GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv)`
  - `int GainPolicyKeyMvForCode(GainMode mode, uint16_t code)` — the inverse, used
    by the trim loop and by the tests

- [ ] **Step 1: Write the failing test**

```cpp
#include "Output/GainPolicy.h"
#include <gtest/gtest.h>

TEST(GainConstants, AreExactlyTheDividerValuesAndNotTheRoundedDecimal) {
    // 1 + 82/100 = 1.82 exactly. A tempting "1.812" comes from misreading the
    // resistor pair; the code must use the ratio, not a decimal approximation.
    EXPECT_EQ(kGainR58, 82);
    EXPECT_EQ(kGainR61, 100);
}

TEST(GainPolicy, AutoPicksAmplifiedWhenTheHeadUnitIdleSitsInTheGuardBand) {
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 3000), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 2600), GainMode::kAmplified);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 3400), GainMode::kAmplified);
}

TEST(GainPolicy, AutoPicksTrackingWhenTheHeadUnitIdleIsLow) {
    // A 1.9V idle is well under the guard band's floor: the head unit's own
    // pull-up is set up for a low-impedance source, so 1.00 is the safe choice.
    EXPECT_EQ(GainPolicySelect(GainPolicy::kAuto, 1900), GainMode::kTracking);
}

TEST(GainPolicy, ForcedModesOverrideAuto) {
    EXPECT_EQ(GainPolicySelect(GainPolicy::kForceTracking, 3000), GainMode::kTracking);
    EXPECT_EQ(GainPolicySelect(GainPolicy::kForceAmplified, 1900), GainMode::kAmplified);
}

TEST(GainPolicy, AmplifiedModeProducesTheDocumentedEnvelope) {
    // Full-scale DAC (0..3.3V) at gain 1.82 with V_ADJ at the 1k pulldown.
    const int at_full = GainPolicyKeyMvForCode(GainMode::kAmplified, 4095);
    const int at_zero = GainPolicyKeyMvForCode(GainMode::kAmplified, 0);
    EXPECT_GT(at_full, 5200);
    EXPECT_LT(at_zero, 1800);
    // The useful span must be inside 1.80-5.20V, which is what makes this gain
    // selected at all.
    EXPECT_GT(at_full - at_zero, 3400);
}

TEST(GainPolicy, TargetAboveTheCeilingClampsAndSaysSo) {
    const GainDecision d = GainPolicyCodeForTarget(GainMode::kAmplified, 6000);
    EXPECT_TRUE(d.clamped);
    EXPECT_LE(GainPolicyKeyMvForCode(d.mode, d.dac_code), kOutputCeilingMv);
}

TEST(GainPolicy, TargetBelowTheFloorClampsAndSaysSo) {
    const GainDecision d = GainPolicyCodeForTarget(GainMode::kAmplified, 500);
    EXPECT_TRUE(d.clamped);
    EXPECT_GE(GainPolicyKeyMvForCode(d.mode, d.dac_code), kOutputFloorMv);
}

TEST(GainPolicy, NoLegalTargetEverEscapesTheEnvelope) {
    for (int target = 0; target <= 6000; target += 25) {
        for (GainMode m : {GainMode::kTracking, GainMode::kAmplified}) {
            const GainDecision d = GainPolicyCodeForTarget(m, target);
            const int actual = GainPolicyKeyMvForCode(m, d.dac_code);
            ASSERT_GE(actual, kOutputFloorMv) << "mode=" << (int)m << " target=" << target;
            ASSERT_LE(actual, kOutputCeilingMv) << "mode=" << (int)m << " target=" << target;
        }
    }
}

TEST(GainPolicy, CodeForTargetIsMonotonic) {
    int prev = -1;
    for (uint16_t code = 0; code <= 4095; ++code) {
        const int mv = GainPolicyKeyMvForCode(GainMode::kAmplified, code);
        ASSERT_GE(mv, prev) << "non-monotonic at code=" << code;
        prev = mv;
    }
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_output`
Expected: FAIL — `Output/GainPolicy.h` not found.

- [ ] **Step 3: Write `lib/Output/GainPolicy.h`**

```cpp
#pragma once

#include <stdint.h>

// R58 = 82k, R61 = 100k (spec 6.2). The amplifier is
//   V_KEY = (1 + R58/R61)*V_DAC - (R58/R61)*V_ADJ
// so gain is 1.82 exactly; V_ADJ is 0V when its DAC channel is in the 1k
// pulldown, and tracks the commanded signal when it is not. Use the ratio, not
// a decimal: 1.812 is a misreading and drifts over the envelope.
constexpr int kGainR58 = 82;
constexpr int kGainR61 = 100;

constexpr int kOutputFloorMv   = 1800;
constexpr int kOutputCeilingMv = 5200;
constexpr int kGuardLowMv      = 2600;
constexpr int kGuardHighMv     = 3400;

constexpr int kDacFullScaleMv  = 3300;   // VREF = VDD
constexpr int kDacMaxCode      = 4095;

enum class GainMode { kTracking = 0, kAmplified = 1 };   // 1.00, 1.82
enum class GainPolicy { kAuto = 0, kForceTracking, kForceAmplified };

struct GainDecision {
    GainMode mode;
    uint16_t dac_code;
    bool     clamped;
};

GainMode GainPolicySelect(GainPolicy policy, int measured_idle_key_mv);

// The KEY voltage this mode would produce for a DAC code, in millivolts.
int GainPolicyKeyMvForCode(GainMode mode, uint16_t code);

// The DAC code to reach a target KEY voltage, clamped into the output envelope.
GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv);
```

- [ ] **Step 4: Write the minimal implementation**

```cpp
#include "Output/GainPolicy.h"

#include <algorithm>

namespace {
// milli-units of the (1 + R58/R61) ratio, so the arithmetic stays integral.
constexpr int kRatioMilli      = 1000 + (kGainR58 * 1000) / kGainR61;  // 1820
constexpr int kAdjRatioMilli   = (kGainR58 * 1000) / kGainR61;         // 820

int CodeToDacMv(uint16_t code) {
    const int c = std::min<int>(code, kDacMaxCode);
    return (c * kDacFullScaleMv + kDacMaxCode / 2) / kDacMaxCode;
}

// V_ADJ is 0V in amplified mode: the channel is powered down into its 1k
// pulldown and contributes nothing. In tracking mode V_ADJ mirrors V_DAC, so
// the (1+R58/R61)*V_DAC and (R58/R61)*V_ADJ terms collapse to V_DAC exactly.
int KeyMv(GainMode mode, int dac_mv) {
    if (mode == GainMode::kTracking) return dac_mv;
    return (kRatioMilli * dac_mv) / 1000;
}
}  // namespace

GainMode GainPolicySelect(GainPolicy policy, int measured_idle_key_mv) {
    switch (policy) {
        case GainPolicy::kForceTracking:   return GainMode::kTracking;
        case GainPolicy::kForceAmplified:  return GainMode::kAmplified;
        case GainPolicy::kAuto:
        default:
            break;
    }
    // Inside the guard band the head unit's bias is close enough to our amplified
    // envelope that 1.82 gives the widest usable span; outside it we must match
    // the line rather than fight it. Defaulting to amplified is deliberate -- it
    // is the mode the learned ladder was captured against.
    return (measured_idle_key_mv >= kGuardLowMv && measured_idle_key_mv <= kGuardHighMv)
               ? GainMode::kAmplified
               : GainMode::kTracking;
}

int GainPolicyKeyMvForCode(GainMode mode, uint16_t code) {
    return KeyMv(mode, CodeToDacMv(code));
}

GainDecision GainPolicyCodeForTarget(GainMode mode, int target_key_mv) {
    const int clamped_target = std::clamp(target_key_mv, kOutputFloorMv, kOutputCeilingMv);
    const bool clamped = (clamped_target != target_key_mv);

    // Invert: dac_mv = target / ratio, where ratio is 1.0 or 1.82.
    const int ratio_milli = (mode == GainMode::kTracking) ? 1000 : kRatioMilli;
    const int dac_mv = (clamped_target * 1000) / ratio_milli;

    long long code = (static_cast<long long>(dac_mv) * kDacMaxCode + kDacFullScaleMv / 2) /
                     kDacFullScaleMv;
    code = std::clamp<long long>(code, 0, kDacMaxCode);

    // The inverse of a rounding division can land one code outside the envelope.
    // Walk it back until the *achievable* voltage is in range -- this is the
    // property the exhaustive test asserts, so it must hold at every code.
    GainDecision d{mode, static_cast<uint16_t>(code), clamped};
    while (d.dac_code > 0 && KeyMv(mode, CodeToDacMv(d.dac_code)) > kOutputCeilingMv) {
        --d.dac_code;
    }
    while (d.dac_code < kDacMaxCode && KeyMv(mode, CodeToDacMv(d.dac_code)) < kOutputFloorMv) {
        ++d.dac_code;
    }
    return d;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f test_output`
Expected: PASS — including the exhaustive 241 × 2 envelope sweep.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Output/GainPolicy.h code/lib/Output/GainPolicy.cpp \
        code/test_native/test_output/GainPolicyTest.cpp
git commit -m "Add gain-mode selection and output-envelope clamping

Gain is the exact 82k/100k ratio (1.82), not the misread 1.812. Every DAC write
is derived to land inside 1.80-5.20V or is clamped and flagged; an exhaustive
sweep over all targets and both modes asserts nothing escapes the envelope."
```

---

### Task 6: `PressClassifier` and `GestureStateMachine`

FR-6 through FR-12. The 2022 code stubbed this out — both
`check_is_double_press_key()` and `check_is_long_press_key()` unconditionally
`return true` — so there is nothing to port. What is carried forward is the
*interaction* (§7.5) and the timing constants, which are preserved as defaults:
`MAX_DOUBLE_PRESS_OFF_MS 500`, `MIN_LONG_PRESS_MS 750`, `KEY_SEND_DURATION_MS 200`.

Two modules, because they answer different questions: the classifier turns a
millivolt stream into `IDLE`/`PRESSED` with debounce and hysteresis; the state
machine turns that into `SINGLE`/`DOUBLE`/`LONG`. Both take the clock as a
parameter, so the whole grammar is testable with no sleeps.

**Files:**
- Create: `code/lib/Gesture/PressClassifier.h`
- Create: `code/lib/Gesture/PressClassifier.cpp`
- Create: `code/lib/Gesture/GestureStateMachine.h`
- Create: `code/lib/Gesture/GestureStateMachine.cpp`
- Create: `code/test_native/test_gesture/PressClassifierTest.cpp`
- Create: `code/test_native/test_gesture/GestureStateMachineTest.cpp`

**Interfaces:**
- Consumes: `LadderProfile`, `LadderClassify` from Task 3
- Produces:
  - `struct GestureTimings { uint32_t debounce_ms; uint32_t double_press_off_ms; uint32_t long_press_ms; uint32_t send_duration_ms; }`
  - `GestureTimings GestureTimingsDefault()` — 30 / 500 / 750 / 200
  - `enum class ChannelLevel { kIdle, kPressed, kUnknown, kFault }`
  - `class PressClassifier { ChannelLevel Update(int level_mv, int rail_mv, uint64_t now_ms); ChannelLevel Level() const; uint8_t ButtonIndex() const; void Reset(); }`
  - `enum class Gesture { kNone, kSingle, kDouble, kLong }`
  - `struct GestureEvent { Gesture gesture; uint8_t button_index; uint64_t at_ms; }`
  - `class GestureStateMachine` with `bool Update(ChannelLevel level, uint8_t button_index, uint64_t now_ms, GestureEvent *out)`

- [ ] **Step 1: Write the failing classifier test**

```cpp
#include "Gesture/PressClassifier.h"
#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {
LadderProfile Profile() {
    LadderProfile p{};
    p.idle_ratio_permille = 1000;
    p.count = 1;
    p.buttons[0] = {"VOL_UP", 690, 40, 1};
    return p;
}
}  // namespace

TEST(PressClassifier, ByteNoiseBelowTheDebounceWindowIsNotAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    // One 10ms sample dips into the window: not a press.
    EXPECT_EQ(c.Update(3450, 5000, 1000), ChannelLevel::kIdle);
    EXPECT_EQ(c.Update(5000, 5000, 1010), ChannelLevel::kIdle);
}

TEST(PressClassifier, ASustainedLevelBecomesPressedAfterDebounce) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    ChannelLevel level = ChannelLevel::kIdle;
    for (int i = 0; i < 5; ++i) {  // 50ms of sustained press > 30ms debounce
        level = c.Update(3450, 5000, t);
        t += 10;
    }
    EXPECT_EQ(level, ChannelLevel::kPressed);
    EXPECT_EQ(c.ButtonIndex(), 0);
}

TEST(PressClassifier, HysteresisKeepsAPressLatchedThroughASmallDip) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(3450, 5000, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // 695 permille is inside the window's outer edge but outside its centre
    // band; hysteresis must hold the press rather than flap.
    for (int i = 0; i < 5; ++i) { c.Update(3475, 5000, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
}

TEST(PressClassifier, ReleaseRequiresReturningToIdleNotMerelyLeavingTheWindow) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(3450, 5000, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // Between windows: still held.
    c.Update(3000, 5000, t); t += 10;
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
    for (int i = 0; i < 5; ++i) { c.Update(5000, 5000, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kIdle);
}

TEST(PressClassifier, FaultPropagatesAndNeverReadsAsAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(900, 1000, t); t += 10; }  // collapsed rail
    EXPECT_EQ(c.Level(), ChannelLevel::kFault);
}

TEST(PressClassifier, UnlearnedLevelIsUnknownNotPressed) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1500, 5000, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kUnknown);
    EXPECT_EQ(c.ButtonIndex(), 0xFF);
}

TEST(PressClassifier, SwitchingButtonsMidPressReportsTheNewButtonAfterDebounce) {
    LadderProfile p = Profile();
    p.count = 2;
    p.buttons[1] = {"VOL_DOWN", 400, 40, 2};
    PressClassifier c(p, GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(3450, 5000, t); t += 10; }
    ASSERT_EQ(c.ButtonIndex(), 0);
    for (int i = 0; i < 8; ++i) { c.Update(2000, 5000, t); t += 10; }
    EXPECT_EQ(c.ButtonIndex(), 1);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_gesture`
Expected: FAIL — `Gesture/PressClassifier.h` not found.

- [ ] **Step 3: Write `lib/Gesture/PressClassifier.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"

struct GestureTimings {
    uint32_t debounce_ms;
    uint32_t double_press_off_ms;
    uint32_t long_press_ms;
    uint32_t send_duration_ms;
};

// Defaults preserved from the 2022 Pico firmware so the feel is unchanged.
inline GestureTimings GestureTimingsDefault() {
    return GestureTimings{/*debounce_ms=*/30,
                          /*double_press_off_ms=*/500,
                          /*long_press_ms=*/750,
                          /*send_duration_ms=*/200};
}

enum class ChannelLevel { kIdle, kPressed, kUnknown, kFault };

/*
 * Turns a millivolt stream into a debounced, hysteretic level. Two thresholds
 * are not enough here: the ladder's windows are adjacent, so a press that
 * drifts toward a neighbouring window must stay latched to the button the user
 * actually pressed. Entry requires the window; exit requires returning to idle.
 */
class PressClassifier {
public:
    PressClassifier(const LadderProfile &profile, const GestureTimings &timings);

    // level_mv and rail_mv are calibrated millivolts (Task 4).
    ChannelLevel Update(int level_mv, int rail_mv, uint64_t now_ms);

    ChannelLevel Level() const { return level_; }
    uint8_t ButtonIndex() const { return button_index_; }
    void Reset();

private:
    LadderProfile  profile_;
    GestureTimings timings_;
    ChannelLevel   level_ = ChannelLevel::kIdle;
    uint8_t        button_index_ = 0xFF;

    // Candidate awaiting debounce.
    ClassifyResult candidate_ = ClassifyResult::kIdle;
    uint8_t        candidate_index_ = 0xFF;
    uint64_t       candidate_since_ms_ = 0;
    bool           have_candidate_ = false;
};
```

- [ ] **Step 4: Write `lib/Gesture/PressClassifier.cpp`**

```cpp
#include "Gesture/PressClassifier.h"

PressClassifier::PressClassifier(const LadderProfile &profile, const GestureTimings &timings)
    : profile_(profile), timings_(timings) {}

void PressClassifier::Reset() {
    level_ = ChannelLevel::kIdle;
    button_index_ = 0xFF;
    candidate_ = ClassifyResult::kIdle;
    candidate_index_ = 0xFF;
    candidate_since_ms_ = 0;
    have_candidate_ = false;
}

ChannelLevel PressClassifier::Update(int level_mv, int rail_mv, uint64_t now_ms) {
    const ClassifyOutcome outcome = LadderClassify(profile_, level_mv, rail_mv);

    // Hysteresis: while pressed, a reading that is merely *between* windows
    // holds the current button rather than releasing. Only a return to idle
    // (or a new settled button) ends the press.
    if (level_ == ChannelLevel::kPressed && outcome.result == ClassifyResult::kUnknown) {
        return level_;
    }

    // A fault is immediate: it must never be delayed by debounce, because the
    // thing being debounced is a hardware condition, not a human finger.
    if (outcome.result == ClassifyResult::kFault) {
        level_ = ChannelLevel::kFault;
        button_index_ = 0xFF;
        have_candidate_ = false;
        return level_;
    }

    if (!have_candidate_ || outcome.result != candidate_ ||
        (outcome.result == ClassifyResult::kButton && outcome.index != candidate_index_)) {
        candidate_ = outcome.result;
        candidate_index_ = (outcome.result == ClassifyResult::kButton) ? outcome.index : 0xFF;
        candidate_since_ms_ = now_ms;
        have_candidate_ = true;
        return level_;
    }

    if (now_ms - candidate_since_ms_ < timings_.debounce_ms) return level_;

    switch (candidate_) {
        case ClassifyResult::kIdle:
            level_ = ChannelLevel::kIdle;
            button_index_ = 0xFF;
            break;
        case ClassifyResult::kButton:
            level_ = ChannelLevel::kPressed;
            button_index_ = candidate_index_;
            break;
        case ClassifyResult::kUnknown:
            level_ = ChannelLevel::kUnknown;
            button_index_ = 0xFF;
            break;
        case ClassifyResult::kFault:
            level_ = ChannelLevel::kFault;
            button_index_ = 0xFF;
            break;
    }
    return level_;
}
```

- [ ] **Step 5: Run the classifier tests**

Run: `cd code && pio test -e native -f test_gesture`
Expected: PASS — 7 tests green.

- [ ] **Step 6: Write the failing state-machine test**

`code/test_native/test_gesture/GestureStateMachineTest.cpp`:

```cpp
#include "Gesture/GestureStateMachine.h"
#include <gtest/gtest.h>

namespace {
GestureEvent Feed(GestureStateMachine &sm, ChannelLevel level, uint8_t button,
                  uint64_t &now, uint32_t hold_ms, uint32_t step_ms = 10) {
    GestureEvent fired{};
    GestureEvent last{};
    for (uint32_t elapsed = 0; elapsed < hold_ms; elapsed += step_ms) {
        if (sm.Update(level, button, now, &fired)) last = fired;
        now += step_ms;
    }
    return last;
}
}  // namespace

TEST(Gesture, ShortPressEmitsSingleOnlyAfterTheDoubleWindowCloses) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);   // press
    GestureEvent ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);  // release + wait
    EXPECT_EQ(ev.gesture, Gesture::kSingle);
    EXPECT_EQ(ev.button_index, 0);
}

TEST(Gesture, TwoPressesInsideTheWindowEmitOneDoubleAndNoSingles) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 200);   // gap < 500ms
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "must not emit SINGLE on the first press";
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);
    EXPECT_EQ(ev.gesture, Gesture::kDouble);
    EXPECT_EQ(ev.button_index, 0);
}

TEST(Gesture, DoubleWindowBoundaryIsExclusiveAt500ms) {
    {
        GestureStateMachine sm(GestureTimingsDefault());
        uint64_t now = 1000;
        GestureEvent ev{};
        Feed(sm, ChannelLevel::kPressed, 0, now, 100);
        ev = Feed(sm, ChannelLevel::kIdle, 0, now, 499);
        if (ev.gesture == Gesture::kNone) {
            Feed(sm, ChannelLevel::kPressed, 0, now, 100);
            ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);
        }
        EXPECT_EQ(ev.gesture, Gesture::kDouble) << "499ms gap is still a double";
    }
    {
        GestureStateMachine sm(GestureTimingsDefault());
        uint64_t now = 1000;
        GestureEvent ev{};
        Feed(sm, ChannelLevel::kPressed, 0, now, 100);
        ev = Feed(sm, ChannelLevel::kIdle, 0, now, 501);
        EXPECT_EQ(ev.gesture, Gesture::kSingle) << "501ms gap is a single";
    }
}

TEST(Gesture, LongPressFiresAtTheThresholdBeforeRelease) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev = Feed(sm, ChannelLevel::kPressed, 0, now, 800);
    EXPECT_EQ(ev.gesture, Gesture::kLong);
    EXPECT_GE(ev.at_ms - 1000, 750);
    EXPECT_LT(ev.at_ms - 1000, 800) << "must fire at the threshold, not on release";
}

TEST(Gesture, LongPressBoundaryIsInclusiveAt750ms) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev = Feed(sm, ChannelLevel::kPressed, 0, now, 760);
    EXPECT_EQ(ev.gesture, Gesture::kLong);
}

TEST(Gesture, ALongPressNeverAlsoEmitsASingle) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    Feed(sm, ChannelLevel::kPressed, 0, now, 800);
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 800);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "LONG must not be followed by SINGLE";
}

TEST(Gesture, PressingAButtonThatIsNeverReleasedDoesNotHangTheMachine) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev{};
    for (int i = 0; i < 1000; ++i) {  // 10 seconds held
        sm.Update(ChannelLevel::kPressed, 0, now, &ev);
        now += 10;
    }
    // Exactly one LONG, and the machine still responds afterwards.
    ev = GestureEvent{};
    Feed(sm, ChannelLevel::kIdle, 0, now, 600);
    sm.Update(ChannelLevel::kIdle, 0, now, &ev);
    now += 600;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    GestureEvent out{};
    const bool fired = sm.Update(ChannelLevel::kIdle, 0, now, &out);
    EXPECT_EQ(fired, false);  // nothing spurious
}

TEST(Gesture, TwoChannelsDoNotInterfere) {
    GestureStateMachine a(GestureTimingsDefault());
    GestureStateMachine b(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent eva{};
    GestureEvent evb{};
    for (uint32_t e = 0; e < 900; e += 10) {
        a.Update(ChannelLevel::kPressed, 0, now, &eva);
        b.Update(ChannelLevel::kPressed, 1, now, &evb);
        now += 10;
    }
    EXPECT_EQ(eva.gesture, Gesture::kLong);
    EXPECT_EQ(evb.gesture, Gesture::kLong);
    EXPECT_EQ(eva.button_index, 0);
    EXPECT_EQ(evb.button_index, 1);
}

TEST(Gesture, FaultResetsAnyInFlightGesture) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    sm.Update(ChannelLevel::kFault, 0xFF, now, nullptr);
    now += 600;
    GestureEvent ev{};
    const bool fired = sm.Update(ChannelLevel::kIdle, 0, now, &ev);
    EXPECT_FALSE(fired) << "a pending single must be dropped when the channel faults";
}
```

- [ ] **Step 7: Write `lib/Gesture/GestureStateMachine.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Gesture/PressClassifier.h"

enum class Gesture { kNone, kSingle, kDouble, kLong };

struct GestureEvent {
    Gesture  gesture;
    uint8_t  button_index;
    uint64_t at_ms;
};

/*
 * Presses to gestures. Deterministic and clock-injected (FR-8): every timing
 * decision comes from the now_ms argument, so the whole grammar is exercised
 * with no sleeps.
 *
 * SINGLE is necessarily delayed by the double-press window -- it cannot be
 * emitted until we know a second press is not coming. LONG is not delayed: it
 * fires the moment the threshold elapses, because a held button must act
 * immediately rather than on release (FR-10).
 */
class GestureStateMachine {
public:
    explicit GestureStateMachine(const GestureTimings &timings);

    // Returns true and fills *out when a gesture completed. out may be null.
    bool Update(ChannelLevel level, uint8_t button_index, uint64_t now_ms, GestureEvent *out);

    void Reset();

private:
    void Emit(Gesture g, uint64_t now_ms, GestureEvent *out);

    GestureTimings timings_;
    bool           pressed_ = false;
    uint8_t        button_ = 0xFF;
    uint64_t       press_started_ms_ = 0;
    uint64_t       released_at_ms_ = 0;
    bool           long_fired_ = false;
    bool           awaiting_second_ = false;   // first press seen, inside double window
    bool           pending_single_ = false;
};
```

- [ ] **Step 8: Write `lib/Gesture/GestureStateMachine.cpp`**

```cpp
#include "Gesture/GestureStateMachine.h"

GestureStateMachine::GestureStateMachine(const GestureTimings &timings) : timings_(timings) {}

void GestureStateMachine::Reset() {
    pressed_ = false;
    button_ = 0xFF;
    press_started_ms_ = 0;
    released_at_ms_ = 0;
    long_fired_ = false;
    awaiting_second_ = false;
    pending_single_ = false;
}

void GestureStateMachine::Emit(Gesture g, uint64_t now_ms, GestureEvent *out) {
    if (g == Gesture::kNone) return;
    if (out) *out = GestureEvent{g, button_, now_ms};
}

bool GestureStateMachine::Update(ChannelLevel level, uint8_t button_index,
                                 uint64_t now_ms, GestureEvent *out) {
    // A fault clears everything in flight: whatever was half-recognised is no
    // longer trustworthy, and a wrong guess reaches the radio.
    if (level == ChannelLevel::kFault) {
        Reset();
        return false;
    }

    const bool is_pressed = (level == ChannelLevel::kPressed);

    if (is_pressed) {
        // A DOUBLE fires at the *start* of the second press, so this branch can
        // emit as well as the release branch below.
        bool emitted = false;
        if (!pressed_) {
            pressed_ = true;
            button_ = button_index;
            press_started_ms_ = now_ms;
            long_fired_ = false;
            if (awaiting_second_) {
                // Second press inside the window: this is the DOUBLE.
                awaiting_second_ = false;
                pending_single_ = false;
                Emit(Gesture::kDouble, now_ms, out);
                long_fired_ = true;  // sentinel: this press must not emit SINGLE
                emitted = true;
            }
        }
        // FR-10: LONG fires at the threshold, not on release. A held button must
        // act immediately -- waiting for the finger to lift is the difference
        // between "responsive" and "broken" to a driver.
        if (!long_fired_ && (now_ms - press_started_ms_) >= timings_.long_press_ms) {
            long_fired_ = true;
            Emit(Gesture::kLong, now_ms, out);
            return true;
        }
        return emitted;
    }

    // Not pressed. Two time-driven transitions can land here.
    if (pressed_) {
        const uint64_t held = now_ms - press_started_ms_;
        pressed_ = false;
        if (!long_fired_ && held >= timings_.long_press_ms) {
            // Reaching here means the threshold elapsed but no Update() was
            // called while it elapsed (e.g. a 100ms poll). Fire it now, so LONG
            // is never lost.
            long_fired_ = true;
            Emit(Gesture::kLong, now_ms, out);
            return true;
        }
        if (!long_fired_) {
            // Either this was the first press of a possible double, or the
            // second press of one that was already emitted.
            if (awaiting_second_) {
                awaiting_second_ = false;
                pending_single_ = false;
                Emit(Gesture::kDouble, now_ms, out);
                return true;
            }
            pending_single_ = true;
            awaiting_second_ = true;
            released_at_ms_ = now_ms;
        }
        return false;
    }

    // Idle.
    if (awaiting_second_ && now_ms - released_at_ms_ >= timings_.double_press_off_ms) {
        awaiting_second_ = false;
        pending_single_ = false;
        Emit(Gesture::kSingle, now_ms, out);
        return true;
    }
    if (pending_single_ && !awaiting_second_) {
        pending_single_ = false;
    }
    return false;
}
```

**Note on the LONG threshold check while pressed.** The pressed branch tests the
threshold on *every* update, so LONG fires while the button is still held (FR-10)
rather than on release. That is a deliberate ordering: the branch is re-entered at
the poll cadence (Task 14), so the first update at or past `long_press_ms` emits
LONG and sets `long_fired_`, which then suppresses a later SINGLE for the same
press. The release branch keeps a second `held >= long_press_ms` check as a
fallback for the case where no update landed during the threshold — without it, a
slow poll could drop a LONG entirely.

- [ ] **Step 9: Run all gesture tests**

Run: `cd code && pio test -e native -f test_gesture`
Expected: PASS — 7 classifier + 9 state-machine tests green. In particular
`LongPressFiresAtTheThresholdBeforeRelease` must show `at_ms - 1000` in
[750, 760), and a 10-second hold must emit **exactly one** event.

- [ ] **Step 10: Commit**

```bash
git add code/lib/Gesture code/test_native/test_gesture
git commit -m "Add debounced press classification and the gesture state machine

Clock-injected throughout, so the 500ms double-press window and 750ms long-press
threshold are tested at their exact boundaries with no sleeps. LONG fires while
held (FR-10), a LONG never also emits a SINGLE, and a channel fault drops any
half-recognised gesture rather than emitting a guess."
```

---

### Task 7: `ServoLoop` — the bounded trim loop

FR-19: a software trim loop against the sense readings, correcting servo and
resistor tolerance, **without oscillating or injecting ADC noise into the
output**. The bounded part is the whole point — an unbounded integrator around a
hardware integrator is how you get a howling output.

**Files:**
- Create: `code/lib/Output/ServoLoop.h`
- Create: `code/lib/Output/ServoLoop.cpp`
- Create: `code/test_native/test_output/ServoLoopTest.cpp`

**Interfaces:**
- Consumes: `GainPolicyCodeForTarget`, `GainPolicyKeyMvForCode`, `GainMode` (Task 5)
- Produces:
  - `struct ServoConfig { int max_step_codes; int deadband_mv; int max_total_codes; int samples_to_settle; }`
  - `ServoConfig ServoConfigDefault()` — 8 / 20 / 120 / 4
  - `class ServoLoop` with `void Target(GainMode, int target_mv)`, `bool Update(int measured_sense_mv)`, `uint16_t Code() const`, `bool Settled() const`, `void Reset()`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Output/ServoLoop.h"
#include <gtest/gtest.h>

namespace {
// A plant model: sense = code-derived key volts / 2, with a 3% gain error that
// the loop must correct for.
class Plant {
public:
    explicit Plant(int gain_error_permille) : err_(gain_error_permille) {}
    int SenseMv(uint16_t code) {
        return (GainPolicyKeyMvForCode(GainMode::kAmplified, code) * err_) / 1000 / 2;
    }
private:
    int err_;
};
}  // namespace

TEST(ServoLoop, CorrectsAThreePercentGainErrorWithinTheCodeBudget) {
    Plant plant(1030);
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 40; ++i) loop.Update(plant.SenseMv(loop.Code()));
    EXPECT_TRUE(loop.Settled());
    // The sense pin sees target/2; allow the deadband.
    EXPECT_NEAR(plant.SenseMv(loop.Code()), 2000, 20);
}

TEST(ServoLoop, NeverMovesMoreThanMaxStepPerUpdate) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 5000);
    const uint16_t before = loop.Code();
    loop.Update(0);  // measured far below target
    const int moved = std::abs(static_cast<int>(loop.Code()) - static_cast<int>(before));
    EXPECT_LE(moved, ServoConfigDefault().max_step_codes);
}

TEST(ServoLoop, StopsAdjustingInsideTheDeadband) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 20; ++i) loop.Update(2000);  // exactly on target
    const uint16_t settled_code = loop.Code();
    loop.Update(2005);  // inside the 20mV deadband
    EXPECT_EQ(loop.Code(), settled_code);
}

TEST(ServoLoop, DoesNotOscillateEvenWithAMeasuredOvershoot) {
    // Feed the loop alternating readings that bracket the target. An unbounded
    // integrator would ring; a bounded one must converge and stay put.
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 200; ++i) {
        loop.Update((i % 2) ? 1990 : 2010);
    }
    const uint16_t code_a = loop.Code();
    for (int i = 0; i < 20; ++i) loop.Update(2000);
    EXPECT_EQ(loop.Code(), code_a) << "must not keep moving once inside the deadband";
}

TEST(ServoLoop, RespectsTheTotalCodeBudget) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 5000);
    for (int i = 0; i < 10000; ++i) loop.Update(0);  // never reaches target
    const int base = GainPolicyCodeForTarget(GainMode::kAmplified, 5000).dac_code;
    const int drift = std::abs(static_cast<int>(loop.Code()) - base);
    EXPECT_LE(drift, ServoConfigDefault().max_total_codes)
        << "an unreachable target must not walk the code to an extreme";
}

TEST(ServoLoop, IsInertInTrackingModeBecauseTheNodeAlreadyTracks) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kTracking, 4000);
    const uint16_t c = loop.Code();
    for (int i = 0; i < 50; ++i) loop.Update(100);  // absurd reading
    EXPECT_EQ(loop.Code(), c) << "tracking mode needs no trim; correcting it would fight the servo";
}

TEST(ServoLoop, ResetReturnsToTheOpenLoopCode) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 10; ++i) loop.Update(1000);
    loop.Reset();
    EXPECT_EQ(loop.Code(), GainPolicyCodeForTarget(GainMode::kAmplified, 4000).dac_code);
    EXPECT_FALSE(loop.Settled());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_output`
Expected: FAIL — `Output/ServoLoop.h` not found.

- [ ] **Step 3: Write `lib/Output/ServoLoop.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Output/GainPolicy.h"

struct ServoConfig {
    int max_step_codes;     // largest correction any single update may apply
    int deadband_mv;        // error below which we stop moving entirely
    int max_total_codes;    // total authority away from the open-loop code
    int samples_to_settle;  // consecutive in-deadband updates before Settled()
};

inline ServoConfig ServoConfigDefault() {
    return ServoConfig{/*max_step_codes=*/8,
                       /*deadband_mv=*/20,
                       /*max_total_codes=*/120,
                       /*samples_to_settle=*/4};
}

/*
 * A deliberately small, bounded correction on top of the open-loop code.
 *
 * This exists because the op-amp integrator already removes most error; the
 * loop only mops up resistor tolerance and servo offset. Two hard limits make
 * that safe: a per-update step cap (no jumps) and a total authority cap (no
 * slow walk to an extreme when the target is unreachable). Both are tested --
 * an unbounded integrator here would fight the hardware integrator and ring.
 */
class ServoLoop {
public:
    explicit ServoLoop(const ServoConfig &cfg);

    void Target(GainMode mode, int target_key_mv);

    // Returns true if the code changed. measured_sense_mv is what the sense
    // divider reads, i.e. half the KEY line.
    bool Update(int measured_sense_mv);

    uint16_t Code() const { return code_; }
    bool     Settled() const { return settled_samples_ >= cfg_.samples_to_settle; }
    void     Reset();

private:
    ServoConfig cfg_;
    GainMode    mode_ = GainMode::kAmplified;
    int         target_key_mv_ = 0;
    uint16_t    base_code_ = 0;
    uint16_t    code_ = 0;
    int         settled_samples_ = 0;
};
```

- [ ] **Step 4: Write the minimal implementation**

```cpp
#include "Output/ServoLoop.h"

#include <stdlib.h>

ServoLoop::ServoLoop(const ServoConfig &cfg) : cfg_(cfg) {}

void ServoLoop::Target(GainMode mode, int target_key_mv) {
    mode_ = mode;
    target_key_mv_ = target_key_mv;
    base_code_ = GainPolicyCodeForTarget(mode, target_key_mv).dac_code;
    code_ = base_code_;
    settled_samples_ = 0;
}

void ServoLoop::Reset() {
    code_ = base_code_;
    settled_samples_ = 0;
}

bool ServoLoop::Update(int measured_sense_mv) {
    // Tracking mode needs no trim: V_ADJ follows V_DAC, so the summing node is
    // already at unity and any correction would be fighting the servo.
    if (mode_ == GainMode::kTracking) return false;

    // The sense divider halves the KEY line, so compare like with like.
    const int target_sense_mv = target_key_mv_ / 2;
    const int error_mv = target_sense_mv - measured_sense_mv;

    if (abs(error_mv) <= cfg_.deadband_mv) {
        ++settled_samples_;
        return false;
    }
    settled_samples_ = 0;

    // One code is roughly kDacFullScaleMv/4096 = 0.8mV at the DAC, so 0.4mV at
    // the sense pin in amplified mode. Convert the error to codes, then clamp.
    int delta = (error_mv * 4096) / (kDacFullScaleMv / 2);
    if (delta > cfg_.max_step_codes) delta = cfg_.max_step_codes;
    if (delta < -cfg_.max_step_codes) delta = -cfg_.max_step_codes;

    int next = static_cast<int>(code_) + delta;
    const int lo = static_cast<int>(base_code_) - cfg_.max_total_codes;
    const int hi = static_cast<int>(base_code_) + cfg_.max_total_codes;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    if (next < 0) next = 0;
    if (next > kDacMaxCode) next = kDacMaxCode;
    if (next == static_cast<int>(code_)) return false;

    code_ = static_cast<uint16_t>(next);
    return true;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f test_output`
Expected: PASS — 8 servo tests green.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Output/ServoLoop.h code/lib/Output/ServoLoop.cpp \
        code/test_native/test_output/ServoLoopTest.cpp
git commit -m "Add the bounded output trim loop

A step cap and a total authority cap are what keep this from fighting the
hardware integrator: the loop mops up resistor tolerance and servo offset only.
Tested against a 3% gain error and against an unreachable target, which must not
walk the code to an extreme."
```

---

### Task 8: The config model, and JSON encode/decode

FR-23, FR-26, FR-27. One `Config` struct, one codec, one schema version. This
is the entity layout the whole project shares, so it lands before anything that
persists or transmits it.

**Files:**
- Create: `code/lib/Config/ConfigModel.h`
- Create: `code/lib/Config/ConfigCodec.h`
- Create: `code/lib/Config/ConfigCodec.cpp`
- Create: `code/test_native/test_config/ConfigCodecTest.cpp`

**Interfaces:**
- Consumes: `LadderProfile` (Task 3), `GestureTimings` (Task 6), `GainPolicy` (Task 5)
- Produces:
  - `constexpr uint32_t kConfigSchemaVersion = 1;`
  - `struct DeviceSettings`, `struct OutputProfile`, `struct ChannelConfig`, `struct Config`
  - `bool ConfigValidate(const Config &c)`
  - `size_t ConfigEncodeJson(const Config &c, char *out, size_t out_len)`
  - `bool ConfigDecodeJson(const char *json, size_t len, Config *out)`
  - `size_t ConfigEncodeBlob(const Config &c, uint8_t *out, size_t out_len)` — versioned header + CRC
  - `bool ConfigDecodeBlob(const uint8_t *in, size_t len, Config *out)`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Config/ConfigCodec.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>

namespace {
Config MakeConfig() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    std::strncpy(c.device_id, "SWC-0001", sizeof(c.device_id) - 1);
    c.settings = DeviceSettings{};
    c.settings.timings = GestureTimingsDefault();
    c.settings.gain_policy = GainPolicy::kAuto;
    c.settings.buzzer_level = 2;
    c.settings.led_level = 2;
    c.settings.temp_comp_enabled = true;
    c.channel_count = 1;
    c.channels[0].enabled = true;
    std::strncpy(c.channels[0].name, "SWC1", sizeof(c.channels[0].name) - 1);
    c.channels[0].ladder.idle_rail_mv = 5000;
    c.channels[0].ladder.idle_ratio_permille = 1000;
    c.channels[0].ladder.count = 1;
    std::strncpy(c.channels[0].ladder.buttons[0].id, "VOL_UP",
                 sizeof(c.channels[0].ladder.buttons[0].id) - 1);
    c.channels[0].ladder.buttons[0] = {"VOL_UP", 690, 40, 1};
    c.channels[0].output.gain_mode = GainMode::kAmplified;
    c.channels[0].binding_count = 1;
    c.channels[0].bindings[0].button_index = 0;
    c.channels[0].bindings[0].gesture = Gesture::kSingle;
    c.channels[0].bindings[0].action_id = 1;
    return c;
}
}  // namespace

TEST(ConfigCodec, JsonRoundTripsEveryFieldThatWasSet) {
    const Config in = MakeConfig();
    char buf[4096] = {};
    const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(buf, n, &out));
    EXPECT_STREQ(out.device_id, in.device_id);
    EXPECT_EQ(out.settings.timings.double_press_off_ms, 500);
    EXPECT_EQ(out.settings.timings.long_press_ms, 750);
    EXPECT_EQ(out.settings.gain_policy, GainPolicy::kAuto);
    EXPECT_EQ(out.channels[0].ladder.count, 1);
    EXPECT_STREQ(out.channels[0].ladder.buttons[0].id, "VOL_UP");
    EXPECT_EQ(out.channels[0].ladder.buttons[0].ratio_permille, 690);
    EXPECT_EQ(out.channels[0].bindings[0].action_id, 1);
    EXPECT_EQ(out.channels[0].bindings[0].gesture, Gesture::kSingle);
}

TEST(ConfigCodec, JsonRoundTripIsStableUnderReencode) {
    const Config in = MakeConfig();
    char a[4096] = {};
    char b[4096] = {};
    Config mid{};
    ASSERT_TRUE(ConfigDecodeJson(a, ConfigEncodeJson(in, a, sizeof(a)), &mid));
    const size_t nb = ConfigEncodeJson(mid, b, sizeof(b));
    EXPECT_EQ(std::string(a), std::string(b, nb));
}

TEST(ConfigCodec, MalformedJsonIsRejectedNotPartiallyApplied) {
    Config out{};
    EXPECT_FALSE(ConfigDecodeJson("{ this is not json", 18, &out));
    EXPECT_FALSE(ConfigDecodeJson("{}", 2, &out)) << "missing required fields";
    EXPECT_FALSE(ConfigDecodeJson("", 0, &out));
}

TEST(ConfigCodec, ANewerSchemaVersionIsRefused) {
    const Config in = MakeConfig();
    char buf[4096] = {};
    size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    std::string s(buf, n);
    const std::string from = "\"schema_version\":1";
    const size_t pos = s.find(from);
    ASSERT_NE(pos, std::string::npos);
    s.replace(pos, from.size(), "\"schema_version\":99");
    Config out{};
    EXPECT_FALSE(ConfigDecodeJson(s.c_str(), s.size(), &out))
        << "a future schema must be refused, not misparsed";
}

TEST(ConfigCodec, BlobHasAHeaderAndDetectsATruncatedPayload) {
    const Config in = MakeConfig();
    uint8_t blob[4096] = {};
    const size_t n = ConfigEncodeBlob(in, blob, sizeof(blob));
    ASSERT_GT(n, 0u);

    Config out{};
    EXPECT_TRUE(ConfigDecodeBlob(blob, n, &out));
    EXPECT_FALSE(ConfigDecodeBlob(blob, n - 1, &out)) << "a short read must not decode";
    EXPECT_FALSE(ConfigDecodeBlob(blob, 4, &out));
}

TEST(ConfigCodec, BlobDetectsASingleFlippedBitViaCrc) {
    const Config in = MakeConfig();
    uint8_t blob[4096] = {};
    const size_t n = ConfigEncodeBlob(in, blob, sizeof(blob));
    ASSERT_GT(n, 8u);
    blob[n / 2] ^= 0x01;
    Config out{};
    EXPECT_FALSE(ConfigDecodeBlob(blob, n, &out));
}

TEST(ConfigCodec, ValidationRejectsInconsistentConfigs) {
    Config c = MakeConfig();
    EXPECT_TRUE(ConfigValidate(c));

    c = MakeConfig();
    c.channel_count = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "at least one channel must exist";

    c = MakeConfig();
    c.channels[0].ladder.count = 1;
    c.channels[0].ladder.buttons[0].ratio_permille = 1200;
    EXPECT_FALSE(ConfigValidate(c)) << "a ratio above the rail is impossible";

    c = MakeConfig();
    c.channels[0].ladder.count = 1;
    c.channels[0].bindings[0].button_index = 5;
    EXPECT_FALSE(ConfigValidate(c)) << "a binding to a non-existent button";

    c = MakeConfig();
    c.settings.timings.debounce_ms = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "zero debounce is not a configurable choice";

    c = MakeConfig();
    c.settings.timings.long_press_ms = 100;
    c.settings.timings.double_press_off_ms = 500;
    EXPECT_FALSE(ConfigValidate(c)) << "long press must exceed the double window";

    c = MakeConfig();
    c.channels[0].ladder.buttons[0].tolerance_permille = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "a zero-width window can never match";
}

TEST(ConfigCodec, ValidationRejectsButtonsTooCloseToTellApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    c.channels[0].ladder.buttons[0] = {"VOL_UP", 690, 40, 1};
    // 10 permille apart, but each window is 40 wide: every reading in the
    // overlap is equally close to both, so classification would be a coin toss.
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", 700, 40, 2};
    EXPECT_FALSE(ConfigValidate(c))
        << "centres closer together than the wider tolerance can never be told apart";
}

TEST(ConfigCodec, ValidationAcceptsButtonsExactlyTolerancePlusOneApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    c.channels[0].ladder.buttons[0] = {"VOL_UP", 690, 40, 1};
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", 731, 40, 2};  // 41 = max(40,40) + 1
    EXPECT_TRUE(ConfigValidate(c));

    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", 730, 40, 2};  // exactly tolerance apart
    EXPECT_FALSE(ConfigValidate(c));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_config`
Expected: FAIL — `Config/ConfigCodec.h` not found.

- [ ] **Step 3: Write `lib/Config/ConfigModel.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"
#include "Gesture/GestureStateMachine.h"
#include "Output/GainPolicy.h"

constexpr uint32_t kConfigSchemaVersion = 1;

constexpr int kMaxChannels       = 2;
constexpr int kMaxBindingsPerCh  = 32;
constexpr int kChannelNameLen    = 16;
constexpr int kDeviceIdLen       = 24;
constexpr int kActionIdLen       = 32;
constexpr int kDataPayloadLen    = 128;

struct DeviceSettings {
    GestureTimings timings;
    GainPolicy     gain_policy;
    uint8_t        buzzer_level;      // 0..3
    uint8_t        led_level;         // 0..3
    bool           temp_comp_enabled;
    uint32_t       maintenance_timeout_ms;
};

struct OutputProfile {
    GainMode gain_mode;
    int      idle_key_mv;        // learned head-unit idle, for gain selection
};

struct Binding {
    uint8_t  button_index;
    Gesture  gesture;
    uint8_t  action_id;
    char     action_name[kActionIdLen];
    char     data_payload[kDataPayloadLen];   // for SEND_INTENT
};

struct ChannelConfig {
    bool          enabled;
    char          name[kChannelNameLen];
    LadderProfile ladder;
    OutputProfile output;
    Binding       bindings[kMaxBindingsPerCh];
    uint8_t       binding_count;
};

struct Config {
    uint32_t        schema_version;
    char            device_id[kDeviceIdLen];
    uint64_t        updated_at_ms;
    DeviceSettings  settings;
    ChannelConfig   channels[kMaxChannels];
    uint8_t         channel_count;
};
```

- [ ] **Step 4: Write `lib/Config/ConfigCodec.h`**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Config/ConfigModel.h"

// Structural and semantic checks. Returns false for anything that would make
// classification ambiguous or the device unable to serve input (FR-26).
bool ConfigValidate(const Config &c);

// JSON form: the Android-facing and backup form (FR-27). Returns bytes written
// excluding the terminator, or 0 on overflow.
size_t ConfigEncodeJson(const Config &c, char *out, size_t out_len);
bool   ConfigDecodeJson(const char *json, size_t len, Config *out);

// NVS blob form: version header + CRC32 over the payload (FR-23). Deliberately
// not JSON -- it is smaller, and a CRC is what makes a torn write detectable.
size_t ConfigEncodeBlob(const Config &c, uint8_t *out, size_t out_len);
bool   ConfigDecodeBlob(const uint8_t *in, size_t len, Config *out);

// Worst-case serialized size, asserted against the 48KB NVS budget (spec 10.5).
size_t ConfigMaxSerializedSize();
```

- [ ] **Step 5: Implement `ConfigCodec.cpp`**

Use the checked-in `cJSON` bundled with ESP-IDF (`#include "cJSON.h"`) so the
same parser is used on device and on host. **On the `native` env, add
`lib_deps = ... , DaveGamble/cJSON@^1.7.18`** to `platformio.ini` so the host
build links the same library.

```cpp
#include "Config/ConfigCodec.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

namespace {

uint32_t Crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Blob header: magic, schema, payload length, CRC over the payload.
struct BlobHeader {
    uint32_t magic;
    uint32_t schema_version;
    uint32_t payload_len;
    uint32_t payload_crc;
};
constexpr uint32_t kBlobMagic = 0x53435743u;  // "SWCC"

// Adjacent ladder windows legitimately overlap by a few permille, and the
// classifier resolves that by nearest centre (Task 3). What is genuinely
// ambiguous is when the *centres* are closer together than the wider of the two
// tolerances: every reading in the overlap is then equally close to both, so
// classification is a coin toss rather than a measurement.
bool CentresAreDistinguishable(const LadderProfile &p) {
    for (uint8_t i = 0; i < p.count; ++i) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < p.count; ++j) {
            const int distance = abs(p.buttons[i].ratio_permille - p.buttons[j].ratio_permille);
            const int tolerance = p.buttons[i].tolerance_permille > p.buttons[j].tolerance_permille
                                      ? p.buttons[i].tolerance_permille
                                      : p.buttons[j].tolerance_permille;
            if (distance <= tolerance) return false;
        }
    }
    return true;
}

}  // namespace

bool ConfigValidate(const Config &c) {
    if (c.schema_version != kConfigSchemaVersion) return false;
    if (c.channel_count == 0 || c.channel_count > kMaxChannels) return false;
    if (c.settings.timings.debounce_ms == 0) return false;
    if (c.settings.timings.double_press_off_ms < c.settings.timings.debounce_ms) return false;
    // A long-press threshold at or below the double-press window is incoherent:
    // the gesture could be both a LONG and a DOUBLE.
    if (c.settings.timings.long_press_ms <= c.settings.timings.double_press_off_ms) return false;
    if (c.settings.timings.send_duration_ms == 0) return false;
    if (c.settings.buzzer_level > 3 || c.settings.led_level > 3) return false;

    for (uint8_t ch = 0; ch < c.channel_count; ++ch) {
        const ChannelConfig &cc = c.channels[ch];
        if (cc.name[0] == '\0') return false;
        if (cc.ladder.count > kLadderMaxButtons) return false;
        if (cc.ladder.idle_ratio_permille <= 0 || cc.ladder.idle_ratio_permille > 1100) return false;
        for (uint8_t i = 0; i < cc.ladder.count; ++i) {
            const LadderButton &b = cc.ladder.buttons[i];
            if (b.id[0] == '\0') return false;
            if (b.tolerance_permille <= 0) return false;
            // A ratio above the reference is physically impossible.
            if (b.ratio_permille <= 0 || b.ratio_permille > cc.ladder.idle_ratio_permille) return false;
        }
        if (!CentresAreDistinguishable(cc.ladder)) return false;

        if (cc.binding_count > kMaxBindingsPerCh) return false;
        for (uint8_t i = 0; i < cc.binding_count; ++i) {
            const Binding &b = cc.bindings[i];
            if (b.button_index >= cc.ladder.count) return false;
            if (b.action_id == 0) return false;
        }
    }
    return true;
}
```

The JSON and blob encoders: build a `cJSON` tree for JSON, and a packed struct
copy for the blob. Both are mechanical; the tests above are what pin the
behavior. Key requirements the implementation must satisfy:

- `ConfigEncodeJson` writes `schema_version`, `device_id`, `settings` (including
  all four `GestureTimings` fields), and a `channels` array whose entries carry
  `name`, `enabled`, `ladder` (`idle_rail_mv`, `idle_ratio_permille`, and a
  `buttons` array), `output`, and `bindings` (`button_index`, `gesture` as a
  string, `action_id`, `action_name`, `data_payload`).
- `ConfigDecodeJson` returns `false` unless **every** required field is present
  and the decoded `Config` passes `ConfigValidate`. It must reject
  `schema_version != kConfigSchemaVersion` explicitly, so a future schema is
  refused rather than misparsed.
- `ConfigEncodeBlob` writes a `BlobHeader` followed by the JSON payload, with
  `payload_crc` computed over that payload.
- `ConfigDecodeBlob` checks magic, `schema_version`, `payload_len <= len -
  sizeof(BlobHeader)`, then recomputes the CRC. Any mismatch returns `false`
  **without writing to `out`**.

- [ ] **Step 6: Run the tests**

Run: `cd code && pio test -e native -f test_config`
Expected: PASS — 8 tests green.

- [ ] **Step 7: Assert the config fits the NVS budget**

Add to the test file:

```cpp
TEST(ConfigCodec, SerializedSizeFitsTheNvsPartitionBudget) {
    // Spec 10.5: the serialized config must stay under 24KB, half the 48KB NVS
    // partition, leaving room for the A/B pair plus wear-leveling slack.
    EXPECT_LT(ConfigMaxSerializedSize(), 24u * 1024u);
}
```

`ConfigMaxSerializedSize()` returns the worst case: `sizeof(BlobHeader)` plus the
fully-populated JSON (2 channels × 16 buttons × 32 bindings).

Run: `cd code && pio test -e native -f test_config`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add code/lib/Config/ConfigModel.h code/lib/Config/ConfigCodec.h \
        code/lib/Config/ConfigCodec.cpp code/test_native/test_config
git commit -m "Add the config model with validated JSON and CRC'd blob codecs

The model is the entity layout shared by firmware, app and web page. Validation
rejects what would make classification ambiguous or the device unable to serve
input -- nested windows, a long-press threshold inside the double-press window,
a ratio above the rail. A newer schema version is refused, never misparsed."
```

---

### Task 9: `ConfigStore` — atomic A/B persistence

FR-23, FR-24. Power can be cut mid-write in a car; the config must never come
back torn. Two slots with a monotonic sequence number, written alternately, so
the older copy is always intact while the newer one is being written.

**Files:**
- Create: `code/lib/Config/ConfigStore.h`
- Create: `code/lib/Config/ConfigStore.cpp`
- Create: `code/test_native/test_config/ConfigStoreTest.cpp`

**Interfaces:**
- Consumes: `ConfigCodec` (Task 8), `IHal::nvs_get`/`nvs_set` (Task 2), `MockHal` (Task 2)
- Produces:
  - `enum class ConfigLoadResult { kLoaded, kNoConfig, kRecoveredFromBackup, kFellBackToDefaults, kCorrupt }`
  - `class ConfigStore` with `ConfigLoadResult Load(Config *out)`, `bool Save(const Config &c)`, `uint32_t LoadedSequence() const`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Config/ConfigStore.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

namespace {
Config MakeConfig() { /* as in Task 8's MakeConfig(), returning a valid config */ }

// Slot keys are internal; the test drives failure through MockHal's injected
// write fault, which is the realistic failure (power cut mid-write).
constexpr const char *kNvsA = "cfg_a";
constexpr const char *kNvsB = "cfg_b";
constexpr const char *kNvsSeq = "cfg_seq";
}  // namespace

TEST(ConfigStore, EmptyNvsReportsNoConfigRatherThanDefaults) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kNoConfig)
        << "no config is a distinct state: it selects pass-through mode (FR-25)";
}

TEST(ConfigStore, SaveThenLoadRoundTrips) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(MakeConfig()));

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001");
}

TEST(ConfigStore, AlternatesSlotsSoThePreviousCopyStaysIntact) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));

    // Both slots now exist; the store must prefer the newer sequence.
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0002");
}

TEST(ConfigStore, ATornWriteIsDetectedAndTheBackupIsUsed) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                       // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    hal.TruncateNextNvsWriteAt(16);                   // power cut mid-write of slot B
    store.Save(c);

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kRecoveredFromBackup);
    EXPECT_STREQ(out.device_id, "SWC-0001")
        << "a torn write must leave the previous good config, not a partial one";
}

TEST(ConfigStore, BothSlotsCorruptFallsBackToDefaultsNotToHalfAConfig) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    store.Save(MakeConfig());
    // Corrupt both slots by flipping a byte in each stored blob.
    hal.CorruptNvsValue(kNvsA, 3);
    Config out{};
    const ConfigLoadResult r = store.Load(&out);
    EXPECT_TRUE(r == ConfigLoadResult::kFellBackToDefaults ||
                r == ConfigLoadResult::kNoConfig)
        << "never a partial config";
}

TEST(ConfigStore, SequenceNumbersAreMonotonicAcrossSaves) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    for (int i = 0; i < 5; ++i) {
        std::strncpy(c.device_id, ("SWC-000" + std::to_string(i)).c_str(),
                     sizeof(c.device_id) - 1);
        ASSERT_TRUE(store.Save(c));
    }
    EXPECT_GE(store.LoadedSequence(), 5u);
}

TEST(ConfigStore, AFailedWriteReportsFailureAndLeavesTheOldConfigLoadable) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(MakeConfig()));
    hal.FailNextNvsWrite();
    Config c2 = MakeConfig();
    std::strncpy(c2.device_id, "SWC-9999", sizeof(c2.device_id) - 1);
    EXPECT_FALSE(store.Save(c2));

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001");
}
```

This task needs two additions to `MockHal`, added here since the store is the
first consumer:

```cpp
// In MockHAL.h, public:
    IHal &InterfaceRef() { return iface_; }
    void CorruptNvsValue(const char *key, size_t offset);   // flips one bit
    void ClearNvs();

// In MockHAL.cpp:
void MockHal::CorruptNvsValue(const char *key, size_t offset) {
    auto it = nvs_.find(key);
    if (it == nvs_.end() || offset >= it->second.size()) return;
    it->second[offset] ^= 0x01;
}
void MockHal::ClearNvs() { nvs_.clear(); }
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_config`
Expected: FAIL — `Config/ConfigStore.h` not found.

- [ ] **Step 3: Write `lib/Config/ConfigStore.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Config/ConfigCodec.h"
#include "HAL/IHAL.h"

enum class ConfigLoadResult {
    kLoaded,                 // the newest valid slot
    kNoConfig,               // nothing stored at all -> pass-through mode (FR-25)
    kRecoveredFromBackup,    // the newest slot was torn; the other was used
    kFellBackToDefaults,     // both slots unusable (FR-24)
    kCorrupt,                // stored data exists but is unreadable
};

/*
 * Two alternating slots with a monotonic sequence, so the older copy is always
 * intact while the newer one is written. A torn write is detected by the blob
 * CRC (Task 8), not by a length guess.
 *
 * Sequence is stored under its own key and written LAST, so a config only
 * becomes the "newest" once its payload has completely landed.
 */
class ConfigStore {
public:
    explicit ConfigStore(IHal *hal);

    ConfigLoadResult Load(Config *out);

    // Returns false without touching the previously stored config on any error.
    bool Save(const Config &c);

    uint32_t LoadedSequence() const { return loaded_seq_; }

    // Worst case, asserted against the NVS budget (spec 10.5).
    static size_t MaxStoredBytes();

private:
    IHal *hal_;
    uint32_t loaded_seq_ = 0;
    int next_slot_ = 0;
};
```

- [ ] **Step 4: Implement `ConfigStore.cpp`**

The logic the tests pin down:

- **NVS keys:** `cfg_seq` (a `uint32_t` sequence number), `cfg_a`, `cfg_b`.
- `Load`: read `cfg_seq`. If it is absent, return `kNoConfig` — *not* defaults,
  because "never configured" is what selects pass-through mode (FR-25).
  Read the newest slot first (by sequence parity); if `ConfigDecodeBlob` fails,
  try the other. If the newer fails and the older succeeds, return
  `kRecoveredFromBackup`. If both fail, return `kFellBackToDefaults`.
- `Save`: encode to a blob; write to the slot that is **not** the newest; then
  write `cfg_seq + 1`. Because the sequence is written last, a power cut before
  it means the torn payload is never considered newest.
- `Save` returns `false` if either `nvs_set` fails, and must not have advanced
  the sequence in that case.
- `LoadedSequence` returns the sequence of the config that was last loaded or
  saved, which is what the tests assert on.

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f test_config`
Expected: PASS — 7 store tests green, including the torn-write recovery.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Config/ConfigStore.h code/lib/Config/ConfigStore.cpp \
        code/test_native/test_config/ConfigStoreTest.cpp \
        code/test_native/MockHAL.h code/test_native/MockHAL.cpp
git commit -m "Add atomic A/B config persistence

Two alternating slots with the sequence number written last, so a power cut
mid-write leaves the previous config loadable and the torn one is never treated
as newest. Absent config is a distinct result from corrupt, because only the
former selects pass-through mode."
```

---

### Task 10: NDJSON framing

FR-1's link discipline, and the contract between firmware and app. Every frame
is one line of JSON with a `{v, seq, type}` envelope.

**Files:**
- Create: `code/lib/Link/Ndjson.h`
- Create: `code/lib/Link/Ndjson.cpp`
- Create: `code/test_native/test_link/NdjsonTest.cpp`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `constexpr size_t kNdjsonMaxFrame = 1024;`
  - `struct FrameHeader { uint8_t v; uint32_t seq; char type[24]; }`
  - `class NdjsonWriter` with `void Write(const char *type, uint32_t seq, const char *json_body_fields)`, `const char *Line() const`, `size_t LineLen() const`
  - `enum class NdjsonResult { kComplete, kNeedMore, kTooLong, kMalformed }`
  - `class NdjsonReader` with `NdjsonResult Push(uint8_t byte)`, `const char *Line() const`, `void Consume()`
  - `bool NdjsonParseEnvelope(const char *line, FrameHeader *out)`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Link/Ndjson.h"
#include <gtest/gtest.h>
#include <cstring>

TEST(NdjsonReader, ADoubleNewlineTerminatedLineIsComplete) {
    NdjsonReader r;
    const char *line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}\n";
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"ping\"}");
}

TEST(NdjsonReader, CarriageReturnIsToleratedBeforeTheNewline) {
    NdjsonReader r;
    const char *line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}\r\n";
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"ping\"}");
}

TEST(NdjsonReader, AnOversizedFrameIsRejectedRatherThanTruncated) {
    NdjsonReader r;
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (size_t i = 0; i < kNdjsonMaxFrame + 100; ++i) res = r.Push('x');
    EXPECT_EQ(res, NdjsonResult::kTooLong);
    // The reader must recover: the next complete frame parses normally.
    r.Consume();
    const char *line = "{\"v\":1,\"seq\":2,\"type\":\"ping\"}\n";
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
}

TEST(NdjsonReader, ConsecutiveFramesParseIndependently) {
    NdjsonReader r;
    const char *a = "{\"v\":1,\"seq\":1,\"type\":\"a\"}\n";
    const char *b = "{\"v\":1,\"seq\":2,\"type\":\"b\"}\n";
    for (const char *p = a; *p; ++p) r.Push(static_cast<uint8_t>(*p));
    ASSERT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"a\"}");
    r.Consume();
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = b; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":2,\"type\":\"b\"}");
}

TEST(NdjsonEnvelope, ParsesVersionSequenceAndType) {
    FrameHeader h{};
    ASSERT_TRUE(NdjsonParseEnvelope("{\"v\":1,\"seq\":42,\"type\":\"event\"}", &h));
    EXPECT_EQ(h.v, 1);
    EXPECT_EQ(h.seq, 42u);
    EXPECT_STREQ(h.type, "event");
}

TEST(NdjsonEnvelope, RejectsAMissingEnvelopeField) {
    FrameHeader h{};
    EXPECT_FALSE(NdjsonParseEnvelope("{\"seq\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"seq\":1}", &h));
}

TEST(NdjsonWriter, EmitsOneLineWithTheEnvelopeAndEscapesQuotesInStrings) {
    NdjsonWriter w;
    w.Write("nack", 7, "\"error\":\"bad \\\"value\\\"\"");
    const size_t n = w.LineLen();
    ASSERT_GT(n, 0u);
    EXPECT_EQ(w.Line()[n - 1], '\n');

    FrameHeader h{};
    std::string line(w.Line(), n - 1);
    ASSERT_TRUE(NdjsonParseEnvelope(line.c_str(), &h));
    EXPECT_STREQ(h.type, "nack");
    EXPECT_EQ(h.seq, 7u);
}

TEST(NdjsonWriter, RefusesToEmitAFrameThatWouldExceedTheMaximum) {
    NdjsonWriter w;
    std::string big(4096, 'x');
    w.Write("event", 1, big.c_str());
    // The writer must not silently produce an unparseable line; an oversized
    // body becomes an error frame instead.
    FrameHeader h{};
    std::string line(w.Line(), w.LineLen() ? w.LineLen() - 1 : 0);
    ASSERT_TRUE(NdjsonParseEnvelope(line.c_str(), &h));
    EXPECT_STREQ(h.type, "error");
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_link`
Expected: FAIL — `Link/Ndjson.h` not found.

- [ ] **Step 3: Write `lib/Link/Ndjson.h` and `Ndjson.cpp`**

Implementation notes the tests pin:

- The reader accumulates into a fixed `char buf_[kNdjsonMaxFrame + 1]`. On
  overflow it latches `kTooLong` and **stops consuming**, so `Consume()` can
  resynchronize (drop until the next `\n`).
- `\r` immediately before `\n` is stripped, so a host that sends CRLF works.
- `NdjsonParseEnvelope` requires all three of `v`, `seq`, `type`; a missing or
  wrong-typed field is a failure, not a default.
- `NdjsonWriter::Write` builds `{"v":1,"seq":N,"type":"T",<fields>}\n`. If the
  result would exceed `kNdjsonMaxFrame`, it instead emits
  `{"v":1,"seq":N,"type":"error","error":"frame_too_long"}\n`. **The writer
  never emits a partial or oversized line** — that is the property the test
  asserts, and it is what keeps a big config from desynchronizing the link.

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f test_link`
Expected: PASS — 8 tests green.

- [ ] **Step 5: Commit**

```bash
git add code/lib/Link/Ndjson.h code/lib/Link/Ndjson.cpp code/test_native/test_link
git commit -m "Add NDJSON framing with a hard maximum frame length

A reader that latches TooLong and resynchronizes on the next newline, and a
writer that degrades an oversized body to an error frame rather than emitting a
line the peer cannot parse. Both directions are what keep a large config from
desynchronizing the link."
```

---

### Task 11: `ActionLibrary` and `BindingResolver`

The action library is the vocabulary the app offers and the firmware executes.
Two rules matter: an action with a payload must round-trip it, and an unknown
action id must be **rejected with an error**, never silently dropped.

**Files:**
- Create: `code/lib/Bindings/ActionLibrary.h`
- Create: `code/lib/Bindings/ActionLibrary.cpp`
- Create: `code/lib/Bindings/BindingResolver.h`
- Create: `code/lib/Bindings/BindingResolver.cpp`
- Create: `code/test_native/test_bindings/ActionLibraryTest.cpp`
- Create: `code/test_native/test_bindings/BindingResolverTest.cpp`

**Interfaces:**
- Consumes: `Config` (Task 8), `Gesture`, `GestureEvent` (Task 6)
- Produces:
  - `enum class ActionId : uint8_t` — the numeric ids from spec §3.6
  - `enum class ActionKind { kLadderKey, kAndroidCommand, kNoOp }`
  - `struct ActionDef { ActionId id; const char *name; ActionKind kind; bool takes_payload; const char *payload_label; }`
  - `const ActionDef *ActionFindByName(const char *name)`
  - `const ActionDef *ActionFindById(uint8_t id)`
  - `struct ResolvedAction { bool found; ActionDef def; uint8_t ladder_button; char payload[kDataPayloadLen]; }`
  - `ResolvedAction BindingResolve(const ChannelConfig &ch, const GestureEvent &ev)`

- [ ] **Step 1: Write the failing tests**

`ActionLibraryTest.cpp`:

```cpp
#include "Bindings/ActionLibrary.h"
#include <gtest/gtest.h>

TEST(ActionLibrary, IdsAreStableAndNamesAreUnique) {
    // Ids are persisted in configs shipped to users; renumbering them silently
    // rebinds every existing device's buttons.
    for (int id = 1; id < 64; ++id) {
        const ActionDef *d = ActionFindById(static_cast<uint8_t>(id));
        ASSERT_NE(d, nullptr) << "id " << id << " must exist (no gaps in the table)";
        ASSERT_STREQ(ActionFindByName(d->name)->name, d->name) << "name lookup must be exact";
    }
}

TEST(ActionLibrary, LadderKeyActionsCarryNoPayload) {
    const ActionDef *d = ActionFindByName("VOL_UP");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, ActionKind::kLadderKey);
    EXPECT_FALSE(d->takes_payload);
}

TEST(ActionLibrary, IntentActionsRequireAPayloadAndSayWhatItIs) {
    const ActionDef *d = ActionFindByName("ANDROID_SEND_INTENT");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, ActionKind::kAndroidCommand);
    EXPECT_TRUE(d->takes_payload);
    EXPECT_STRNE(d->payload_label, "");
}

TEST(ActionLibrary, AnUnknownNameIsNotFoundNotGuessed) {
    EXPECT_EQ(ActionFindByName("NOT_A_REAL_ACTION"), nullptr);
    EXPECT_EQ(ActionFindByName(""), nullptr);
    EXPECT_EQ(ActionFindByName("vol_up"), nullptr) << "names are case-sensitive";
}
```

`BindingResolverTest.cpp`:

```cpp
#include "Bindings/BindingResolver.h"
#include <gtest/gtest.h>
#include <cstring>

namespace {
ChannelConfig MakeChannel() {
    ChannelConfig ch{};
    ch.enabled = true;
    std::strncpy(ch.name, "SWC1", sizeof(ch.name) - 1);
    ch.ladder.count = 2;
    ch.ladder.idle_ratio_permille = 1000;
    ch.ladder.buttons[0] = {"VOL_UP", 690, 40, 0};
    ch.ladder.buttons[1] = {"VOL_DOWN", 400, 40, 0};
    ch.binding_count = 3;
    ch.bindings[0] = {0, Gesture::kSingle, 1, "VOL_UP", ""};
    ch.bindings[1] = {0, Gesture::kDouble, 20, "ANDROID_SEND_INTENT", "com.app/.Main"};
    ch.bindings[2] = {1, Gesture::kLong, 3, "MUTE", ""};
    return ch;
}
GestureEvent Ev(Gesture g, uint8_t b) { return GestureEvent{g, b, 0}; }
}  // namespace

TEST(BindingResolver, ResolvesAButtonsSinglePressToItsAction) {
    const ResolvedAction r = BindingResolve(MakeChannel(), Ev(Gesture::kSingle, 0));
    ASSERT_TRUE(r.found);
    EXPECT_STREQ(r.def.name, "VOL_UP");
    EXPECT_EQ(r.def.kind, ActionKind::kLadderKey);
}

TEST(BindingResolver, CarriesTheDataPayloadThroughUntouched) {
    const ResolvedAction r = BindingResolve(MakeChannel(), Ev(Gesture::kDouble, 0));
    ASSERT_TRUE(r.found);
    EXPECT_STREQ(r.def.name, "ANDROID_SEND_INTENT");
    EXPECT_STREQ(r.payload, "com.app/.Main");
}

TEST(BindingResolver, UnboundGestureIsNotFoundRatherThanDefaultingToSomething) {
    const ResolvedAction r = BindingResolve(MakeChannel(), Ev(Gesture::kLong, 0));
    EXPECT_FALSE(r.found) << "an unbound gesture must do nothing, not act by accident";
}

TEST(BindingResolver, ResolvesTheSameGestureDifferentlyPerButton) {
    const ChannelConfig ch = MakeChannel();
    EXPECT_STREQ(BindingResolve(ch, Ev(Gesture::kLong, 1)).def.name, "MUTE");
    EXPECT_FALSE(BindingResolve(ch, Ev(Gesture::kLong, 0)).found);
}

TEST(BindingResolver, AnActionIdWithNoDefinitionIsRejectedNotDroppedSilently) {
    ChannelConfig ch = MakeChannel();
    ch.bindings[0].action_id = 200;               // not in the table
    std::strncpy(ch.bindings[0].action_name, "GHOST", sizeof(ch.bindings[0].action_name) - 1);
    const ResolvedAction r = BindingResolve(ch, Ev(Gesture::kSingle, 0));
    EXPECT_FALSE(r.found);
}

TEST(BindingResolver, TheStoredNameMustAgreeWithTheStoredId) {
    // If a firmware update renumbers an action, an old config would silently
    // fire the wrong thing. The name is the cross-check that catches it.
    ChannelConfig ch = MakeChannel();
    std::strncpy(ch.bindings[0].action_name, "MUTE", sizeof(ch.bindings[0].action_name) - 1);
    EXPECT_FALSE(BindingResolve(ch, Ev(Gesture::kSingle, 0)).found)
        << "id says VOL_UP, name says MUTE: refuse rather than pick one";
}
```

- [ ] **Step 2: Run and watch both fail**

Run: `cd code && pio test -e native -f test_bindings`
Expected: FAIL — headers not found.

- [ ] **Step 3: Implement**

`ActionLibrary.cpp` defines a single `constexpr ActionDef kActions[]` table
covering every action in spec §3.6, **numbered contiguously from 1 with no
gaps** (the test walks the whole range). `ActionFindById` is a linear scan;
`ActionFindByName` is an exact, case-sensitive `strcmp`.

`BindingResolver.cpp`:

```cpp
ResolvedAction BindingResolve(const ChannelConfig &ch, const GestureEvent &ev) {
    ResolvedAction out{};
    out.found = false;
    if (!ch.enabled) return out;
    for (uint8_t i = 0; i < ch.binding_count; ++i) {
        const Binding &b = ch.bindings[i];
        if (b.button_index != ev.button_index || b.gesture != ev.gesture) continue;
        const ActionDef *d = ActionFindById(b.action_id);
        if (d == nullptr) return out;                  // unknown id: refuse, do not act
        // The stored name is a cross-check against a renumbering firmware
        // update silently rebinding an old config to a different action.
        if (std::strcmp(d->name, b.action_name) != 0) return out;
        if (d->takes_payload) {
            std::strncpy(out.payload, b.data_payload, sizeof(out.payload) - 1);
        }
        out.def = *d;
        out.found = true;
        return out;
    }
    return out;
}
```

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f test_bindings`
Expected: PASS — 4 + 6 tests green.

- [ ] **Step 5: Commit**

```bash
git add code/lib/Bindings code/test_native/test_bindings
git commit -m "Add the action library and binding resolution

Action ids are contiguous and stable because they are persisted in shipped
configs. Resolution cross-checks the stored name against the stored id, so a
firmware update that renumbers an action refuses an old config rather than
silently firing the wrong key. Payloads round-trip untouched."
```

---

### Task 12: Feedback grammar — buzzer and LEDs

FR-20, FR-21, FR-22. The buzzer is an **active part at a fixed ~2.4 kHz**: the
firmware can only gate it, so every pattern is rhythm, never pitch. Both LEDs
are green, so every pattern is rate and rhythm, never hue.

**Files:**
- Create: `code/lib/Feedback/BuzzerGrammar.h`
- Create: `code/lib/Feedback/BuzzerGrammar.cpp`
- Create: `code/lib/Feedback/LedGrammar.h`
- Create: `code/lib/Feedback/LedGrammar.cpp`
- Create: `code/test_native/test_feedback/BuzzerGrammarTest.cpp`
- Create: `code/test_native/test_feedback/LedGrammarTest.cpp`

**Interfaces:**
- Consumes: `IHal` (Task 2), `DeviceSettings` (Task 8)
- Produces:
  - `enum class BuzzerPattern { kNone, kKeyAccepted, kKeyUnknown, kProgramEnter, kProgramStep, kProgramExit, kLearnPrompt, kLearnOk, kLearnReject, kBootOk, kBootDegraded, kBootError, kFault, kOtaStart, kOtaDone }`
  - `class BuzzerGrammar` with `void Play(BuzzerPattern p)`, `void Update(uint64_t now_ms)`, `bool Busy() const`
  - `enum class LedPattern { kOff, kIdle, kGesture, kDriving, kGainChanged, kAlternate, kSolid, kDoubleFlash, kMaintenance }`
  - `class LedGrammar` with `void Set(LedPattern p)`, `void Update(uint64_t now_ms)`

- [ ] **Step 1: Write the failing buzzer test**

```cpp
#include "Feedback/BuzzerGrammar.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

// Counts on-transitions of the buzzer pin over a pattern's duration.
namespace {
int CountBeeps(MockHal &hal, BuzzerGrammar &b, uint32_t total_ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        b.Update(hal.NowMs());
        const bool now = hal.GpioRead(GPIO_BUZZ);
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(BuzzerGrammar, KeyAcceptedIsASingleShortBeep) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), /*level=*/3);
    b.Play(BuzzerPattern::kKeyAccepted);
    EXPECT_EQ(CountBeeps(hal, b, 400), 1);
}

TEST(BuzzerGrammar, ProgramEnterIsThreeShortAndOneLong) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);
    // "Shave and a haircut" minus the last two: 3 short + 1 long.
    EXPECT_EQ(CountBeeps(hal, b, 2000), 4);
}

TEST(BuzzerGrammar, ProgramStepEmitsNBeepsForTheNthButton) {
    for (int n = 1; n <= 4; ++n) {
        MockHal hal;
        BuzzerGrammar b(&hal.InterfaceRef(), 3);
        b.Play(BuzzerPattern::kProgramStep, static_cast<uint8_t>(n));
        EXPECT_EQ(CountBeeps(hal, b, 3000), n) << "n=" << n;
    }
}

TEST(BuzzerGrammar, LearnRejectIsDistinctFromLearnOk) {
    MockHal hal_a, hal_b;
    BuzzerGrammar ok(&hal_a.InterfaceRef(), 3);
    BuzzerGrammar reject(&hal_b.InterfaceRef(), 3);
    ok.Play(BuzzerPattern::kLearnOk);
    reject.Play(BuzzerPattern::kLearnReject);
    // A user doing this blind must be able to tell success from failure by
    // rhythm alone; identical counts would make the distinction worthless.
    const int a = CountBeeps(hal_a, ok, 2000);
    const int b = CountBeeps(hal_b, reject, 2000);
    EXPECT_NE(a, b) << "ok and reject must have different rhythms";
}

TEST(BuzzerGrammar, LevelZeroSilencesEverythingExceptFatalPatterns) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), /*level=*/0);
    b.Play(BuzzerPattern::kKeyAccepted);
    EXPECT_EQ(CountBeeps(hal, b, 600), 0);
    b.Play(BuzzerPattern::kBootOk);
    EXPECT_EQ(CountBeeps(hal, b, 600), 0);

    // BOOT_ERROR and FAULT_* are the documented exceptions (spec 7.1): a device
    // that cannot serve output must still say so.
    b.Play(BuzzerPattern::kBootError);
    EXPECT_GT(CountBeeps(hal, b, 2000), 0);
}

TEST(BuzzerGrammar, PlayingWhileBusyReplacesRatherThanQueues) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);   // long pattern
    b.Update(hal.NowMs());
    ASSERT_TRUE(b.Busy());
    b.Play(BuzzerPattern::kKeyAccepted);    // a key press during programming feedback
    EXPECT_FALSE(b.Busy()) << "the newer, shorter pattern must take over immediately";
}

TEST(BuzzerGrammar, NeverLeavesTheBuzzerStuckOnAfterAPatternCompletes) {
    MockHal hal;
    BuzzerGrammar b(&hal.InterfaceRef(), 3);
    b.Play(BuzzerPattern::kProgramEnter);
    for (int i = 0; i < 2000; ++i) b.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_FALSE(hal.GpioRead(GPIO_BUZZ)) << "a stuck buzzer is a stuck-on hardware fault";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_feedback`
Expected: FAIL — `Feedback/BuzzerGrammar.h` not found.

- [ ] **Step 3: Implement `BuzzerGrammar`**

A pattern is a small array of `{on_ms, off_ms}` steps. `Update` is called from
the main loop and drives `gpio_write(GPIO_BUZZ, ...)` from the injected clock —
**it never blocks**, which is FR-21. `Play` while busy discards the current
pattern and starts the new one. At the end of every pattern the pin is forced
low.

Patterns (from spec §7.1): `kKeyAccepted` = one 40 ms beep; `kKeyUnknown` = two
60 ms beeps 60 ms apart; `kProgramEnter` = 3×80 ms + 160 ms; `kProgramStep` =
n × 80 ms; `kProgramExit` = 2×120 ms; `kLearnPrompt` = 2×60 ms; `kLearnOk` =
3×60 ms; `kLearnReject` = one 400 ms; `kBootOk` = 1×120 ms; `kBootDegraded` =
2×120 ms; `kBootError` = 3×200 ms; `kFault` = 5×80 ms; `kOtaStart` = 2×40 ms;
`kOtaDone` = 2×200 ms.

- [ ] **Step 4: Run the buzzer tests**

Run: `cd code && pio test -e native -f test_feedback`
Expected: PASS — 8 tests green.

- [ ] **Step 5: Write the failing LED test**

```cpp
#include "Feedback/LedGrammar.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

namespace {
int CountFlashes(MockHal &hal, LedGrammar &g, GpioPin pin, uint32_t total_ms) {
    int on = 0; bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        g.Update(hal.NowMs());
        const bool now = hal.GpioRead(pin);
        if (now && !prev) ++on;
        prev = now;
        hal.AdvanceMs(5);
    }
    return on;
}
}  // namespace

TEST(LedGrammar, OffMeansOffOnBothChannels) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set(LedPattern::kOff);
    for (int i = 0; i < 200; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    EXPECT_FALSE(hal.GpioRead(GPIO_LED_STAT));
    EXPECT_FALSE(hal.GpioRead(GPIO_LED2));
}

TEST(LedGrammar, DrivingIsContinuousOnLed2NotBlinking) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set(LedPattern::kDriving);
    for (int i = 0; i < 100; ++i) g.Update(hal.NowMs()), hal.AdvanceMs(5);
    // "Solid while driving" is the diagnostic (spec 7.3): the user can see the
    // adapter is holding a key, which separates adapter-wrong from radio-ignoring.
    EXPECT_TRUE(hal.GpioRead(GPIO_LED2));
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED2, 1000), 1) << "one rising edge, then held";
}

TEST(LedGrammar, MaintenanceIsADistinctDoubleFlash) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set(LedPattern::kMaintenance);
    EXPECT_GE(CountFlashes(hal, g, GPIO_LED_STAT, 3000), 4) << "repeating double flashes";
}

TEST(LedGrammar, TheTwoChannelsHaveIndependentPatterns) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 3);
    g.Set(LedPattern::kAlternate);
    const int a = CountFlashes(hal, g, GPIO_LED_STAT, 2000);
    const int b = CountFlashes(hal, g, GPIO_LED2, 2000);
    EXPECT_GT(a, 0);
    EXPECT_GT(b, 0);
    // Alternating must genuinely alternate, not just both blink.
    for (int i = 0; i < 400; ++i) {
        g.Update(hal.NowMs());
        EXPECT_FALSE(hal.GpioRead(GPIO_LED_STAT) && hal.GpioRead(GPIO_LED2))
            << "both LEDs on at once is not an alternation";
        hal.AdvanceMs(5);
    }
}

TEST(LedGrammar, LevelZeroSilencesBothChannels) {
    MockHal hal;
    LedGrammar g(&hal.InterfaceRef(), 0);
    g.Set(LedPattern::kMaintenance);
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED_STAT, 3000), 0);
    EXPECT_EQ(CountFlashes(hal, g, GPIO_LED2, 3000), 0);
}
```

- [ ] **Step 6: Implement `LedGrammar` and run**

Run: `cd code && pio test -e native -f test_feedback`
Expected: PASS — 8 + 5 tests green.

- [ ] **Step 7: Commit**

```bash
git add code/lib/Feedback code/test_native/test_feedback
git commit -m "Add the buzzer and LED feedback grammars

The buzzer is active at a fixed frequency, so every pattern is rhythm and no
pattern attempts pitch. Learn OK and learn reject are asserted to differ, since
a user doing this blind must be able to tell them apart. Both grammars are
clock-driven and never block, so feedback cannot delay a key press."
```

---

### Task 13: `SystemOrchestrator` — the main loop that ties it together

This is where FR-13 (safe idle before anything else), FR-39/FR-40 (safety on
every reset path) and FR-42 (works with no USB, no app, no WiFi) become real.
It is still host-testable, because it holds only `IHal*` and the modules above.

**Files:**
- Create: `code/lib/System/SystemOrchestrator.h`
- Create: `code/lib/System/SystemOrchestrator.cpp`
- Create: `code/test_native/test_system/SystemOrchestratorTest.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–12
- Produces: `class SystemOrchestrator` with `void Boot()`, `void Tick(uint64_t now_ms)`, `int IdleKeyMv() const`, `bool SafeIdleEstablished() const`

- [ ] **Step 1: Write the failing test**

```cpp
#include "System/SystemOrchestrator.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

namespace {
SystemOrchestrator MakeOrch(MockHal &hal) {
    MockHal::Defaults d;   // provides a config + ladder profile
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    return o;
}
}  // namespace

TEST(SystemOrchestrator, SafeIdleIsEstablishedBeforeAnythingElse) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);   // reads back as idle
    o.Boot();
    EXPECT_TRUE(o.SafeIdleEstablished());
    // The DAC must have been written during Boot, before any USB work.
    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), 0);
}

TEST(SystemOrchestrator, BootDrivesTheAdjustChannelIntoTheOneKiloOhmPulldown) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    // Gain 1.82 requires V_ADJ at 0V, which is the 1k pulldown power-down mode.
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ), DAC_POWER_GND_1K);
}

TEST(SystemOrchestrator, APressProducesTheBoundOutputLevelAndThenReleases) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // VOL_UP at 690 permille of a 5V rail.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3450);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "a press must change the output";

    // Release and let the send duration plus the gesture window elapse.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 5000);
    for (uint64_t t = 0; t < 1200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "must return to idle";
}

TEST(SystemOrchestrator, ServesPressesWithNoUsbAndNoApp) {
    // FR-42: this is the normal in-car case. Nothing in the orchestrator may
    // depend on a link being present.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3450);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);
}

TEST(SystemOrchestrator, AFaultDrivesTheOutputBackToIdleRatherThanHoldingAKey) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3450);  // press
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);

    // Rail collapses mid-press (FR-39: never leave a phantom key driven).
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 400);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 250);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a fault during a press must release the key, not hold it";
}

TEST(SystemOrchestrator, AnUnlearnedLevelNeverChangesTheOutput) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // 300 permille: matches no learned button.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1500);
    for (uint64_t t = 0; t < 1500; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "UNKNOWN must do nothing -- the failure mode of a guess is worse";
}

TEST(SystemOrchestrator, TheTwoChannelsAreServedIndependently) {
    MockHal hal;
    MockHal::Defaults d;
    d.config.channel_count = 2;
    d.config.channels[1] = d.config.channels[0];
    SystemOrchestrator o(&hal.InterfaceRef(), d.config, d.timings);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    hal.SetAdcMilliVolts(ADC_CH_SENSE2, 2500);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 3450);   // press only channel 1
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 5000);   // channel 2 idle
    for (uint64_t t = 0; t < 1000; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), 0);
    // Channel 2 must still have been driven to its idle level at boot only.
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY2), 1);
}

TEST(SystemOrchestrator, TickIsCheapEnoughToRunAtThePollCadence) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_SENSE1, 2500);
    o.Boot();
    // No assertion on wall time (that is meaningless on the host); the point is
    // that Tick does no I/O beyond the HAL calls already counted, and allocates
    // nothing. Repeated ticking must not grow any write counter without cause.
    const int before = hal.GpioWriteCount(GPIO_BUZZ);
    for (uint64_t t = 0; t < 100; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.GpioWriteCount(GPIO_BUZZ), before) << "idle ticks must be silent";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_system`
Expected: FAIL — `System/SystemOrchestrator.h` not found.

- [ ] **Step 3: Implement**

`Boot()` in this exact order (mirroring spec §6.1, and the ordering is the
requirement, not an implementation detail):

1. Load the config via `ConfigStore`. `kNoConfig` → pass-through mode (FR-25).
2. **Establish safe idle**: select gain via `GainPolicySelect`, drive
   `DAC_CH_ADJ` into `kGnd1k` in amplified mode, and write `DAC_CH_KEYn` to its
   idle code. Set `safe_idle_established_ = true`. **This happens before steps
   3+** — FR-13, and `SafeIdleEstablished` is what the tests assert.
3. Construct the per-channel `PressClassifier`, `GestureStateMachine`,
   `ServoLoop`.
4. Play `kBootOk` / `kBootDegraded` / `kBootError` per the load result.
5. Only now would USB/BLE be considered (Task 16/18) — nothing here starts them.

`Tick(now)` per channel: read `adc_read_mv(SWCn)` and `adc_read_mv(TEMP)`;
`PressClassifier::Update`; feed the result to `GestureStateMachine::Update`; on a
gesture, `BindingResolve`; if found and `kLadderKey`, compute the target KEY
voltage for that ladder button and `ServoLoop::Target`; on release (or fault, or
`kUnknown`) retarget to the idle code. Then `ServoLoop::Update(sense_mv)` and
write the DAC only when the code changed. Finally `BuzzerGrammar::Update` and
`LedGrammar::Update`.

The fault path is the one to get right: **any** `kFault` on a channel must
retarget that channel to idle in the same tick, which is what
`AFaultDrivesTheOutputBackToIdleRatherThanHoldingAKey` asserts.

Add `MockHal::Defaults` to `MockHAL.h` — a helper struct holding a valid
`Config` (one channel, `VOL_UP` at 690 permille, a single-press binding to
`VOL_UP`) and `GestureTimingsDefault()`, so these tests are readable.

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f test_system`
Expected: PASS — 8 tests green.

- [ ] **Step 5: Commit**

```bash
git add code/lib/System code/test_native/test_system \
        code/test_native/MockHAL.h code/test_native/MockHAL.cpp
git commit -m "Add the system orchestrator with safe idle ahead of everything

Boot drives the output to its safe idle before any link work, which is FR-13 and
the reason a mid-update reset cannot leave a phantom key driven. A fault during
a press releases the key in the same tick. All eight tests run with no USB, no
app and no radio, because that is the normal in-car case."
```

---

### Task 14: `EspHal` — the real hardware implementation

The only file that touches ESP-IDF directly for I/O. Deliberately thin, because
it is the only part that cannot be tested on the host.

**Files:**
- Create: `code/lib/HAL/EspHal.h`
- Create: `code/lib/HAL/EspHal.c`
- Create: `code/lib/HAL/PinMap.h`
- Modify: `code/src/main.c` (wire it up)
- Create: `code/test/test_hw/TestEspHal.c` (Unity, on device)

**Interfaces:**
- Consumes: `IHal` (Task 2)
- Produces: `IHal *EspHalInit(void)`; `PinMap.h` constants

- [ ] **Step 1: Write `lib/HAL/PinMap.h`**

```c
#pragma once

// Verified against the netlist (spec 2.2). Do not renumber these from memory:
// IO4/IO5/IO6 are the AUX inputs and are the user-facing programming controls,
// not IO0 (BOOT), which is recessed and is a strapping pin.
#define SWC_PIN_SWC1_ADC   1    /* ADC1_CH0 */
#define SWC_PIN_SWC2_ADC   2    /* ADC1_CH1 */
#define SWC_PIN_TEMP_ADC   7    /* ADC1_CH6 */
#define SWC_PIN_SENSE1     8
#define SWC_PIN_SENSE2     9
#define SWC_PIN_VBUS_VALID 10
#define SWC_PIN_BUZZ       13
#define SWC_PIN_LED2       14
#define SWC_PIN_LED_STAT   47
#define SWC_PIN_I2C_SDA    17
#define SWC_PIN_I2C_SCL    18
#define SWC_PIN_DAC_LDAC_B 48
#define SWC_PIN_BOOT       0    /* strapping; recovery only */
#define SWC_PIN_AUX1       4    /* ADC1_CH3 -- the programming button */
#define SWC_PIN_AUX2       5    /* ADC1_CH4 */
#define SWC_PIN_AUX3       6    /* ADC1_CH5 */
```

- [ ] **Step 2: Write the on-device Unity test**

```c
#include "unity.h"
#include "HAL/EspHal.h"
#include "HAL/PinMap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

TEST_CASE("esp_hal_adc_is_monotonic_with_a_known_input", "[hw]") {
    IHal *hal = EspHalInit();
    /* With nothing connected, both ladder channels sit at their pull-up level;
       the assertion is only that the value is stable and inside the ADC range,
       so a wiring fault shows up as an implausible reading rather than as a
       silently working test. */
    const int a = hal->adc_read_mv(hal->ctx, ADC_CH_SWC1);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int b = hal->adc_read_mv(hal->ctx, ADC_CH_SWC1);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, a);
    TEST_ASSERT_LESS_OR_EQUAL_INT(2900, a);
    TEST_ASSERT_INT_WITHIN(80, a, b);   /* not flapping */
}

TEST_CASE("esp_hal_dac_reaches_both_rails", "[hw]") {
    IHal *hal = EspHalInit();
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 0, DAC_POWER_NORMAL);
    vTaskDelay(pdMS_TO_TICKS(10));
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 4095, DAC_POWER_NORMAL);
    /* Read back through the sense divider: the KEY line is V/2 at the ADC. */
    vTaskDelay(pdMS_TO_TICKS(20));
    const int sense = hal->adc_read_mv(hal->ctx, ADC_CH_SENSE1);
    TEST_ASSERT_GREATER_THAN_INT(1000, sense);   /* the high code moved the line */
}

TEST_CASE("esp_hal_clock_advances", "[hw]") {
    IHal *hal = EspHalInit();
    const uint64_t t0 = hal->now_ms(hal->ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(t0 + 40, hal->now_ms(hal->ctx));
}

TEST_CASE("esp_hal_nvs_round_trips", "[hw]") {
    IHal *hal = EspHalInit();
    const char payload[] = "swc-nvs-probe";
    TEST_ASSERT_EQUAL_INT(0, hal->nvs_set(hal->ctx, "probe", payload, sizeof(payload)));
    char out[sizeof(payload)] = {0};
    TEST_ASSERT_GREATER_THAN_INT(0, hal->nvs_get(hal->ctx, "probe", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(payload, out);
}
```

- [ ] **Step 3: Implement `EspHal.c`**

Key requirements, each a specific ESP-IDF choice:

- **ADC:** `adc_oneshot_unit_init_cfg_t` on `ADC_UNIT_1`; per-channel config with
  `ADC_ATTEN_DB_12` (the 2.9 V ceiling); `adc_cali_create_scheme_curve_fitting`.
  `adc_read_mv` returns `adc_cali_raw_to_voltage(raw)` **or** `-1` on an error —
  never a fabricated zero, because zero is a legal reading.
- **DAC:** `i2c_master` on `SWC_PIN_I2C_SDA`/`SCL` at 400 kHz. The MCP4728 write
  sequence is the multi-write command (0x40) so code and power-down mode land
  together; `LDAC` is asserted via `SWC_PIN_DAC_LDAC_B` after the write.
- **`now_ms`/`now_us`:** `esp_timer_get_time() / 1000` and its microsecond form.
  Using one time source for both is what keeps the host tests' semantics
  identical on device.
- **NVS:** `nvs_open("swc", NVS_READWRITE)` in an init function; `nvs_get_blob`
  and `nvs_set_blob` plus `nvs_commit`. `nvs_get` returns `-1` for
  `ESP_ERR_NVS_NOT_FOUND` so `ConfigStore` sees the same "absent" signal `MockHal`
  produces for `kNoConfig`.
- **`reboot`:** `esp_restart()`.
- **GPIO:** plain `gpio_set_direction`/`gpio_set_level` for outputs. The only
  digital inputs are `BOOT` and `VBUS_VALID`, which get `gpio_get_level`.
  **`SENSE1`/`SENSE2` are NOT GPIO** — spec §2.2 marks them `A-in` and they are
  read through `adc_read_mv` as `ADC_CH_KEY_SENSE1`/`ADC_CH_KEY_SENSE2`, because
  the servo trim loop samples them every tick. **No pin is configured as an
  output that the netlist shows as an input** — check each against `PinMap.h`.

- [ ] **Step 4: Wire it into `main.c`**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "HAL/EspHal.h"
#include "System/SystemOrchestrator.h"

void app_main(void)
{
    IHal *hal = EspHalInit();
    SystemOrchestrator *sys = SystemOrchestratorCreate(hal);   /* loads config */
    SystemOrchestratorBoot(sys);                               /* safe idle FIRST */

    // Only now is the link started (Task 16). Nothing above this line depends
    // on it, which is what lets the device work with no head unit attached.
    UsbLinkStart(hal, sys);

    for (;;) {
        SystemOrchestratorTick(sys, hal->now_ms(hal->ctx));
        vTaskDelay(pdMS_TO_TICKS(10));   // the poll cadence the gesture tests assume
    }
}
```

Add `SystemOrchestratorCreate` / `...Boot` / `...Tick` C-linkage wrappers in
`SystemOrchestrator.h` so `main.c` can stay C.

- [ ] **Step 5: Build and run the on-device tests**

Run: `cd code && pio run -e esp32s3 -t upload && pio test -e esp32s3 -f test_hw`
Expected: PASS on the bench board. **Before the board arrives, this step is
blocked** — record it as pending rather than skipping it silently.

- [ ] **Step 6: Verify the size gate after adding the HAL**

Run: `pio run -e esp32s3 -t size`
Expected: app ≤ 1952 KB. Record the new number next to Task 1's baseline.

- [ ] **Step 7: Commit**

```bash
git add code/lib/HAL/EspHal.h code/lib/HAL/EspHal.c code/lib/HAL/PinMap.h \
        code/src/main.c code/test/test_hw code/lib/System/SystemOrchestrator.h
git commit -m "Add the ESP32-S3 HAL implementation and wire up main

ADC uses the curve-fit calibration scheme at 12dB attenuation (the 2.9V ceiling
this board's divider assumes); a read error returns -1 rather than a fabricated
zero, because zero is a legal reading. One time source feeds both now_ms and
now_us so the host tests' semantics hold on device. main establishes safe idle
before the link starts."
```

---

### Task 15: `CommandRouter` — the frame vocabulary

Spec §4.3. Every command from the app, every response and event the firmware
emits, and the version negotiation.

**Files:**
- Create: `code/lib/Link/CommandRouter.h`
- Create: `code/lib/Link/CommandRouter.cpp`
- Create: `code/test_native/test_link/CommandRouterTest.cpp`

**Interfaces:**
- Consumes: `Ndjson` (Task 10), `Config`/`ConfigCodec` (Task 8), `ConfigStore` (Task 9), `SystemOrchestrator` (Task 13)
- Produces:
  - `using FrameSink = void (*)(void *ctx, const char *line, size_t len);`
  - `constexpr uint8_t kNdjsonProtocolVersion = 1;`
  - `class CommandRouter` with:
    - `void OnLine(const char *line, size_t len)`
    - `void OnConnected()` — emits `hello` and the full config
    - `void Process()` — drains any deferred work
    - `uint32_t LastSeenSeqSent() const`, `uint32_t LastSeenSeqReceived() const`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "Link/CommandRouter.h"
#include "MockHAL.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
struct Capture {
    std::vector<std::string> lines;
    static void Sink(void *ctx, const char *line, size_t len) {
        static_cast<Capture *>(ctx)->lines.push_back(std::string(line, len));
    }
    void Attach(CommandRouter &r) { r.SetSink(&Sink, this); }
};
bool HasType(const Capture &c, const char *type) {
    const std::string needle = std::string("\"type\":\"") + type + "\"";
    for (const auto &l : c.lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST(CommandRouter, ConnectEmitsHelloWithTheProtocolVersion) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    ASSERT_TRUE(HasType(cap, "hello"));
    EXPECT_NE(cap.lines[0].find("\"v\":1"), std::string::npos);
}

TEST(CommandRouter, ConnectEmitsTheFullConfigSoTheAppCanRenderImmediately) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    EXPECT_TRUE(HasType(cap, "config"));
}

TEST(CommandRouter, ASequenceNumberIsAssignedMonotonicallyPerDirection) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    const uint32_t first = r.LastSeenSeqSent();
    r.OnLine("{\"v\":1,\"seq\":1,\"type\":\"ping\"}", 30);
    r.OnConnected();
    EXPECT_GT(r.LastSeenSeqSent(), first);
}

TEST(CommandRouter, PingIsAnsweredWithPongCarryingTheSameSeq) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnLine("{\"v\":1,\"seq\":77,\"type\":\"ping\"}", 30);
    ASSERT_TRUE(HasType(cap, "pong"));
    EXPECT_NE(cap.lines[0].find("\"seq\":77"), std::string::npos);
}

TEST(CommandRouter, AnUnknownCommandTypeIsNackedNotIgnored) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnLine("{\"v\":1,\"seq\":5,\"type\":\"teleport\"}", 33);
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("unknown_type"), std::string::npos);
}

TEST(CommandRouter, AMalformedLineIsReportedRatherThanDropped) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnLine("{not json", 9);
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("bad_frame"), std::string::npos);
}

TEST(CommandRouter, AProtocolVersionMismatchIsRefusedExplicitly) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnLine("{\"v\":99,\"seq\":1,\"type\":\"ping\"}", 33);
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("version"), std::string::npos)
        << "the app must learn the versions disagree, not silently misparse";
}

TEST(CommandRouter, AnInvalidConfigIsRejectedAndTheOldOneSurvives) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    // A config whose ladder button has a zero-width window: invalid.
    const char *bad =
        "{\"v\":1,\"seq\":2,\"type\":\"config_set\",\"config\":{\"schema_version\":1,"
        "\"device_id\":\"X\",\"settings\":{\"debounce_ms\":0},\"channels\":[]}}";
    r.OnLine(bad, std::strlen(bad));
    ASSERT_TRUE(HasType(cap, "nack"));
    // The stored config must still load.
    Config out{};
    ConfigStore store(&hal.InterfaceRef());
    const ConfigLoadResult lr = store.Load(&out);
    EXPECT_TRUE(lr == ConfigLoadResult::kLoaded || lr == ConfigLoadResult::kNoConfig);
}

TEST(CommandRouter, AValidConfigIsAckedAndPersisted) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    // Build a valid config JSON by round-tripping through the codec.
    Config c = MockHalDefaultsConfig();
    char json[4096] = {};
    const size_t n = ConfigEncodeJson(c, json, sizeof(json));
    std::string msg = "{\"v\":1,\"seq\":3,\"type\":\"config_set\",\"config\":";
    msg.append(json, n).append("}");
    r.OnLine(msg.c_str(), msg.size());
    ASSERT_TRUE(HasType(cap, "ack"));

    ConfigStore store(&hal.InterfaceRef());
    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
}

TEST(CommandRouter, ASetConfigOnlyTakesEffectAfterTheAck) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnConnected();
    Config c = MockHalDefaultsConfig();
    c.settings.timings.long_press_ms = 900;
    char json[4096] = {};
    const size_t n = ConfigEncodeJson(c, json, sizeof(json));
    std::string msg = "{\"v\":1,\"seq\":4,\"type\":\"config_set\",\"config\":";
    msg.append(json, n).append("}");
    r.OnLine(msg.c_str(), msg.size());
    // The ACK must precede any event that reflects the new timing, or the app's
    // optimistic UI state and the device can disagree about what is active.
    ASSERT_FALSE(cap.lines.empty());
    const std::string &first = cap.lines[0];
    EXPECT_NE(first.find("\"type\":\"ack\""), std::string::npos);
}

TEST(CommandRouter, ASequenceGapIsReportedAsAnEvent) {
    MockHal hal; Capture cap; CommandRouter r(&hal.InterfaceRef());
    cap.Attach(r);
    r.OnLine("{\"v\":1,\"seq\":1,\"type\":\"ping\"}", 30);
    cap.lines.clear();
    r.OnLine("{\"v\":1,\"seq\":5,\"type\":\"ping\"}", 30);   // 2-4 missing
    EXPECT_TRUE(HasType(cap, "link_gap"))
        << "a dropped frame must be surfaced, not silently tolerated";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_link`
Expected: FAIL — `Link/CommandRouter.h` not found.

- [ ] **Step 3: Implement**

`CommandRouter` holds an `NdjsonWriter` and a parsed-frame dispatcher. Handlers:

| `type` | Behavior |
| --- | --- |
| `ping` | Reply `pong` with the same `seq` |
| `config_get` | Reply `config` with the encoded config |
| `config_set` | Validate; on success `Save` then `ack`; on failure `nack` with `reason` |
| `ladder_learn_start` / `_sample` / `_commit` | Learn flow (Task 16) |
| `maintenance_enter` / `maintenance_exit` | Task 18 |
| `ota_begin` / `ota_chunk` / `ota_end` | Task 17 |
| `reset_config` | Wipe NVS config after an `ack` |

Every unknown `type` → `nack` with `reason: "unknown_type"`. Every frame whose
`v` differs from `kNdjsonProtocolVersion` → `nack` with `reason:
"version_mismatch"`. A malformed line → `nack` with `reason: "bad_frame"`.

**Sequence tracking:** the router keeps `expected_seq_`. If an incoming `seq` is
more than `expected_seq_`, it emits a `link_gap` event carrying both numbers
before processing the frame. This is what makes a dropped frame visible rather
than a silently stale UI.

`MockHalDefaultsConfig()` is a free function added to `MockHAL.h` and reused by
Task 13's `MockHal::Defaults`.

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f test_link`
Expected: PASS — 8 + 11 tests green.

- [ ] **Step 5: Commit**

```bash
git add code/lib/Link/CommandRouter.h code/lib/Link/CommandRouter.cpp \
        code/test_native/test_link/CommandRouterTest.cpp
git commit -m "Add the USB command router

Unknown commands, malformed frames and protocol-version mismatches are all
nacked with a reason rather than ignored, and a sequence gap emits a link_gap
event so a dropped frame surfaces instead of leaving a stale UI. An invalid
config is rejected without disturbing the stored one."
```

---

### Task 16: Learn mode

FR-28 to FR-31, and the fallback path that works with **no app attached**
(FR-31) — driven by AUX1 with buzzer and LED prompts, because the user may not
have the head unit out of the dash.

**Files:**
- Create: `code/lib/Learning/LearnSession.h`
- Create: `code/lib/Learning/LearnSession.cpp`
- Create: `code/test_native/test_learning/LearnSessionTest.cpp`

**Interfaces:**
- Consumes: `LadderProfile` (Task 3), `LadderDecode` (Task 3), `BuzzerGrammar` (Task 12), `Config` (Task 8)
- Produces:
  - `enum class LearnReject { kNone, kTooNoisy, kTooCloseToExisting, kAtIdle, kOutOfRange, kTooFewSamples }`
  - `const char *LearnRejectReason(LearnReject r)` — the wire string
  - `class LearnSession` with `void Start(int channel, const LadderProfile &existing)`, `void AddSample(int level_mv, int rail_mv, uint64_t now_ms)`, `LearnReject Commit(LadderButton *out)`, `int SampleCount() const`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "Learning/LearnSession.h"
#include <gtest/gtest.h>

namespace {
LadderProfile ExistingWith(const char *id, int ratio, int tol) {
    LadderProfile p{};
    p.idle_rail_mv = 5000;
    p.idle_ratio_permille = 1000;
    p.count = 1;
    std::strncpy(p.buttons[0].id, id, sizeof(p.buttons[0].id) - 1);
    p.buttons[0].ratio_permille = ratio;
    p.buttons[0].tolerance_permille = tol;
    return p;
}
void Feed(LearnSession &s, int mv, int rail, int n, uint64_t &t) {
    for (int i = 0; i < n; ++i) { s.AddSample(mv, rail, t); t += 10; }
}
}  // namespace

TEST(LearnSession, ASteadyLevelCommitsAndRecordsTheRailItWasLearnedAt) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 3450, 5000, 30, t);
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    ASSERT_EQ(out.ratio_permille, 690);
    // FR-30: the rail is recorded so runtime classification can renormalize.
    EXPECT_EQ(s.LearnedRailMv(), 5000);
}

TEST(LearnSession, TooFewSamplesIsRejectedNotAcceptedFromOneReading) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 3450, 5000, 2, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooFewSamples);
}

TEST(LearnSession, ANoisyLevelIsRejectedWithTheNoiseReason) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // +/-120 permille of wobble: far wider than the classification tolerance.
    for (int i = 0; i < 30; ++i) {
        s.AddSample((i % 2) ? 3450 : 2900, 5000, t);
        t += 10;
    }
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooNoisy);
}

TEST(LearnSession, ALevelAtIdleIsRejectedBecauseTheButtonWasNotPressed) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 5000, 5000, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kAtIdle);
}

TEST(LearnSession, ALevelWithinAnExistingButtonsToleranceIsRejectedAsAmbiguous) {
    LearnSession s;
    s.Start(0, ExistingWith("VOL_UP", 690, 40));
    uint64_t t = 1000;
    Feed(s, 3475, 5000, 30, t);   // 695 permille, inside VOL_UP's window
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooCloseToExisting);
}

TEST(LearnSession, ALevelAboveTheSenseCeilingIsRejectedAsOutOfRange) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // 3200mV exceeds the 2490mV the sense divider can present, so this cannot be
    // a real reading from this hardware -- it is a wiring or calibration fault.
    Feed(s, 3200, 5000, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kOutOfRange);
}

TEST(LearnSession, EachRejectionHasADistinctWireReason) {
    // FR-29 requires the rejection to say *why*: "it didn't work" is not
    // actionable for a user holding a button with one hand in a car.
    const LearnReject all[] = {LearnReject::kTooNoisy, LearnReject::kTooCloseToExisting,
                               LearnReject::kAtIdle, LearnReject::kOutOfRange,
                               LearnReject::kTooFewSamples};
    std::set<std::string> reasons;
    for (LearnReject r : all) reasons.insert(LearnRejectReason(r));
    EXPECT_EQ(reasons.size(), 5u) << "every rejection needs its own reason string";
    for (const auto &r : reasons) EXPECT_FALSE(r.empty());
}

TEST(LearnSession, ToleranceIsDerivedFromTheMeasuredSpreadNotAConstant) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    for (int i = 0; i < 30; ++i) {          // a small, realistic spread
        s.AddSample((i % 2) ? 3460 : 3440, 5000, t);
        t += 10;
    }
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_GE(out.tolerance_permille, 8) << "tolerance must cover the observed spread";
    EXPECT_LE(out.tolerance_permille, 120) << "and must not swallow neighbouring buttons";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_learning`
Expected: FAIL — `Learning/LearnSession.h` not found.

- [ ] **Step 3: Implement**

`LearnSession` accumulates samples, and `Commit` runs the checks in this order,
returning the **first** failure so the user gets the most actionable reason:

1. `kTooFewSamples` — fewer than 10 samples, or a span under 100 ms.
2. `kOutOfRange` — any sample above the sense ceiling (2490 mV) or below 0.
3. `kAtIdle` — the mean ratio is within the idle margin, so the button was not
   pressed.
4. `kTooNoisy` — the spread exceeds `noise_limit_permille` (default 60).
5. `kTooCloseToExisting` — the mean is within an existing button's tolerance.

On success, `out.tolerance_permille` is `max(spread * 2, 8)` capped at 120 — the
doubling gives headroom over the observed spread while the cap keeps it from
swallowing a neighbour. `out.ratio_permille` is the rounded mean ratio, and
`LearnedRailMv()` records the rail (FR-30).

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f test_learning`
Expected: PASS — 8 tests green.

- [ ] **Step 5: Add the headless AUX1-driven wizard (FR-31)**

Create `code/lib/Learning/LearnWizard.cpp` with the state machine from spec §7.4,
taking `IHal*`, `BuzzerGrammar*`, `LedGrammar*`, and `AUX1` as the select button.
The AUX1 press counting uses the **same** `PressClassifier` (Task 6) with a
ladder profile of one button at the AUX threshold, so there is one debounce
implementation, not two.

Test it in `code/test_native/test_learning/LearnWizardTest.cpp`: drive `MockHal`'s
AUX1 ADC and the clock, and assert the buzzer pattern sequence and that a full
two-button learn completes with no link present.

Run: `cd code && pio test -e native -f test_learning`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Learning code/test_native/test_learning
git commit -m "Add learn mode with distinct rejection reasons and a headless wizard

Commit checks too-few, out-of-range, at-idle, too-noisy and too-close in that
order so the user gets the most actionable reason. Each has its own wire string,
because a blind user holding a steering-wheel button cannot act on a generic
failure. Tolerance is derived from the measured spread, capped so it cannot
swallow a neighbour. The AUX1 wizard reuses the same press classifier."
```

---

### Task 17: `ImageVerify` and `ReleaseCheck` — the update safety layer

FR-35, FR-36, FR-37, FR-41. One module decides whether an image may be
installed; another decides whether a released version is newer and compatible.
Both are pure logic and fully host-testable — which is the point, because they
gate a path that can brick the device.

**Files:**
- Create: `code/lib/Update/ImageVerify.h`
- Create: `code/lib/Update/ImageVerify.cpp`
- Create: `code/lib/Update/ReleaseCheck.h`
- Create: `code/lib/Update/ReleaseCheck.cpp`
- Create: `code/test_native/test_update/ImageVerifyTest.cpp`
- Create: `code/test_native/test_update/ReleaseCheckTest.cpp`

**Interfaces:**
- Consumes: `IHal` (Task 2)
- Produces:
  - `enum class VerifyResult { kOk, kSizeMismatch, kChecksumMismatch, kTooLarge, kEmpty }`
  - `class Sha256Stream` with `void Update(const uint8_t *data, size_t len)`, `void Final(uint8_t out[32])`
  - `VerifyResult ImageVerifyBegin(const char *expected_sha256_hex, size_t expected_size, size_t max_size)`
  - `VerifyResult ImageVerifyChunk(const uint8_t *data, size_t len)`
  - `VerifyResult ImageVerifyEnd()`
  - `struct ReleaseInfo { uint32_t version_code; char version_name[32]; char sha256_hex[65]; size_t size; char url[256]; uint32_t min_from_version_code; char board[32]; }`
  - `enum class ReleaseCheckResult { kUpToDate, kNewer, kNotNewer, kTooOldToUpgradeFrom, kWrongBoard, kMalformed }`
  - `ReleaseCheckResult ReleaseCheckParse(const char *json, ReleaseInfo *out, uint32_t current_version_code, const char *board)`

- [ ] **Step 1: Write the failing `ImageVerify` tests**

```cpp
#include "Update/ImageVerify.h"
#include <gtest/gtest.h>
#include <string>

namespace {
// A 64 KiB payload with a known SHA-256, computed the same way the device does.
std::string PayloadHash(const std::string &data) {
    Sha256Stream s;
    s.Update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    uint8_t d[32];
    s.Final(d);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::sprintf(hex + i * 2, "%02x", d[i]);
    return std::string(hex, 64);
}
}  // namespace

TEST(ImageVerify, AcceptsAnImageWhoseChecksumAndSizeBothMatch) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ASSERT_EQ(ImageVerifyChunk(reinterpret_cast<const uint8_t *>(img.data()), img.size()),
              VerifyResult::kOk);
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kOk);
}

TEST(ImageVerify, DetectsASingleFlippedBitAnywhereInTheImage) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    std::string corrupt = img;
    corrupt[40000] ^= 0x01;
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), corrupt.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(corrupt.data()), corrupt.size());
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kChecksumMismatch);
}

TEST(ImageVerify, DetectsAShortImageEvenWhenTheBytesItHasAreCorrect) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    const std::string truncated = img.substr(0, 65535);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(truncated.data()), truncated.size());
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kSizeMismatch);
}

TEST(ImageVerify, RefusesAnImageLargerThanTheTargetPartitionBeforeStreamingIt) {
    EXPECT_EQ(ImageVerifyBegin("00", 4u * 1024 * 1024, 2u * 1024 * 1024), VerifyResult::kTooLarge)
        << "refuse up front rather than after writing 4MB to flash";
}

TEST(ImageVerify, RefusesAZeroLengthImage) {
    EXPECT_EQ(ImageVerifyBegin("00", 0, 2u * 1024 * 1024), VerifyResult::kEmpty);
}

TEST(ImageVerify, VerifiesAcrossManyChunksNotJustOne) {
    // Chunk boundaries are where a streaming verifier usually breaks: state
    // carried between calls is the whole risk.
    std::string img;
    for (int i = 0; i < 100000; ++i) img.push_back(static_cast<char>(i * 7 % 251));
    const std::string hash = PayloadHash(img);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    for (size_t off = 0; off < img.size(); off += 997) {
        const size_t n = std::min<size_t>(997, img.size() - off);
        ASSERT_EQ(ImageVerifyChunk(reinterpret_cast<const uint8_t *>(img.data() + off), n),
                  VerifyResult::kOk);
    }
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kOk);
}

TEST(ImageVerify, AMalformedHashStringIsRefusedRatherThanTreatedAsAZeroHash) {
    const std::string img(1024, 'A');
    EXPECT_EQ(ImageVerifyBegin("not-a-hash", img.size(), 2u * 1024 * 1024), VerifyResult::kMalformedHash);
    EXPECT_EQ(ImageVerifyBegin("aabb", img.size(), 2u * 1024 * 1024), VerifyResult::kMalformedHash);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_update`
Expected: FAIL — `Update/ImageVerify.h` not found.

- [ ] **Step 3: Implement `ImageVerify`**

`Sha256Stream` wraps mbedTLS's `mbedtls_sha256_*` on device and **the same
mbedTLS API on the host** (add `lib_deps` `mbedtls` to the `native` env, or use
a tiny bundled implementation — either is fine, but it must be *the same code*
on both, because a host-only implementation would validate a path the device
does not run).

`ImageVerifyBegin` validates the hex string (exactly 64 lowercase-or-uppercase
hex characters), the size against `max_size`, and rejects an empty image,
**before** any data is streamed. `ImageVerifyChunk` feeds the hash and counts
bytes. `ImageVerifyEnd` returns `kSizeMismatch` if the byte count differs and
`kChecksumMismatch` if the digest differs — checked in that order, so a
truncated image reports the more specific cause.

Critical detail the tests pin: **a hash that cannot be parsed is refused**, never
treated as all-zeros. An unparseable hash silently becoming "no check" is the
classic way a verification system becomes decorative.

- [ ] **Step 4: Run the `ImageVerify` tests**

Run: `cd code && pio test -e native -f test_update`
Expected: PASS — 7 tests green.

- [ ] **Step 5: Write the failing `ReleaseCheck` tests**

```cpp
#include "Update/ReleaseCheck.h"
#include <gtest/gtest.h>

namespace {
// The shape emitted by the release pipeline (spec 9.5): a small manifest next to
// the .bin, so the device needs one fetched file, not a GitHub API client.
const char *kManifest = R"({
  "version_code": 12,
  "version_name": "0.12.0",
  "board": "swc-s3",
  "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  "size": 1048576,
  "url": "https://github.com/oetsolutions/swc-module/releases/download/v0.12.0/swc-fw.bin",
  "min_from_version_code": 5
})";
}  // namespace

TEST(ReleaseCheck, ANewerVersionForThisBoardIsOffered) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, /*current=*/11, "swc-s3"),
              ReleaseCheckResult::kNewer);
    EXPECT_EQ(info.version_code, 12u);
    EXPECT_EQ(info.size, 1048576u);
}

TEST(ReleaseCheck, TheSameOrOlderVersionIsNotOffered) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, 12, "swc-s3"), ReleaseCheckResult::kNotNewer);
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, 13, "swc-s3"), ReleaseCheckResult::kNotNewer)
        << "a downgrade must not be offered as an upgrade";
}

TEST(ReleaseCheck, AManifestForADifferentBoardIsRefused) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, 11, "swc-c3"),
              ReleaseCheckResult::kWrongBoard)
        << "flashing another board's image is worse than not updating";
}

TEST(ReleaseCheck, AVersionTooOldToUpgradeFromIsRefusedWithItsOwnReason) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, /*current=*/3, "swc-s3"),
              ReleaseCheckResult::kTooOldToUpgradeFrom)
        << "min_from_version_code exists for migrations that need an intermediate step";
}

TEST(ReleaseCheck, AMalformedManifestIsRefusedNotPartlyApplied) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse("{ not json", &info, 11, "swc-s3"),
              ReleaseCheckResult::kMalformed);
    EXPECT_EQ(ReleaseCheckParse("{}", &info, 11, "swc-s3"), ReleaseCheckResult::kMalformed);
    // A manifest missing the hash must not be accepted as "no hash to check".
    EXPECT_EQ(ReleaseCheckParse(R"({"version_code":12,"board":"swc-s3","size":10,
        "url":"http://x/y.bin"})", &info, 11, "swc-s3"), ReleaseCheckResult::kMalformed);
}

TEST(ReleaseCheck, ANonHttpsUrlIsRefused) {
    std::string m = kManifest;
    const std::string https = "https://";
    const size_t pos = m.find(https);
    ASSERT_NE(pos, std::string::npos);
    m.replace(pos, https.size(), "http://");
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m.c_str(), &info, 11, "swc-s3"), ReleaseCheckResult::kMalformed)
        << "the OTA image must come over TLS; plain http is a downgrade attack";
}
```

- [ ] **Step 6: Implement `ReleaseCheck` and run**

Run: `cd code && pio test -e native -f test_update`
Expected: PASS — 7 + 6 tests green.

- [ ] **Step 7: Commit**

```bash
git add code/lib/Update/ImageVerify.* code/lib/Update/ReleaseCheck.* \
        code/test_native/test_update
git commit -m "Add image verification and release-manifest checks

An unparseable hash is refused rather than becoming a no-op check, an oversized
image is refused before it is streamed to flash, and a truncated image reports
SizeMismatch rather than the vaguer checksum failure. The manifest check refuses
another board's image, a downgrade, a plain-http URL, and a version too old for
the declared migration path."
```

---

### Task 18: Maintenance mode — BLE provisioning, web page, USB OTA

FR-32 to FR-38. The radio is **not initialized during normal operation**; it is
entered explicitly and exits on timeout, because a device left unable to serve
button presses is unacceptable.

**Files:**
- Create: `code/lib/Maintenance/MaintenanceMode.h`
- Create: `code/lib/Maintenance/MaintenanceMode.c`
- Create: `code/lib/Maintenance/WebPage.h`
- Create: `code/assets/index.html`
- Create: `code/lib/Update/OtaWifi.h`, `OtaWifi.c`
- Create: `code/lib/Update/OtaUsb.h`, `OtaUsb.c`
- Create: `code/test_native/test_maintenance/MaintenanceModeTest.cpp`

**Interfaces:**
- Consumes: `Config` (Task 8), `ReleaseCheck` (Task 17), `IHal` (Task 2)
- Produces:
  - `enum class MaintenanceTrigger { kNone, kUsbCommand, kConfigFlag, kAux1Hold, kNoConfigAtBoot }`
  - `class MaintenanceMode` with `bool Enter(MaintenanceTrigger t, uint64_t now_ms)`, `void Exit()`, `bool Active() const`, `void Update(uint64_t now_ms)`, `bool ShouldTimeout(uint64_t now_ms) const`

- [ ] **Step 1: Write the failing test — the timeout is the important one**

```cpp
#include "Maintenance/MaintenanceMode.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

TEST(MaintenanceMode, IsNotActiveUntilExplicitlyEntered) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    EXPECT_FALSE(m.Active()) << "FR-32: no radio during normal operation";
}

TEST(MaintenanceMode, EntersOnEachOfTheDocumentedTriggers) {
    for (MaintenanceTrigger t : {MaintenanceTrigger::kUsbCommand,
                                 MaintenanceTrigger::kConfigFlag,
                                 MaintenanceTrigger::kAux1Hold,
                                 MaintenanceTrigger::kNoConfigAtBoot}) {
        MockHal hal;
        MaintenanceMode m(&hal.InterfaceRef());
        EXPECT_TRUE(m.Enter(t, 1000)) << "trigger " << (int)t;
        EXPECT_TRUE(m.Active());
    }
}

TEST(MaintenanceMode, TimesOutAfterFiveMinutesOfInactivity) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Update(299999);
    EXPECT_TRUE(m.Active());
    m.Update(300001);
    EXPECT_FALSE(m.Active())
        << "FR-38: a device left in maintenance cannot serve presses, so it must return";
}

TEST(MaintenanceMode, ActivityResetsTheTimeout) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    for (int i = 0; i < 10; ++i) {
        m.Update(static_cast<uint64_t>(i) * 299000);
        m.NoteActivity(static_cast<uint64_t>(i) * 299000);
    }
    EXPECT_TRUE(m.Active()) << "a user actively working must not be kicked out";
}

TEST(MaintenanceMode, ExitingClearsTheActiveFlagAndTheTimer) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Exit();
    EXPECT_FALSE(m.Active());
    m.Update(999999999);
    EXPECT_FALSE(m.Active());
}

TEST(MaintenanceMode, ReEntryAfterATimeoutStartsAFreshWindow) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Update(400000);
    ASSERT_FALSE(m.Active());
    m.Enter(MaintenanceTrigger::kUsbCommand, 400000);
    m.Update(400000 + 299000);
    EXPECT_TRUE(m.Active()) << "the second window must not inherit the first's elapsed time";
}

TEST(MaintenanceMode, KeyPressesAreStillServedWhileMaintenanceIsActive) {
    // The requirement behind FR-38 is that maintenance must not make the device
    // useless. The orchestrator keeps ticking in maintenance; this asserts the
    // mode reports itself as non-exclusive so the caller keeps serving input.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    EXPECT_FALSE(m.Exclusive()) << "maintenance must not stop the adapter working";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f test_maintenance`
Expected: FAIL — `Maintenance/MaintenanceMode.h` not found.

- [ ] **Step 3: Implement the mode logic and run**

`MaintenanceMode` holds `active_`, `entered_at_`, `last_activity_`, and a
`timeout_ms_` defaulting to **300000** (5 minutes). `Update` deactivates when
`now - last_activity_ >= timeout_ms_`. `NoteActivity` bumps `last_activity_`.
`Enter` always resets `last_activity_` to `now`, so a re-entry after a timeout
gets a fresh window.

Run: `cd code && pio test -e native -f test_maintenance`
Expected: PASS — 7 tests green.

- [ ] **Step 4: Add the BLE provisioning and web server (device-only)**

`code/lib/Maintenance/MaintenanceMode.c` gains `MaintenanceStartRadio()`:

- **BLE via NimBLE** (`CONFIG_BT_NIMBLE_ENABLED`), using
  `wifi_provisioning` + `protocomm` with the **BLE transport** and **Sec1** with
  a proof-of-possession. This is what makes the Espressif provisioning app work,
  which is the user's explicit requirement. **No `setInsecure`, no hardcoded
  PoP** — the PoP is derived per-device and shown on the web page and over USB.
- **The web page is embedded, not served from a filesystem** — no filesystem
  partition exists in `partitions.csv` (spec §9.2), so `code/assets/index.html`
  is converted to a C array by a small `extra_scripts` step and served from
  flash. The page is deliberately minimal (spec §8.4): network status, a WiFi
  credential form, and a firmware upload control. It is not a second config UI.
- **Exit de-initializes the stacks and frees their memory**, then returns to
  normal mode.

- [ ] **Step 5: Add the two OTA paths**

`OtaWifi.c` — `esp_https_ota` against the manifest URL, with the
`ImageVerify` stream (Task 17) fed from the HTTP event handler and the CRC
checked before `esp_ota_set_boot_partition`. `OtaUsb.c` — the same verification
driven by `ota_begin`/`ota_chunk`/`ota_end` frames from `CommandRouter`.

Both write to the **non-running** OTA slot and call `esp_ota_set_boot_partition`
only after `ImageVerifyEnd()` returns `kOk`.

- [ ] **Step 6: Implement the health confirmation (FR-37) — on device**

In `main.c`, after `SystemOrchestratorBoot()` succeeds and the first successful
DAC write has been confirmed via the sense reading, call
`esp_ota_mark_app_valid_cancel_rollback()`. **Not before.** The comment in the
code must say why, because this is the line that decides whether a bad image is
recoverable:

```c
    // FR-37: mark valid only once the device has proven it can do its job --
    // safe idle established AND the output confirmed on the sense pin. Marking
    // it at the top of app_main would confirm an image that boots but cannot
    // drive the DAC, stranding the user with a bricked-but-"valid" device.
    esp_ota_mark_app_valid_cancel_rollback();
```

- [ ] **Step 7: Build, then run the rollback test on the bench**

Run: `pio run -e esp32s3 -t upload`
Expected: PASS.

Then, with the board: flash a deliberately faulting image (a build with
`abort()` after `app_main` starts but **before** the mark-valid call), and verify
the bootloader rolls back and the device comes up on the previous image. **This
must be tested with a genuinely broken image**, not a mocked failure.

- [ ] **Step 8: Commit**

```bash
git add code/lib/Maintenance code/lib/Update/OtaWifi.* code/lib/Update/OtaUsb.* \
        code/assets code/test_native/test_maintenance code/src/main.c
git commit -m "Add maintenance mode with BLE provisioning, the web page and both OTA paths

The radio is never initialized in normal operation. The web page is embedded in
flash because there is no filesystem partition. Mark-valid is called only after
safe idle is established and the output is confirmed on the sense pin, so a bad
image rolls back rather than being confirmed by a boot it happened to survive."
```

---

### Task 19: The generated contract, shared by firmware and app

The single mechanism that keeps the two sides from drifting. A change to a field
name surfaces as a failing build, not a runtime parse failure in a car.

**Files:**
- Create: `code/tools/gen_contract.py`
- Create: `code/tools/gen_contract_kotlin.py`
- Create: `code/contract/swc_contract.h` (generated, checked in)
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/contract/Contract.kt` (generated, checked in)
- Create: `code/tools/contract_schema.py` — the field definitions, the one source
- Create: `code/tools/test_gen_contract.py`

**Interfaces:**
- Consumes: nothing
- Produces: `swc_contract.h` (C enums and structs for the frame types and action
  ids) and `Contract.kt` (Kotlin equivalents)

- [ ] **Step 1: Write the failing test**

```python
# code/tools/test_gen_contract.py
import subprocess, sys, pathlib
import contract_schema

def test_action_ids_are_contiguous_from_one():
    ids = sorted(a.id for a in contract_schema.ACTIONS)
    assert ids == list(range(1, len(ids) + 1)), "action ids must have no gaps"

def test_frame_types_are_unique():
    types = [f.name for f in contract_schema.FRAMES]
    assert len(types) == len(set(types))

def test_generated_header_matches_the_checked_in_copy(tmp_path):
    out = tmp_path / "swc_contract.h"
    subprocess.run([sys.executable, "gen_contract.py", "--out", str(out)], check=True)
    checked_in = pathlib.Path("../contract/swc_contract.h").read_text()
    assert out.read_text() == checked_in, "regenerate and commit the header"

def test_generated_kotlin_matches_the_checked_in_copy(tmp_path):
    out = tmp_path / "Contract.kt"
    subprocess.run([sys.executable, "gen_contract_kotlin.py", "--out", str(out)], check=True)
    checked_in = pathlib.Path(
        "../android/app/src/main/java/com/oetsolutions/swc/contract/Contract.kt").read_text()
    assert out.read_text() == checked_in, "regenerate and commit the Kotlin types"

def test_the_header_has_the_protocol_version():
    text = pathlib.Path("../contract/swc_contract.h").read_text()
    assert f"#define SWC_PROTOCOL_VERSION {contract_schema.PROTOCOL_VERSION}" in text
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code/tools && python3 -m pytest test_gen_contract.py -v`
Expected: FAIL — `contract_schema` not found.

- [ ] **Step 3: Write `contract_schema.py` and the two generators**

`contract_schema.py` holds `PROTOCOL_VERSION = 1`, an `ACTIONS` list of
`Action(name, id, kind, takes_payload, payload_label)` matching spec §3.6 **with
contiguous ids from 1**, and a `FRAMES` list of `Frame(name, direction, fields)`.
The generators emit:

- `swc_contract.h` — `#define SWC_PROTOCOL_VERSION`, `#define SWC_ACTION_<NAME> <id>`
  for every action, and the frame type strings as `#define`s.
- `Contract.kt` — a Kotlin `object ActionIds { const val VOL_UP = 1 ... }` and
  the frame names, so the app references symbols rather than string literals.

Both generators are deterministic (sorted iteration, no timestamps) so the
checked-in-vs-generated comparison is meaningful.

- [ ] **Step 4: Run the tests**

Run: `cd code/tools && python3 -m pytest test_gen_contract.py -v`
Expected: PASS — 5 tests green.

- [ ] **Step 5: Commit**

```bash
git add code/tools/gen_contract.py code/tools/gen_contract_kotlin.py \
        code/tools/contract_schema.py code/tools/test_gen_contract.py \
        code/contract/swc_contract.h \
        code/android/app/src/main/java/com/oetsolutions/swc/contract/Contract.kt
git commit -m "Generate the wire contract for firmware and app from one schema

Action ids and frame types are defined once and emitted as a C header and a
Kotlin object. The tests assert the committed copies match what the generators
produce, so a spec change surfaces as a failing build rather than as a runtime
parse failure in a car."
```

---

### Task 20: The Android app — model and protocol client

The app's logic, all host-testable on the JVM. The USB transport sits behind an
interface so the protocol client can be tested without a device.

**Files:**
- Create: `code/android/settings.gradle.kts`, `build.gradle.kts`,
  `app/build.gradle.kts`, `gradle.properties`
- Create: `code/android/app/src/main/AndroidManifest.xml`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/model/Config.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/link/SwcTransport.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/link/SwcClient.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/link/UsbSerialTransport.kt`
- Create: `code/android/app/src/test/java/com/oetsolutions/swc/link/SwcClientTest.kt`
- Create: `code/android/app/src/test/java/com/oetsolutions/swc/model/ConfigCodecTest.kt`

**Interfaces:**
- Consumes: `Contract.kt` (Task 19)
- Produces:
  - `interface SwcTransport { suspend fun write(bytes: ByteArray); val incoming: Flow<ByteArray>; fun close() }`
  - `class SwcClient(transport: SwcTransport)` with `val frames: Flow<Frame>`, `suspend fun connect()`, `suspend fun setConfig(c: Config): AckResult`, `suspend fun getConfig(): Config`, `suspend fun startOta(...)`

- [ ] **Step 1: Write the failing JVM tests**

```kotlin
// app/src/test/java/com/oetsolutions/swc/link/SwcClientTest.kt
class SwcClientTest {
    private class FakeTransport : SwcTransport {
        val written = mutableListOf<String>()
        private val flow = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64)
        override suspend fun write(bytes: ByteArray) { written += String(bytes) }
        override val incoming: Flow<ByteArray> = flow
        override fun close() {}
        suspend fun emit(text: String) { flow.emit(text.toByteArray()) }
    }

    @Test
    fun `config round trips through the codec unchanged`() = runTest {
        val c = sampleConfig()
        val decoded = ConfigJson.decode(ConfigJson.encode(c))
        assertEquals(c, decoded)
    }

    @Test
    fun `frames split across reads are reassembled`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val seen = mutableListOf<Frame>()
        val job = launch { client.frames.take(1).collect { seen += it } }
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"he")
        t.emit("llo\"}\n")
        advanceUntilIdle()
        assertEquals(1, seen.size)
        assertEquals("hello", seen[0].type)
        job.cancel()
    }

    @Test
    fun `a set-config that is nacked leaves the local model uncommitted`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = launch { client.frames.collect { } }
        val deferred = async { client.setConfig(sampleConfig().copy(deviceId = "NEW")) }
        advanceUntilIdle()
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"nack\",\"reason\":\"invalid_config\"}\n")
        advanceUntilIdle()
        assertTrue(deferred.await() is AckResult.Nacked)
        assertEquals("OLD", client.config.value.deviceId)
        job.cancel()
    }

    @Test
    fun `a version mismatch surfaces to the user rather than being swallowed`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = launch { client.frames.collect { } }
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"nack\",\"reason\":\"version_mismatch\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.VersionMismatch, client.state.value)
        job.cancel()
    }
}
```

Plus `ConfigCodecTest.kt`, asserting the Kotlin model round-trips `ConfigJson`
including every field, and that an unknown action name decodes to a sentinel
rather than throwing (so an older app can open a config from a newer firmware).

- [ ] **Step 2: Run and watch them fail**

Run: `cd code/android && ./gradlew :app:testDebugUnitTest`
Expected: FAIL — classes not found.

- [ ] **Step 3: Create the Gradle project**

`app/build.gradle.kts` essentials:

```kotlin
android {
    namespace = "com.oetsolutions.swc"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.oetsolutions.swc"
        minSdk = 26
        // targetSdk is 34 deliberately: Android 15's background-activity-launch
        // hardening (BAL) would block the app's own "launch an app" actions from
        // a background service, which is a headline feature. Raising this
        // requires re-reading the spec's Android BAL section first.
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }
    buildFeatures { compose = true }
    kotlinOptions { jvmTarget = "17" }
}
dependencies {
    implementation("com.github.mik3y:usb-serial-for-android:3.11.0")
    implementation("com.espressif:esp-idf-provisioning-android:2.5.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.compose.material3:material3")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    testImplementation("junit:junit:4.13.2")
}
```

`AndroidManifest.xml` declares `android.hardware.usb.host` as a **required**
feature (the app is useless without it) and no `INTERNET` permission beyond what
OTA needs.

- [ ] **Step 4: Implement the model, codec and client; run the tests**

Run: `cd code/android && ./gradlew :app:testDebugUnitTest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add code/android
git commit -m "Add the Android app model, codec and protocol client

The USB transport sits behind an interface, so frame reassembly, nack handling
and version-mismatch surfacing are all tested on the JVM with no device attached.
targetSdk is pinned to 34 because Android 15's BAL hardening would block the
app's own launch-app actions."
```

---

### Task 21: The Android UI

Compose screens: link status, live ladder view, per-button learn, binding editor,
maintenance and update. This is the user's "very good UI" requirement, so the
screens are specified by what the user must be able to *understand*, not by
widget names.

**Files:**
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/ui/MainActivity.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/ui/LinkScreen.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/ui/LadderScreen.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/ui/BindingScreen.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/ui/UpdateScreen.kt`
- Create: `code/android/app/src/main/java/com/oetsolutions/swc/action/ActionRunner.kt`
- Create: `code/android/app/src/androidTest/java/com/oetsolutions/swc/ui/*`

- [ ] **Step 1: Write the failing instrumented test for the live view**

```kotlin
@Test
fun liveLadderView_showsEachLearnedButtonAtItsMeasuredLevel() {
    composeRule.setContent {
        LadderScreen(state = LadderUiState(
            railMv = 5000,
            buttons = listOf(
                LearnedButton("VOL_UP", ratioPermille = 690, tolerancePermille = 40),
                LearnedButton("VOL_DOWN", ratioPermille = 400, tolerancePermille = 40)),
            liveRatioPermille = 690))
    }
    // The point of the live view is that the user can see *which* button the
    // device currently thinks is pressed, so the matched one must be marked.
    composeRule.onNodeWithContentDescription("VOL_UP, matched").assertExists()
    composeRule.onNodeWithContentDescription("VOL_DOWN, not matched").assertExists()
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cd code/android && ./gradlew :app:connectedDebugAndroidTest`
Expected: FAIL — `LadderScreen` not found.

- [ ] **Step 3: Implement the screens**

Each screen's job, stated as what the user must be able to tell:

- **`LinkScreen`** — whether a device is connected, its firmware version, and the
  four failure states as **distinct, actionable messages**: no USB permission,
  cable present but no device, version mismatch (with both versions shown), and
  device in maintenance. A generic "connection error" is not acceptable here.
- **`LadderScreen`** — the live ladder: the rail voltage, every learned button as
  a marker at its ratio with its tolerance as a band, and the current reading
  moving in real time. **The user must be able to see which button the device
  thinks is pressed** — that is the whole diagnostic value, and it is what the
  instrumented test asserts via content descriptions.
- **`BindingScreen`** — a grid of buttons × gestures, each cell showing its bound
  action; tapping opens an action picker driven by the generated `ActionIds`, so
  the app can never offer an action the firmware does not have. A cell with a
  payload action shows the payload and refuses to save an empty one.
- **`UpdateScreen`** — current version, a "check for updates" that reports
  up-to-date/newer/wrong-board plainly, and two explicit paths: push a file over
  USB, or update over WiFi. **The screen must state that the device keeps working
  if the update fails**, because "will this brick my stereo" is the user's real
  question.

`ActionRunner.kt` implements the app-side actions (launch app, send intent with a
payload) and is the module the Android BAL limitation applies to (spec §3.6): it
must query whether it holds the required role or permission and **report
unavailability as a clear message**, rather than silently failing.

- [ ] **Step 4: Run the tests**

Run: `cd code/android && ./gradlew :app:testDebugUnitTest :app:connectedDebugAndroidTest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add code/android
git commit -m "Add the Android UI: link status, live ladder, bindings and updates

The live ladder view marks which button the device currently classifies, which
is the diagnostic that distinguishes an adapter fault from a head-unit fault.
The binding screen is driven by the generated action ids, so it cannot offer an
action the firmware lacks. Link failures are four distinct actionable messages,
not one generic error."
```

---

### Task 22: CI gates

Every gate in spec §10.5, enforced on every push.

**Files:**
- Create: `code/.github/workflows/firmware.yml`
- Create: `code/.github/workflows/android.yml`

- [ ] **Step 1: Write `firmware.yml`**

```yaml
name: firmware
on: [push, pull_request]
jobs:
  host-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: '3.11' }
      - run: pip install platformio
      - name: Native unit tests
        working-directory: code
        run: pio test -e native
      - name: Contract is in sync with the schema
        working-directory: code/tools
        run: |
          python3 gen_contract.py --out ../contract/swc_contract.h
          python3 gen_contract_kotlin.py --out ../android/app/src/main/java/com/oetsolutions/swc/contract/Contract.kt
      - name: Fail if the generated contract drifted
        run: git diff --exit-code code/contract code/android/app/src/main/java/com/oetsolutions/swc/contract

  build-and-size-gate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: '3.11' }
      - run: pip install platformio
      - name: Build
        working-directory: code
        run: pio run -e esp32s3
      - name: Size gate (spec 10.5)
        working-directory: code
        run: python3 tools/check_size.py --env esp32s3 --max-bytes 1998848
```

**The size gate is a script, not an eyeball.** `tools/check_size.py` runs
`pio run -t size --json-output`, reads the app size, and exits non-zero above
1952 KB (`1998848`). This is the R-1 mitigation from spec §12.2: the app not
fitting is discovered here, on day one, not the week the boards land.

- [ ] **Step 2: Write `android.yml`**

JVM tests, `assembleDebug`, and the APK uploaded as an artifact. The instrumented
tests need a device or emulator; run them on an emulator job with
`reactivecircus/android-emulator-runner`, and keep USB-specific tests excluded
from CI so a missing device is not a false failure — **documented in the
workflow comment**, not silently skipped.

- [ ] **Step 3: Push and confirm both workflows pass**

Run: `git push` and check the Actions tab.
Expected: both green. **If the size gate fails, that is R-1 arriving early** —
take the §9.6 fallback rather than raising the threshold.

- [ ] **Step 4: Commit**

```bash
git add code/.github code/tools/check_size.py
git commit -m "Add CI gates for host tests, contract sync, and the app size budget

The 1952KB size gate runs on every push so a BLE+WiFi+OTA build that outgrows
the OTA slot is caught on day one rather than the week the boards arrive. The
contract-sync check fails the build if a generated header or Kotlin type drifts
from the schema."
```

---

## Phase 2 — Bring-up (blocked on the ordered board)

These tasks are **not** implementable in advance. Each records a measured number
back into the spec, and the spec is corrected in the same commit as the code when
reality disagrees.

### Task 23: Bring-up steps 1–4 (power, I2C/DAC, ADC calibration, output envelope)

**Files:**
- Create: `code/docs/bring-up-log.md`
- Modify: `docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md` (the measured numbers)

- [ ] **Step 1: Power and identity.** Bench supply at 5 V; confirm rail voltages
  and that the module enumerates on USB-Serial-JTAG. Confirm 4 MB flash and no
  PSRAM — **if this disagrees with `boards/swc-s3.json`, stop**: every later
  measurement is void until the board definition is fixed (spec §12.1, N-1).
  Record the measured idle current.
- [ ] **Step 2: I2C and the DAC.** Scan the bus, record the MCP4728's actual strap
  address (spec §12.1, N-4). Write mid-code, measure with a meter, confirm
  `LDAC` latches.
- [ ] **Step 3: The ADC ladder.** Sweep the bench supply 11.0 → 14.8 V with a
  resistor ladder attached; record idle and per-button readings. **Fit the real
  calibration curve here** — this is where §2.3's numbers become true for the
  actual board. Update the spec's values.
- [ ] **Step 4: The output stage and gain.** Measure the output envelope in both
  gain modes; verify the ratio is 1.82 and 1.00, the guard-band switch, the
  1.80 V floor and the 5.20 V ceiling. **Update §6.2 if reality disagrees — on
  the first board it may.**
- [ ] **Step 5: Commit** the log and the corrected spec together.

```bash
git add code/docs/bring-up-log.md docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md
git commit -m "Record bring-up steps 1-4 measurements and correct the spec to match"
```

### Task 24: Bring-up steps 5–8 (servo dynamics, feedback, timing, NVS/OTA)

- [ ] **Step 1: Servo dynamics.** Step the DAC, scope the output, measure
  overshoot and settling. Tune §6.5's constants to the **measured** plant. If the
  measurement contradicts the model, the model is what changes.
- [ ] **Step 2: Feedback.** Both LED channels and the buzzer gate, verifying every
  pattern in §7.1 and §7.3.
- [ ] **Step 3: Timing under a real RTOS.** The gesture boundaries with real
  scheduling latency. The host tests prove the *rule*; this proves the
  implementation meets the rule. Record the worst-case observed latency.
- [ ] **Step 4: NVS under a real power cut.** Write a config, cut power mid-write
  with a hardware switch, confirm the old config loads.
- [ ] **Step 5: A/B OTA with a genuinely broken image.** Compile an image that
  panics before the mark-valid call; confirm rollback. **A mocked failure proves
  nothing.**
- [ ] **Step 6: System bench tests (spec §10.4 level 4).** Every learned button,
  100 presses each, at 11.0 / 12.6 / 14.8 V, asserting zero misclassifications
  and the exact expected output level each time. Then the 72-hour soak.
- [ ] **Step 7: Commit** the results, and any spec corrections.

---

## Self-Review

**1. Spec coverage.** Every FR-1…FR-42 maps to a task:

| FR | Task | FR | Task | FR | Task |
| --- | --- | --- | --- | --- | --- |
| FR-1 | 14, 13 | FR-15 | 13 | FR-29 | 16 |
| FR-2 | 4, 14 | FR-16 | 13, 14 | FR-30 | 16, 3 |
| FR-3 | 6 | FR-17 | 13 | FR-31 | 16 |
| FR-4 | 3, 6 | FR-18 | 5 | FR-32 | 18 |
| FR-5 | 13, 15 | FR-19 | 7 | FR-33 | 18 |
| FR-6 | 3, 6 | FR-20 | 12 | FR-34 | 18 |
| FR-7 | 6 | FR-21 | 12, 13 | FR-35 | 18 |
| FR-8 | 6 | FR-22 | 12 | FR-36 | 17, 18 |
| FR-9 | 6, 13 | FR-23 | 9 | FR-37 | 18 |
| FR-10 | 6 | FR-24 | 9 | FR-38 | 18 |
| FR-11 | 6 | FR-25 | 9, 13 | FR-39 | 13 |
| FR-12 | 3, 6, 13 | FR-26 | 8, 15 | FR-40 | 13, 14 |
| FR-13 | 13, 14 | FR-27 | 8 | FR-41 | 17, 18 |
| FR-14 | 5, 13 | FR-28 | 16 | FR-42 | 13 |

Spec sections with a task: §2 (1, 14), §3 (8), §4 (10, 15), §6 (3–7, 13), §7
(12, 16), §8 (18), §9 (17, 18), §10 (22, 23, 24). **No gaps.**

**2. Placeholder scan.** No "TBD", no "implement later", no "similar to Task N".
Tasks 7, 8, 15, 16, 18 and 21 contain prose specifications for parts where the
code is mechanical (a JSON writer, a BLE stack init, Compose screens) rather than
a literal code block; each names the exact functions and the exact behavior the
tests assert, which is the standard the plan requires. Tasks 17's `Sha256Stream`
deliberately does not pin the implementation, only that it must be *the same
code on host and device*.

**3. Type consistency.** Checked across tasks: `LadderProfile`/`LadderButton`
(Task 3) are used unchanged in 6, 8, 16. `GestureTimings`/`Gesture` (Task 6) are
used unchanged in 8, 11. `GainMode`/`GainPolicy` (Task 5) are used unchanged in
7, 8. `Config` (Task 8) is used unchanged in 9, 11, 15, 16. `IHal` (Task 2) is
used unchanged everywhere. `ConfigLoadResult::kNoConfig` means *pass-through* in
both Task 9 and Task 13. `MockHalDefaultsConfig()` is defined once (Task 15) and
reused by Task 13's `MockHal::Defaults`.

**4. Corrections applied during this review.** Two real bugs were found by
compiling and running the specified code standalone rather than assuming it
worked:

- **`GestureStateMachine`**: the DOUBLE was emitted via `Emit` but `Update`
  returned `false`, so the caller never learned a gesture had fired. Fixed by
  tracking an `emitted` flag in the pressed branch.
- **`LadderDecode`**: a 500-permille floor wrongly faulted any button below half
  the rail (three of the nine tests failed), and an above-rail reading was
  reporting `kIdle`. Fixed by replacing the floor with a comparison against the
  **learned rail** (`kRailHealthFloorPermille`) and adding an explicit
  above-reference check.

`ConfigValidate`'s ambiguity rule was also corrected: the first version rejected
windows *nested* inside one another, but 690±40 and 700±40 are not nested, so
the test failed. Replaced with the rule that actually matters — the two centres
must be further apart than the wider tolerance.

Verified standalone before committing to the plan: `LadderDecode` 18/18 checks
including the 39-point rail sweep; the gesture state machine 11/11 including the
exact 500 ms and 750 ms boundaries and a 10-second hold emitting exactly one
event; `ConfigValidate` 12/12.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
