# SWC Adapter Firmware + Android App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the ESP-IDF firmware and the Android app for the SWC steering-wheel-control adapter, so a resistor-ladder input is decoded into the exact output levels an aftermarket head unit expects, and an Android app on the head unit configures that mapping and provides actions the head unit lacks.

**Architecture:** Two deliverables in one repo under `code/`. The firmware is a PlatformIO/ESP-IDF project split into pure-logic modules (`lib/`) that take an `IHAL&` and are tested on the host with GoogleTest, plus a thin ESP-IDF shell (`src/`) tested on-device with Unity. The Android app is Kotlin/Compose talking NDJSON over USB CDC, sharing one generated contract header so the two sides cannot drift. The firmware's logic is written and tested **before** the board arrives; on-device measurement is a separate, ordered bring-up phase.

**Tech Stack:** ESP-IDF v5.x via PlatformIO (`framework = espidf`), NimBLE, TinyUSB CDC, esp_https_ota, mbedTLS, NVS; GoogleTest (host) + Unity (device); Kotlin + Jetpack Compose, `usb-serial-for-android` v3.11.x, `esp-idf-provisioning-android`; GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md`

## Global Constraints

> **Rewritten 2026-09-18 against the repaired spec.** The previous revision of this
> section was itself defective (it said `platform = espressif32`, quoted a 1952 KB
> budget, and carried a 12 V ladder assumption). Every value below is now taken
> from the spec, and the spec is the authority where they differ.

### Platform and build

- **Framework:** `framework = espidf`. **Never** Arduino. The reported WiFi/SSL/webserver crash class is an Arduino-core defect (spec §1).
- **Platform:** **pioarduino's fork pinned to an exact release tag** —
  `https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip`.
  **Not** `platform = espressif32`: stock PlatformIO's `framework-espidf` tops out
  at 4.60100.0 (IDF 4.6.1), which predates the mbedTLS fix this project needs.
  Pin the *tag*, never the rolling `stable` zip, which crashes the installer under
  Python 3.14 (`exists(None)` in `safe_framework_cleanup`). **Verified in Task 1:
  resolves IDF 5.5.5 and builds green.**
- **Target:** ESP32-S3, **4 MB flash, no PSRAM**. No allocation may assume PSRAM.
- **`CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` is required.** The string form is silently
  ignored (it is a Kconfig *choice*) and `board_build.flash_size` is **not**
  honoured on pioarduino's IDF path. **Verified in Task 1:** without it the image
  silently builds for 2 MB.
- **`CONFIG_MBEDTLS_HARDWARE_AES` = n** in `sdkconfig.defaults`. **Verified in
  Task 1:** live, not inert (IDF 5.5.5 defaults it to `y`).
- **App size gate:** ≤ **1920 KB** per OTA slot (`pio run -t size`). Fallback if exceeded: spec §9.6.
- **Partitions:** exactly as spec §9.2 —
  `nvs 0x9000/0xC000`, `otadata 0x15000/0x2000`, `phy_init 0x17000/0x1000`,
  `app0 0x20000/0x1E0000`, `app1 0x200000/0x1E0000`, `coredump 0x3F0000/0x10000`.
  **App partitions must be 64 KiB-aligned** — IDF's `gen_esp32part.py` sets
  `ALIGNMENT[APP_TYPE] = 0x10000` and rejects the whole table otherwise. An
  earlier revision's `app1 @ 0x208000` **did not build**; that is why the slots
  are 1920 KB and not 1952 KB. `0x3E0000–0x3EFFFF` is reserved slack.
- **NVS partition:** 48 KB (12 × 4096 B pages; 48,384 B of usable entry space).
  Two limits, and the smaller one binds: a **single NVS value ≤ 4000 B** (hard
  IDF cap — `ENTRY_SIZE 32 × (ENTRY_COUNT−1) 125`), and **two slots + `cfg_seq`
  ≤ 48,384 B** at **2,112 B per 2048-byte chunk** (payload + NVS's metadata and
  `BLOB_IDX` entries). Note this is *not* "a slot ≤ 24 KB": two 24 KB slots need
  49,152 B and do not fit.
  The worst-case config is **22,407 B** and even a realistic one is ~3.9 KB, so a
  slot is **chunked** (`cfg_a_0…n`, 2048 B per chunk, **11 chunks worst case**);
  see "NVS layout" in the Shared contract. Two such slots are 46,496 B of the
  48,384 B — **96 %**, so the widths and caps in spec §3.5 are load-bearing.
- **TinyUSB is NOT part of IDF.** IDF 5.5.5 ships no `components/tinyusb` and
  defines no `CONFIG_TINYUSB_*`; the app USB link needs the managed component
  `espressif/esp_tinyusb` added via `idf_component.yml`. **Verified in Task 1.**
  Until a task adds it, spec §4.1's app interface does not exist.

### The HAL seam — the single naming authority (spec §10.2, frozen)

- **`lib/HAL/IHAL.h` is a C header, and its enumerators are prefixed constants.**
  It is the one header both the C host and the C++ application include, so it is
  `extern "C"` with C enums: `ADC_CH_SWC1`, `DAC_CH_KEY1`, `DAC_POWER_GND_1K`,
  `GPIO_LED_STAT`. Never `AdcChannel::kSwc1`. Every task names channels this way.
- **The interface is frozen** (spec §10.2). Its members are exactly:
  `adc_read_mv`, `dac_set_code`, `dac_power_mode`, `dac_ldac`, `gpio_write`,
  `gpio_read`, `buzzer_on`, `now_ms`, `now_us`, `nvs_get`, `nvs_set`, `reboot`,
  `ctx`. There is **no** `Interface()` accessor — the `IHAL *` is passed in, and
  the mock exposes it as `InterfaceRef()`. Changing this interface is a **spec
  change**.
- **`/SENSE1` and `/SENSE2` are ADC channels (`ADC_CH_KEY_SENSE1`,
  `ADC_CH_KEY_SENSE2`), not `GpioPin`s.** Spec §2.2 marks them `A-in` — they are
  the KEY-line ÷2 sense divider that the servo trim loop reads. The only
  `GpioPin` inputs are `GPIO_BOOT` and `GPIO_VBUS_VALID`.

### Analog facts

- **ADC:** 12-bit, `0–4095`. The S3 has **no DAC** and the calibrated ceiling is
  **2.9 V** at 12 dB attenuation. `V_SENSE ≤ 2.49 V` by the exact ÷2 divider.
- **Gain:** `V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ` with `R58=82k`,
  `R61=100k` → gain exactly **1.82**, **not** 1.812. Tracking mode gain **1.00**.
- **Output envelope:** 1.80 V – 5.20 V. Guard band 2.6 V – 3.4 V. AUTO default is **1.82**.
- **No DAC Hi-Z.** Release = command above idle + sink FET off.
- **The ladder input is the OTHER side of the board and must not be conflated with
  the output.** Spec §6.3: the steering-pad ladder is a **series chain whose common
  is tied to GND**, so a press pulls the input **down** and **idle is the HIGH
  state**; `V_pin = 3.3 · R_ladder / (R_ladder + R_pullup)` with `R_pullup` =
  `R15`/`R16` 10 kΩ to **+3V3**. **`R1`/`R2` are not in that divider** — they feed
  a high-Z ADC pin and are the RC anti-alias filter. **There is no 12 V term
  anywhere in the transfer function.** Classification is
  `n = V_ADC / V_ADC_idle`, invariant to the +3V3 rail.
- **Never guess an unlearned button.** `UNKNOWN` → `event{button: null}`, always.

### Feedback and UX

- **Buzzer is active at a fixed ~2.4 kHz.** No pitch control. All feedback is rhythm/count/duration.
- **Both LEDs are green.** No colour grammar.
- **Safe idle is established before USB/BLE/WiFi init** (spec §6.1, step 8 before step 9).

### Protocol

- **NDJSON** line protocol with a `{v, seq, type}` envelope, 1024-byte line cap.
- **Config transfer is chunked** (`config_begin`/`config_chunk`/`config_end`,
  spec §4.2) because a whole config is several KB and cannot fit one line.
  Frame names are exactly those in spec §4.3 — notably `ping` → `status` (not
  `pong`), `nack` carries `err`/`detail` (not `reason`), and the learn frames are
  `learn_start`/`learn_stop`/`learn_commit`.
- **Reporter locale:** user-facing strings in `en-US`; no units other than mV/V/mm/ms in the protocol.
- **Android `targetSdk = 34`, `minSdk = 26`.** Do not raise targetSdk without re-reading spec §3.6 on Android BAL.

### Test discipline

- Host tests are **GoogleTest** under `test_native/` and run with the `native`
  env; on-device tests are **Unity** under `test/`.
- Test output must be pristine — no stray warnings or noise (warnings are findings).
- A test that asserts nothing, or that asserts against a mock's own bookkeeping
  rather than the behavior under test, is a defect regardless of who wrote it.
- **Every FR in spec §11's matrix must have its test in this plan.** An earlier
  revision claimed "no gaps" while FR-3, FR-5, FR-9, FR-16, FR-17, FR-25 and FR-40
  had no test at all; §11 is the checklist.

---

## File Structure

Created under `code/` in the PCB repo.

| Path | Responsibility |
| --- | --- |
| `code/platformio.ini` | Envs: `native`, `esp32s3` (no `esp32s3-ota` — OTA is a runtime path, not a separate env) |
| `code/partitions.csv` | Spec §9.2 — **1920 KB slots**, 64 KiB-aligned |
| `code/sdkconfig.defaults` | Non-default IDF knobs, incl. `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` |
| `code/boards/swc-s3.json` | Board definition: 4 MB, no PSRAM |
| `code/src/main.c` | Wiring only: create HAL, start tasks |
| `code/lib/HAL/IHAL.h` | The seam (spec §10.2, **frozen**) |
| `code/lib/HAL/EspHal.{h,c}` | Real implementation |
| `code/test_native/MockHAL.h` | Host implementation, injectable clock |
| `code/lib/Analog/AdcReader.{h,c}` | ADC sampling, filter, calibrated mV |
| `code/lib/Analog/CalibrationCurve.{h,c}` | ADC raw→mV |
| `code/lib/Analog/LadderDecode.{h,c}` | Ratio-normalized classification |
| `code/lib/Gesture/PressClassifier.{h,c}` | Level → press events |
| `code/lib/Gesture/GestureStateMachine.{h,c}` | Presses → SINGLE/DOUBLE/LONG |
| `code/lib/Output/DacMcp4728.{h,c}` | I²C DAC driver |
| `code/lib/Output/GainPolicy.{h,c}` | Gain-mode selection |
| `code/lib/Output/ServoLoop.{h,c}` | Bounded trim loop |
| `code/lib/Bindings/ActionLibrary.{h,c}` | Action kinds → semantics |
| `code/lib/Bindings/BindingResolver.{h,c}` | Gesture → action |
| `code/lib/Feedback/BuzzerGrammar.{h,c}` | Buzzer patterns |
| `code/lib/Feedback/LedGrammar.{h,c}` | LED patterns |
| `code/lib/Config/ConfigCodec.{h,c}` | Config ↔ JSON |
| `code/lib/Config/ConfigStore.{h,c}` | A/B NVS persistence |
| `code/lib/Link/Ndjson.{h,c}` | Frame encode/decode |
| `code/lib/Link/UsbCdc.{h,c}` | TinyUSB CDC transport (**needs `espressif/esp_tinyusb`**) |
| `code/lib/Link/CommandRouter.{h,c}` | Frame → handler |
| `code/lib/Maintenance/BleProvisioning.{h,c}` | NimBLE provisioning |
| `code/lib/Maintenance/WebPage.{h,c}` | Token-authenticated web config |
| `code/lib/Maintenance/MaintenanceMode.{h,c}` | Mode entry/exit, timeout |
| `code/lib/Update/ImageVerify.{h,c}` | SHA-256 streaming verify |
| `code/lib/Update/ReleaseCheck.{h,c}` | Manifest fetch/compare |
| `code/lib/Update/OtaUsb.{h,c}` | USB OTA path |
| `code/lib/Update/OtaWifi.{h,c}` | `esp_https_ota` path, pinned CA |
| `code/contract/swc_contract.h` | Generated, checked in |
| `code/tools/gen_contract.py` | Generates the above |
| `code/tools/gen_contract_kotlin.py` | Generates Kotlin types |
| `code/android/...` | Gradle project |

**Four modules the previous revision declared but no task created —
`AdcReader`, `DacMcp4728`, `UsbCdc`, `BleProvisioning` — now each have an owning
task below.** That was the single largest structural gap in the plan: the File
Structure table promised them and the task list never built them.

**CI workflows live at the repository root**, not under `code/` — GitHub Actions
reads `.github/workflows/` from the repo root only. Each workflow needs
`working-directory: code` (or `code/android`). Spec §10.1.

---

## Shared contract — the single source of every cross-task name

> **This section is new, and it is the fix for the plan's dominant failure mode.**
> A preflight scan of the previous revision found **34 cross-task interface
> conflicts** — one task defining `ADC_CH_SENSE1`, another `ADC_CH_KEY_SENSE1`;
> one task's `GainPolicySelect` returning the opposite gain from the spec; two
> definitions of the action table; two of the protocol version; a `Config` that
> could not represent spec §3.5's bindings. Every one of those was a name or a
> value invented independently in a task that could not see the others.
>
> **The rule: a task never invents a shared name or a shared constant.** If a task
> needs a type, an enumerator, a frame name, a timing, a pattern or a struct
> shape that another task also touches, it is defined **here**, once, and the task
> *uses* it. A task may add a private `static` helper freely; it may not add a
> second definition of anything below.
>
> The spec is still the authority. Where this section and the spec disagree, the
> spec wins and this section is a bug — but they are written to agree, and each
> entry cites its spec section so the check is mechanical.

### C types and enums (`lib/HAL/IHAL.h` — frozen, spec §10.2)

```c
typedef uint16_t MilliVolt;    // 0–2900 at the pin (spec §3.2)
typedef uint16_t AdcRaw;       // 0–4095, 12-bit (spec §3.2)
typedef uint32_t TimestampMs;  // u32 monotonic (spec §3.2)

typedef enum { ADC_CH_SWC1, ADC_CH_SWC2, ADC_CH_TEMP,
               ADC_CH_AUX1, ADC_CH_AUX2, ADC_CH_AUX3,
               ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2,
               ADC_CH_COUNT } AdcChannel;
typedef enum { DAC_CH_KEY1, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2,
               DAC_CH_COUNT } DacChannel;
typedef enum { DAC_POWER_NORMAL, DAC_POWER_GND_1K, DAC_POWER_GND_100K,
               DAC_POWER_GND_500K } DacPowerMode;
typedef enum { GPIO_LED_STAT, GPIO_LED2,
               GPIO_BOOT, GPIO_VBUS_VALID, GPIO_COUNT } GpioPin;
```

**`GpioPin` holds only raw pins — the buzzer and LDAC are NOT in it.** Spec §10.2
gives them their own frozen members, `buzzer_on` and `dac_ldac`, because they are
*semantic* lines: the buzzer is a rhythm grammar (§7.2) and LDAC is DAC
sequencing (§2.3), so both have a right way to be driven that a bare pin write
cannot express. An earlier revision of this enum listed `GPIO_BUZZ` and
`GPIO_DAC_LDAC_B` alongside those members — **two routes to one physical line**,
which `MockHal` would then have to store twice. The test for that trap is
`BuzzerIsOn()` vs `GpioRead(GPIO_BUZZ)`: follow the frozen spec and the second one
never moves. LEDs and the two digital inputs are genuinely raw pins and stay.

**Eight ADC channels, per spec §2.2's pin map** — `IO1`/`IO2` (SWC1/2 ladder),
`IO7` (NTC temp), `IO4`/`IO5`/`IO6` (AUX1–3, "fully usable analog inputs"), and
`IO8`/`IO9` (the KEY-line ÷2 senses). **`ADC_CH_AUX1–3` are real and must not be
dropped** — they are the local programming/test buttons, and the whole headless
learn fallback (spec §7.4) presses them.

**Four DAC channels, not five — and no spare.** Verified against the netlist:
`U4.VOUTA` → ch1 signal, `VOUTB` → `/V_ADJ1`, `VOUTC` → ch2 signal, `VOUTD` →
`/V_ADJ2`. An earlier revision had `DAC_CH_ADJ` (one shared adjust) plus a
`DAC_CH_SPARE` that does not exist; the adjust channel is **per channel**,
because each channel's gain mode is selected independently (spec §2.3).

**`ADC_CH_KEY_SENSE1/2` — not `ADC_CH_SENSE1/2`.** The sense channels are the
KEY-line ÷2 divider (spec §2.2 marks them `A-in`). **There is no
`GPIO_SENSE1`/`GPIO_SENSE2`** — a `MockHal::GpioRead` switch casing those is the
exact defect this section exists to prevent. The only `GpioPin` inputs are
`GPIO_BOOT` and `GPIO_VBUS_VALID`.

The `IHAL` struct itself is **verbatim from spec §10.2** and is reproduced there;
do not restate it here. Its 13 members are exactly: `adc_read_mv`, `dac_set_code`,
`dac_power_mode`, `dac_ldac`, `gpio_write`, `gpio_read`, `buzzer_on`, `now_ms`,
`now_us`, `nvs_get`, `nvs_set`, `reboot`, `ctx`. **No `Interface()` accessor** —
the reference is passed in; the test-side accessor is `InterfaceRef()`.

**`dac_set_code` takes only `(ctx, ch, code)` — it does NOT take a power mode.**
Power mode is a separate call, `dac_power_mode(ctx, ch, mode)`, and `dac_ldac` is
its own member too. An earlier revision folded the power mode into
`dac_set_code` and renamed the LDAC member `dac_ldac_assert`, which does not
match the frozen interface — and the fold is wrong on the merits, because the
gain-mode selection *is* a power-mode change (spec §2.3: `PD1:PD0 = 01` → gain
1.82) made independently of any code write.

**`dac_set_code` returns `void`, and that is deliberate** (spec §6.8): the DAC
driver retries with backoff internally and, on persistent failure, releases the
line and latches a fault — it never drives a guessed code. A `bool` return was
considered and rejected because the retry policy lives *inside* the driver, so
there is no useful failure for a caller to branch on. Fault visibility is via
`AdcReader`/orchestrator state and the `FAULT_DAC` pattern, not a return code.

### Gestures, channels, actions (spec §3.3, §3.5, §3.6)

```c
typedef enum { GESTURE_NONE, GESTURE_SINGLE, GESTURE_DOUBLE, GESTURE_TRIPLE,
               GESTURE_LONG, GESTURE_LONG_REPEAT, GESTURE_COUNT } Gesture;
```

`TRIPLE` and `LONG_REPEAT` are in the enum **from day one** (spec §3.3) so the
schema needs no migration; they are gated behind a capability flag and the first
cut implements `SINGLE`/`DOUBLE`/`LONG`. `COMBO` is deferred to v2.

**Binding channel** is `SWC1 | SWC2 | AUX1 | AUX2 | AUX3 | ANY`. `ANY` is a real
value, not a placeholder.

**The 11 action kinds** (spec §3.6) — `NONE`, `HW_KEY`, `HW_KEY_RELEASE`,
`APP_LAUNCH`, `APP_INTENT`, `KEYCODE`, `MEDIA`, `VOLUME`, `SYSTEM`, `BUZZ`,
`APP_RAW`. **There are no numeric action ids.** An earlier revision invented ids
`1–63`; the spec defines none, and the contract generator must not synthesize
them. `VOL_UP` is a `LadderButton.id`, **never** an action name — an earlier
revision conflated the two, which made bindings unrepresentable.

**`Binding` is a top-level join table** (spec §3.5), not nested per channel:

```c
enum { kMaxBindings = 32, kMaxActionsPerBinding = 2 };

typedef struct {
    uint8_t  kind;                 // spec 3.6's 11 kinds; NOT a numeric id
    bool     takes_payload;        // whether `payload` is meaningful
    char     target[40];           // package / action / command / pattern
    char     payload[kDataPayloadLen];  // the kind's data (APP_INTENT's `data`, ...)
} Action;

typedef struct {
    char     id[16];
    uint8_t  channel;              // SWC1|SWC2|AUX1..3|ANY
    char     button[16];           // LadderButton.id, or "NONE"
    uint8_t  gesture;
    bool     enabled;
    uint8_t  action_count;         // 0 is legal and MEANS "swallow the gesture"
    Action   actions[kMaxActionsPerBinding];  // ordered, executed best-effort
} Binding;
```

**The string widths are part of the budget, not free choices** (spec §3.5's
width table): `target[40]`, `payload[48]`, ids and names `[16]`. A provable
staging bound covers every field at its maximum, so these four widths are what
make 32 × 2 fit at all.

**`kMaxActionsPerBinding` is 2, and the number is measured, not chosen** (spec
§3.5): at the widths above, the structural worst case is 32 bindings × 2 actions
= **22,407 B** of JSON — 11 chunks, which with two slots plus `cfg_seq` is
**46,496 B of the partition's 48,384 B usable bytes (96 %)**. Three actions per
binding needs 14 chunks = 59,168 B = 122 % and does not fit. An earlier revision
carried `actions[4]`, which **cannot be stored**; the cap is the partition's, not
a preference.

**That same earlier revision also quoted 21,411 B → 11 chunks → 96 % for 32 × 2,
and those figures do not describe 32 × 2.** They are reproducible only at *one*
action per binding. At 2 actions with the unbounded widths that revision declared
(`target[64] payload[128] id[24]`) the arithmetic gives **30,021 B → 15 chunks →
63,392 B = 131 %** — an overflow the "96 %" text hid. Both numbers were in the
same paragraph. The width table is what makes the stated figure true; if you
change a width, re-measure rather than editing the percentage.

**An empty `actions` list is not `enabled: false`** (spec §3.5): empty swallows
the gesture, disabled lets a lower-priority binding match. Both states must be
representable and tested.

**`OutputProfile` carries `idle_dac_code`** (spec §3.7, value `4095`) — spec
§6.7's whole hardware-default argument depends on it. An earlier revision had
`OutputProfile{gain_mode, idle_key_mv}`, which omitted it; **`idle_key_mv` is not
a config field at all** — the learned idle is a *runtime measurement* (Task 13's
`IdleKeyMv()`), not something persisted, and nothing ever read it from the
config. Every orchestrator test compares the DAC against `idle_dac_code`, so
without the field the config could not supply the code those tests assert on.

### Gain policy (spec §6.2)

```c
typedef enum { GAIN_POLICY_AUTO, GAIN_POLICY_FORCE_5V, GAIN_POLICY_FORCE_3V } GainPolicy;
typedef enum { GAIN_MODE_AMPLIFIED, GAIN_MODE_TRACKING } GainMode;
```

**Wire names are `AUTO`/`FORCE_5V`/`FORCE_3V`** — an earlier revision used
`kForceTracking`/`kForceAmplified`, which are undefined on the wire.

**The selection rule, which an earlier revision had exactly backwards:**

| `V_KEY_idle` | Mode | Gain |
| --- | --- | --- |
| `≥ 3.4 V` | `AMPLIFIED` | **1.82** |
| `2.6–3.4 V` (guard band) | **hold current mode, re-measure** | — |
| `< 2.6 V` | `TRACKING` | **1.00** |

**Default to 1.82 whenever the measurement is absent or ambiguous.** The only
dangerous error is over-ranging a 3 V head unit; under-ranging a 5 V unit merely
wastes range (spec §6.2's asymmetry argument). Gain is **re-evaluated**, not
latched — on `/VBUS_VALID` transitions and periodically while idle.

**Command targets must stay within `[min_ladder, V_KEY_idle − 0.20 V]`.**

### Timing defaults (spec §3.7, §6.x)

| Setting | Value |
| --- | --- |
| `debounce_ms` | 25 |
| `double_press_gap_ms` | **500** |
| `long_press_ms` | **750** |
| `long_repeat_ms` | 250 |
| `send_duration_ms` | 200 |
| `release_margin_mv` | 900 |

**500/750, not 400/700.** The spec's worked example said 400/700 and Appendix A
plus §6.x plus the plan's own defaults said 500/750; the spec has been corrected
to 500/750 and this table is the single definition.

### Buzzer patterns (spec §7.2) — the complete table

```c
typedef enum {
    BUZZ_BOOT_OK, BUZZ_BOOT_DEGRADED, BUZZ_BOOT_ERROR,
    BUZZ_KEY_ACCEPTED, BUZZ_KEY_UNKNOWN,
    BUZZ_PROGRAM_ENTER, BUZZ_PROGRAM_STEP, BUZZ_PROGRAM_SAVED,
    BUZZ_PROGRAM_EXIT, BUZZ_PROGRAM_CANCEL,
    BUZZ_LEARN_PROMPT, BUZZ_LEARN_OK, BUZZ_LEARN_REJECT,
    BUZZ_FAULT_DAC, BUZZ_FAULT_CONFIG, BUZZ_FACTORY_RESET,
    BUZZ_OTA_START, BUZZ_OTA_OK, BUZZ_OTA_FAIL,
    BUZZ_PATTERN_COUNT
} BuzzerPattern;
```

| Pattern | on/off ms | reps | | Pattern | on/off ms | reps |
| --- | --- | --- | --- | --- | --- | --- |
| `BOOT_OK` | 60/60 | 1 | | `LEARN_PROMPT` | 100/100 | 1 |
| `BOOT_DEGRADED` | 60/60 | 3 | | `LEARN_OK` | 40/30 | 2 |
| `BOOT_ERROR` | 500/200 | 2 | | `LEARN_REJECT` | 300/80 | 2 |
| `KEY_ACCEPTED` | 25/0 | 1 | | `FAULT_DAC` | 500/300 | 3 |
| `KEY_UNKNOWN` | 120/80 | 1 | | `FAULT_CONFIG` | 500/300 | 4 |
| `PROGRAM_ENTER` | 40/40 | 2 | | `FACTORY_RESET` | 800/200 | 3 |
| `PROGRAM_STEP` | 40/40 | **1** | | `OTA_START` | long single | — |
| `PROGRAM_SAVED` | 40/20 | 4 | | `OTA_OK` | rising double | — |
| `PROGRAM_EXIT` | 200/0 | 1 | | `OTA_FAIL` | harsh triple | — |
| `PROGRAM_CANCEL` | 300/100 | 1 | | | | |

**`PROGRAM_STEP` is one 40/40 pulse; the *caller* repeats it *n* times** for the
*n*-th button (spec §7.4). It is **not** a runtime `reps` argument — the grammar
(`pattern := pulse(on, off), repeat, gap`) makes `repeat` a property of the named
pattern. So the API is `Play(BuzzerPattern p)` with **arity 1**, and a caller
needing *n* beeps loops. An earlier revision declared arity 1 and then called
arity 2 in its own tests, and omitted `PROGRAM_SAVED`, `PROGRAM_CANCEL`,
`FACTORY_RESET` entirely.

**No pattern may exceed ~2 s** (spec §7.2), because the buzzer is non-blocking.

### LED grammars (spec §7.3)

`LED_STAT` is the **state** channel; `LED2` is the **activity** channel. Both are
green — **nothing relies on hue**. The two channels have separate grammars
(1 Hz breathe, 5 Hz blink, 0.5 s pulse), not one shared flat enum.

### Frame vocabulary (spec §4.3) — exact names

`hello`, `event`, `status`, `ladder_sample`, `ack`, `nack`, `log`,
`config_get`, `config_set`, `config_patch`, `config_begin`, `config_chunk`,
`config_end`, `learn_start`, `learn_stop`, `learn_commit`, `test_key`,
`identify`, `reboot`, `ping`, `time_sync`, `ota_begin`, `ota_chunk`, `ota_end`.

Four corrections an earlier revision needs, all spec §4.3:

- **`ping` → `status`**, not `pong`.
- **`nack` carries `err` and `detail`**, not `reason`.
- **The learn frames are `learn_start`/`learn_stop`/`learn_commit`**, not
  `ladder_learn_start`/`_sample`/`_commit`.
- **`reset_config` and `link_gap` are not frames.** Reset is a `config_set` with
  defaults, or a `reboot` with a boot target.

**The protocol version has exactly one definition** — the generated
`SWC_PROTOCOL_VERSION` in `contract/swc_contract.h` (spec §10.2). A hand-written
`kNdjsonProtocolVersion` in the link module was a second source of truth and
defeats the anti-drift generator.

### Manifest shape (spec §9.5) — nested semver

```jsonc
{ "latest_version": "1.4.0", "channel": "stable",
  "firmware": { "version": "1.4.0", "url": "...", "size_bytes": 1234567,
                "sha256": "..." },
  "min_from_version": "1.0.0" }
```

**Nested with semver strings, not flat with `version_code`/`board`/
`min_from_version_code`.** An earlier revision's flat manifest would make the
device refuse every real release. TLS is verified against a **pinned CA** — never
`setInsecure()` (spec §9.5).

### The ladder model (spec §3.4, §6.3)

```c
typedef struct {
    char      id[16];              // stable slug, referenced by Binding.button
    char      name[16];
    MilliVolt mv_center;
    MilliVolt mv_tolerance;
    MilliVolt learned_at_rail_mv;  // the +3V3 rail (≈3300), NOT 12 V
    int16_t   temp_c_at_learn;     // tenths of °C
    uint16_t  sample_count;
    uint8_t   confidence;          // 0–100
} LadderButton;

typedef struct {
    uint8_t      source;           // LADDER_3V3
    MilliVolt    idle_mv;
    uint8_t      count;
    LadderButton buttons[16];
} LadderProfile;
```

**Every field is required.** An earlier revision dropped `sample_count`,
`confidence`, `temp_c_at_learn`, per-button `learned_at_rail_mv` and `source`,
which made spec §3.4's learn-quality scoring and §6.4's temperature model
impossible.

**Units are millivolts at the pin, 0–2900.** The ADC ceiling is 2.9 V, so **no
ladder level can exceed 2900 mV** — an earlier revision's tests used 5000+ mV
levels and an 11000–14800 mV "rail sweep", which no hardware can produce. The
rail sweep is now a **+3V3 sweep (3.14–3.47 V)** and is a ratio test.

**Tolerance is derived at learn time** as the midpoint of the gap to the nearest
neighbouring button, capped by a configurable maximum (spec §3.4) — *not*
`max(2×spread, 8)` as an earlier revision had it.

### NVS layout (spec §3.8)

Namespace `swc_cfg`, sequence key `cfg_seq`, and a **chunked slot** per side:
`cfg_a_0…n` / `cfg_b_0…n`, where chunk 0 carries the blob header (magic, schema,
total length, chunk count, per-slot CRC) and the remaining chunks carry the
payload. A slot is still read and validated **as a unit** — the CRC covers the
whole slot, so a slot is either entirely good or rejected.

**The chunked path is not conditional, and that is the decision this section
exists to record.** Spec §3.8 says "measure, then decide", and the measurement is
now done and is in the IDF source rather than in a datasheet:

- A single NVS value is capped at `ENTRY_SIZE × (ENTRY_COUNT − 1)` = 32 × 125 =
  **4000 bytes**. `nvs_page.cpp` returns `ESP_ERR_NVS_VALUE_TOO_LONG` above it.
  This is a hard limit, not a budget.
- The worst-case `Config` is **22,407 B** as JSON (~15.9 KB packed; 32 top-level
  bindings × 2 actions dominate). **Even a realistic config (~3.9 KB) sits on the
  4000-byte line, and a moderate one (~7.9 KB) exceeds it** — so the single-value
  form fails on the common case, not a hypothetical one.

Hence `ConfigChunkCountFor()` (Task 8) and the fixed `kConfigChunkBytes = 2048`:
worst case **11 chunks per slot**, bounded key count. Measured slot cost is
**96 % of usable NVS entry space** for two worst-case slots (46,496 B of
48,384 B — the partition is 12 × 4096 B pages, each with 126 × 32 B entries), so
it fits with much less slack than "48 KB" suggests — and the 48 KB is shared with
WiFi provisioning credentials (spec §9.2), so this is an upper bound, not a
private budget. Each chunk key costs **2112 B**,
not 2080: NVS writes a 32-byte metadata entry and the 2048 payload bytes
(`nvs_page.cpp:185`) and *then* a separate 32-byte `BLOB_IDX` entry for the key
(`nvs_storage.cpp:353`). An earlier revision counted 2080 and understated the
two-slot cost by 576 B.
An earlier revision of this
section had "plus `cfg_a_0…n` **only if** the measured size exceeds one value"
and Task 9 write a single `nvs_set` per slot — which would have returned
`ESP_ERR_NVS_VALUE_TOO_LONG` and been reported as a save failure.
**Two slots share the 48 KB partition, so they cannot both be 24 KB** either; an
earlier revision's "2 × 24 KB in a 48 KB partition plus a sequence key" does not
fit.

### Test-harness API (used by every task)

`MockHal` is a **C++ class** in `test_native/MockHAL.h` (declared) and
`test_native/MockHAL.cpp` (defined), both created by Task 2 and **already
committed** — an earlier revision of *this section* sketched a C-style
`MockHalCreate`/`MockHalInterface` free-function API that no task actually
called, and named the file in lowercase. **The class below is authoritative**;
it is what the ~100 call sites across Tasks 3–24 use.

```cpp
class MockHal {
 public:
  IHAL &InterfaceRef();                       // passed to every unit under test
  void  AdvanceMs(uint64_t ms);               // synthetic clock
  void  SetAdcMilliVolts(AdcChannel ch, int mv);
  void  SetGpioInput(GpioPin pin, bool level);
  void  DacSetCode(DacChannel ch, uint16_t code);
  // NOTE: the member name `DacPowerMode` HIDES the enum type of the same name
  // for the rest of class scope, so every LATER use of the type inside the
  // class must be qualified `::DacPowerMode`. The declaring line itself is
  // fine; the return type below, the thunk parameter and the `dac_mode_`
  // member are not. Unqualified, this header does not compile:
  //   error: must use 'enum' tag to refer to type 'DacPowerMode' in this scope
  void  DacPowerMode(DacChannel ch, ::DacPowerMode mode);
  uint16_t    LastDacCode(DacChannel ch) const;
  ::DacPowerMode LastDacPowerMode(DacChannel ch) const;
  int   DacWriteCount(DacChannel ch) const;
  void  DacLdac(bool assert);
  bool  LastLdac() const;
  void  BuzzerOn(bool on);                    // the semantic line, not a GpioPin
  bool  BuzzerIsOn() const;
  int   BuzzerOnCount() const;
  void  GpioWrite(GpioPin pin, bool level);
  bool  GpioRead(GpioPin pin) const;
  int   GpioWriteCount(GpioPin pin) const;
  int   NvsSet(const char *key, const void *in, size_t len);
  int   NvsGet(const char *key, void *out, size_t len);
  void  FailNextNvsWrite();
  void  TruncateNextNvsWriteAt(size_t n);
  // Target a SPECIFIC key rather than "the next write". Task 9 needs this: a
  // slot is written as several chunks and the sequence key is written last, so
  // "the next write" is always a payload chunk and can never express the
  // sequence-lost tear. Added by Task 9 -- the one genuine addition to MockHal
  // after Task 2, and it is new capability, not a duplicate of anything above.
  void  TruncateNvsWriteTo(const char *key, size_t n);
  void  CorruptNvsValue(const char *key, size_t offset);   // flips one bit
  void  ClearNvs();
  int   RebootCount() const;
};
```

**One accessor, one name — `InterfaceRef()`.** The interface is handed *in* to
every unit under test; there is no `Interface()` and no free-function
`MockHalInterface`. An earlier revision had `Interface()` in one task and
`InterfaceRef()` in another, with ~45 call sites split between them.

**Method names and signatures are fixed.** `SetAdcMilliVolts` (not `SetAdc`),
`LastDacCode` returning `uint16_t` (not `int`), `SetGpioInput` (not `SetGpio`),
`NvsGet`/`NvsSet` (not `MockHalNvsGet`), `LastLdac` (not `LastLdacAsserted` —
the interface member is `dac_ldac`), `CorruptNvsValue(key, offset)` taking a
**byte offset** (not `CorruptNvsValue(key)`). Renaming or re-signing any of them
breaks call sites in later tasks.

`MockHal` is declared in `test_native/MockHAL.h` and defined in
`test_native/MockHAL.cpp`. **Task 2 created and committed both, and no later
task adds anything to them** — an earlier revision had Task 7 add
`CorruptNvsValue`/`ClearNvs` and Task 9 add them again, which is a duplicate
definition and does not compile. The header is edited only if a task genuinely
needs a new fault injector that does not exist; check the class above first.

**Every suite needs its own `test_main.cpp` — not just Task 2's.** GoogleTest's
`gtest_main.cc` is excluded by googletest's `library.json` `srcFilter`, so
nothing supplies `main()` and the link fails with `Undefined symbols: _main`.
Task 2 writes the first one at `code/test_native/test_hal/test_main.cpp`; **every
later task that creates a new suite directory must copy that same four-line file
into it.** The directories are:

| Suite | Introduced by |
| --- | --- |
| `test_hal` | Task 2 (the original) |
| `test_analog` | Task 3 |
| `test_output` | Task 5 — **and Task 7 adds a second suite to it; do NOT add a second `main()`** |
| `test_gesture` | Task 6 |
| `test_config` | Task 8 |
| `test_link` | Task 10 |
| `test_bindings` | Task 11 |
| `test_feedback` | Task 12 |
| `test_system` | Task 13 |
| `test_learning` | Task 16 |
| `test_update` | Task 17 |
| `test_maintenance` | Task 18 |

The four-line body (from Task 2):

```cpp
#include <gtest/gtest.h>
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

---

## Phase 0 — Foundations (no hardware needed)

### Task 1: PlatformIO environment that actually builds for the ESP32-S3

> **STATUS: COMPLETE — commits `4832e3f` + `a2d7094`, build green.** Verified:
> `pio run -e esp32s3` → `[SUCCESS]`, resolving **IDF 5.5.5** on pioarduino
> `55.03.311`. The steps below are retained as the record of what was built, with
> the four defects the brief's verbatim content contained now corrected inline.
> **Do not re-run this task.** Its measured baseline is in
> `code/docs/bring-up-log.md`.

This task exists because the framework choice is the project's riskiest
assumption and it must be proven before any real code is written. Stock
PlatformIO cannot supply IDF 5.x (its `framework-espidf` stops at 4.6.1), so the
platform is pioarduino's fork pinned to a release tag — never the rolling
`stable` zip, which crashes the installer under Python 3.14.

**Files (as actually built — nine, not the six the brief listed):**
- Create: `code/platformio.ini`
- Create: `code/sdkconfig.defaults`
- Create: `code/partitions.csv`
- Create: `code/boards/swc-s3.json`
- Create: `code/.gitignore`
- Create: `code/src/main.c`
- Create: `code/CMakeLists.txt` — **omitted from the brief; IDF cannot build without it**
- Create: `code/src/CMakeLists.txt` — same
- Create: `code/docs/bring-up-log.md` — the measured size baseline

**Interfaces:**
- Consumes: nothing (first task)
- Produces: the env names `esp32s3` and `native`, used by every later task's test
  commands

**The four defects found and fixed (all verified):**

1. **`framework = espidf` could not build** — `No module named
   'SCons.Tool.FortranCommon'`. Upstream pioarduino bug: `platform.py` installs
   `tool-scons` only under `if "espidf" in frameworks:`, and pioarduino's pinned
   `4.40801.0` artifact is a 2-file stub with no `SCons/` tree, while Core pins
   `~4.41101.0`; the two fight over one directory mid-build. Resolved when Core's
   pin won; **the platform pin was not changed.** Latent recurrence risk — a
   future `pio upgrade` can break the IDF env with the same opaque error.
2. **`partitions.csv` was rejected by IDF** — `Partition app1 invalid: Offset
   0x208000 is not aligned to 0x10000`. App partitions must be 64 KiB-aligned
   (`gen_esp32part.py`, `ALIGNMENT[APP_TYPE] = 0x10000`). Fixed → **1920 KB
   slots**, costing 32 KB each. This is why the app budget is 1920 KB, not 1952.
3. **Flash size silently stayed at 2 MB** — `board_build.flash_size = 4MB` is not
   honoured on pioarduino's IDF path, and the string form
   `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` is silently ignored because it is a Kconfig
   *choice*. Fixed with **`CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`**.
4. **`-t partition-table` does not exist** on 55.03.311. The table was verified
   by decoding `.pio/build/esp32s3/partitions.bin` (3072 bytes, ends at
   `0x400000`) instead.

**Three sdkconfig keys are inert** — IDF 5.5.5 defines none of them, and accepts
them silently with no warning, so each was checked against the generated
`sdkconfig.json`: `CONFIG_TINYUSB_CDC_ENABLED` (no `components/tinyusb` in IDF —
TinyUSB is the managed component `espressif/esp_tinyusb`), and
`CONFIG_ESP_ADC_CAL_USE_EFUSE_CALIBRATION` /
`CONFIG_ESP_ADC_CAL_DEFAULT_ATTENUATION_12` (the real keys are
`ADC_CALI_EFUSE_TP_ENABLE` / `ADC_CALI_EFUSE_VREF_ENABLE` / `ADC_CALI_LUT_ENABLE`;
attenuation is a runtime `adc_oneshot` argument in IDF 5.x, not Kconfig). **The
TinyUSB finding means spec §4.1's app USB link does not exist yet** — the `UsbCdc`
task must add the managed component. The two ADC keys must be re-confirmed before
the ADC task.

- [ ] **Step 1: Write `platformio.ini` with both envs** *(DONE — as below, with
  no `esp32s3-ota` env; OTA is a runtime path, not a separate build)*

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
; PlatformIO has exactly ONE global test_dir, and it is NOT a per-env option --
; `test_dir` inside an [env:*] section is silently ignored (verified against
; PlatformIO 6.2.0, project/options.py). So both trees live under one root:
; test/ for the device suites (Unity) and test_native/ for the host suites
; (GoogleTest), each env ignoring the other.
;
; Suite names are PATHS RELATIVE TO test_dir and are matched with fnmatch, so a
; suite is ignored only if its whole relative path matches a pattern. That is
; why each env needs BOTH forms: `test_native` (the tree directory itself,
; which PlatformIO also collects as a suite) and `test_native/*` (the suites
; inside it).
;
; test_dir must be the project root, because test_native/ sits beside test/.
; The root is also where PlatformIO puts its own build output
; (.pio/build/<env>/test/...), and the walker cannot tell that from a real
; suite -- it collects it and copies the trees into it again, so the suite list
; grows on every run. Hence the `.pio/*` ignores, which are load-bearing.
test_dir = .

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
test_ignore =
    test_native
    test_native/*
    .pio/*
monitor_speed = 115200

[env:native]
; Host tests: pure logic only, no ESP32 toolchain involved. GoogleTest.
;
; `test/*` must be ignored: those suites are Unity, built for the device, and
; GoogleTest cannot link them on the host.
;
; Each suite needs its OWN `main()`. GoogleTest's gtest_main.cc is filtered out
; of the PlatformIO library build by googletest's library.json srcFilter, so
; nothing else supplies main() and the test binary fails to link with
; "Undefined symbols: _main". The fix is a small test_main.cpp inside the suite
; (the proven pattern in the coop_controller project), NOT a gtest_main
; injection script -- adding CPPPATH for it breaks the gmock objects.
platform = native
build_flags =
    ${env.build_flags}
    -std=gnu++17
    -I lib
    -I test_native
    -D SWC_NATIVE_TEST
test_framework = googletest
test_ignore =
    test
    test/*
    .pio/*
build_src_filter = -<*>
test_build_src = yes
lib_deps =
    google/googletest@^1.15.2
```

**Two things about this file are load-bearing and were verified on this machine,
not reasoned about** (PlatformIO 6.2.0, Python 3.14.7):

1. **`test_dir` is global-only.** Setting it inside `[env:native]` prints
   `Warning! Ignore unknown configuration option 'test_dir'` and silently does
   nothing — the env then looks in the default `test/` and reports "Nothing to
   build". This is why `test_dir = .` sits at the top, with both trees under it.
2. **`test_ignore` matches whole relative paths.** With `test_dir = .`, a suite
   is named `test_native/test_hal`, so `test_ignore = test_native` alone leaves
   `test_native/test_hal` to run. Both the bare tree name and the `/*` form are
   needed. `.pio/*` is not tidiness: without it the walker collects PlatformIO's
   own `.pio/build/<env>/test/...` output, which then self-replicates on every
   run and grows the suite list.

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

- [ ] **Step 3: Write `code/partitions.csv` (spec §9.2) — 1920 KB slots, 64 KiB-aligned**

```csv
# name,     type, subtype,  offset,    size,      flags
nvs,        data, nvs,      0x9000,    0xC000,
otadata,    data, ota,      0x15000,   0x2000,
phy_init,   data, phy,      0x17000,   0x1000,
app0,       app,  ota_0,    0x20000,   0x1E0000,
app1,       app,  ota_1,    0x200000,  0x1E0000,
coredump,   data, coredump, 0x3F0000,  0x10000,
```

**`app0`/`app1` are 0x1E0000 = 1920 KB, and `app1` starts at 0x200000.** Both
app offsets are 64 KiB-aligned, which IDF's `gen_esp32part.py` requires
(`ALIGNMENT[APP_TYPE] = 0x10000`) — the brief's `app1 @ 0x208000` is not, and the
table was **rejected wholesale** with `Partition app1 invalid: Offset 0x208000 is
not aligned to 0x10000`. `0x3E0000–0x3EFFFF` is reserved slack, so `coredump`
keeps its specified `0x3F0000`. The table ends at exactly `0x400000`.

- [ ] **Step 4: Write `code/sdkconfig.defaults`**

```ini
# --- Console and USB -------------------------------------------------------
# Console on the ROM USB-Serial-JTAG peripheral; the app link is TinyUSB CDC on
# the same physical port but a separate USB interface (spec 4.1). Keeping them
# distinct is deliberate: the console must not corrupt app frames.
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=n

# --- Flash size ------------------------------------------------------------
# REQUIRED. board_build.flash_size is NOT honoured on pioarduino's IDF path
# (the board->sdkconfig injection is Arduino-gated), and the string form
# CONFIG_ESPTOOLPY_FLASHSIZE="4MB" is silently ignored because it is a Kconfig
# CHOICE. Without this the image silently builds for 2 MB.
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

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
# NOTE: CONFIG_ESP_ADC_CAL_USE_EFUSE_CALIBRATION and
# CONFIG_ESP_ADC_CAL_DEFAULT_ATTENUATION_12 DO NOT EXIST in IDF 5.5.5. They are
# accepted silently and do nothing, which is worse than an error because they
# look configured. The real keys are ADC_CALI_EFUSE_TP_ENABLE /
# ADC_CALI_EFUSE_VREF_ENABLE / ADC_CALI_LUT_ENABLE, and attenuation is a runtime
# adc_oneshot argument in IDF 5.x, not Kconfig. Confirm the calibration defaults
# before the ADC task rather than assuming these lines did anything.
#
# --- Crash handling --------------------------------------------------------
# Coredump partition exists (partitions.csv). Reset in release; gdbstub in dev.
CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

> **Do not add `CONFIG_TINYUSB_CDC_ENABLED=y`.** IDF 5.5.5 has no
> `components/tinyusb` and defines no such symbol — it is accepted **silently and
> does nothing**. The app USB link (spec §4.1) needs the managed component
> `espressif/esp_tinyusb` in `idf_component.yml`; that is the `UsbCdc` task's job,
> and until it is done the console on USB-Serial-JTAG works and there is **no app
> interface at all**.

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

- [x] **Step 7: Verify the image size baseline** *(DONE)*

Run: `pio run -e esp32s3 -t size`
Expected: prints a size summary with the app partition at **0x1E0000 (1920 KB)**.
Record the number in `code/docs/bring-up-log.md` as `size-baseline:` — every
later budget comparison is against it, and the number is a measurement, not a
value to invent here.

**Measured:** `text=156257, data=59116, bss=375761, dec=591134 (0x9051e)`;
linked `firmware.bin` = **215,488 bytes**, 10.96 % of a 1920 KB slot. Scaffolding
with no app code, so this is a **floor, not a budget**.

- [x] **Step 8: Verify the partition table is what the spec says** *(DONE,
  differently)*

`pio run -e esp32s3 -t partition-table` **does not exist** on pioarduino
55.03.311 — it fails with `*** Do not know how to make File target
'partition-table'`. Verified equivalently instead: `.pio/build/esp32s3/partitions.bin`
is **3072 bytes** and decodes to exactly `partitions.csv`, ending at `0x400000`
with `app0 0x20000/1920K` and `app1 0x200000/1920K`.

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
- Create: `code/test_native/test_hal/test_main.cpp` — **not optional.** GoogleTest's
  `gtest_main.cc` is filtered out of PlatformIO's library build by googletest's
  own `library.json` `srcFilter`, so nothing supplies `main()` and every host
  suite fails to link with `Undefined symbols: _main`. Each suite needs its own
  four-line `main()`; see the `[env:native]` note in Task 1.

  ```cpp
  #include <gtest/gtest.h>

  int main(int argc, char **argv) {
      ::testing::InitGoogleTest(&argc, argv);
      return RUN_ALL_TESTS();
  }
  ```

**Interfaces:**
- Consumes: the `native` env from Task 1
- Produces:
  - `AdcChannel` — C enum `{ ADC_CH_SWC1, ADC_CH_SWC2, ADC_CH_TEMP, ADC_CH_AUX1,
    ADC_CH_AUX2, ADC_CH_AUX3, ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2, ADC_CH_COUNT }`
  - `DacChannel` — C enum `{ DAC_CH_KEY1, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2,
    DAC_CH_COUNT }` (**four channels, no spare** — verified against the netlist:
    `U4.VOUTA`→ch1 signal, `VOUTB`→`/V_ADJ1`, `VOUTC`→ch2 signal,
    `VOUTD`→`/V_ADJ2`)
  - `DacPowerMode` — C enum `{ DAC_POWER_NORMAL, DAC_POWER_GND_1K,
    DAC_POWER_GND_100K, DAC_POWER_GND_500K }`
  - `GpioPin` — C enum `{ GPIO_LED_STAT, GPIO_LED2, GPIO_BOOT, GPIO_VBUS_VALID,
    GPIO_COUNT }`. **Raw pins only.** The buzzer (`/BUZZ`, IO13) and the MCP4728
    `~LDAC` (IO48) are deliberately absent: each already has its own frozen member
    (`buzzer_on`, `dac_ldac`), and they are semantic lines — a rhythm grammar
    (§7.2) and DAC sequencing (§2.3) — that a bare pin write cannot express.
    Listing them here as well would give two routes to one physical line and make
    the mock store the same state twice.
  - `struct IHAL { ... }` — **13 members**, verbatim from spec §10.2:
    `adc_read_mv`, `dac_set_code`, `dac_power_mode`, `dac_ldac`, `gpio_write`,
    `gpio_read`, `buzzer_on`, `now_ms`, `now_us`, `nvs_get`, `nvs_set`, `reboot`,
    `ctx`. `dac_set_code` takes `(ctx, ch, code)` only — **the power mode is its
    own member**, because selecting a channel's gain mode *is* a power-mode
    change (spec §2.3) made independently of any code write.
  - `MockHal` — a C++ class with `InterfaceRef()`, `AdvanceMs(uint64_t)`,
    `SetAdcMilliVolts(AdcChannel, int)`, `GpioWrite/GpioRead`, `BuzzerOn`,
    `LastDacCode(DacChannel)`, `NvsGet/NvsSet`

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
    hal.DacSetCode(DAC_CH_KEY1, 0x800);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x800);
    hal.DacSetCode(DAC_CH_KEY1, 0x123);
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), 0x123);
    // Power mode is a separate call: selecting a gain mode is a power-mode
    // change (spec 2.3) and is not coupled to any code write.
    hal.DacPowerMode(DAC_CH_ADJ1, DAC_POWER_GND_1K);
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K);
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_KEY1), DAC_POWER_NORMAL);
}

TEST(MockHalBuzzer, IsAnOnOffLineNotAGpioPin) {
    MockHal hal;
    EXPECT_FALSE(hal.BuzzerIsOn());
    EXPECT_EQ(hal.BuzzerOnCount(), 0);
    hal.BuzzerOn(true);
    EXPECT_TRUE(hal.BuzzerIsOn());
    EXPECT_EQ(hal.BuzzerOnCount(), 1);

    // The buzzer has its own frozen member (spec 10.2) and is NOT a GpioPin, so
    // driving it must not move any pin counter. If GPIO_BUZZ is ever added back
    // to the enum there are then two routes to one physical line, and this is
    // the assertion that catches it.
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 0)
        << "the buzzer must not be driven through gpio_write";
    EXPECT_FALSE(hal.LastLdac()) << "nothing here touched ~LDAC";
}

TEST(MockHalGpio, ReadsBackWhatWasWrittenAndTracksWriteCount) {
    MockHal hal;
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 0);
    hal.GpioWrite(GPIO_LED_STAT, true);
    EXPECT_TRUE(hal.GpioRead(GPIO_LED_STAT));
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), 1);
}

TEST(MockHalNvs, PersistsBytesAcrossCallsAndCanBeMadeToFail) {
    MockHal hal;
    const char payload[] = "config-blob";
    ASSERT_EQ(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
    char out[sizeof(payload)] = {};
    // NvsGet returns the number of bytes copied, not 0.
    ASSERT_EQ(hal.NvsGet("cfg", out, sizeof(out)),
              static_cast<int>(sizeof(payload)));
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

TEST(MockHalNvs, CorruptingOneBitIsVisibleToTheReader) {
    MockHal hal;
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(hal.NvsSet("cfg", payload, sizeof(payload)), 0);
    hal.CorruptNvsValue("cfg", 2);
    uint8_t out[sizeof(payload)] = {};
    ASSERT_EQ(hal.NvsGet("cfg", out, sizeof(out)),
              static_cast<int>(sizeof(payload)));
    EXPECT_EQ(out[2], 0x02) << "bit 0 of byte 2 must have flipped";
    EXPECT_NE(std::memcmp(out, payload, sizeof(payload)), 0);
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

Run: `cd code && pio test -e native -f '*test_hal'`
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
    /* Per channel: one signal DAC output, one V_ADJ output. Verified against
     * the netlist -- U4.VOUTA -> ch1 signal, VOUTB -> /V_ADJ1, VOUTC -> ch2
     * signal, VOUTD -> /V_ADJ2. There is no spare channel. */
    DAC_CH_KEY1 = 0, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2,
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
    GPIO_LED_STAT = 0, GPIO_LED2,
    /* The only two GPIO inputs. SENSE1/SENSE2 are ADC channels above. The
     * buzzer and ~LDAC are NOT here -- they are the semantic members
     * buzzer_on and dac_ldac, which is where the rhythm grammar and the DAC
     * sequencing contract live. */
    GPIO_BOOT, GPIO_VBUS_VALID,
    GPIO_COUNT
} GpioPin;

/*
 * The only interface between logic and silicon. Every module above lib/HAL
 * takes an IHAL* so it can be exercised on the host with MockHal.
 *
 * now_ms/now_us are part of the HAL on purpose: every timing rule in the spec
 * (500ms double-press window, 750ms long-press threshold, 200ms key send,
 * 5-minute maintenance timeout) is a tested rule, and the only way to test a
 * timing rule without sleeping is to make the clock an input.
 *
 * dac_set_code does NOT take a power mode. Setting a channel's gain mode *is*
 * a power-mode change (spec 2.3: PD1:PD0 = 01 selects gain 1.82), and it is
 * made independently of any code write, so it is its own member.
 *
 * dac_set_code returns void deliberately (spec 6.8): the driver retries with
 * backoff internally and latches a fault on persistent failure; it never
 * drives a guessed code. There is no useful failure for a caller to branch on.
 */
typedef struct IHAL {
    int      (*adc_read_mv)(void *ctx, AdcChannel ch);
    void     (*dac_set_code)(void *ctx, DacChannel ch, uint16_t code);
    void     (*dac_power_mode)(void *ctx, DacChannel ch, DacPowerMode mode);
    void     (*dac_ldac)(void *ctx, bool assert);
    void     (*gpio_write)(void *ctx, GpioPin pin, bool level);
    bool     (*gpio_read)(void *ctx, GpioPin pin);
    void     (*buzzer_on)(void *ctx, bool on);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    int      (*nvs_get)(void *ctx, const char *key, void *out, size_t len);
    int      (*nvs_set)(void *ctx, const char *key, const void *in, size_t len);
    void     (*reboot)(void *ctx);
    void      *ctx;
} IHAL;

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
 * Host implementation of IHAL. All state is observable, so tests assert on
 * what the code *did to the hardware*, not on internal variables.
 */
class MockHal {
public:
    MockHal();

    IHAL &InterfaceRef() { return iface_; }   // every later task's tests take &hal.InterfaceRef()

    // --- clock -------------------------------------------------------------
    uint64_t NowMs() { return now_ms_; }
    uint64_t NowUs() { return now_ms_ * 1000ULL; }
    void AdvanceMs(uint64_t ms) { now_ms_ += ms; }

    // --- analog ------------------------------------------------------------
    void SetAdcMilliVolts(AdcChannel ch, int mv) { adc_mv_[static_cast<int>(ch)] = mv; }
    int AdcReadMv(AdcChannel ch) { return adc_mv_[static_cast<int>(ch)]; }

    void DacSetCode(DacChannel ch, uint16_t code);
    void DacPowerMode(DacChannel ch, ::DacPowerMode mode);
    void DacLdac(bool assert) { ldac_asserted_ = assert; }
    uint16_t LastDacCode(DacChannel ch) const;
    ::DacPowerMode LastDacPowerMode(DacChannel ch) const;
    int DacWriteCount(DacChannel ch) const;
    bool LastLdac() const { return ldac_asserted_; }

    // --- gpio --------------------------------------------------------------
    void GpioWrite(GpioPin pin, bool level);
    bool GpioRead(GpioPin pin) const;
    void SetGpioInput(GpioPin pin, bool level) { gpio_in_[static_cast<int>(pin)] = level; }
    int GpioWriteCount(GpioPin pin) const;

    // --- buzzer ------------------------------------------------------------
    // The buzzer is active at a fixed ~2.4 kHz with no pitch control (spec
    // 5.5), so it is a plain on/off line, not a GPIO. Level is what tests
    // assert on; BuzzerOnCount counts drive calls, which is what makes
    // "idle ticks must be silent" checkable.
    void BuzzerOn(bool on) { buzzer_on_ = on; ++buzzer_calls_; }
    bool BuzzerIsOn() const { return buzzer_on_; }
    int BuzzerOnCount() const { return buzzer_calls_; }

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
    // Flip one bit at `offset` in a stored blob, to prove CRC catches it.
    void CorruptNvsValue(const char *key, size_t offset);
    void ClearNvs() { nvs_.clear(); }
    int RebootCount() const { return reboot_count_; }

    // Advance the clock and hand it to the interface (for poll loops).
    void Tick(uint64_t ms) { AdvanceMs(ms); }

private:
    static int  AdcReadMvThunk(void *ctx, AdcChannel ch);
    static void DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code);
    static void DacPowerModeThunk(void *ctx, DacChannel ch, ::DacPowerMode m);
    static void DacLdacThunk(void *ctx, bool assert);
    static void GpioWriteThunk(void *ctx, GpioPin pin, bool level);
    static bool GpioReadThunk(void *ctx, GpioPin pin);
    static void BuzzerOnThunk(void *ctx, bool on);
    static uint64_t NowMsThunk(void *ctx);
    static uint64_t NowUsThunk(void *ctx);
    static int  NvsGetThunk(void *ctx, const char *key, void *out, size_t len);
    static int  NvsSetThunk(void *ctx, const char *key, const void *in, size_t len);
    static void RebootThunk(void *ctx);

    IHAL iface_{};
    uint64_t now_ms_ = 0;
    int adc_mv_[ADC_CH_COUNT] = {};
    uint16_t dac_code_[DAC_CH_COUNT] = {};
    ::DacPowerMode dac_mode_[DAC_CH_COUNT] = {};
    int dac_writes_[DAC_CH_COUNT] = {};
    bool ldac_asserted_ = false;
    bool gpio_out_[GPIO_COUNT] = {};
    bool gpio_in_[GPIO_COUNT] = {};
    int gpio_writes_[GPIO_COUNT] = {};
    bool buzzer_on_ = false;
    int buzzer_calls_ = 0;
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
    iface_.dac_power_mode = &MockHal::DacPowerModeThunk;
    iface_.dac_ldac       = &MockHal::DacLdacThunk;
    iface_.gpio_write     = &MockHal::GpioWriteThunk;
    iface_.gpio_read      = &MockHal::GpioReadThunk;
    iface_.buzzer_on      = &MockHal::BuzzerOnThunk;
    iface_.now_ms         = &MockHal::NowMsThunk;
    iface_.now_us         = &MockHal::NowUsThunk;
    iface_.nvs_get        = &MockHal::NvsGetThunk;
    iface_.nvs_set        = &MockHal::NvsSetThunk;
    iface_.reboot         = &MockHal::RebootThunk;
    iface_.ctx            = this;
}

void MockHal::DacSetCode(DacChannel ch, uint16_t code) {
    const int i = static_cast<int>(ch);
    dac_code_[i] = code;
    ++dac_writes_[i];
}

void MockHal::DacPowerMode(DacChannel ch, ::DacPowerMode mode) {
    dac_mode_[static_cast<int>(ch)] = mode;
}

void MockHal::CorruptNvsValue(const char *key, size_t offset) {
    auto it = nvs_.find(key);
    if (it == nvs_.end() || offset >= it->second.size()) return;
    it->second[offset] ^= 0x01;
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
    // Outputs read back what was written (the LEDs); the two digital inputs read
    // their programmed input state. Nothing else is a GpioPin -- SENSE1/SENSE2
    // are ADC channels (AdcReadMv), and the buzzer and ~LDAC are the semantic
    // members BuzzerIsOn() and LastLdac().
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
void MockHal::DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code) {
    static_cast<MockHal *>(ctx)->DacSetCode(ch, code);
}
void MockHal::DacPowerModeThunk(void *ctx, DacChannel ch, ::DacPowerMode m) {
    static_cast<MockHal *>(ctx)->DacPowerMode(ch, m);
}
void MockHal::DacLdacThunk(void *ctx, bool assert) {
    static_cast<MockHal *>(ctx)->ldac_asserted_ = assert;
}
void MockHal::BuzzerOnThunk(void *ctx, bool on) {
    static_cast<MockHal *>(ctx)->BuzzerOn(on);
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

Run: `cd code && pio test -e native -f '*test_hal'`
Expected: PASS — 9 tests green.

- [ ] **Step 7: Commit**

```bash
git add code/lib/HAL/IHAL.h code/test_native/MockHAL.h code/test_native/MockHAL.cpp \
        code/test_native/test_hal/MockHalTest.cpp \
        code/test_native/test_hal/test_main.cpp
git commit -m "Add the IHAL seam and a MockHal with an injectable clock

The clock is part of the HAL so every timing rule in the spec (500ms double
press, 750ms long press, 200ms key send, 5-minute maintenance timeout) is
testable without sleeping. MockHal also injects NVS write failures and torn
writes, which is how the A/B persistence scheme gets tested for power loss."
```

---

## Phase 1 — Analog and classification (the core correctness problem)

### Task 3: `LadderDecode` — ratio-normalized classification

The single most important behavior in the device. The requirement is not "decode
a ladder"; it is **classify the same physical button identically across the
+3V3 rail's ±5 % tolerance band** (spec §11 FR-30). Normalizing by the measured
idle level is what makes that true, and this task's tests are the entire
justification for the design.

**The input pulls DOWN, so idle is the HIGH reading** (spec §6.3). Every learned
button's ratio is *below* 1000 permille. An earlier revision had this inverted,
with buttons above idle and a 5 V ladder swept against an 11–14.8 V "vehicle
rail" — no such term exists in the transfer function.

**Files:**
- Create: `code/lib/Analog/LadderDecode.h`
- Create: `code/lib/Analog/LadderDecode.cpp`
- Create: `code/test_native/test_analog/LadderDecodeTest.cpp`
- Create: `code/test_native/test_analog/test_main.cpp` — required; copy Task 2's
  four-line `main()` (see the Test-harness API note in the Shared contract).

**Interfaces:**
- Consumes: nothing
- Produces:
  - `struct LadderButton { char id[16]; char name[16]; MilliVolt mv_center; MilliVolt mv_tolerance; MilliVolt learned_at_rail_mv; int16_t temp_c_at_learn; uint16_t sample_count; uint8_t confidence; }` — spec §3.4's eight fields. **No `ratio_permille`/`tolerance_permille` field and no `action_id`**: the ratio is derived at classify time (storing both forms is 105 % of the partition), and numeric action ids do not exist in this model.
  - `struct LadderProfile { uint8_t source; MilliVolt learned_idle_mv; uint8_t count; LadderButton buttons[16]; }`
  - `enum class ClassifyResult { kIdle, kButton, kUnknown, kFault }`
  - `struct ClassifyOutcome { ClassifyResult result; uint8_t index; int16_t ratio_permille; }`
  - `ClassifyOutcome LadderClassify(const LadderProfile &p, int level_mv, int idle_mv)`
  - `int16_t LadderRatioPermille(int level_mv, int idle_mv)` — `level_mv * 1000 / idle_mv`, rounded

- [ ] **Step 1: Write the failing test — starting with the rail-immunity test**

`code/test_native/test_analog/LadderDecodeTest.cpp`:

```cpp
#include "Analog/LadderDecode.h"
#include <gtest/gtest.h>

namespace {

constexpr int kIdleMv = 2835;

/*
 * The spec 3.7 default ladder, in the units the decoder actually works in.
 *
 * The decoder normalizes against the IDLE READING, not against a rail
 * (spec 6.3: n = V_ADC / V_ADC_idle). A press pulls the input DOWN from idle,
 * so every button's ratio is BELOW 1000 and idle is 1000 by construction.
 *
 *   2835 mV idle -> VOL_UP 1430, VOL_DOWN 1785, NEXT 2145
 *   in permille of idle: 1430/2835 = 504, 1785/2835 = 630, 2145/2835 = 757
 *   tolerance 120 mV = 42 permille; 110 mV = 39 permille
 *
 * The fields are MILLIVOLTS (spec 3.4), and they are scaled to `learned_idle_mv`.
 * That scaling is the whole point of storing mv against a recorded rail: the
 * levels above are what the ladder produced at the 2835 mV nominal rail, so a
 * profile "learned at" a different rail must carry levels measured at THAT rail
 * (V_button = ratio x V_rail). Without it the fixture would claim a rail it did
 * not use, and the derived window would drift with the sweep instead of staying
 * fixed -- passing for the wrong reason.
 *
 * The permille figures above are what the classifier derives, and are noted so
 * the expectations below stay readable. Storing the permille instead of the mv
 * was 105% of the NVS partition (spec 3.5).
 */
LadderProfile MakeProfile(int learned_idle_mv) {
    const auto at_rail = [learned_idle_mv](int nominal_mv) {
        return static_cast<uint16_t>(
            (static_cast<long long>(nominal_mv) * learned_idle_mv + kIdleMv / 2) / kIdleMv);
    };
    LadderProfile p{};
    p.learned_idle_mv = learned_idle_mv;
    p.count = 3;
    p.buttons[0] = {"VOL_UP",   "Volume Up",   at_rail(1430), at_rail(120), 3300, 235, 200, 98};
    p.buttons[1] = {"VOL_DOWN", "Volume Down", at_rail(1785), at_rail(120), 3300, 235, 200, 97};
    p.buttons[2] = {"NEXT",     "Next Track",  at_rail(2145), at_rail(110), 3300, 235, 200, 99};
    return p;
}

}  // namespace

TEST(LadderRatio, IsLevelOverIdleInPermille) {
    EXPECT_EQ(LadderRatioPermille(2500, 5000), 500);
    EXPECT_EQ(LadderRatioPermille(5000, 5000), 1000);
    EXPECT_EQ(LadderRatioPermille(0, 5000), 0);
    EXPECT_EQ(LadderRatioPermille(1150, 5750), 200);
}

TEST(LadderClassify, IdleReturnsIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    EXPECT_EQ(LadderClassify(p, kIdleMv, kIdleMv).result, ClassifyResult::kIdle);
    EXPECT_EQ(LadderClassify(p, kIdleMv - 20, kIdleMv).result, ClassifyResult::kIdle);
}

TEST(LadderClassify, EachLearnedButtonClassifiesToItsOwnIndex) {
    LadderProfile p = MakeProfile(kIdleMv);
    EXPECT_EQ(LadderClassify(p, 1430, kIdleMv).index, 0);  // 504 permille
    EXPECT_EQ(LadderClassify(p, 1785, kIdleMv).index, 1);  // 630 permille
    EXPECT_EQ(LadderClassify(p, 2145, kIdleMv).index, 2);  // 757 permille
}

/*
 * THE test. The +3V3 rail moves across its regulator tolerance band (spec 11
 * FR-30). The same physical button must classify identically across the whole
 * band. This is why the decode normalizes: an absolute millivolt window would
 * classify correctly at exactly one rail voltage. Both the numerator and the
 * idle reference come off the same ADC with the same reference, so the ratio
 * is invariant to the rail.
 *
 * NOTE 1: this is a +3V3 sweep, NOT a vehicle-rail sweep. An earlier revision
 * swept 11.0-14.8 V against a 5 V ladder; no such term exists in the transfer
 * function (spec 6.3).
 *
 * NOTE 2: the idle reference has only **2.29 % of headroom** before it reaches
 * the 2900 mV ADC ceiling — with the spec 3.7 nominal idle of 2835 mV, that is
 * ~3375 mV of rail. Above that the idle reading clips while the button reading
 * does not, so the ratio is distorted. It survives anyway (the distortion is
 * ~3.5 % at +6 % overvoltage, well inside the +/-8.3 % window), which the
 * clipping test below asserts. But the margin is thin, and it is another reason
 * the bring-up measurement (spec 10.6) gates the R15/R16 decision.
 */
TEST(LadderClassify, SameButtonClassifiesIdenticallyAcrossTheThreeVoltThreeSweep) {
    for (int rail_mv = 3140; rail_mv <= 3470; rail_mv += 10) {
        // Idle scales with the rail; so does the button level. The ratio does not.
        const int idle_mv  = (kIdleMv * rail_mv) / 3300;
        const int level_mv = (idle_mv * 504) / 1000;   // VOL_UP at 504 permille
        LadderProfile p = MakeProfile(idle_mv);
        const ClassifyOutcome out = LadderClassify(p, level_mv, idle_mv);
        ASSERT_EQ(out.result, ClassifyResult::kButton)
            << "rail=" << rail_mv << " idle=" << idle_mv << " level=" << level_mv;
        ASSERT_EQ(out.index, 0) << "rail=" << rail_mv;
    }
}

TEST(LadderClassify, StillClassifiesWhenTheIdleReferenceClipsAtTheAdcCeiling) {
    // At +6% of rail the true idle (3006 mV) is above the 2900 mV ceiling, so
    // the measured idle reference saturates while the pressed reading does not.
    // The ratio shifts but must stay inside the button's window: classification
    // degrades gracefully rather than dropping the press.
    //
    // MakeProfile scales its levels to the rail it is learned at, so at 3006 the
    // VOL_UP level is 3006 x 0.504 = 1515 mV -- still below the ceiling, which is
    // why the press survives while the reference does not.
    LadderProfile p = MakeProfile(3006);
    const int clipped_idle_mv = 2900;
    const int pressed_mv = (3006 * 504) / 1000;   // 1515 mV, still below the ceiling
    const ClassifyOutcome out = LadderClassify(p, pressed_mv, clipped_idle_mv);
    EXPECT_EQ(out.result, ClassifyResult::kButton)
        << "a saturated idle reference must not lose the press";
    EXPECT_EQ(out.index, 0);
}

TEST(LadderClassify, UnlearnedLevelIsUnknownAndNeverGuessed) {
    LadderProfile p = MakeProfile(kIdleMv);
    // 2400 mV is 847 permille: between NEXT (757 +/- 39) and idle (1000 - 30).
    const ClassifyOutcome out = LadderClassify(p, 2400, kIdleMv);
    EXPECT_EQ(out.result, ClassifyResult::kUnknown);
}

TEST(LadderClassify, LevelAboveTheReferenceIsAFaultNotAButtonOrIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    // A short to a supply above the idle reference. Reporting this as IDLE
    // would be the worst outcome -- the user's button would do nothing and
    // nothing would say why.
    EXPECT_EQ(LadderClassify(p, 3000, kIdleMv).result, ClassifyResult::kFault);
}

TEST(LadderClassify, CollapsedRailIsAFaultNotAnIdle) {
    LadderProfile p = MakeProfile(kIdleMv);
    // FR-30: an idle reading at or below 20% of the learned value is a rail
    // fault, not idle. Checked against the LEARNED idle, because ratio
    // normalization deliberately cancels rail movement out of the ratios.
    EXPECT_EQ(LadderClassify(p, 500, 500).result, ClassifyResult::kFault);
}

TEST(LadderClassify, ToleranceBoundaryIsInclusiveAtTheEdgeAndExclusiveBeyond) {
    LadderProfile p = MakeProfile(kIdleMv);
    // window 504 +/- 42 permille -> [462, 546]
    EXPECT_EQ(LadderClassify(p, 1310, kIdleMv).result, ClassifyResult::kButton);  // 462 incl
    EXPECT_EQ(LadderClassify(p, 1548, kIdleMv).result, ClassifyResult::kButton);  // 546 incl
    EXPECT_EQ(LadderClassify(p, 1307, kIdleMv).result, ClassifyResult::kUnknown); // 461
    EXPECT_EQ(LadderClassify(p, 1551, kIdleMv).result, ClassifyResult::kUnknown); // 547
}

TEST(LadderClassify, OverlappingWindowsResolveToTheNearestCentreNotTheFirstMatch) {
    LadderProfile p = MakeProfile(kIdleMv);
    // Two deliberately overlapping windows, in millivolts at the 2835 idle:
    // A centre 1418 (500 permille) half 227 (80 permille) -> [420,580]
    // B centre 1531 (540 permille) half 227 (80 permille) -> [460,620]
    p.count = 2;
    p.buttons[0] = {"A", "Button A", 1418, 227, 3300, 235, 200, 98};
    p.buttons[1] = {"B", "Button B", 1531, 227, 3300, 235, 200, 98};
    EXPECT_EQ(LadderClassify(p, 1418, kIdleMv).index, 0);  // 500 -> centre 0
    EXPECT_EQ(LadderClassify(p, 1531, kIdleMv).index, 1);  // 540 -> centre 1
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_analog'`
Expected: FAIL — `Analog/LadderDecode.h` not found.

- [ ] **Step 3: Write `lib/Analog/LadderDecode.h`**

```cpp
#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

// Spec 3.2's value type: a pin voltage in millivolts, 0-2900 (the ADC's
// calibrated ceiling). The Shared contract puts this in IHAL.h; it is declared
// here instead because Task 2's IHAL.h block never actually defines it, and
// widening a frozen C header that Task 2's committed tests depend on is a
// larger change than this task needs. When IHAL.h gains the typedef, delete
// this one -- a duplicate typedef of the same type is not an error in C++.
using MilliVolt = uint16_t;

// Ratios are permille (thousandths of the idle reference) so the whole
// comparison path is integer arithmetic. Floating point on this target is
// slower and the windows are generous enough (tens of permille) that integer
// rounding is irrelevant.
constexpr int kLadderMaxButtons = 16;
// Widths are a budget input, not a preference: the config JSON must fit two
// NVS slots, and these strings are part of the structural worst case
// (spec 3.5's width table). Widening them overflows the partition.
constexpr int kLadderIdLen   = 16;
constexpr int kLadderNameLen = 16;

// The spec 3.4 shape, in millivolts at the pin. The *ratio* the classifier
// compares is DERIVED at classify time from mv_center and the profile's learned
// idle -- it is deliberately not stored, because storing both forms is 105% of
// the NVS partition (spec 3.5) and a stored ratio is a second home for a value
// that mv_center already determines.
struct LadderButton {
    char      id[kLadderIdLen];        // stable slug, referenced by Binding.button
    char      name[kLadderNameLen];    // display only
    MilliVolt mv_center;               // pin voltage when this button is held
    MilliVolt mv_tolerance;            // half-width of the accept window
    MilliVolt learned_at_rail_mv;      // the +3V3 rail (approx 3300), NOT 12 V
    int16_t   temp_c_at_learn;         // tenths of a degree C, for FR-17
    uint16_t  sample_count;            // samples averaged at learn
    uint8_t   confidence;              // learn-quality score, 0-100
};

struct LadderProfile {
    uint8_t      source;            // spec 3.1: a direct analog input
    // The idle reading at LEARN time. This is the normalization reference, so it
    // must NOT be the current idle: mv_center is pinned to the rail that was
    // present when it was learned, and the rail-health check (FR-30) compares
    // the CURRENT idle against this one. The wire field is `idle_mv` (spec 3.7);
    // the struct keeps the `learned_` prefix because the distinction is what the
    // rail-health check is made of.
    MilliVolt    learned_idle_mv;
    uint8_t      count;
    LadderButton buttons[kLadderMaxButtons];
};

enum class ClassifyResult { kIdle, kButton, kUnknown, kFault };

struct ClassifyOutcome {
    ClassifyResult result;
    uint8_t        index;   // valid only when result == kButton
    int16_t        ratio_permille;
};

// level_mv as a fraction of the IDLE reading, in permille (spec 6.3:
// n = V_ADC / V_ADC_idle). Idle is 1000 by construction; a press pulls the
// input DOWN, so every learned button's ratio is below 1000.
// Callers must guarantee idle_mv > 0; LadderClassify reports kFault otherwise.
int16_t LadderRatioPermille(int level_mv, int idle_mv);

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int idle_mv);
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
// The idle reading has collapsed relative to the one learned. Expressed
// against the LEARNED idle, not the ratio: ratio normalization deliberately
// cancels rail movement out of the ratios, so a dead supply looks perfectly
// normal to it. This is the check that catches that (FR-30).
constexpr int16_t kRailHealthFloorPermille = 200;
}  // namespace

int16_t LadderRatioPermille(int level_mv, int idle_mv) {
    if (idle_mv <= 0) return -1;
    const long long scaled = (static_cast<long long>(level_mv) * 1000LL + idle_mv / 2) / idle_mv;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int idle_mv) {
    ClassifyOutcome out{ClassifyResult::kFault, 0, 0};
    if (idle_mv <= 0) return out;

    const int16_t ratio = LadderRatioPermille(level_mv, idle_mv);
    out.ratio_permille = ratio;

    // FR-30: a 3V3 sag to <=20% of the learned idle is a rail fault. Checked
    // before anything else, because at that level every ratio is garbage.
    if (profile.learned_idle_mv > 0 &&
        idle_mv < (profile.learned_idle_mv * kRailHealthFloorPermille) / 1000) {
        return out;                                                    // reference collapsed
    }

    if (ratio < 0 || ratio > 1000 + kIdleMarginPermille) return out;   // above the reference

    if (ratio >= 1000 - kIdleMarginPermille) {
        out.result = ClassifyResult::kIdle;
        return out;
    }

    // Nearest-centre match, so two overlapping windows resolve deterministically
    // to whichever button the user actually pressed rather than to array order.
    //
    // The centre and half-width are DERIVED from millivolts against the LEARNED
    // idle, not read from stored ratio fields (spec 3.4/3.5: storing both forms
    // does not fit the NVS budget). Deriving is also the more correct of the two:
    // the learned idle is the rail the mv_center values were measured at, so the
    // ratio is the same number either way, but a stored copy could drift from it.
    int best = -1;
    int best_distance = 0;
    for (uint8_t i = 0; i < profile.count && i < kLadderMaxButtons; ++i) {
        const int centre = LadderRatioPermille(profile.buttons[i].mv_center,
                                               profile.learned_idle_mv);
        const int half   = LadderRatioPermille(profile.buttons[i].mv_tolerance,
                                               profile.learned_idle_mv);
        if (centre < 0 || half < 0) continue;
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

Run: `cd code && pio test -e native -f '*test_analog'`
Expected: PASS — 10 tests green, including the 34-point +3V3 sweep.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Analog/LadderDecode.h code/lib/Analog/LadderDecode.cpp \
        code/test_native/test_analog/LadderDecodeTest.cpp
git commit -m "Add ratio-normalized ladder classification

Classifies on V_ADC/V_ADC_idle rather than absolute millivolts, so a button
learned at one rail voltage classifies identically across the +3V3 tolerance
band (spec 11 FR-30). An unlearned level is kUnknown and is never guessed; a
collapsed rail or a reading above the idle reference is a fault, never a button."
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
  - `enum class CalibrationSource { kEFuseCurveFit, kLinearFallback }`
  - `struct AdcCalibration { uint16_t raw_low; uint16_t raw_high; int mv_low; int mv_high; CalibrationSource source; }`
  - `AdcCalibration AdcCalibrationSelect(bool curve_fit_supported)`
  - `int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw)`

**This module owns spec §3.2's fallback requirement.** IDF's
`adc_cali_create_scheme_curve_fitting()` returns `ESP_ERR_NOT_SUPPORTED` on
modules whose eFuses are missing, and the spec is explicit that the firmware
must *not* silently mis-scale: it falls back to a documented linear
approximation **and reports that it did**. That "and reports" is why
`CalibrationSource` is a field on the struct rather than a local — a silent
fallback is the fault, and a value nothing can read is not a report.
`AdcCalibrationSelect` is the decision, host-testable without an eFuse; Task 14
supplies `curve_fit_supported` from the real call's return code.

- [ ] **Step 1: Write the failing test**

```cpp
#include "Analog/CalibrationCurve.h"
#include <gtest/gtest.h>

TEST(AdcCalibration, EndpointsMapExactly) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_low), cal.mv_low);
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_high), cal.mv_high);
}

TEST(AdcCalibration, IsMonotonicAcrossTheEntireRawRange) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
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
    const AdcCalibration cal = AdcCalibrationSelect(true);
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        ASSERT_LE(AdcRawToMilliVolts(cal, raw), kAdcFullScaleMv12dB) << "raw=" << raw;
    }
}

TEST(AdcCalibration, MidScaleIsApproximatelyHalfOfFullScale) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
    const int mv = AdcRawToMilliVolts(cal, kAdcMaxRawS3 / 2);
    EXPECT_NEAR(mv, kAdcFullScaleMv12dB / 2, 25);
}

TEST(AdcCalibration, AMissingEFuseFallsBackAndSaysSo) {
    // Spec 3.2: curve-fitting calibration is per-chip and eFuse-backed, and
    // returns ESP_ERR_NOT_SUPPORTED on modules whose eFuses are blank. The spec
    // forbids silently mis-scaling every reading, so the fallback must both
    // happen AND be reported -- which is what CalibrationSource is for.
    const AdcCalibration curve = AdcCalibrationSelect(true);
    const AdcCalibration fallback = AdcCalibrationSelect(false);
    EXPECT_EQ(curve.source, CalibrationSource::kEFuseCurveFit);
    EXPECT_EQ(fallback.source, CalibrationSource::kLinearFallback);
    // The fallback is still a usable conversion: monotonic, in range, and
    // agreeing with the calibrated curve at both endpoints.
    int prev = -1;
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        const int mv = AdcRawToMilliVolts(fallback, raw);
        ASSERT_GE(mv, prev) << "non-monotonic at raw=" << raw;
        ASSERT_LE(mv, kAdcFullScaleMv12dB) << "raw=" << raw;
        prev = mv;
    }
    EXPECT_EQ(AdcRawToMilliVolts(fallback, fallback.raw_low), fallback.mv_low);
    EXPECT_EQ(AdcRawToMilliVolts(fallback, fallback.raw_high), fallback.mv_high);
}

TEST(AdcCalibration, TheSenseBufferClipsBeforeTheAdcCeiling) {
    // Spec 2.3: the sense buffer U6B is ITSELF an op-amp on +5V, so its output
    // saturates near 4.98V whatever the KEY line does. Through the exact /2
    // divider that is 2490mV. The 5.20V figure is the head-unit IDLE ENVELOPE
    // bound (spec 6.2) and does NOT reach the divider unclipped -- taking it
    // literally would give 2600mV, a voltage U6B cannot produce.
    const int v_buf_saturation_mv = 4980;
    const int v_sense_mv = v_buf_saturation_mv / 2;
    EXPECT_LT(v_sense_mv, kAdcFullScaleMv12dB);
    EXPECT_EQ(v_sense_mv, 2490);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_analog'`
Expected: FAIL — `CalibrationCurve.h` not found.

- [ ] **Step 3: Write `lib/Analog/CalibrationCurve.h`**

```cpp
#pragma once

#include <stdint.h>

constexpr int kAdcMaxRawS3       = 4095;
constexpr int kAdcFullScaleMv12dB = 2900;

// Spec 3.2: curve-fitting calibration is eFuse-backed and per-chip. It is NOT
// universally available -- adc_cali_create_scheme_curve_fitting() returns
// ESP_ERR_NOT_SUPPORTED on modules with blank eFuses, and the spec requires the
// firmware to fall back to a documented linear approximation *and report that
// it did*, rather than silently mis-scaling every reading. Carrying the source
// on the struct is what makes "report" possible; a caller that never checks it
// is the silent-fallback fault the spec names.
enum class CalibrationSource { kEFuseCurveFit, kLinearFallback };

// Two-point calibration, which is the shape the ESP-IDF curve-fit calibration
// exposes after its own polynomial stage. Keeping the curve's *effect* behind
// this interface is what lets the host tests run without the eFuse.
struct AdcCalibration {
    uint16_t          raw_low;
    uint16_t          raw_high;
    int               mv_low;
    int               mv_high;
    CalibrationSource source;
};

// Chooses the curve for this chip. Task 14 passes the real
// adc_cali_create_scheme_curve_fitting() return code; the host tests pass both
// values to exercise the fallback without an eFuse.
AdcCalibration AdcCalibrationSelect(bool curve_fit_supported);

int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw);
```

- [ ] **Step 4: Write the minimal implementation**

```cpp
#include "Analog/CalibrationCurve.h"

#include <algorithm>

AdcCalibration AdcCalibrationSelect(bool curve_fit_supported) {
    // Both branches use the same data-sheet endpoints for 12dB attenuation. The
    // real difference is the curve *between* them: the eFuse curve-fit is a
    // per-chip polynomial (spec 2.3, -30..0mV post-calibration error), while
    // the fallback is the straight line the spec calls "a documented linear
    // approximation". The source field is what tells a caller which one it got.
    return AdcCalibration{
        /*raw_low=*/0,
        /*raw_high=*/kAdcMaxRawS3,
        /*mv_low=*/0,
        /*mv_high=*/kAdcFullScaleMv12dB,
        /*source=*/curve_fit_supported ? CalibrationSource::kEFuseCurveFit
                                       : CalibrationSource::kLinearFallback,
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

Run: `cd code && pio test -e native -f '*test_analog'`
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
- Create: `code/test_native/test_output/test_main.cpp` — required; copy Task 2's
  four-line `main()`. Task 7 adds a second suite to this same directory and must
  **not** add a second `main()`.

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

Run: `cd code && pio test -e native -f '*test_output'`
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
// The V_ADJ coefficient (0.82). KeyMv below collapses the V_ADJ term away in
// both modes rather than evaluating it, so this constant documents the divider
// pair rather than feeding the arithmetic. It is deliberately unused -- the
// project builds with -Wall -Wextra -Werror, which rejects an unreferenced
// constexpr, so it carries the attribute. (clang enforces that; the device's
// GCC 14.2.0 does not, so this only fails on the host build -- which is the
// stricter of the two.)
[[maybe_unused]] constexpr int kAdjRatioMilli = (kGainR58 * 1000) / kGainR61;  // 820

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

Run: `cd code && pio test -e native -f '*test_output'`
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
- Create: `code/test_native/test_gesture/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

**Interfaces:**
- Consumes: `LadderProfile`, `LadderClassify` from Task 3
- Produces:
  - `struct GestureTimings { uint32_t debounce_ms; uint32_t double_press_off_ms; uint32_t long_press_ms; uint32_t send_duration_ms; }`
  - `GestureTimings GestureTimingsDefault()` — 25 / 500 / 750 / 200
  - `enum class ChannelLevel { kIdle, kPressed, kUnknown, kFault }`
  - `class PressClassifier { ChannelLevel Update(int level_mv, int idle_mv, uint64_t now_ms); ChannelLevel Level() const; uint8_t ButtonIndex() const; void Reset(); }`
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
    p.learned_idle_mv = 2835;
    p.count = 1;
    p.buttons[0] = {"VOL_UP", 504, 42, 1};
    return p;
}
}  // namespace

TEST(PressClassifier, ByteNoiseBelowTheDebounceWindowIsNotAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    // One 10ms sample dips into the window: not a press.
    EXPECT_EQ(c.Update(1430, 2835, 1000), ChannelLevel::kIdle);
    EXPECT_EQ(c.Update(2835, 2835, 1010), ChannelLevel::kIdle);
}

TEST(PressClassifier, ASustainedLevelBecomesPressedAfterDebounce) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    ChannelLevel level = ChannelLevel::kIdle;
    for (int i = 0; i < 5; ++i) {  // 50ms of sustained press > 25ms debounce
        level = c.Update(1430, 2835, t);
        t += 10;
    }
    EXPECT_EQ(level, ChannelLevel::kPressed);
    EXPECT_EQ(c.ButtonIndex(), 0);
}

TEST(PressClassifier, HysteresisKeepsAPressLatchedThroughASmallDip) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // 1605 mV is 566 permille: in the GAP between VOL_UP's window (504+/-42,
    // so [462,546]) and VOL_DOWN's ([588,672]). It classifies kUnknown, which
    // is the ONLY branch hysteresis acts on -- a value still inside the window
    // would classify kButton and never reach it. Hysteresis must hold the press
    // rather than let a drifting finger release it.
    for (int i = 0; i < 5; ++i) { c.Update(1605, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
}

TEST(PressClassifier, ReleaseRequiresReturningToIdleNotMerelyLeavingTheWindow) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.Level(), ChannelLevel::kPressed);
    // 2400 mV is 846 permille -- between the window and idle: still held.
    c.Update(2400, 2835, t); t += 10;
    EXPECT_EQ(c.Level(), ChannelLevel::kPressed);
    for (int i = 0; i < 5; ++i) { c.Update(2835, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kIdle);
}

TEST(PressClassifier, FaultPropagatesAndNeverReadsAsAPress) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(500, 500, t); t += 10; }  // collapsed rail
    EXPECT_EQ(c.Level(), ChannelLevel::kFault);
}

TEST(PressClassifier, UnlearnedLevelIsUnknownNotPressed) {
    PressClassifier c(Profile(), GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(2400, 2835, t); t += 10; }
    EXPECT_EQ(c.Level(), ChannelLevel::kUnknown);
    EXPECT_EQ(c.ButtonIndex(), 0xFF);
}

TEST(PressClassifier, SwitchingButtonsMidPressReportsTheNewButtonAfterDebounce) {
    LadderProfile p = Profile();
    p.count = 2;
    p.buttons[1] = {"VOL_DOWN", 630, 42, 2};
    PressClassifier c(p, GestureTimingsDefault());
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) { c.Update(1430, 2835, t); t += 10; }
    ASSERT_EQ(c.ButtonIndex(), 0);
    for (int i = 0; i < 8; ++i) { c.Update(1785, 2835, t); t += 10; }
    EXPECT_EQ(c.ButtonIndex(), 1);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_gesture'`
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

// Defaults from spec 3.7, so the feel matches the 2022 Pico firmware.
inline GestureTimings GestureTimingsDefault() {
    return GestureTimings{/*debounce_ms=*/25,
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

    // level_mv is the calibrated reading and idle_mv the current idle
    // reference; both in millivolts (Task 4). Classification normalizes the
    // former by the latter (spec 6.3).
    ChannelLevel Update(int level_mv, int idle_mv, uint64_t now_ms);

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

ChannelLevel PressClassifier::Update(int level_mv, int idle_mv, uint64_t now_ms) {
    const ClassifyOutcome outcome = LadderClassify(profile_, level_mv, idle_mv);

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

Run: `cd code && pio test -e native -f '*test_gesture'`
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
    // DOUBLE fires at the START of the second press, not on its release -- see
    // the implementation's pressed-branch comment, and note that
    // DoubleWindowBoundaryIsInclusiveAt499AndExclusiveAt500 reads kDouble from a
    // kPressed Update. So THIS Feed carries the DOUBLE. An earlier revision read
    // it from the release Feed below, which correctly returns kNone, so that
    // form could never pass -- verified by building this block verbatim against
    // the committed GestureStateMachine: 8 of 9 passed, this one failed.
    ev = Feed(sm, ChannelLevel::kPressed, 0, now, 100);
    EXPECT_EQ(ev.gesture, Gesture::kDouble);
    EXPECT_EQ(ev.button_index, 0);
    // ...and the release must be silent: FR-11's "the second press of a DOUBLE
    // must not emit its own SINGLE".
    ev = Feed(sm, ChannelLevel::kIdle, 0, now, 600);
    EXPECT_EQ(ev.gesture, Gesture::kNone) << "the second press must not also emit a SINGLE";
}

TEST(Gesture, DoubleWindowBoundaryIsInclusiveAt499AndExclusiveAt500) {
    // Spec 10.4 requires the 500ms boundary at exactly 499/500/501. Feed()
    // CANNOT express it: its loop steps 10ms and runs `elapsed < hold_ms`, so
    // Feed(idle,499) and Feed(idle,500) advance the clock identically (both to
    // gap 490) and the boundary is invisible. Direct Update calls instead.
    // The window closes when `now - released_at_ >= double_press_off_ms`, so a
    // 499ms gap is still a double and a 500ms gap is already a single.
    struct Case { uint32_t gap_ms; Gesture expect; };
    const Case cases[] = {{499, Gesture::kDouble},
                          {500, Gesture::kSingle},
                          {501, Gesture::kSingle}};
    for (const Case &c : cases) {
        GestureStateMachine sm(GestureTimingsDefault());
        GestureEvent ev{};
        sm.Update(ChannelLevel::kPressed, 0, 1000, &ev);   // press begins
        sm.Update(ChannelLevel::kIdle, 0, 1100, &ev);      // release; gap starts here
        const bool closed = sm.Update(ChannelLevel::kIdle, 0, 1100 + c.gap_ms, &ev);
        if (c.expect == Gesture::kSingle) {
            EXPECT_TRUE(closed) << "gap=" << c.gap_ms << "ms must have closed the window";
            EXPECT_EQ(ev.gesture, Gesture::kSingle) << "gap=" << c.gap_ms;
        } else {
            EXPECT_FALSE(closed) << "gap=" << c.gap_ms << "ms must not have closed yet";
            const bool dbl = sm.Update(ChannelLevel::kPressed, 0, 1100 + c.gap_ms + 10, &ev);
            EXPECT_TRUE(dbl) << "gap=" << c.gap_ms;
            EXPECT_EQ(ev.gesture, Gesture::kDouble) << "gap=" << c.gap_ms;
        }
    }
}

TEST(Gesture, LongPressFiresAtTheThresholdBeforeRelease) {
    GestureStateMachine sm(GestureTimingsDefault());
    uint64_t now = 1000;
    GestureEvent ev = Feed(sm, ChannelLevel::kPressed, 0, now, 800);
    EXPECT_EQ(ev.gesture, Gesture::kLong);
    EXPECT_GE(ev.at_ms - 1000, 750u);
    EXPECT_LT(ev.at_ms - 1000, 800u) << "must fire at the threshold, not on release";
}

TEST(Gesture, LongPressBoundaryIsSilentAt749InclusiveAt750AndAt751) {
    // Spec 10.4 requires the 750ms boundary at 749/750/751, the LONG counterpart
    // to the 500ms double window's 499/500/501. Driven by direct Update calls
    // rather than Feed(): Feed's loop is `elapsed < hold_ms` with a 10ms step,
    // so it can only land on a multiple of 10 and cannot express 749 or 751.
    // The threshold is `>=`, so 749 is silent and both 750 and 751 fire.
    const uint32_t offsets[] = {749, 750, 751};
    const bool expect_long[]  = {false, true, true};
    for (int i = 0; i < 3; ++i) {
        GestureStateMachine sm(GestureTimingsDefault());
        GestureEvent ev{};
        sm.Update(ChannelLevel::kPressed, 0, 1000, &ev);          // press begins
        const bool fired = sm.Update(ChannelLevel::kPressed, 0, 1000 + offsets[i], &ev);
        EXPECT_EQ(fired, expect_long[i]) << "offset=" << offsets[i] << "ms";
        if (expect_long[i]) {
            EXPECT_EQ(ev.gesture, Gesture::kLong) << "offset=" << offsets[i];
        }
    }
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

Run: `cd code && pio test -e native -f '*test_gesture'`
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
  (no `test_main.cpp` — Task 5 already created one for this directory)

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
    // Feed the loop alternating readings that BRACKET the target from outside
    // the deadband. An unbounded integrator would ring; a bounded one must stay
    // inside its authority band and never leave it.
    //
    // The readings must be further than deadband_mv (20) from target/2 = 2000.
    // 1990/2010 are only +-10mV -- INSIDE the deadband -- so the loop never
    // moves and this test would pass with the step cap deleted. Use +-100mV.
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    const uint16_t base = GainPolicyCodeForTarget(GainMode::kAmplified, 4000).dac_code;
    bool moved = false;
    for (int i = 0; i < 200; ++i) {
        loop.Update((i % 2) ? 1900 : 2100);
        if (loop.Code() != base) moved = true;
        const int drift = std::abs(static_cast<int>(loop.Code()) - static_cast<int>(base));
        ASSERT_LE(drift, ServoConfigDefault().max_total_codes)
            << "left the authority band at i=" << i;
    }
    // Bounded, but it MUST have trimmed: a loop that never moves trivially
    // satisfies the bound above. Note the final code returns to `base` by
    // parity (the last update is odd -> 1900), so assert on movement seen
    // during the run, not on where it happened to stop.
    EXPECT_TRUE(moved) << "the loop must have trimmed, not sat still";
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

Run: `cd code && pio test -e native -f '*test_output'`
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

Run: `cd code && pio test -e native -f '*test_output'`
Expected: PASS — 7 servo tests green.

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
- Create: `code/test_native/test_config/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

**Interfaces:**
- Consumes: `LadderProfile` (Task 3), `GestureTimings` (Task 6), `GainPolicy` (Task 5)
- Produces:
  - `constexpr uint32_t kConfigSchemaVersion = 1;`
  - `enum class BindingChannel`, `enum class ActionKind` (spec §3.6's 11 kinds)
  - `struct Action`, `struct Binding` (TOP-LEVEL — a sibling of `ChannelConfig`,
    not a member of it; spec §3.1/§3.7), `struct AuxButtonConfig`,
    `struct DeviceSettings`, `struct OutputProfile`, `struct ChannelConfig`,
    `struct Config`
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
// The scratch every test here encodes into. It MUST be sized from
// ConfigMaxSerializedSize() and not guessed: the model's JSON form is 22,407 B
// at the worst case, so the `char buf[4096]` an earlier revision used overflowed
// on every single test -- ConfigEncodeJson returns 0, the first ASSERT_GT(n, 0u)
// fires, and the suite fails for a reason that has nothing to do with the codec
// under test. A hard-coded literal also cannot notice the model growing; this
// can, and ConfigMaxSerializedSize() is constexpr precisely so it can be used
// here.
//
// static_assert, not a runtime check: if ConfigMaxSerializedSize() ever
// under-reports the true worst case, every buffer here is silently too small
// again, and the size test below would be asserting the same wrong number. The
// assert is what ties the test's memory to the model.
constexpr size_t kScratch = ConfigMaxSerializedSize();
static_assert(kScratch > 0u, "ConfigMaxSerializedSize must be a real bound");
// The size test asserts this same number fits two NVS slots; if it stops doing
// so, that test fails -- but only after this one has already allocated. Keeping
// the two together is what makes the failure legible.
static_assert(kScratch < 100000u, "a bound this large is a bug, not a config");

Config MakeConfig() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    std::strncpy(c.device_id, "SWC-0001", sizeof(c.device_id) - 1);
    c.updated_at_ms = 1700000000000ULL;
    c.settings = DeviceSettings{};
    c.settings.timings = GestureTimingsDefault();
    c.settings.gain_policy = GainPolicy::kAuto;
    c.settings.buzzer_level = 2;
    c.settings.led_level = 2;
    c.settings.temp_comp_enabled = true;
    c.settings.maintenance_timeout_ms = 300000;   // spec 5-minute maintenance window
    c.channel_count = 1;
    c.channels[0].enabled = true;
    std::strncpy(c.channels[0].name, "SWC1", sizeof(c.channels[0].name) - 1);
    c.channels[0].ladder.learned_idle_mv = 2835;
    c.channels[0].ladder.count = 1;
    // The initializer sets the whole struct including id; a separate strncpy
    // before it would be overwritten and is not there. Fields are spec 3.4's
    // millivolts: 1430 mV at the 2835 idle is the 504 permille the classifier
    // derives (1430 x 1000 / 2835 = 504).
    c.channels[0].ladder.buttons[0] = {"VOL_UP", "Volume Up", 1430, 120, 3300, 235, 200, 98};
    c.channels[0].output.gain_mode = GainMode::kAmplified;
    c.channels[0].output.idle_dac_code = 4095;    // spec 3.7's default; full scale is the safe state (6.7)
    // Bindings are TOP-LEVEL (spec 3.1/3.5), keyed by (channel, button, gesture).
    c.binding_count = 2;
    std::strncpy(c.bindings[0].id, "b1", sizeof(c.bindings[0].id) - 1);
    c.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[0].button, "VOL_UP", sizeof(c.bindings[0].button) - 1);
    c.bindings[0].gesture = Gesture::kSingle;
    c.bindings[0].enabled = true;
    c.bindings[0].action_count = 1;
    c.bindings[0].actions[0].kind = ActionKind::kHwKey;
    c.bindings[0].actions[0].key_resistance_mohm = 24000;
    // The second binding is the product's core case (spec 3.5/3.6): one button
    // whose SINGLE drives the head unit while its DOUBLE tells the app, with a
    // data payload. A single-action, id-keyed Binding could not express this,
    // which is why the round trip below asserts BOTH actions survive.
    std::strncpy(c.bindings[1].id, "b2", sizeof(c.bindings[1].id) - 1);
    c.bindings[1].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[1].button, "VOL_UP", sizeof(c.bindings[1].button) - 1);
    c.bindings[1].gesture = Gesture::kDouble;
    c.bindings[1].enabled = true;
    c.bindings[1].action_count = 2;
    c.bindings[1].actions[0].kind = ActionKind::kHwKeyRelease;
    c.bindings[1].actions[1].kind = ActionKind::kAppIntent;
    std::strncpy(c.bindings[1].actions[1].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                 sizeof(c.bindings[1].actions[1].target) - 1);
    std::strncpy(c.bindings[1].actions[1].payload, "geo:40.7608,-111.8910",
                 sizeof(c.bindings[1].actions[1].payload) - 1);
    return c;
}
}  // namespace

TEST(ConfigCodec, JsonRoundTripsEveryFieldThatWasSet) {
    const Config in = MakeConfig();
    char buf[kScratch] = {};
    const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(buf, n, &out));
    // Every field MakeConfig sets is checked here. A field the encoder silently
    // drops would otherwise pass: JsonRoundTripIsStableUnderReencode only proves
    // the encoder is deterministic, not complete -- a dropped field is stable.
    EXPECT_EQ(out.schema_version, in.schema_version);
    EXPECT_STREQ(out.device_id, in.device_id);
    EXPECT_EQ(out.settings.timings.debounce_ms, in.settings.timings.debounce_ms);
    EXPECT_EQ(out.settings.timings.double_press_off_ms, 500);
    EXPECT_EQ(out.settings.timings.long_press_ms, 750);
    EXPECT_EQ(out.settings.timings.send_duration_ms, in.settings.timings.send_duration_ms);
    EXPECT_EQ(out.settings.gain_policy, GainPolicy::kAuto);
    EXPECT_EQ(out.settings.buzzer_level, 2);
    EXPECT_EQ(out.settings.led_level, 2);
    EXPECT_TRUE(out.settings.temp_comp_enabled);
    EXPECT_EQ(out.settings.maintenance_timeout_ms, in.settings.maintenance_timeout_ms);

    EXPECT_EQ(out.channel_count, 1);
    EXPECT_TRUE(out.channels[0].enabled);
    EXPECT_STREQ(out.channels[0].name, "SWC1");
    EXPECT_EQ(out.channels[0].ladder.learned_idle_mv, 2835);
    EXPECT_EQ(out.channels[0].ladder.count, 1);
    EXPECT_EQ(out.channels[0].ladder.buttons[0].id, std::string("VOL_UP"));
    EXPECT_EQ(out.channels[0].ladder.buttons[0].mv_center, 1430);
    EXPECT_EQ(out.channels[0].ladder.buttons[0].mv_tolerance, 120);
    EXPECT_EQ(out.channels[0].output.gain_mode, GainMode::kAmplified);
    EXPECT_EQ(out.channels[0].output.idle_dac_code, 4095);

    // Bindings are top-level and carry an ordered action list.
    ASSERT_EQ(out.binding_count, 2);
    EXPECT_STREQ(out.bindings[0].id, "b1");
    EXPECT_EQ(out.bindings[0].channel, static_cast<uint8_t>(BindingChannel::kSwc1));
    EXPECT_STREQ(out.bindings[0].button, "VOL_UP");
    EXPECT_EQ(out.bindings[0].gesture, Gesture::kSingle);
    EXPECT_TRUE(out.bindings[0].enabled);
    ASSERT_EQ(out.bindings[0].action_count, 1);
    EXPECT_EQ(out.bindings[0].actions[0].kind, ActionKind::kHwKey);
    EXPECT_EQ(out.bindings[0].actions[0].key_resistance_mohm, 24000u);

    // The two-action binding is the product's core case; both must survive, in
    // order, with the payload intact.
    EXPECT_EQ(out.bindings[1].gesture, Gesture::kDouble);
    ASSERT_EQ(out.bindings[1].action_count, 2);
    EXPECT_EQ(out.bindings[1].actions[0].kind, ActionKind::kHwKeyRelease);
    EXPECT_EQ(out.bindings[1].actions[1].kind, ActionKind::kAppIntent);
    EXPECT_STREQ(out.bindings[1].actions[1].target, "com.oetsolutions.swc.ACTION_NAVIGATE");
    EXPECT_STREQ(out.bindings[1].actions[1].payload, "geo:40.7608,-111.8910");
}

TEST(ConfigCodec, JsonRoundTripIsStableUnderReencode) {
    const Config in = MakeConfig();
    char a[kScratch] = {};
    char b[kScratch] = {};
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
    char buf[kScratch] = {};
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
    uint8_t blob[kScratch] = {};
    const size_t n = ConfigEncodeBlob(in, blob, sizeof(blob));
    ASSERT_GT(n, 0u);

    Config out{};
    EXPECT_TRUE(ConfigDecodeBlob(blob, n, &out));
    EXPECT_FALSE(ConfigDecodeBlob(blob, n - 1, &out)) << "a short read must not decode";
    EXPECT_FALSE(ConfigDecodeBlob(blob, 4, &out));
}

TEST(ConfigCodec, BlobDetectsASingleFlippedBitViaCrc) {
    const Config in = MakeConfig();
    uint8_t blob[kScratch] = {};
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
    // Above the ADC ceiling: spec 3.2 caps a pin reading at 2900 mV, so a
    // mv_center above it is not a value the hardware can produce.
    c.channels[0].ladder.buttons[0].mv_center = 3300;
    EXPECT_FALSE(ConfigValidate(c)) << "a pin voltage above the ADC ceiling is impossible";

    c = MakeConfig();
    // A binding naming a button that does not exist on its channel.
    std::strncpy(c.bindings[0].button, "NO_SUCH_BUTTON", sizeof(c.bindings[0].button) - 1);
    EXPECT_FALSE(ConfigValidate(c)) << "a binding to a non-existent button";

    c = MakeConfig();
    c.binding_count = static_cast<uint8_t>(kMaxBindings + 1);
    EXPECT_FALSE(ConfigValidate(c)) << "more bindings than the budget allows";

    c = MakeConfig();
    c.bindings[1].action_count = static_cast<uint8_t>(kMaxActionsPerBinding + 1);
    EXPECT_FALSE(ConfigValidate(c)) << "more actions per binding than the budget allows";

    c = MakeConfig();
    // An empty action list is LEGAL and means "swallow the gesture" (spec 3.5);
    // it must not be confused with enabled:false, which is also legal.
    c.bindings[1].action_count = 0;
    EXPECT_TRUE(ConfigValidate(c)) << "empty actions swallow the gesture; that is valid";
    c.bindings[1].enabled = false;
    EXPECT_TRUE(ConfigValidate(c)) << "disabled is a different valid state";

    c = MakeConfig();
    c.settings.timings.debounce_ms = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "zero debounce is not a configurable choice";

    c = MakeConfig();
    c.settings.timings.long_press_ms = 100;
    c.settings.timings.double_press_off_ms = 500;
    EXPECT_FALSE(ConfigValidate(c)) << "long press must exceed the double window";

    c = MakeConfig();
    c.channels[0].ladder.buttons[0].mv_tolerance = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "a zero-width window can never match";
}

TEST(ConfigCodec, ValidationRejectsButtonsTooCloseToTellApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    // 1418 and 1445 mV are 27 mV apart at a 120 mV half-width: every reading in
    // the overlap is equally close to both, so classification would be a coin
    // toss. (In permille of the 2835 idle: 500 and 510, each window 42 wide.)
    c.channels[0].ladder.buttons[0] = {"VOL_UP",   "Volume Up",   1418, 120, 3300, 235, 200, 98};
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1445, 120, 3300, 235, 200, 97};
    EXPECT_FALSE(ConfigValidate(c))
        << "centres closer together than the wider tolerance can never be told apart";
}

TEST(ConfigCodec, ValidationAcceptsButtonsExactlyTolerancePlusOneApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    // The check is on the DERIVED permille window, so the boundary is expressed
    // in millivolts that land on it: 1430 mV (504 permille) and 1551 mV
    // (547 permille) are 43 permille apart = max(42,42) + 1.
    c.channels[0].ladder.buttons[0] = {"VOL_UP",   "Volume Up",   1430, 120, 3300, 235, 200, 98};
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1551, 120, 3300, 235, 200, 97};
    EXPECT_TRUE(ConfigValidate(c));

    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1548, 120, 3300, 235, 200, 97};
    EXPECT_FALSE(ConfigValidate(c)) << "exactly tolerance apart still overlaps";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_config'`
Expected: FAIL — `Config/ConfigCodec.h` not found.

- [ ] **Step 3: Write `lib/Config/ConfigModel.h`**

```cpp
#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"
#include "Gesture/GestureStateMachine.h"
#include "Output/GainPolicy.h"

constexpr uint32_t kConfigSchemaVersion = 1;

constexpr int kMaxChannels          = 2;
constexpr int kMaxBindings          = 32;   // TOTAL, across all channels (spec 3.5)
constexpr int kMaxActionsPerBinding = 2;    // measured against the NVS budget, not chosen
constexpr int kMaxAuxButtons        = 3;
constexpr int kChannelNameLen       = 16;
constexpr int kDeviceIdLen          = 24;
constexpr int kBindingIdLen         = 16;   // slugs: "vol_up", "next"
constexpr int kActionTargetLen      = 40;   // holds com.oetsolutions.swc.ACTION_NAVIGATE (34)
constexpr int kDataPayloadLen       = 48;   // holds geo:40.7608,-111.8910?q=Home (28)

// These four widths are NOT free. They are a budget input, together with
// kMaxBindings and kMaxActionsPerBinding: the structural worst case -- every
// string field at its declared maximum -- is what ConfigMaxSerializedSize()
// must bound, and it has to fit two NVS slots. At 40/48/16/16 with 32 bindings
// x 2 actions the JSON is 22,407 B -> 11 chunks -> 46,496 B of the partition's
// 48,384 B (96 %). Widening any of them overflows: the 64/128/24 an earlier
// revision declared gives 30,021 B -> 15 chunks -> 63,392 B (131 %), which the
// device cannot store at all. If a field must grow, re-measure before widening
// -- and see spec 3.5's width table, which is the binding statement of this.

// An action is identified by its KIND and params. There is no action id (spec
// 3.6): an earlier revision invented ids 1-63 and a 3-value ActionKind, which
// could not express the 11 kinds and made the product's core case -- one button
// whose SINGLE sends a HW_KEY while its DOUBLE sends an APP_INTENT -- a shape
// the type could not hold.
// A binding's input, spec 3.5: `SWC1 | SWC2 | AUX1 | AUX2 | AUX3 | ANY`.
// `ANY` is a real value, not a placeholder -- it is how one gesture is bound
// once and honoured from either steering-wheel channel.
enum class BindingChannel : uint8_t {
    kSwc1, kSwc2, kAux1, kAux2, kAux3, kAny,
};

enum class ActionKind : uint8_t {
    kNone, kHwKey, kHwKeyRelease, kAppLaunch, kAppIntent,
    kKeycode, kMedia, kVolume, kSystem, kBuzzer, kAppRaw,
};

struct Action {
    ActionKind kind;
    char       target[kActionTargetLen];   // package / intent action / command / pattern
    char       payload[kDataPayloadLen];   // APP_INTENT's data; MV for kHwKey
    uint16_t   dac_code;                   // kHwKey when commanded by code
    uint32_t   key_resistance_mohm;        // kHwKey when commanded by resistance
};

struct Binding {
    char     id[kBindingIdLen];
    uint8_t  channel;                      // SWC1|SWC2|AUX1..3|ANY
    char     button[kBindingIdLen];        // LadderButton.id, or "NONE"
    Gesture  gesture;
    bool     enabled;
    uint8_t  action_count;                 // 0 is legal and MEANS "swallow the gesture"
    Action   actions[kMaxActionsPerBinding];   // ordered, executed best-effort
};

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
    // The DAC code the KEY line is held at when idle (spec 3.7: 4095, and spec
    // 6.7 explains why full scale is the SAFE state -- the output only sinks,
    // so a high command releases the line). This is what Boot() writes in its
    // "establish safe idle" step, and what every orchestrator test compares
    // against after a press.
    uint16_t idle_dac_code;
};

struct AuxButtonConfig {
    char    id[kBindingIdLen];
    uint8_t source;                   // spec 3.1: a direct digital/analog input
    int16_t mv_center;
    int16_t mv_tolerance;
};

struct ChannelConfig {
    bool          enabled;
    char          name[kChannelNameLen];
    LadderProfile ladder;
    OutputProfile output;
};

struct Config {
    uint32_t        schema_version;
    char            device_id[kDeviceIdLen];
    uint64_t        updated_at_ms;
    DeviceSettings  settings;
    ChannelConfig   channels[kMaxChannels];
    uint8_t         channel_count;
    AuxButtonConfig aux[kMaxAuxButtons];    // AUX1-AUX3 (spec 3.1)
    uint8_t         aux_count;
    Binding         bindings[kMaxBindings]; // TOP-LEVEL join table (spec 3.1/3.5)
    uint8_t         binding_count;
};
```

**Why `bindings` is top-level and not inside `ChannelConfig`.** Spec §3.1's entity
map puts `Binding` beside `AuxButton`, and §3.7's worked example indents
`"bindings"` as a sibling of `"channels"`. A binding's own `channel` field is what
ties it to an input, and that field must be able to say `ANY` (a gesture usable
from either channel) or name an AUX input — neither of which a per-channel array
can express. An earlier revision nested them, which silently made AUX and ANY
bindings unrepresentable and capped the device at half the bindings the spec
budgets for.

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

// NVS blob form: version header + CRC32 over the JSON payload (FR-23). The
// payload is JSON, the same bytes ConfigEncodeJson produces -- one codec, so the
// two forms cannot drift -- and the CRC is what makes a torn write detectable.
size_t ConfigEncodeBlob(const Config &c, uint8_t *out, size_t out_len);
bool   ConfigDecodeBlob(const uint8_t *in, size_t len, Config *out);

// Worst-case serialized size, asserted against the NVS budget (spec 10.5).
//
// constexpr, and that is load-bearing rather than stylistic: the tests size
// their encode buffers from it (`constexpr size_t kScratch =
// ConfigMaxSerializedSize();`), which is only legal if it is a constant
// expression. A plain function would force those buffers back to a hard-coded
// literal -- which is exactly how they went stale at 4096 B for a 22 KB model.
// It is a pure function of the constants in ConfigModel.h, so it can be one.
constexpr size_t ConfigMaxSerializedSize();

// How many NVS keys a blob of this size needs, at a fixed chunk size well under
// the 4000-byte single-value cap. A slot is written as `cfg_a_0..n` with the
// count and a per-slot CRC in the header chunk (spec 3.8), because a real config
// is 10-15 KB and CANNOT be one NVS value.
constexpr int ConfigChunkCountFor(size_t blob_len);

// The fixed chunk size the store writes. Must be < 4000 to leave entry
// overhead, and is a compile-time constant so the key count is bounded.
constexpr size_t kConfigChunkBytes = 2048;
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
//
// Compared in the DERIVED permille the classifier actually uses, not in raw
// millivolts. Two buttons 10 mV apart at a high rail are a much narrower window
// than 10 mV apart at a low one, so the raw-mv distance is not the quantity the
// ambiguity depends on.
bool CentresAreDistinguishable(const LadderProfile &p) {
    if (p.learned_idle_mv == 0) return false;   // no reference, nothing to derive
    for (uint8_t i = 0; i < p.count; ++i) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < p.count; ++j) {
            const int ci = LadderRatioPermille(p.buttons[i].mv_center, p.learned_idle_mv);
            const int cj = LadderRatioPermille(p.buttons[j].mv_center, p.learned_idle_mv);
            const int ti = LadderRatioPermille(p.buttons[i].mv_tolerance, p.learned_idle_mv);
            const int tj = LadderRatioPermille(p.buttons[j].mv_tolerance, p.learned_idle_mv);
            if (ci < 0 || cj < 0 || ti < 0 || tj < 0) return false;
            const int distance  = abs(ci - cj);
            const int tolerance = ti > tj ? ti : tj;
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
        // The idle reference must be a plausible ADC reading: above zero and
        // no higher than the 2900 mV ADC ceiling (spec 3.2).
        if (cc.ladder.learned_idle_mv <= 0 || cc.ladder.learned_idle_mv > 2900) return false;
        for (uint8_t i = 0; i < cc.ladder.count; ++i) {
            const LadderButton &b = cc.ladder.buttons[i];
            if (b.id[0] == '\0') return false;
            // A button at or above the idle reference is physically impossible:
            // a press pulls the input DOWN (spec 6.3), so every button sits
            // below the idle. Both bounds are checked against the 2900 mV ADC
            // ceiling (spec 3.2) rather than a 3300 mV rail -- no pin reading
            // can exceed the ceiling, so a value above it is not a measurement.
            if (b.mv_center == 0 || b.mv_center > 2900) return false;
            if (b.mv_tolerance == 0) return false;
            // And the DERIVED window must be a real one: a tolerance that
            // rounds to zero permille can never match anything.
            if (LadderRatioPermille(b.mv_tolerance, cc.ladder.learned_idle_mv) <= 0) return false;
        }
        if (!CentresAreDistinguishable(cc.ladder)) return false;
    }

    // Bindings are a top-level table (spec 3.1/3.5), so their checks are too.
    if (c.binding_count > kMaxBindings) return false;
    for (uint8_t i = 0; i < c.binding_count; ++i) {
        const Binding &b = c.bindings[i];
        if (b.action_count > kMaxActionsPerBinding) return false;
        if (b.channel >= static_cast<uint8_t>(BindingChannel::kAny) + 1) return false;
        // `button` is a LadderButton.id, "NONE", or -- for the AUX inputs -- an
        // AUX id. A binding that names neither is a binding to nothing, which
        // would silently never fire; refuse it instead.
        if (!BindingNamesARealInput(c, b)) return false;
        // An empty action list is legal (spec 3.5: it swallows the gesture), so
        // action_count == 0 is NOT an error. What is an error is an action that
        // is neither a known kind nor carries the field its kind requires.
        for (uint8_t a = 0; a < b.action_count; ++a) {
            if (!ActionIsWellFormed(b.actions[a])) return false;
        }
    }
    return true;
}
```

Both encoders are mechanical; the tests above are what pin the behavior. **The
blob's payload IS the JSON** — `ConfigEncodeBlob` prepends a header and CRCs the
JSON bytes; it does not pack the struct. Key requirements the implementation must
satisfy:

- `ConfigEncodeJson` writes `schema_version`, `device_id`, `updated_at_ms`,
  `settings` (including all four `GestureTimings` fields), an `aux` array (spec
  §3.1's AUX1–AUX3), and a `channels` array whose entries carry `name`, `enabled`,
  `ladder` (**`idle_mv`** — the struct field is `learned_idle_mv`, but the wire
  key is `idle_mv`, per spec §3.7's worked example — and a `buttons` array whose
  entries carry `id`, `mv_center`, `mv_tolerance`, `learned_at_rail_mv`,
  `temp_c_at_learn`, `sample_count`, `confidence`), and `output` (**`gain_mode`**
  and **`idle_dac_code`**, spec §3.7).
- **`bindings` is a TOP-LEVEL array, a sibling of `channels`** (spec §3.1/§3.7),
  and each entry carries `id`, `channel` (a string: `SWC1`/`SWC2`/`AUX1..3`/`ANY`),
  `button` (a `LadderButton.id` or `"NONE"`), `gesture` (a string), `enabled`, and
  `actions` — an **ordered array** whose entries carry `kind` (a string, one of
  spec §3.6's 11) plus that kind's own params. There is **no `action_id` and no
  `action_name`**; an earlier revision wrote both, which is a numeric id table
  the spec does not define.
- **An empty `actions` array round-trips as empty and stays distinct from
  `"enabled": false`** (spec §3.5). Both are valid, they mean different things,
  and the decoder must not collapse one into the other.
- `ConfigDecodeJson` returns `false` unless **every** required field is present
  and the decoded `Config` passes `ConfigValidate`. It must reject
  `schema_version != kConfigSchemaVersion` explicitly, so a future schema is
  refused rather than misparsed.
- `ConfigEncodeBlob` writes a `BlobHeader` followed by the JSON payload, with
  `payload_crc` computed over that payload.
- `ConfigDecodeBlob` checks magic, `schema_version`, `payload_len <= len -
  sizeof(BlobHeader)`, then recomputes the CRC. Any mismatch returns `false`
  **without writing to `out`**.
- `ConfigMaxSerializedSize` is **`constexpr`** and returns the worst case:
  `sizeof(BlobHeader)` plus the fully-populated JSON. "Fully populated" is the
  spec §3.5 cardinality — 2 channels, 16 ladder buttons each, 3 AUX buttons, and
  **32 top-level bindings each carrying 2 actions** — with every string field at
  its maximum length, so the bound does not depend on what a particular config
  happens to contain. An earlier revision computed this as "2 channels × 16
  buttons × 32 bindings", which is the *nested* model's 64-binding arithmetic and
  is both the wrong shape and the wrong number. The blob is JSON payload — do not
  pack the struct, or the two forms drift.

  Being `constexpr` is required, not decorative: the test's `kScratch` and
  Task 15's staging buffer are both sized from it at compile time. Implement it
  as a sum of `sizeof` and the `kMax*`/`k*Len` constants in `ConfigModel.h`, so
  it stays a constant expression — do not call `snprintf`, `strlen` on runtime
  data, or anything else that only exists at run time.

  **The measured values, so this is a decision and not a hope:** `Config` is
  **~15.9 KB packed**; the fully-populated JSON is **22,407 B**; that is **11
  chunks** at 2048 B. Two slots plus `cfg_seq` cost 2 × 11 × 2,112 + 32 =
  **46,496 B of the partition's 48,384 B (96 %)**. It fits — with almost no
  margin. A third action per binding does not: it needs 14 chunks and 59,168 B,
  which is why `kMaxActionsPerBinding` is 2 and why Step 7 asserts the bound
  rather than trusting it. These figures live in spec §3.5/§3.8 too; if you
  change the cardinality **or any string width**, change all three and re-measure.
- `ConfigChunkCountFor(blob_len)` is also **`constexpr`** — it returns
  `ceil(blob_len / kConfigChunkBytes)`, a pure function of its argument and a
  constant. Task 9 uses it to write `cfg_a_0…n`; being constexpr is what lets
  Step 7's test compute the chunk count in a constant expression rather than
  asserting against a hard-coded number that could go stale.

- [ ] **Step 6: Run the tests**

Run: `cd code && pio test -e native -f '*test_config'`
Expected: PASS — 10 tests green.

- [ ] **Step 7: Assert the config fits the NVS budget**

Add to the test file:

```cpp
TEST(ConfigCodec, SerializedSizeFitsTheNvsPartitionBudget) {
    // Spec 10.5, two limits -- and the second is the one that is easy to miss.
    //
    // (a) A single NVS *value* is capped at ENTRY_SIZE * (ENTRY_COUNT - 1)
    //     = 32 * 125 = 4000 bytes. That is a hard IDF limit (nvs_page.cpp
    //     returns ESP_ERR_NVS_VALUE_TOO_LONG above it), not a budget to tune.
    //     Even a REALISTIC config (~3.9 KB) sits on that line, and a moderate
    //     one (~7.9 KB) blows past it -- so a single-value slot is not a
    //     hypothetical failure, it is the common case.
    EXPECT_GT(ConfigMaxSerializedSize(), 4000u)
        << "the worst case must force chunking, or Task 9's chunked path is dead code";

    // (b) BOTH slots, plus the sequence key, must fit the partition's usable
    //     entry space: 12 pages x 126 entries x 32 B = 48,384 B.
    //
    //     Each chunk key costs 2112 B, NOT 2048. NVS writes a 32-byte metadata
    //     entry plus the payload (nvs_page.cpp:185), and then a separate
    //     32-byte BLOB_IDX entry for the key (nvs_storage.cpp:353). Counting
    //     only the payload understates the budget.
    //
    //     NOTE this is deliberately NOT the weaker "a slot fits in 24 KB"
    //     assertion an earlier revision had: two 24 KB slots need 49,152 B of
    //     entry space and DO NOT FIT a 48,384-byte partition. Asserting that
    //     bound would pass on a config the device cannot actually store twice.
    constexpr size_t kUsableEntryBytes    = 32u * 126u * 12u;              // 48,384
    constexpr size_t kEntryBytesPerChunk  = 32u + kConfigChunkBytes + 32u; // 2,112
    constexpr size_t kSequenceKeyBytes    = 32u;
    const size_t chunks =
        static_cast<size_t>(ConfigChunkCountFor(ConfigMaxSerializedSize()));
    EXPECT_LE(2u * chunks * kEntryBytesPerChunk + kSequenceKeyBytes, kUsableEntryBytes)
        << "two slots + cfg_seq must fit the partition, with entry overhead counted";
}
```

**Measured worst case, for the record** (computed by building the structs and
serializing them, not estimated): at spec §3.5's cardinality and widths — 32
top-level bindings × 2 actions, `target[40] payload[48]`, ids `[16]`, 2 channels
× 16 buttons, 3 AUX — the JSON is **22,407 B → 11 chunks**, and two slots plus
`cfg_seq` cost **46,496 B of the 48,384 B of usable NVS entry space — 96 %**.
`Config` itself is ~15.9 KB packed; the blob is JSON payload, so the JSON figure
is the one that binds. A realistic config (9 buttons, 9 bindings, short payloads)
is ~3.9 KB → 2 chunks → 17 %. **That spread is the reason the bound is asserted
rather than assumed.**

**This paragraph used to say 12,120 B packed / ~17.8 KB / 9 chunks / 79 %.** Those
figures came from the *pre-rewrite* model (a per-channel binding array, so
`2 × 32 = 64` bindings and a `Binding` of 172 B) and were wrong twice over: wrong
shape, and wrong in the *safe* direction, which is the direction that hides a
problem. Re-measuring produced the 22,407 B / 96 % above, which is close enough
to the ceiling that the widths and the action cap are both load-bearing. When a
figure here disagrees with spec §3.5, the spec is the authority.

The per-chunk cost is **2112 B**, not 2080: a 32-byte metadata entry plus the
2048 payload bytes (`nvs_page.cpp:185`), plus a 32-byte `BLOB_IDX` entry that
NVS writes once per key (`nvs_storage.cpp:353`). Counting only the first two
understates a two-slot budget by 576 B — small here, but it is the difference
between "fits" and "fits with the margin you think it has".

**And a caution about the assertion itself.** `ConfigMaxSerializedSize()` is the
number both this test and Task 15's staging buffer are sized from, so if it
under-reports the true worst case the test passes while the device cannot store
what it claims to support — *a gate that cannot fail reads as a gate that
passed*. That is exactly what the 21,411 B figure did before it was re-measured.
The `static_assert(kScratch > 0u, …)` in the test file's `kScratch` is the cheap
half of the guard; this test is the other half. Neither can detect an
under-report on its own, which is why both figures are recorded here.

Run: `cd code && pio test -e native -f '*test_config'`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add code/lib/Config/ConfigModel.h code/lib/Config/ConfigCodec.h \
        code/lib/Config/ConfigCodec.cpp code/test_native/test_config \
        code/platformio.ini
git commit -m "Add the config model with validated JSON and CRC'd blob codecs

The model is the entity layout shared by firmware, app and web page. Validation
rejects what would make classification ambiguous or the device unable to serve
input -- nested windows, a long-press threshold inside the double-press window,
a ratio above the rail. A newer schema version is refused, never misparsed.

platformio.ini carries the new cJSON host dependency the native env needs to
link the same parser the device uses."
```

`code/platformio.ini` is in that list deliberately: Step 5 adds
`DaveGamble/cJSON@^1.7.18` to the `native` env's `lib_deps`, and without it the
host build fails to link. A dependency change that is not committed is a broken
build for everyone else.

---

### Task 9: `ConfigStore` — atomic A/B persistence

FR-23, FR-24. Power can be cut mid-write in a car; the config must never come
back torn. Two slots with a monotonic sequence number, written alternately, so
the older copy is always intact while the newer one is being written.

**Files:**
- Create: `code/lib/Config/ConfigStore.h`
- Create: `code/lib/Config/ConfigStore.cpp`
- Create: `code/test_native/test_config/ConfigStoreTest.cpp`
- Modify: `code/test_native/MockHAL.h`, `code/test_native/MockHAL.cpp` — add
  `TruncateNvsWriteTo(key, n)` **only**. No `test_main.cpp`: Task 8 owns the one
  for `test_config/`.

**Interfaces:**
- Consumes: `ConfigCodec` (Task 8), `IHAL::nvs_get`/`nvs_set` (Task 2), `MockHal` (Task 2)
- Produces:
  - `enum class ConfigLoadResult { kLoaded, kNoConfig, kRecoveredFromBackup, kFellBackToDefaults }`
  - `class ConfigStore` with `ConfigLoadResult Load(Config *out)`, `bool Save(const Config &c)`, `uint32_t LoadedSequence() const`

- [ ] **Step 1: Write the failing test**

```cpp
#include "Config/ConfigStore.h"
#include "MockHAL.h"
#include <gtest/gtest.h>

namespace {
Config MakeConfig() { /* as in Task 8's MakeConfig(), returning a valid config */ }

// Slot keys are internal; the test drives failure through MockHal's injected
// write fault, which is the realistic failure (power cut mid-write). Only the
// first chunk of each slot is named here, because only chunk 0 is corrupted
// directly (it carries the header and the slot CRC, so corrupting it is what
// makes the whole slot unreadable) -- the sequence key is reached through the
// fault injector instead, so naming it would be an unused constant.
constexpr const char *kNvsA = "cfg_a_0";
constexpr const char *kNvsB = "cfg_b_0";
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

    // Truncate the PAYLOAD write, not the sequence write. The store writes the
    // slot first and cfg_seq last, so the realistic torn case -- power lost
    // while the payload is landing -- is the slot write. NvsSet here both lands
    // a short value AND reports failure, which is exactly what a brown-out
    // mid-write looks like: garbage on the medium, and a driver that says so.
    hal.TruncateNextNvsWriteAt(16);
    EXPECT_FALSE(store.Save(c)) << "a failed write must report failure";

    Config out{};
    // The sequence was never advanced, so slot A is still the newest slot and
    // is still intact: this is kLoaded, not kRecoveredFromBackup. Reaching the
    // backup path needs a TORN SEQUENCE write, which the next test covers --
    // the distinction matters because kLoaded means "nothing was wrong".
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001")
        << "a torn payload write must leave the previous good config, not a partial one";
}

TEST(ConfigStore, ATornSequenceWriteLeavesTheBackupAuthoritative) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                        // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));                        // slot B, seq 2
    std::strncpy(c.device_id, "SWC-0003", sizeof(c.device_id) - 1);

    // The other half of the tear: the payload chunks land, the sequence write is
    // the one that is lost. Save has already written slot A (the non-newest one)
    // with SWC-0003, so seq stays 2 -> slot B (SWC-0002) is still the newest
    // and still valid. This is the case the write-order protocol exists for.
    //
    // It must target the key, not "the next write": a slot is several chunks,
    // so the next write is a payload chunk and the sequence write is never
    // reached. That is why Task 9 adds TruncateNvsWriteTo.
    hal.TruncateNvsWriteTo("cfg_seq", 2);
    EXPECT_FALSE(store.Save(c));

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0002")
        << "an unlanded sequence write must not promote the payload to newest";
}

TEST(ConfigStore, BothSlotsCorruptFallsBackToDefaultsNotToHalfAConfig) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                        // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));                        // slot B, seq 2
    // Corrupt BOTH slots -- that is what the test name says, and with only one
    // corrupted the store would simply use the other and this would be a
    // duplicate of ATornWriteIsDetectedAndTheBackupIsUsed.
    hal.CorruptNvsValue(kNvsA, 3);
    hal.CorruptNvsValue(kNvsB, 3);
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

**`MockHal` needs exactly one addition here: `TruncateNvsWriteTo(key, n)`.** An
earlier revision of this section said to add `InterfaceRef`, `CorruptNvsValue`
and `ClearNvs` "since the store is the first consumer" — but Task 2 already
defines all three and is committed, so adding them again is a duplicate
definition that will not compile. Those three are only *used* here. The fourth is
different: the existing `TruncateNextNvsWriteAt` targets "the next write", which
with chunked slots is always a payload chunk, so the sequence-lost tear cannot be
expressed at all. That one is genuine new capability.

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_config'`
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
    kFellBackToDefaults,     // no slot yielded a usable config (FR-24)
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
    explicit ConfigStore(IHAL *hal);

    ConfigLoadResult Load(Config *out);

    // Returns false without touching the previously stored config on any error.
    bool Save(const Config &c);

    uint32_t LoadedSequence() const { return loaded_seq_; }

private:
    IHAL *hal_;
    uint32_t loaded_seq_ = 0;
};
```

**No `MaxStoredBytes()`.** An earlier revision declared it "asserted against the
NVS budget (spec 10.5)". §10.5's config-fits-NVS gate is `ConfigCodec`'s
round-trip size assertion against the 48 KB partition — Task 8 owns it. The
store writes at most two slots, so a size accessor here has no consumer and no
gate to serve.

**No `next_slot_` member.** The newest slot is derived from `cfg_seq`'s parity,
so a cached index is a second home for the same fact — exactly the drift this
plan has been cleaning up. `Load` derives it, and `Save` writes the other one.

**This is the one task that DOES add to `MockHal`** — `TruncateNvsWriteTo(key, n)`,
declared in the Shared contract's harness block. `TruncateNextNvsWriteAt` is
"the next write", and with chunked slots the next write is always a payload
chunk, so it cannot express the sequence-lost tear at all. Add the method to
`test_native/MockHAL.h` and `MockHAL.cpp` and **commit those two files with this
task** — a fault injector that only exists in a working tree is not usable by the
next task.

- [ ] **Step 4: Implement `ConfigStore.cpp`**

The logic the tests pin down:

- **NVS keys:** `cfg_seq` (a `uint32_t` sequence number), and per slot
  `cfg_<slot>_0…n` where `<slot>` is `a` or `b`. Chunk 0 carries the blob header
  (magic, schema, total length, chunk count, slot CRC); the rest carry payload.
  Chunk size is `kConfigChunkBytes` (2048) and the count comes from
  `ConfigChunkCountFor()` (Task 8) — both fixed, so the key count is bounded.
- `Load`: read `cfg_seq`. If it is absent, return `kNoConfig` — *not* defaults,
  because "never configured" is what selects pass-through mode (FR-25).
  The newest slot is `cfg_seq` **parity**: the slot a save writes when the
  sequence it is about to write is odd is `cfg_a`. Read the newest slot's chunks
  and validate the slot CRC; if that fails, try the other slot. If the newer
  fails and the older succeeds, return `kRecoveredFromBackup`. If neither yields
  a config, return `kFellBackToDefaults`.
- `Save`: encode to a blob; write **all of the non-newest slot's chunks**; then
  write `cfg_seq + 1`. The sequence is written last, and that ordering is the
  whole protocol. It splits the tear in two, and the tests pin both halves:
  - **Payload torn** (`TruncateNextNvsWriteAt(16)`): a chunk write fails, so the
    sequence is never written. The old sequence still points at the old slot,
    which is untouched and valid → `kLoaded` with the *old* config. The torn
    bytes are never read, because they are not the newest slot.
  - **Sequence torn** (`TruncateNvsWriteTo("cfg_seq", 2)`): every payload chunk
    landed in the non-newest slot, but the sequence write failed. The sequence
    still points at the previous slot, which is still intact → `kLoaded` with
    the *previous* config. The landed payload is ignored, not promoted.
  In both cases a failed `Save` returns `false` and the sequence does not
  advance. `kRecoveredFromBackup` is reserved for the case where the sequence
  *did* advance but the newest slot's CRC then failed — a slot that was written
  whole once and rotted, not a write that was interrupted.
- A **partial chunk set is a slot failure, not a partial config.** If chunk `i`
  is missing or short, the slot is rejected whole: the header's chunk count and
  total length are checked before the CRC, so a truncated set can never decode.
- `Save` returns `false` if any `nvs_set` fails, and must not have advanced
  the sequence in that case.
- `LoadedSequence` returns the sequence of the config that was last loaded or
  saved, which is what the tests assert on.

- [ ] **Step 5: Run the tests**

Run: `cd code && pio test -e native -f '*test_config'`
Expected: PASS — 8 store tests green, including both tear cases.

- [ ] **Step 6: Commit**

```bash
git add code/lib/Config/ConfigStore.h code/lib/Config/ConfigStore.cpp \
        code/test_native/test_config/ConfigStoreTest.cpp \
        code/test_native/MockHAL.h code/test_native/MockHAL.cpp
git commit -m "Add atomic A/B config persistence

Two alternating slots with the sequence number written last, so a power cut
mid-write leaves the previous config loadable and the torn one is never treated
as newest. A slot is written as bounded chunks, because a real config is larger
than one NVS value can hold. Absent config is a distinct result from corrupt,
because only the former selects pass-through mode.

MockHAL gains TruncateNvsWriteTo: the existing fault injector targets the next
write, which with chunked slots is always a payload chunk, so the sequence-lost
tear was not expressible."
```

`MockHAL.h`/`MockHAL.cpp` are here for the one genuine reason: this task adds
`TruncateNvsWriteTo`. They are otherwise Task 2's files.

---

### Task 10: NDJSON framing

FR-1's link discipline, and the contract between firmware and app. Every frame
is one line of JSON with a `{v, seq, type}` envelope.

**Files:**
- Create: `code/lib/Link/Ndjson.h`
- Create: `code/lib/Link/Ndjson.cpp`
- Create: `code/test_native/test_link/NdjsonTest.cpp`
- Create: `code/test_native/test_link/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

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

Run: `cd code && pio test -e native -f '*test_link'`
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

Run: `cd code && pio test -e native -f '*test_link'`
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
- Create: `code/test_native/test_bindings/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

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
    ch.ladder.learned_idle_mv = 2835;
    ch.ladder.buttons[0] = {"VOL_UP", 504, 42, 0};
    ch.ladder.buttons[1] = {"VOL_DOWN", 630, 42, 0};
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

Run: `cd code && pio test -e native -f '*test_bindings'`
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

Run: `cd code && pio test -e native -f '*test_bindings'`
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
- Create: `code/test_native/test_feedback/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

**Interfaces:**
- Consumes: `IHAL` (Task 2), `DeviceSettings` (Task 8)
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

// Counts on-transitions of the buzzer line over a pattern's duration. The
// buzzer is the semantic member buzzer_on (spec 10.2), not a GpioPin.
namespace {
int CountBeeps(MockHal &hal, BuzzerGrammar &b, uint32_t total_ms) {
    int on = 0;
    bool prev = false;
    for (uint32_t t = 0; t < total_ms; t += 5) {
        b.Update(hal.NowMs());
        const bool now = hal.BuzzerIsOn();
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
    EXPECT_FALSE(hal.BuzzerIsOn()) << "a stuck buzzer is a stuck-on hardware fault";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_feedback'`
Expected: FAIL — `Feedback/BuzzerGrammar.h` not found.

- [ ] **Step 3: Implement `BuzzerGrammar`**

A pattern is a small array of `{on_ms, off_ms}` steps. `Update` is called from
the main loop and drives `buzzer_on(...)` from the injected clock — **it never
blocks**, which is FR-21. `Play` while busy discards the current pattern and
starts the new one. At the end of every pattern the line is forced low.

Patterns (from spec §7.1): `kKeyAccepted` = one 40 ms beep; `kKeyUnknown` = two
60 ms beeps 60 ms apart; `kProgramEnter` = 3×80 ms + 160 ms; `kProgramStep` =
n × 80 ms; `kProgramExit` = 2×120 ms; `kLearnPrompt` = 2×60 ms; `kLearnOk` =
3×60 ms; `kLearnReject` = one 400 ms; `kBootOk` = 1×120 ms; `kBootDegraded` =
2×120 ms; `kBootError` = 3×200 ms; `kFault` = 5×80 ms; `kOtaStart` = 2×40 ms;
`kOtaDone` = 2×200 ms.

- [ ] **Step 4: Run the buzzer tests**

Run: `cd code && pio test -e native -f '*test_feedback'`
Expected: PASS — 7 buzzer tests green.

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

Run: `cd code && pio test -e native -f '*test_feedback'`
Expected: PASS — 7 + 5 tests green.

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
It is still host-testable, because it holds only `IHAL*` and the modules above.

**Files:**
- Create: `code/lib/System/SystemOrchestrator.h`
- Create: `code/lib/System/SystemOrchestrator.cpp`
- Create: `code/test_native/test_system/SystemOrchestratorTest.cpp`
- Create: `code/test_native/test_system/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

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
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2500);   // reads back as idle
    o.Boot();
    EXPECT_TRUE(o.SafeIdleEstablished());
    // The DAC must have been written during Boot, before any USB work.
    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), 0);
}

TEST(SystemOrchestrator, BootDrivesTheAdjustChannelIntoTheOneKiloOhmPulldown) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    o.Boot();
    // Gain 1.82 requires V_ADJ at 0V, which is the 1k pulldown power-down mode.
    EXPECT_EQ(hal.LastDacPowerMode(DAC_CH_ADJ1), DAC_POWER_GND_1K);
}

TEST(SystemOrchestrator, APressProducesTheBoundOutputLevelAndThenReleases) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // VOL_UP at its learned 1430 mV. The ladder pulls DOWN from an idle of
    // 2835 mV, so a press is a LOWER reading -- never above idle.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "a press must change the output";

    // Release and let the send duration plus the gesture window elapse.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    for (uint64_t t = 0; t < 1200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code) << "must return to idle";
}

TEST(SystemOrchestrator, ServesPressesWithNoUsbAndNoApp) {
    // FR-42: this is the normal in-car case. Nothing in the orchestrator may
    // depend on a link being present.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);
}

TEST(SystemOrchestrator, ARailSagDuringAPressReleasesTheKey) {
    // FR-39: never leave a phantom key driven. FR-30: a 3V3 sag to <=20% of the
    // learned idle is a RAIL FAULT, not an idle reading and not a button.
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);  // press
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    ASSERT_NE(hal.LastDacCode(DAC_CH_KEY1), idle_code);

    // The +3V3 rail sags: 500 mV is 18% of the learned 2835 mV idle.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 500);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 250);
    for (uint64_t t = 0; t < 200; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), idle_code)
        << "a rail fault during a press must release the key, not hold it";
}

TEST(SystemOrchestrator, AnUnlearnedLevelNeverChangesTheOutput) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    o.Boot();
    const int idle_code = hal.LastDacCode(DAC_CH_KEY1);

    // 2400 mV: between the highest button (next, 2145 +/- 110) and idle (2835),
    // so it matches no learned button and is far above the rail-fault floor.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);
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
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, 2490);
    o.Boot();

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);   // press only channel 1
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);   // channel 2 idle
    for (uint64_t t = 0; t < 1000; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_GT(hal.DacWriteCount(DAC_CH_KEY1), 0);
    // Channel 2 must still have been driven to its idle level at boot only.
    EXPECT_EQ(hal.DacWriteCount(DAC_CH_KEY2), 1);
}

TEST(SystemOrchestrator, TickIsCheapEnoughToRunAtThePollCadence) {
    MockHal hal;
    auto o = MakeOrch(hal);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2500);
    o.Boot();
    // No assertion on wall time (that is meaningless on the host); the point is
    // that Tick does no I/O beyond the HAL calls already counted, and allocates
    // nothing. Repeated ticking must not grow any counter without cause.
    const int before = hal.BuzzerOnCount();
    for (uint64_t t = 0; t < 100; t += 10) { o.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_EQ(hal.BuzzerOnCount(), before) << "idle ticks must be silent";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_system'`
Expected: FAIL — `System/SystemOrchestrator.h` not found.

- [ ] **Step 3: Implement**

`Boot()` in this exact order (mirroring spec §6.1, and the ordering is the
requirement, not an implementation detail):

1. Load the config via `ConfigStore`. `kNoConfig` → pass-through mode (FR-25).
2. **Establish safe idle**: select gain via `GainPolicySelect`, drive
   `DAC_CH_ADJ1`/`DAC_CH_ADJ2` into `kGnd1k` in amplified mode, and write
   `DAC_CH_KEYn` to its idle code. Set `safe_idle_established_ = true`. **This
   happens before steps 3+** — FR-13, and `SafeIdleEstablished` is what the
   tests assert.
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
`Config` and `GestureTimingsDefault()`, so these tests are readable. The single
channel's ladder is **`idle_mv` 2835** with **`VOL_UP` at `mv_center` 1430,
`mv_tolerance` 120**, and a single-press binding to `VOL_UP`. These are the
spec §3.7 defaults; **they are not arbitrary** — a press pulls the input *down*
from idle (§6.3), so every button's `mv_center` is *below* `idle_mv`.

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f '*test_system'`
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
- Consumes: `IHAL` (Task 2)
- Produces: `IHAL *EspHalInit(void)`; `PinMap.h` constants

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
    IHAL *hal = EspHalInit();
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
    IHAL *hal = EspHalInit();
    hal->dac_power_mode(hal->ctx, DAC_CH_ADJ1, DAC_POWER_GND_1K);
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    hal->dac_set_code(hal->ctx, DAC_CH_KEY1, 4095);
    /* Read back through the sense divider: the KEY line is V/2 at the ADC. */
    vTaskDelay(pdMS_TO_TICKS(20));
    const int sense = hal->adc_read_mv(hal->ctx, ADC_CH_KEY_SENSE1);
    TEST_ASSERT_GREATER_THAN_INT(1000, sense);   /* the high code moved the line */
}

TEST_CASE("esp_hal_clock_advances", "[hw]") {
    IHAL *hal = EspHalInit();
    const uint64_t t0 = hal->now_ms(hal->ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(t0 + 40, hal->now_ms(hal->ctx));
}

TEST_CASE("esp_hal_nvs_round_trips", "[hw]") {
    IHAL *hal = EspHalInit();
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
  `ADC_ATTEN_DB_12` (the 2.9 V ceiling). **Call
  `adc_cali_create_scheme_curve_fitting()` and hand its result to
  `AdcCalibrationSelect()`** (Task 4) — do not call
  `adc_cali_raw_to_voltage()` from a bare handle. On `ESP_ERR_NOT_SUPPORTED`
  (blank eFuses, spec §3.2) the selected curve is
  `CalibrationSource::kLinearFallback`, and the HAL must **report** that rather
  than proceed silently. Use the mechanisms the spec already defines — a `log`
  frame (§4.3) at init, and a `BOOT_DEGRADED` boot (§7.2), the same class of
  condition as a config fallback. Do not add a field to the status frame for
  this; §4.3's payload is fixed. `adc_read_mv` converts the raw sample with
  `AdcRawToMilliVolts(cal, raw)` and returns `-1` on a driver error — never a
  fabricated zero, because zero is a legal reading.
  *This is the only consumer of Task 4.* Without this wiring `CalibrationCurve`
  is dead code and the spec's mandatory-fallback requirement is unimplemented.
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
- **`buzzer_on` and `dac_ldac` are implemented here, on `SWC_PIN_BUZZ` and
  `SWC_PIN_DAC_LDAC_B`.** They are not `GpioPin`s (spec §10.2): the buzzer is a
  rhythm grammar and `~LDAC` is DAC sequencing, so each gets a named function
  that says what it does rather than a raw pin write at the call site. Both still
  use `gpio_set_level` underneath — the pin map constants stay, the enum entries
  do not.

- [ ] **Step 4: Wire it into `main.c`**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "HAL/EspHal.h"
#include "System/SystemOrchestrator.h"

void app_main(void)
{
    IHAL *hal = EspHalInit();
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
Expected: app ≤ **1920 KB**. Record the new number next to Task 1's baseline.

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

Run: `cd code && pio test -e native -f '*test_link'`
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

Run: `cd code && pio test -e native -f '*test_link'`
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
- Create: `code/test_native/test_learning/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

**Interfaces:**
- Consumes: `LadderProfile` (Task 3), `LadderDecode` (Task 3), `BuzzerGrammar` (Task 12), `Config` (Task 8)
- Produces:
  - `enum class LearnReject { kNone, kTooNoisy, kTooCloseToExisting, kAtIdle, kOutOfRange, kTooFewSamples }`
  - `const char *LearnRejectReason(LearnReject r)` — the wire string
  - `class LearnSession` with `void Start(int channel, const LadderProfile &existing)`, `void AddSample(int level_mv, int idle_mv, uint64_t now_ms)`, `LearnReject Commit(LadderButton *out)`, `int SampleCount() const`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "Learning/LearnSession.h"
#include <gtest/gtest.h>

namespace {
LadderProfile ExistingWith(const char *id, int ratio, int tol) {
    LadderProfile p{};
    p.learned_idle_mv = 2835;
    p.count = 1;
    std::strncpy(p.buttons[0].id, id, sizeof(p.buttons[0].id) - 1);
    p.buttons[0].ratio_permille = ratio;
    p.buttons[0].tolerance_permille = tol;
    return p;
}
void Feed(LearnSession &s, int mv, int idle, int n, uint64_t &t) {
    for (int i = 0; i < n; ++i) { s.AddSample(mv, idle, t); t += 10; }
}
}  // namespace

TEST(LearnSession, ASteadyLevelCommitsAndRecordsTheIdleItWasLearnedAt) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 30, t);
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    ASSERT_EQ(out.ratio_permille, 504);
    // FR-30: the idle reference is recorded so runtime classification can
    // detect a rail fault and the app can display absolute millivolts.
    EXPECT_EQ(s.LearnedIdleMv(), 2835);
}

TEST(LearnSession, TooFewSamplesIsRejectedNotAcceptedFromOneReading) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 1430, 2835, 2, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooFewSamples);
}

TEST(LearnSession, ANoisyLevelIsRejectedWithTheNoiseReason) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // A 550 mV swing at a 2835 mV idle is ~195 permille of wobble: far wider
    // than the classification tolerance.
    for (int i = 0; i < 30; ++i) {
        s.AddSample((i % 2) ? 1430 : 1980, 2835, t);
        t += 10;
    }
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooNoisy);
}

TEST(LearnSession, ALevelAtIdleIsRejectedBecauseTheButtonWasNotPressed) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    Feed(s, 2835, 2835, 30, t);
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kAtIdle);
}

TEST(LearnSession, ALevelWithinAnExistingButtonsToleranceIsRejectedAsAmbiguous) {
    LearnSession s;
    s.Start(0, ExistingWith("VOL_UP", 504, 42));
    uint64_t t = 1000;
    Feed(s, 1450, 2835, 30, t);   // 511 permille, inside VOL_UP's window
    LadderButton out{};
    EXPECT_EQ(s.Commit(&out), LearnReject::kTooCloseToExisting);
}

TEST(LearnSession, ALevelAboveTheAdcCeilingIsRejectedAsOutOfRange) {
    LearnSession s;
    s.Start(0, LadderProfile{});
    uint64_t t = 1000;
    // 3000 mV exceeds the 2900 mV calibrated ADC ceiling (spec 3.2), so this
    // cannot be a real reading from this hardware -- it is a wiring or
    // calibration fault. NOTE: this is the INPUT side. The 2490 mV figure
    // belongs to the output sense divider (spec 2.3) and is a different net.
    Feed(s, 3000, 2835, 30, t);
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
        s.AddSample((i % 2) ? 1445 : 1415, 2835, t);
        t += 10;
    }
    LadderButton out{};
    ASSERT_EQ(s.Commit(&out), LearnReject::kNone);
    EXPECT_GE(out.tolerance_permille, 8) << "tolerance must cover the observed spread";
    EXPECT_LE(out.tolerance_permille, 120) << "and must not swallow neighbouring buttons";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd code && pio test -e native -f '*test_learning'`
Expected: FAIL — `Learning/LearnSession.h` not found.

- [ ] **Step 3: Implement**

`LearnSession` accumulates samples, and `Commit` runs the checks in this order,
returning the **first** failure so the user gets the most actionable reason:

1. `kTooFewSamples` — fewer than 10 samples, or a span under 100 ms.
2. `kOutOfRange` — any sample above the **2900 mV** calibrated ADC ceiling
   (spec §3.2) or below 0. This is the *input* ladder's ceiling; the 2490 mV
   figure is the *output* sense divider (spec §2.3) and is a different net.
3. `kAtIdle` — the mean ratio is within the idle margin, so the button was not
   pressed.
4. `kTooNoisy` — the spread exceeds `noise_limit_permille` (default 60).
5. `kTooCloseToExisting` — the mean is within an existing button's tolerance.

On success, `out.tolerance_permille` is the midpoint of the gap to the nearest
neighbouring button capped by a configurable maximum (spec §3.4) — **not**
`max(spread * 2, 8)`. `out.ratio_permille` is the rounded mean ratio, and
`LearnedIdleMv()` records the idle reference (FR-30).

- [ ] **Step 4: Run the tests**

Run: `cd code && pio test -e native -f '*test_learning'`
Expected: PASS — 8 tests green.

- [ ] **Step 5: Add the headless AUX1-driven wizard (FR-31)**

Create `code/lib/Learning/LearnWizard.cpp` with the state machine from spec §7.4,
taking `IHAL*`, `BuzzerGrammar*`, `LedGrammar*`, and `AUX1` as the select button.
The AUX1 press counting uses the **same** `PressClassifier` (Task 6) with a
ladder profile of one button at the AUX threshold, so there is one debounce
implementation, not two.

Test it in `code/test_native/test_learning/LearnWizardTest.cpp`: drive `MockHal`'s
AUX1 ADC and the clock, and assert the buzzer pattern sequence and that a full
two-button learn completes with no link present.

Run: `cd code && pio test -e native -f '*test_learning'`
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
- Create: `code/test_native/test_update/test_main.cpp` — required; copy Task 2's
  four-line `main()`.

**Interfaces:**
- Consumes: `IHAL` (Task 2)
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

Run: `cd code && pio test -e native -f '*test_update'`
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

Run: `cd code && pio test -e native -f '*test_update'`
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

Run: `cd code && pio test -e native -f '*test_update'`
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
- Create: `code/test_native/test_maintenance/test_main.cpp` — required; copy
  Task 2's four-line `main()`.

**Interfaces:**
- Consumes: `Config` (Task 8), `ReleaseCheck` (Task 17), `IHAL` (Task 2)
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

Run: `cd code && pio test -e native -f '*test_maintenance'`
Expected: FAIL — `Maintenance/MaintenanceMode.h` not found.

- [ ] **Step 3: Implement the mode logic and run**

`MaintenanceMode` holds `active_`, `entered_at_`, `last_activity_`, and a
`timeout_ms_` defaulting to **300000** (5 minutes). `Update` deactivates when
`now - last_activity_ >= timeout_ms_`. `NoteActivity` bumps `last_activity_`.
`Enter` always resets `last_activity_` to `now`, so a re-entry after a timeout
gets a fresh window.

Run: `cd code && pio test -e native -f '*test_maintenance'`
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
            idleMv = 2835,
            buttons = listOf(
                LearnedButton("VOL_UP", ratioPermille = 504, tolerancePermille = 42),
                LearnedButton("VOL_DOWN", ratioPermille = 630, tolerancePermille = 120)),
            liveRatioPermille = 504))
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
        run: python3 tools/check_size.py --env esp32s3 --max-bytes 1966080
```

**The size gate is a script, not an eyeball.** `tools/check_size.py` runs
`pio run -t size --json-output`, reads the app size, and exits non-zero above
**1920 KB (`1966080`)** — the real slot size, which Task 1 established by
correcting the partition table's alignment. This is the R-1 mitigation from spec
§12.2: the app not fitting is discovered here, on day one, not the week the
boards land.

- [ ] **Step 2: Write `android.yml`**

JVM tests, `assembleDebug`, and the APK uploaded as an artifact. The instrumented
tests need a device or emulator; run them on an emulator job with
`reactivecircus/android-emulator-runner`, and keep USB-specific tests excluded
from CI so a missing device is not a false failure — **documented in the
workflow comment**, not silently skipped.

- [ ] **Step 3: Confirm both workflows pass**

**Do not `git push` from inside this task.** Pushing is a shared-remote side
effect: it is visible to others, it can trigger CI on shared infrastructure, and
it is not reversible in the way a local commit is. An earlier revision of this
plan put a bare `git push` in a task step, which would have an implementer
subagent publish to the remote without the user ever deciding to.

Instead: commit locally, then **hand the push to the user** as an explicit,
separate decision. Record in the task report that the workflows are committed and
unverified-because-unpushed, and let the user run the push when they choose.

Once pushed (by the user), check the Actions tab. Expected: both green. **If the
size gate fails, that is R-1 arriving early** — take the §9.6 fallback rather
than raising the threshold.

- [ ] **Step 4: Commit**

```bash
git add code/.github code/tools/check_size.py
git commit -m "Add CI gates for host tests, contract sync, and the app size budget

The 1920KB size gate runs on every push so a BLE+WiFi+OTA build that outgrows
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
- [ ] **Step 3: The ADC ladder — and the R15/R16 decision.** With a resistor
  ladder attached, **sweep the +3V3 rail 3.14 → 3.47 V** and record the idle and
  per-button readings. **Do NOT sweep a 12 V vehicle rail** — no vehicle-rail
  term exists in the transfer function (spec §6.3), and the input must never see
  more than the 2.9 V ADC ceiling.
  Then the measurement that gates a PCB change: **record the idle resistance and
  confirm `R_ladder_idle ≤ R_pullup · 7.25`** (≈72.5 kΩ at the 10 kΩ `R15`/`R16`,
  spec §6.3 consequence 4). If it is over, `R15`/`R16` go **smaller**, not
  larger. Also note the ceiling headroom: the nominal 2835 mV idle reaches the
  2900 mV ADC ceiling at only **+2.3 %** of rail, so confirm the real idle is
  not already clipping. **Fit the real calibration curve here** — this is where
  §2.3's numbers become true for the actual board. Update the spec's values.
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
  100 presses each, at **3.14 / 3.30 / 3.47 V of +3V3 rail**, asserting zero
  misclassifications and the exact expected output level each time. (Not a
  12 V vehicle-rail sweep — that rail does not feed the ladder.) Then the
  72-hour soak.
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
7, 8. `Config` (Task 8) is used unchanged in 9, 11, 15, 16. `IHAL` (Task 2) is
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
  the rail (three of the tests then in the suite failed), and an above-rail reading
  was reporting `kIdle`. Fixed by comparing the **learned idle** against the
  current one (`kRailHealthFloorPermille`, FR-30's ≤20 % floor) and adding an
  explicit above-reference check.

`ConfigValidate`'s ambiguity rule was also corrected: the first version rejected
windows *nested* inside one another, but two 42-permille-wide windows 10 permille
apart are not nested, so the test failed. Replaced with the rule that actually
matters — the two centres must be further apart than the wider tolerance.

Verified standalone before committing to the plan: `LadderDecode` 18/18 checks
including the 34-point +3V3 sweep; the gesture state machine 11/11 including the
exact 500 ms and 750 ms boundaries and a 10-second hold emitting exactly one
event; `ConfigValidate` 12/12.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
