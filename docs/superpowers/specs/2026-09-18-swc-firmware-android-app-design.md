# SWC Adapter — Firmware & Head-Unit App Design Spec

**Date:** 2026-09-18
**Status:** DRAFT — awaiting review
**Deliverable:** Working firmware for the `SWC.kicad_pcb` board (ESP32-S3), an
Android app for the head unit, and the web/BLE maintenance path — all under
`code/` in this repository.

**Companion plan:** `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md`

---

## 1. What this builds, and why

The board already exists and is ordered. It reads a vehicle's steering-wheel
button ladder on each of two channels, and drives a servo-controlled current
sink to present the head unit with a key resistance the head unit understands.
The hardware is fixed; **this document specifies the software that makes it
work**, and nothing in it may contradict the board.

Three components:

| # | Component | Runs on | Owns |
| --- | --- | --- | --- |
| A | **Firmware** | ESP32-S3 on the SWC board | Ladder acquisition, press classification, DAC/servo output, buzzer + LED feedback, config storage, USB command link, BLE provisioning, WiFi OTA |
| B | **Head-unit app** | The Android head unit | Config UI, per-vehicle ladder learning, action mapping (launch app / intent / key / media), the USB peer, USB OTA |
| C | **Maintenance web page** | Served by the ESP32 | Minimal: join WiFi, then OTA. Not a config UI |

The three requirements that shaped everything:

1. **The software is written before the board arrives.** The board is finalized
   and ordered. Every piece of logic that *can* be tested without hardware
   *must* be, so that bring-up is a tuning exercise, not a debugging one.
2. **A crash costs the user their steering-wheel controls while driving.** The
   failure mode is not a dropped request; it is a driver with no working
   buttons. Safety behavior is a first-class requirement, not a hardening pass.
3. **The USB-C port is the only connector, and it carries both power and data.**
   The same port must be the command link, the debug path, and (on the bench)
   the flashing path. There is no second connector to fall back on.

### 1.1 What is explicitly out of scope

- Any change to `SWC.kicad_pcb` / `SWC.kicad_sch`. The board is ordered. Defects
  found on the board are recorded in §12, not fixed here.
- The enclosure (`plastic_case/`) — already built, separate spec.
- A full web configuration UI. The ESP32 serves one maintenance page only (§8).
- Remote/cloud management. There is no server component.

---

## 2. Hardware contract (verified from the netlist)

**This section is the interface. Everything downstream depends on it.** Every
row is traceable to `mcp__kicad__export_netlist` output; nothing here is
inferred from prose or from part names. `DESIGN.md` §4 is the prose companion
and agrees with this table; where they ever differ, this table wins because it
was read from the netlist.

### 2.1 The MCU

| Item | Value |
| --- | --- |
| Module | `U3` DOIT `ESPS3-32-N4` (LCSC C49164655) |
| Silicon | ESP32-S3, dual-core Xtensa LX7, **no on-chip DAC** |
| Flash | **4 MB, no PSRAM** (see §9 — this is a hard budgeting constraint) |
| Footprint | `RF_Module:ESP32-S3-WROOM-1` (a WROOM-1 land-pattern clone) |
| USB | Native USB 1.1 Full-Speed only — the S3 has no High-Speed PHY |
| ADC ceiling | **2.9 V** calibrated at 12 dB attenuation (not 3.1 V) |
| Antenna | Module overhangs the board edge by 5.9 mm; keepout is off-board |

### 2.2 Complete pin map

Every GPIO the board connects, with its net, its direction and its function.
These are the only pins available — there is no spare-pin freedom here.

| GPIO | Net name | Dir | Function |
| --- | --- | --- | --- |
| IO1 | `/SWC1_ADC` | A-in | Channel 1 ladder, ADC1_CH0 |
| IO2 | `/SWC2_ADC` | A-in | Channel 2 ladder, ADC1_CH1 |
| IO7 | `/TEMP_ADC` | A-in | NTC `RT1` divider (10 k B3380) |
| IO8 | `/SENSE1` | A-in | Channel 1 KEY-line sense, KEY voltage ÷ 2 |
| IO9 | `/SENSE2` | A-in | Channel 2 KEY-line sense, KEY voltage ÷ 2 |
| IO10 | `/VBUS_VALID` | D-in | VBUS present (`R56`/`R57` ÷ 2 off fused VBUS) |
| IO13 | `/BUZZ` | D-out | Buzzer `BZ1` low-side `Q3` gate, via `R27` 100 Ω |
| IO14 | `/LED2` | D-out | LED `D12` (green), via `R26` 1 k |
| IO47 | `/LED_STAT` | D-out | LED `D6` (green), via `R7` 1 k |
| IO17 | `/I2C_SDA_3V3` | I/O | I²C SDA to `U4` MCP4728 |
| IO18 | `/I2C_SCL_3V3` | I/O | I²C SCL to `U4` MCP4728 |
| IO48 | `/DAC_LDAC_B_3V3` | D-out | MCP4728 `~LDAC` (active low), `R13` 10 k pulldown |
| IO0 | `/BOOT` | D-in | `SW1` BOOT — **strapping pin**, also usable as a user input |
| IO3 | `/IO3` | — | Spare, `TP1` test point only |
| IO4 | `/AUX1_F` | A-in | **AUX1** direct input, ADC1_CH3 |
| IO5 | `/AUX2_F` | A-in | **AUX2** direct input, ADC1_CH4 |
| IO6 | `/AUX3_F` | A-in | **AUX3** direct input, ADC1_CH5 |
| IO11 | `/IO11` | — | Spare, `TP2` only |
| IO12 | `/IO12` | — | Spare, `TP3` only |
| IO15 | `/IO15` | — | Spare, `TP4` only |
| IO16 | `/IO16` | — | Spare, `TP5` only |
| IO21 | `/IO21` | — | Spare, `TP6` only |
| IO19/IO20 | module USB | USB | `USB_D-` / `USB_D+` (not broken out as GPIO) |
| — | `/TXD0`, `/RXD0` | — | `TP7`/`TP8` only; ROM UART, no header pins |

**Constraints that follow:**

- **IO0 is a strapping pin.** It must not be held low at boot — holding it low at
  power-on enters the ROM serial-download bootloader, *not* a user action. This
  is why the programming and maintenance triggers (§7.5, §8.2) are **runtime
  long-presses on a running device**, never a hold-at-power-on.
- **AUX1–AUX3 (IO4/IO5/IO6) are fully usable analog inputs**, wired exactly like
  the SWC channels but with 1 kΩ series resistors. Together with the two SWC
  channels that is **five analog key inputs** — and the AUX inputs are the
  intended local programming/test buttons, since they need no external ladder.
- **IO4 is ADC1_CH3 on the S3.** Some Arduino cores map a DAC to IO4 on *other*
  ESP32 variants; the S3 has no DAC at all, so this is only a caution against
  copy-pasted board definitions, not a real hazard.
- **IO3, IO11, IO12, IO15, IO16, IO21 are broken out to test points only.**
  There is no header. They are not usable for user-facing features.
- **ADC1 only.** IO1, IO2, IO4, IO5, IO6, IO7, IO8, IO9 are all on ADC1. ADC2 is
  unusable while WiFi is active on ESP32-S3 — irrelevant in normal mode (the
  radio is off, §8.1) but a real constraint if any ADC2 pin is ever used later.
- There is **no external USB-UART bridge** and **no spare UART pins**. All
  serial I/O is over the native USB, and the console must live on the ROM
  USB-Serial-JTAG peripheral (§4.1).

### 2.3 The analog output stage (per channel)

The output is a **closed-loop integrator servo**, not an open-loop buffer.
Verified from the netlist and matching `DESIGN.md` §4.4:

```
V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ
        with R58 = 82 kΩ, R61 = 100 kΩ   →   V_KEY = 1.82·V_DAC − 0.82·V_ADJ
```

| Element | Refs (ch 1 / ch 2) | Value | Role |
| --- | --- | --- | --- |
| DAC signal channel | `U4` VOUTA / VOUTC | 12-bit, 0–3.3 V | Sets the target; drives integrator via 100 k |
| DAC gain channel (`V_ADJ`) | `U4` VOUTB / VOUTD | 12-bit, 0–3.3 V | **Gain-mode selector** |
| Integrator | `U6A` / `U6C` + 100 nF | — | Forces `U6−` = `V_DAC` at DC |
| Gain-set resistors | `R58`/`R61`, `R59`/`R60` | 82 k / 100 k | Sets gain = 1.82 |
| Sink FET | `Q4` / `Q6` (2N7002) | — | **Sinks only** — cannot source |
| Sense buffer | `U6B` / `U6D` | unity follower | Reads KEY line via 1 MΩ `R36`/`R43` |
| Sense divider | `R54`/`R50`, `R55`/`R51` | 10 k / 10 k | **Exact ÷2** → `/SENSEn` |
| Gate pulldown | `R48` / `R49` | 100 k | Holds FET off if op-amp unpowered |

**Two consequences the firmware must respect:**

1. **The gain mode is chosen by what the `V_ADJ` channel *is*, and the MCP4728
   has no high-impedance state.** Its power-down modes are 1 kΩ / 100 kΩ /
   500 kΩ pull-**downs**. The 1 kΩ one is exactly what the 5 V range needs.

   | Target head unit | `V_ADJ` channel | Gain |
   | --- | --- | --- |
   | 5 V range | powered down (`PD1:PD0 = 01`) → defined 1 kΩ to GND | **1.82** |
   | 3 V range | normal, tracking the signal channel's code | **1.00** |

2. **Releasing the KEY line needs no special mode.** `Q4` only sinks. To
   release, command a target *above* the head unit's own idle voltage; the servo
   drives the gate low, `Q4` turns off, and the line floats up through the head
   unit's own pull-up, loaded only by `R36` 1 MΩ. **Idle state = high impedance,
   for free.**

The op-amp runs on +5 V, so `V_buf` saturates near 4.98 V. Gain 1.82 advertises
6.0 V but the line can only be servoed to ~5 V — the surplus is deliberate
release margin, not a bug.

Because the sense divider is an exact ÷2 and the op-amp rail binds before the
ADC ceiling, **`V_SENSE` can never exceed ~2.49 V and the ADC can never
saturate.** No clamp logic is needed in firmware.

### 2.4 The ladder input (per channel)

```
steering-pad ladder (series chain, common tied to GND)
   └─ J2 ─ R1/R2 10 kΩ ─┬─ /SWC1_ADC / /SWC2_ADC ─► ESP32-S3 ADC1
                        ├─ R15/R16 10 kΩ pull-up to +3V3
                        ├─ D4/D5 BAT54S clamp to +3V3 / GND
                        └─ C3/C4 100 nF
```

**The ladder is a resistor chain with its common tied to ground, and a button
shorts the node it sits at to the chain's common — so pressing a button pulls the
input *down*, not up.** (Reference: `Tundra_SWC_steeringpadswitch.bmp` in the
Android_Stereo_Apps working notes; verified with the board's owner, 2026-09-18.)
This is the single most important correction in this section:

- **Idle (no button) is the *high* state:** the ladder's full series resistance
  sits between the pin node and GND, so the node is pulled up toward **+3V3**
  through `R15`/`R16`. It is **not** driven to +12 V, and the pin is therefore
  **never saturated** — the node sits inside the ADC's 2.9 V range.
- **Pressing a button lowers the node voltage** by shunting part of the ladder to
  GND. Different buttons short at different points in the chain, giving different
  resistances to GND and therefore different pin voltages.
- The **10 kΩ pull-up is what makes a bare switch-to-ground button work as well as
  a resistor ladder**, and it is what sets the idle level.
- The BAT54S clamp is **protection only** — against a miswire or a ladder that is
  externally pulled high. It is not the operating point.
- The node voltage is a function of the ladder resistance **and** of the +3V3
  rail, not the vehicle's 12 V rail. `R15`/`R16` and the ladder's own values form
  the divider, so **the pull-up value may need to be adjusted to suit the measured
  ladder resistances** — a bring-up measurement (§10.6), not an assumption.

> **This supersedes `DESIGN.md` §4.1's "pulled to 12 V when idle" prose, which is
> wrong.** An earlier revision of this section repeated that claim and derived a
> saturated-node transfer function from it; that derivation was retracted on
> 2026-09-18. §6.3 is written against the topology above.

`AUX1`–`AUX3` on `J5` are electrically identical but use a **1 kΩ** series
resistor (`R23`–`R25`) and the same pull-up/clamp/filter, on IO4/IO5/IO6.

### 2.5 Power, feedback, and the one user-visible button

- **12 V is optional.** `J1` → `F1` PPTC → `D1` SMBJ18A TVS → `U1` XL1509-5.0
  buck; USB VBUS → `F2` PPTC → `D3`. `D2`/`D3` diode-OR at +5 V, then `U2`
  AMS1117-3.3. Either source alone is sufficient. `/VBUS_VALID` (IO10) tells the
  firmware which one is present.
- **Feedback is a buzzer and two LEDs.** `BZ1` is a magnetic buzzer on +5 V
  switched by `Q3` from `/BUZZ` — **a plain on/off drive, not a PWM tone
  generator.** Its frequency is fixed by the part; the firmware can only gate it.
  This is a real change from the 2022 design, which drove a PWM melody (§6.5).
- **`SW1` (BOOT, IO0) and `SW2` (RESET, EN) are recessed** and are
  "poke with a tool" controls, not thumb buttons — see the enclosure spec §5.6.
  RESET is wired to the chip's EN pin, **not to a GPIO**: the firmware cannot
  read it, and reset causes a full reboot. BOOT *is* readable, but because it is
  recessed and a strapping pin it is reserved for recovery, not user gestures —
  the user-facing physical input is **AUX1** (§2.2, §7.5).

---

## 3. The data model

This is the **initial layout of the entities**, and it is the contract between
all three components. The firmware persists it in NVS, the Android app edits it
and sends it over USB, and the web page only ever touches the small subset in
§3.6. Every field is defined once, here.

### 3.1 Entity map

```
Config ──┬── schema_version, device_id, created_at, updated_at
         ├── DeviceSettings          (timings, gain policy, feedback levels)
         ├── Channel ×2              (SWC1, SWC2)   ── per-channel ladder + output
         │     ├── Ladder                            ── learned per-vehicle
         │     │     └── LadderButton ×N
         │     └── OutputProfile
         ├── AuxButton ×3            (AUX1–AUX3, direct digital/analog inputs)
         └── Binding ×N              ── (input, gesture) → [Action…]
                                       └── Action ×N
```

`Binding` is the join table that makes the product work: it maps *what the user
did with their thumb* to *what should happen*. Everything else is either
calibration data or presentation.

### 3.2 Identifiers and value types

Getting these right up front prevents a class of silent bugs where an ADC
count is mistaken for a millivolt or a resistance.

| Type | Range | Meaning |
| --- | --- | --- |
| `AdcRaw` | u16, 0–4095 | Raw **12-bit** ADC count as read from the S3 at 12 dB atten |
| `MilliVolt` | u16, 0–2900 | Pin voltage in mV, after calibration, **at the pin** |
| `MilliOhm` | u32 | Ladder resistance in mΩ. Not inferred unless learned — see §3.4 |
| `DacCode` | u16, 0–4095 | MCP4728 code. `mV ≈ code × 3300 / 4095` (1 LSB = 806 µV) |
| `TimestampMs` | u32 monotonic | Milliseconds since boot |

**Rule:** the firmware stores ladder calibration as `MilliVolt` at the pin, and
**never** as a resistance, because the board does not measure resistance — it
measures a divided voltage (§6.3), and the ladder's own values are vehicle-
specific and unknown until learned. `MilliOhm` is display-only, computed for the
app's benefit **if and when** the user supplies the ladder's reference voltage.
Storing a resistance as if it were measured would be a lie.

### 3.3 Gestures

```jsonc
"gesture": "SINGLE" | "DOUBLE" | "TRIPLE" | "LONG" | "LONG_REPEAT"
```

`SINGLE`, `DOUBLE`, `LONG` are required and are what the user asked for.
`TRIPLE` and `LONG_REPEAT` are defined in the enum from day one so the schema
does not need a migration when they are implemented; the plan implements
`SINGLE`/`DOUBLE`/`LONG` first and adds the rest behind a capability flag.

A `COMBO` gesture (two ladder buttons held simultaneously — e.g. VOL_UP + VOL_DN)
is **deferred to v2** and is called out in §12. The two-channel hardware makes it
possible, but it interacts badly with the per-channel state machines and should
not be in the first cut.

### 3.4 Learned ladder

The critical insight: **the ladder is a series chain whose common is tied to GND,
and each button shunts a different point in that chain to common, so each button
produces a distinct divider ratio against the +3V3 pull-up.** The absolute pin
voltage therefore depends on the ladder's own resistance values — which are
vehicle-specific and **not known in advance** — and secondarily on the +3V3 rail
(§6.3). Values are stored both as measured millivolts and as a **normalized
ratio**, so runtime classification does not depend on the rail or on absolute
level. See §6.3 for the topology and why an earlier revision's "divider against
the vehicle's 12 V rail" reading was wrong.

```
LadderButton {
  id           : "vol_up"          // stable slug, used by bindings
  name         : "Volume Up"        // display only
  mv_center    : 1240               // MilliVolt at the pin when this button is held
  mv_tolerance : 120                // half-width of the accept window
  learned_at_rail_mv : 3300         // +3V3 rail measured during learn (§6.3)
  temp_c_at_learn    : 23.5         // for the NTC compensation model
  sample_count : 200                // how many samples were averaged
  confidence   : 0.98               // learn-quality score, 0–1
}
```

`mv_tolerance` is **derived at learn time** as the midpoint of the gap to the
nearest neighbouring button, capped by a configurable maximum. A hand-derived
tolerance is the classic cause of "two buttons both trigger the same action".

### 3.5 Bindings and actions

A binding is `(channel, button, gesture)` → ordered list of actions. Most will
have exactly one action; the list exists because "emit the factory key press
**and** tell the app" is a real, wanted combination.

```jsonc
{
  "id": "b1",
  "channel": "SWC1",              // SWC1 | SWC2 | AUX1 | AUX2 | AUX3 | ANY
  "button": "vol_up",             // LadderButton.id, or "NONE" for gestures on the programming button
  "gesture": "SINGLE",
  "enabled": true,
  "actions": [ /* Action[] — executed in order, each independently failable */ ]
}
```

Two rules that matter:

- **A binding with an empty `actions` list is not the same as `enabled: false`.**
  Empty means "swallow this gesture" (recognised, does nothing). Disabled means
  "do not match this gesture at all", so a lower-priority binding may.
- **Action execution is best-effort and ordered.** If action 1 fails, action 2
  still runs (unless action 1 is a `MACRO` with `abort_on_failure: true`). A
  failed app-side action must never prevent the hardware key press.

### 3.6 The action library

This is the union of "what the head unit's own SWC input can do" (the `OUT_`
family) and "the extra functions the Android app provides" (everything else) —
which is precisely the split the user described.

| `kind` | Params | Executed by | Purpose |
| --- | --- | --- | --- |
| `NONE` | — | — | Explicit no-op; useful as a placeholder |
| `OUT_VOLTAGE` | `key_mv` | Firmware | **The core function.** Drive the KEY line to a voltage the head unit reads as a key |
| `OUT_RELEASE` | — | Firmware | Force the KEY line to idle/high-Z |
| `APP_LAUNCH` | `package` | Android | Launch an app by package name |
| `APP_INTENT` | `action`, `data` | Android | Send an arbitrary intent, **including a data payload** — the user's stated example |
| `KEYCODE` | `keycode` | Android | Inject a key event (`KEYCODE_MEDIA_NEXT`, …) |
| `MEDIA` | `command` (`play`/`pause`/`next`/`prev`/`stop`) | Android | Media transport via `MediaSession<｜｜begin▁of▁sentence｜｜>`-style dispatch |
| `VOLUME` | `target` (`media`/`call`/`ring`/`alarm`) | Android | Volume, including absolute set which stock SWC cannot do |
| `SYSTEM` | `command` (`screen_off`/`night_mode`/`screenshot`/`open_settings`) | Android | Head-unit housekeeping |
| `BUZZ` | `pattern` (named) | Firmware | Local audible confirmation, independent of the buzzer grammar |
| `APP_RAW` | `command` | Android | Escape hatch: an app-defined command not yet promoted to a kind |

**Every action carries at most two string params, and that is a budget
constraint rather than a design preference.** An earlier revision of this table
gave `APP_INTENT` five (`action`, `data`, `mime`, `extras{}`, `flags[]`),
`KEYCODE` two, `VOLUME` two and `APP_RAW` two. None of those extra params had a
field in the shared `Action` entity, so a config carrying one could not be
stored or transmitted — `ConfigValidate` refuses it, which is the safe direction
but leaves the table describing actions the product cannot express. Closing that
gap from the other side does not work either: the worst-case `Config` already
occupies **96 % of the `nvs` partition** (the measurement below), and a per-action
params field costs a 12th NVS chunk at *any* width, because chunking is coarse —
even `params[32]` pushes two slots past the partition, and clawing it back means
cutting `kMaxBindings` from 32 to about 18. Two string params per action is what
the partition buys at this cardinality.

So the rule is: **`target` and `payload` are the action's two string slots, and
each kind names them on the wire** — `APP_INTENT`'s `action`/`data`,
`APP_LAUNCH`'s `package`, `KEYCODE`'s `keycode`, `MEDIA`/`SYSTEM`/`APP_RAW`'s
`command`, `VOLUME`'s `target`, `BUZZ`'s `pattern`. The wire key is the kind's own
name for the parameter (§3.7 is written in those names); the struct field is the
generic one. `OUT_VOLTAGE` is the exception that uses the numeric field instead.
A kind needing more than two strings is a v2 item with its own `schema_version`,
not a v1 action.

**Why the output kind is `OUT_VOLTAGE` and not `HW_KEY`.** An earlier revision
called this kind `HW_KEY` and gave it `key_resistance_mohm` or `dac_code`. Both
names were wrong, and for the same reason: **the firmware does not drive a key,
it drives a voltage.** The output stage is a closed-loop integrator servo
(§2.3) — `V_KEY = 1.82·V_DAC − 0.82·V_ADJ` — and what it controls is the voltage
on the KEY line. "Which key" is the *head unit's* interpretation of that voltage,
and it differs per head unit: the same `volume up` is a different voltage in every
car. An action that named a key would be promising an identity the firmware cannot
know, so the action names the voltage and the head unit supplies the meaning.
`dac_code` was wrong for a second, independent reason: a code is coupled to the
gain mode, so changing `gain_mode` would silently change what every stored code
means — the same "two homes for one level" defect that removed
`output.key_values` below. `key_mv` is mode-independent.

The `OUT_` prefix marks the *output* side of the device, which is the distinction
that matters here: `OUT_VOLTAGE`/`OUT_RELEASE` act on the KEY output, while the
ladder buttons and gestures act on the input side and `BUZZ` drives a different
output entirely.

**Resistance is the app's vocabulary, not the firmware's.** The head-unit profile
— its ladder's resistances, its pull-up, its idle level — lives in the Android
app, which is where a per-head-unit model belongs and where it is set up once for
the user's car. The app converts a resistance to the voltage it produces and sends
`key_mv`. The firmware never needs a head-unit constant, which is what keeps
§6.5's "unknown pull-up" from being a firmware input at all. (`test_key`'s
bench/production frame carries the same `key_mv` for the same reason.)

`MACRO` is **not** an action kind — it is a binding with an ordered `actions`
list, which is the same thing with one fewer concept to learn.

**Design note on `OUT_VOLTAGE` vs `APP_INTENT`:** a single physical button can be
bound so that `SINGLE` sends an `OUT_VOLTAGE` (so the stock head unit reacts even
if the app is not running) while `DOUBLE` sends an `APP_INTENT` (an extra function
the head unit never had). That is the whole point of the product, and the data
model expresses it without a special case.

**An action has no id.** It is identified by its `kind` (the 11 above) plus its
params. An earlier revision of the plan invented a numeric action-id table
(`1–63`) and treated ladder-button slugs (`VOL_UP`) as action *names*; the spec
defines neither, and a generated contract must not synthesize them. `VOL_UP` is
a `LadderButton.id`, used in a binding's `button` field — never an action name.

**The output level lives in the action, and there is no per-button key table.**
An earlier revision of §3.7's worked example carried an
`output.key_values: { vol_up: { dac_code: 1240 }, … }` map alongside the
bindings. That is a second home for the level: §3.6's `OUT_VOLTAGE` row already
says an action carries `key_mv`, and §3.5's whole design is
that a binding's own `actions` list is what decides what happens — including the
core case where `vol_up`'s `SINGLE` sends an `OUT_VOLTAGE` while its `LONG` sends
something else entirely. A per-button default map cannot express that, and two
places to look for one level is how they drift. The map is removed; a binding's
`OUT_VOLTAGE` action names its own level. This is also a budget fact rather than a
taste one: 32 buttons × a `{dac_code}` entry is 576 B, which takes the measured
worst case from 22,407 B to 22,983 B → **12 chunks → 50,720 B = 105 % of the
partition** — the map does not fit.

**Cardinality (v1), fixed by the NVS budget rather than by preference:**

| Limit | Value | Why |
| --- | --- | --- |
| Bindings, total | **32** | They are a top-level table, not per-channel; 32 covers 2 channels × several gestures × the AUX inputs |
| Actions per binding | **2** | Both bounds come from the partition — see below |
| Ladder buttons per channel | 16 | Fits `LadderProfile`'s fixed array |
| AUX buttons | 3 | AUX1–AUX3 |

**String field widths are also a budget input, not a preference.** A provable
staging bound has to cover every field at its declared maximum, so the widths
below are load-bearing — widening any of them re-opens the partition arithmetic:

| Field | Width | Holds |
| --- | --- | --- |
| `Action.target` | **40** | `com.oetsolutions.swc.ACTION_NAVIGATE` (34); a longer intent action must be refused by validation, not truncated |
| `Action.payload` | **48** | `geo:40.7608,-111.8910?q=Home` (28); a full street address does **not** fit and is refused |
| `Binding.id`, `LadderButton.id`, `Binding.button` | **16** | Slugs, e.g. `vol_up`, `next` |
| `ChannelConfig.name`, `LadderButton.name` | **16** | Display labels |

**Measured (§3.8's method), and re-measured 2026-09-19 after a defect:** the
structural worst case is every string field at its width above, 2 channels × 16
buttons, 3 AUX, and 32 bindings × 2 actions. That is **22,407 B** as JSON →
**11 chunks**; two slots plus `cfg_seq` cost **46,496 B of the partition's 48,384
B usable bytes (96 %)**.

**An earlier revision of this paragraph quoted the same 11 chunks / 46,496 B /
96 % for a configuration it did not measure.** It said 32 bindings × 2 actions
while the figure is only reproducible at **one** action per binding (20,741 B →
11 chunks). At 2 actions with the *unbounded* widths that revision also declared
(`target[64] payload[128] name[24]`), the same arithmetic gives **30,021 B → 15
chunks → 63,392 B = 131 % — an overflow, not a fit.** Two numbers in one
paragraph describing two different data models, which is why a rewrite that
swept for contradictions *between* documents did not catch it. The widths table
above is what makes the two agree: at 40/48/16/16 the figure is real.

The step up is now measured too: 3 actions per binding at these widths needs
**14 chunks = 59,168 B = 122 %** and does not fit. So 2 is not a round number
chosen for comfort; it is the largest value the existing partition stores with
both slots present, and the string widths are what buy it. Enlarging NVS would
cost app-slot space and re-open the §9.2 partition layout — and note the 48 KB is
not the config's alone: §9.2 records that WiFi provisioning credentials live in
the same `nvs` partition, so the two-slot budget above is an upper bound on what
the config can ever have.

### 3.7 Full worked example

The user's stated scenario — "the head unit may not have a SWC to open a specific
app or a specific intent with data payload, but the Android app can" — written
out:

```jsonc
{
  "schema_version": 1,
  "device_id": "swc-a1b2c3",
  "device": {
    "name": "SWC Adapter",
    "hostname": "swc-adapter",
    "settings": {
      "single_press_ms": 0,
      "double_press_gap_ms": 500,
      "long_press_ms": 750,
      "long_repeat_ms": 250,
      "debounce_ms": 25,
      "release_margin_mv": 900,
      "gain_policy": "AUTO",
      "temp_comp_enabled": true,
      "buzzer_level": "NORMAL",
      "led_level": "NORMAL",
      "usb_protocol_version": 1
    }
  },
  "channels": [
    {
      "id": "SWC1",
      "enabled": true,
      "ladder": {
        "source": "LADDER_3V3",
        "idle_mv": 2835,
        "buttons": [
          { "id": "vol_up",   "name": "Volume Up",   "mv_center": 1430, "mv_tolerance": 120,
            "learned_at_rail_mv": 3300, "temp_c_at_learn": 23.5, "sample_count": 200, "confidence": 0.98 },
          { "id": "vol_dn",   "name": "Volume Down", "mv_center": 1785, "mv_tolerance": 120,
            "learned_at_rail_mv": 3300, "temp_c_at_learn": 23.5, "sample_count": 200, "confidence": 0.97 },
          { "id": "next",     "name": "Next Track",  "mv_center": 2145, "mv_tolerance": 110,
            "learned_at_rail_mv": 3300, "temp_c_at_learn": 23.5, "sample_count": 200, "confidence": 0.99 }
        ]
      },
      "output": {
        "gain_mode": "AUTO",
        "idle_dac_code": 4095
      }
    }
  ],
  "bindings": [
    { "id": "b1", "channel": "SWC1", "button": "vol_up", "gesture": "SINGLE",
      "enabled": true, "actions": [ { "kind": "OUT_VOLTAGE", "key_mv": 2400 } ] },

    { "id": "b2", "channel": "SWC1", "button": "vol_up", "gesture": "LONG",
      "enabled": true, "actions": [ { "kind": "OUT_RELEASE" } ] },

    { "id": "b3", "channel": "SWC1", "button": "next", "gesture": "DOUBLE",
      "enabled": true, "actions": [
        { "kind": "APP_LAUNCH", "package": "com.spotify.music" }
      ] },

    { "id": "b4", "channel": "SWC1", "button": "next", "gesture": "LONG",
      "enabled": true, "actions": [
        { "kind": "APP_INTENT",
          "action": "com.oetsolutions.swc.ACTION_NAVIGATE",
          "data": "geo:40.7608,-111.8910?q=Home",
          "extras": { "started_by": "swc", "profile": "daily" },
          "flags": ["FLAG_ACTIVITY_NEW_TASK"] }
      ] }
  ]
}
```

### 3.8 Persistence, versioning and migration

- **Storage:** NVS, namespace `swc_cfg`. The `Config` is stored as a single JSON
  blob **per slot**, with **two slot keys** (`cfg_a`, `cfg_b`) plus a small
  sequence key (`cfg_seq`) — see the A/B rule below. A partial config is worse
  than no config, which is why each slot is written and verified whole rather
  than field-by-field.
- **Blob size is a measured quantity, not an assumption.** An NVS value has a
  per-entry ceiling. The full `Config` is expected to be several KB and **may
  exceed one NVS value.** The measured serialized size therefore selects the
  strategy, and the firmware must not assume either way:
  - if a slot fits one value → one blob per slot, as above;
  - if it does not → the slot's blob is **chunked across a small fixed number of
    NVS keys** (`cfg_a_0`, `cfg_a_1`, …) with the chunk count and a per-slot CRC
    in a header chunk, so a slot is still read and validated as a unit.

  This is the one place where "measure, then decide" is mandatory rather than
  preferred, because the answer changes the persistence code.
  **Measured, 2026-09-18; re-measured 2026-09-19 — the answer is "it does not
  fit, so a slot is chunked."** On IDF 5.5.5 the ceiling is `ENTRY_SIZE ×
  (ENTRY_COUNT − 1)` = 32 × 125 = **4000 bytes** (`nvs_page.cpp` returns
  `ESP_ERR_NVS_VALUE_TOO_LONG` above it), and the worst-case `Config` — §3.5's
  cardinality (32 top-level bindings × 2 actions, 2 channels × 16 buttons, 3 AUX)
  at §3.5's string widths — is **~21.9 KB** as JSON. **Even a realistic config
  (~3.9 KB) sits on that ceiling and a moderate one (~7.9 KB) exceeds it**, so a
  single-value slot fails on the common case, not a hypothetical one; the chunked
  branch above is the one that ships. Chunk size is a fixed 2048 B and the count
  is bounded (**11 worst case**). Two worst-case slots cost **96 % of the
  partition's usable entry space** (46,496 B of 48,384 B), so it fits, but with
  almost no margin — which is why §3.5's action-per-binding cap is 2 **and** why
  §3.5 fixes the string widths. Each 2048-byte chunk is one NVS *key*, and NVS
  charges **2112 B** for it — a 32-byte metadata entry, the 2048 payload bytes,
  and a 32-byte `BLOB_IDX` entry written once per key (`nvs_storage.cpp:353`; the
  `writeItem` span at `nvs_page.cpp:185` covers only the first two). An earlier
  revision of this figure counted 2080 B and so understated the two-slot cost by
  576 B. This is recorded here because a "measure, then decide" rule with no
  recorded measurement is an assumption with extra steps.

  **Two earlier revisions of this paragraph were wrong in the same way,** and the
  correction is recorded rather than quietly applied. The first said "32
  bindings" while computing from a *per-channel* binding model (2 × 32 = 64
  bindings, ~17.8 KB). The second quoted **21,411 B → 11 chunks → 96 %** as the
  worst case at 32 bindings × 2 actions — but that figure is only reproducible at
  **one** action per binding. At 2 actions with the widths that revision declared
  (`target[64] payload[128] name[24]`) the same arithmetic gives **30,021 B → 15
  chunks → 131 %**, an overflow. The measurement and the cardinality table
  described different configurations *in the same paragraph*. §3.5's width table
  is the fix, and the figure above is now the one that table produces.
- **Dual-slot writes with a monotonic sequence number.** NVS is written **A/B**:
  write the inactive slot, verify it by read-back, bump the sequence, *then* flip
  the active marker. **A power loss mid-write must never destroy a working
  config** — this is not theoretical, because the device lives on a car's
  electrical system and can lose power at any instant, including during a save.
  The reader always takes the higher valid sequence number.
- **A CRC covers the blob.** A corrupt config must be *detected* and replaced
  with known-good defaults, never partially applied — a half-applied mapping is a
  car control that does the wrong thing.
- **Staged, then committed.** A learn run or a bulk edit writes to a staging area
  and commits only after read-back verification. Any failure keeps the previous
  table.
- **`schema_version` is mandatory and checked on read.** A config written by a
  newer firmware is **not** silently interpreted by an older one; it is rejected
  and defaults are used, with a loud log and a buzzer pattern. **The NVS is not
  overwritten until the user confirms**, so a bad flash remains recoverable.
- **Migration is forward-only and additive.** Each version bump needs a
  `migrate_vN_to_vN+1` function. Removing a field requires bumping
  `schema_version` and *keeping* the reader for the old one for at least one
  release.
- **Defaults are always valid.** With no config at all, the device must still
  pass the ladder through to the output 1:1 (§6.9). Out of the box, before any
  configuration, the adapter behaves as a transparent pass-through.
- **Also persisted:** a small learn log (the last few learn runs' raw medians,
  for drift diagnosis) and a boot counter. Nothing else — every extra persisted
  key is another migration to carry.


---

## 4. The USB link

The USB-C port is the only connector. It is simultaneously the power source, the
bench flashing/debug path, and the command link to the head-unit app. **One
transport, three jobs** — the design must not let the debug job corrupt the link
job, or vice versa.

### 4.1 Transport choice

The ESP32-S3 has **two** independent USB peripherals, and this is the design
decision that unlocks the whole thing:

| Peripheral | Use here | Why |
| --- | --- | --- |
| **USB-Serial-JTAG** (ROM) | **Console + JTAG debug only**, and bench flashing | Built into ROM; always enumerates, even with a dead app. Never carries the app protocol. |
| **TinyUSB CDC** (OTG) | **The Android command link** | Application-controlled, independent of the console. |

**Both may be active at once.** Keeping the console on USB-Serial-JTAG and the
protocol on TinyUSB CDC means a debug `printf` can never be mistaken by the
Android app for a protocol frame — the classic failure when both share one CDC.

**This is a hard requirement:** the console must never be configured onto the
TinyUSB CDC port in a production build. §10 tests it.

> **Build consequence, verified 2026-09-18 (Task 1, against IDF 5.5.5): TinyUSB
> is not part of IDF — it is the managed component `espressif/esp_tinyusb`.**
> IDF 5.5.5 ships no `components/tinyusb` and defines **no** `CONFIG_TINYUSB_*`
> symbols, so any such key in `sdkconfig.defaults` is silently inert (accepted
> with no warning, and doing nothing). The app link above is therefore **not
> real until a task adds the component** — via `idf_component.yml` or
> `lib_deps` — and configures it. Until then the console on USB-Serial-JTAG
> works and there is no app interface at all. The task that owns `UsbCdc`
> (§10.1) must add it; this note exists so that task cannot be written without
> noticing.

### 4.2 Framing

Line-oriented **NDJSON** (newline-delimited JSON) in both directions, with an
explicit envelope. Chosen over a binary/COBS format deliberately:

- It is human-readable on a bus analyzer and in a terminal, which is worth a lot
  during bring-up on hardware nobody has yet.
- The payload here is button events and config — tens of bytes, a few per
  second at the very worst, on a Full-Speed link. Framing efficiency is
  irrelevant; debuggability is not.
- JSON parsers exist on both sides already.

```
frame  := object "\n"                 ; one JSON object per line, UTF-8, no embedded newline
object := { "v":1, "seq":u32, "type":TYPE, ...payload }
```

Every frame carries:

| Field | Type | Purpose |
| --- | --- | --- |
| `v` | u8 | **Protocol version.** Mismatch is handled explicitly (§4.5) |
| `seq` | u32 | Per-sender monotonic counter, for gap detection and ack matching |
| `type` | string | What this frame is |

**Line length is capped** (recommend 1024 bytes incl. newline). An over-long
line is discarded with an error frame rather than buffered — an unbounded line
buffer on a 4 MB/no-PSRAM part is a heap-exhaustion bug that a hostile or buggy
peer can trigger.

**The cap applies to every frame, including config transfer, and that is why
config transfer is chunked.** A full `Config` is several KB (§3.8) and cannot
fit one line, so `config_get`/`config_set` carry the config as an **ordered run
of chunks**, each comfortably under the cap:

```
config_get   →  fw replies  config_begin {total_len, crc32}
                            config_chunk {offset, data_b64}   × N
                            config_end   {sha256}         (or nack on failure)

config_set   →  app sends   config_begin {total_len, crc32}
                            config_chunk {offset, data_b64}   × N
                            config_end   {sha256}
                fw replies  ack | nack {err, detail}
```

`data_b64` is **base64**, spelled the same way §9.3's `ota_chunk` spells it. A
chunk is JSON, so it cannot carry raw bytes, and the config's payload is the
*UTF-8 JSON text* of §3.7 — base64 rather than a JSON string literal because the
transport must be byte-exact and must not depend on the peer's escaping rules.
`offset` is the byte offset of this chunk's first *decoded* byte, so a gap or an
overlap is detectable rather than silently concatenated.

Each chunk is sized so the **encoded frame** — envelope, offset, and the base64
expansion of the payload — stays under the 1024-byte line cap. Base64 costs 4
bytes per 3, so the decoded chunk payload is **512 bytes**, giving a frame of
roughly 750 bytes. Sizing the chunk by the *decoded* length and then discovering
the encoded frame is too long is how a chunked transport ends up unable to send
its own chunks.

`config_get` is a real frame. **`config_set` is a logical operation, not a frame
type** — it names "replace the whole config", and it is carried entirely by the
`config_begin`/`config_chunk`/`config_end` run above. There is deliberately no
single-frame `config_set`: a real config is ~22 KB and the line cap is 1024 B, so
such a frame could never carry a legal config, and a type that exists only to be
always-rejected is a trap for the app author.

The receiving side **accumulates into a fixed-size staging buffer sized to the
maximum legal config** (a compile-time constant, so the bound is provable, not
hoped for) and rejects a `config_begin` whose `total_len` exceeds it. Nothing is
committed until `config_end` validates the CRC **and** the SHA-256, which keeps
§3.8's "staged, then committed" rule intact across the wire as well as in NVS.
An interrupted run is discarded wholesale — a partial config is never applied.
`config_patch` (single field) stays on one line, which is most of why it exists.

### 4.3 Frame types

| Direction | `type` | Payload | Notes |
| --- | --- | --- | --- |
| FW → App | `hello` | `fw_version`, `hw_id`, `protocol_v`, `caps[]` | Sent on connect and on request |
| FW → App | `event` | `channel`, `button`, `gesture`, `t_ms`, `level_mv` | **The core event.** Fired on every classified gesture |
| FW → App | `status` | `vbus_present`, `gain_mode`, `rail_mv`, `temp_c`, `uptime_ms`, `heap_free`, `config_state` | Periodic + on change |
| FW → App | `ladder_sample` | `channel`, `level_mv`, `n` | Streamed **only during learn mode** |
| FW → App | `ack` | `for_seq`, `ok`, `err` | Every command is acked |
| FW → App | `nack` | `for_seq`, `err`, `detail` | Explicit failure, with a machine-readable code |
| FW → App | `log` | `level`, `msg` | Diagnostic line. `level` is a word (`INFO`/`WARN`), matched by the app like `gesture`. **Not gated by a flag:** the one producer is FR-18's clamp warning, which marks a config value the device refused to drive as written, and a warning the user can switch off is a warning they will never see. The app shows received lines on the link screen, bounded to the newest few |
| FW → App | `link_gap` | `channel`, `button`, `gesture`, `expected_seq`, `got_seq` | An inbound frame's `seq` skipped ahead, so a frame was lost. Fire-and-forget, like `event` |
| App → FW | `config_get` | — | Request the whole config (replied as a chunked run, §4.2) |
| App → FW | `config_begin` / `config_chunk` / `config_end` | total_len+crc32; offset+data; sha256 | The chunked transport that carries **both** `config_get` and `config_set` (§4.2) |
| App → FW | `config_patch` | `path`, `value` | Single-field change, cheaper and less racy — fits one line |
| App → FW | `learn_start` / `learn_stop` | `channel`, `button_id` | Drive the learn wizard (§6.4) |
| App → FW | `learn_commit` | `channel`, `button_id`, `name` | Accept the streamed samples as this button |
| App → FW | `maintenance_enter` / `maintenance_exit` | — | Enter/leave maintenance mode (§8.2) |
| App → FW | `test_key` | `channel`, `key_mv`, `hold_ms` | Bench/production test of the output stage |
| App → FW | `identify` | `pattern` | Flash LEDs / buzz, so the user knows *which* unit |
| App → FW | `reboot` | `boot_target` (`app`/`bootloader`) | |
| App → FW | `ping` | — | Liveness; FW answers `status` |
| App → FW | `time_sync` | `epoch_ms`, `tz_offset_min` | So timestamps and OTA checks are meaningful |
| App → FW | `ota_begin` / `ota_chunk` / `ota_end` | size/sha256; offset+data; — | USB OTA (§9.3) |

`event` is deliberately **fire-and-forget and never acked by the app**: a button
press must not be held hostage to the app being responsive. The firmware acts on
the local binding first and tells the app second (§6.6).

**`level_mv` is the FILTERED level, not a raw conversion, and the field is named
for what it is.** FR-3 requires the classification to run on a noise-filtered
reading (§6.3), so a field called `raw_mv` would label the filtered number as
unfiltered and invite a support conversation about a "raw" value that no longer
exists anywhere in the signal path. An app that wants to show the user a
multimeter-comparable figure must show the same number the device decided on.

**There is no `confidence` field, and its absence is deliberate.** The earlier
revision of this table listed one. `confidence` (§3.4) is a property of a
*learned button* — a 0–100 learn-quality score, earned by the spread of the
samples that were committed at learn time. It is not a property of a
*classification*: the press classifier answers "which window contains this
reading", and the answer is a window index, not a degree of belief. Emitting a
confidence on `event` would therefore require inventing one, and an invented
number is worse than an absent one — the App screen would render a plausible
percentage that corresponds to nothing the device measured. An app that wants
the button's learn quality reads it from the button in the config, which is
where it is actually stored.

**`button` is the learned id** of the classified button (e.g. `"vol_up"`), or
**null** for a press that matched no learned window. The id is what the app's
bindings grid and the ladder view are both keyed by, so sending an index would
force every consumer to re-derive the mapping.

**An unrecognised press IS reported, with `button: null` and `gesture: "NONE"`**
(FR-12, §6.3, §7.2). The device is not guessing — it is saying "a press happened
and I did not recognise the level", which is the most useful thing it can tell
the app and the only way a user can diagnose a mis-learned button from the app.
Nothing acts on it: no binding can name a button that does not exist, so the
firmware emits the event and plays `KEY_UNKNOWN`, and the app's live view shows a
reading that matched nothing. Suppressing the frame instead would leave the user
staring at a live ladder that never moves while the device is in fact seeing every
press, which is the opposite of the screen's diagnostic purpose.

### 4.4 Keepalive, disconnect and reconnect

The head unit may sleep, suspend, or reboot at any moment, and it supplies power.

- Firmware sends `status` every **2 s** when connected.
- App sends `ping` if it has seen nothing for **5 s**; firmware replies `status`.
- After **10 s** of silence the firmware considers the link down. **This does not
  change key behavior** — bindings continue to work with no app present (§6.6).
- **Reconnect is stateless.** On a new `hello`, no replay of missed events; the
  app re-reads `status` and `config_get` if it needs to. Trying to replay events
  across a USB re-enumeration is a source of duplicate key actions, and is not
  worth the complexity.
- The firmware must tolerate **USB re-enumeration caused by its own reset or by
  a flashing operation** without wedging. On the bench, USB-Serial-JTAG stays up
  across an app crash; the CDC link does not, by design.

### 4.5 Version negotiation

`hello` carries `protocol_v`. If the app's major protocol version differs, the
app **must** show an explicit "firmware/app version mismatch" state rather than
attempting to talk. Silent partial compatibility is how a config gets corrupted.

### 4.6 Why not something else

| Rejected | Why |
| --- | --- |
| Raw binary + COBS | Smaller, but unreadable on a bus analyzer. The payload is tiny; this trades away the main bring-up tool for nothing. |
| JSON over Bluetooth | The car has no BLE by design (§8), and BLE throughput/latency is far worse than wires for the in-car path. BLE is for *maintenance*, not operation. |
| Two CDC interfaces | Android's USB stack on head units is not reliable about multi-interface composite devices; one interface is the safe choice. |
| The app driving the DAC directly | The whole point is that buttons work with **no app running**. The app configures; the firmware acts. |


---

## 5. Functional requirements

Numbered so the plan and the tests can cite them. `MUST` / `SHOULD` / `MAY` are
used in the RFC 2119 sense. Each requirement is stated so that it is
**independently testable**, and §11 maps each to a test.

### 5.1 Acquisition

| # | Requirement |
| --- | --- |
| FR-1 | The firmware MUST sample both ladder channels and the NTC continuously, without blocking the USB link or the output loop. |
| FR-2 | ADC samples MUST be converted to millivolts using **per-chip eFuse calibration**, not a fixed linear scale. |
| FR-3 | The firmware MUST filter samples for noise while preserving a real button press's edge; the filter's settling time MUST be shorter than the configured `debounce_ms`. |
| FR-4 | The firmware MUST detect and report **out-of-range** conditions on a channel (rail collapse, open input, short to 12 V) rather than reporting them as a button. |
| FR-5 | The firmware MUST expose the raw filtered millivolt value during learn mode at a rate the app can render live. |

### 5.2 Classification

| # | Requirement |
| --- | --- |
| FR-6 | The firmware MUST classify each channel's level into `IDLE`, a learned `LadderButton`, or `UNKNOWN`, using the learned windows and **hysteresis**. |
| FR-7 | The firmware MUST detect `SINGLE`, `DOUBLE` and `LONG` presses per channel, using timings from `DeviceSettings`. |
| FR-8 | The state machine MUST be deterministic and driven by an injectable clock, so its full behavior is unit-testable with no hardware. |
| FR-9 | Simultaneous presses on the two channels MUST each be classified independently; neither may block the other. |
| FR-10 | `LONG` MUST fire **at the moment the long-press threshold elapses**, not on release, so a held button acts immediately. |
| FR-11 | A `LONG` press MUST NOT additionally emit a `SINGLE`, and the second press of a `DOUBLE` MUST NOT emit its own `SINGLE`. |
| FR-12 | The firmware MUST NOT emit a gesture for a button it has not learned; such presses are `UNKNOWN` and reported as `event` with `button: null`. |

### 5.3 Output

| # | Requirement |
| --- | --- |
| FR-13 | Before serving any user input, the firmware MUST establish a **safe idle output** — see §6.7. This is the first thing that happens after boot, ahead of USB, BLE or WiFi. |
| FR-14 | The firmware MUST select gain mode per channel from `gain_policy` (§6.2), defaulting to `AUTO` (measure the head unit, decide). |
| FR-15 | The firmware MUST command a key value by writing the DAC code for the bound action **once the gesture has resolved**, and MUST hold it for the press duration (`send_duration_ms`), then release. The output MUST NOT be driven while the gesture is still undecided (§6.6). |
| FR-16 | The firmware MUST implement release as "command above the head unit's idle voltage", which turns the sink FET off and returns the line to high impedance. |
| FR-17 | The firmware MUST apply temperature compensation to the learned windows when `temp_comp_enabled` is set (§6.4). **NOT IMPLEMENTED in v1** — the learned temperature and the enable flag are recorded, the correction is not; see §6.4 and open item N-9. |
| FR-18 | The firmware MUST validate any DAC code against the current gain mode's ceiling before writing it, and clamp with a logged warning rather than driving an out-of-range value. |
| FR-19 | The firmware SHOULD run a bounded software trim loop against the sense readings to correct for servo and resistor tolerance, and MUST NOT oscillate or inject ADC noise into the output (§6.5). |

### 5.4 Feedback

| # | Requirement |
| --- | --- |
| FR-20 | The firmware MUST drive the buzzer and both LEDs to communicate device state per the grammar in §7. |
| FR-21 | Buzzer/LED feedback MUST NEVER be able to block or delay a key press; feedback is scheduled, not synchronous. |
| FR-22 | `buzzer_level` and `led_level` MUST allow the user to silence or dim feedback, including a fully-off setting. |

### 5.5 Configuration

| # | Requirement |
| --- | --- |
| FR-23 | The firmware MUST persist the full `Config` in NVS, atomically, with a checksum. |
| FR-24 | On a checksum failure or an unreadable/newer config, the firmware MUST fall back to defaults and signal it audibly, never partially apply. |
| FR-25 | With **no** configuration, the device MUST function as a transparent 1:1 ladder pass-through (§6.9). |
| FR-26 | Config changes over USB MUST be validated before commit; an invalid config is rejected with a `nack`, leaving the previous config intact. |
| FR-27 | The firmware MUST export and import the entire config as a single JSON document for backup. |

### 5.6 Learning

| # | Requirement |
| --- | --- |
| FR-28 | The firmware MUST provide a learn mode that measures and records a ladder button's level, tolerance and rail voltage, driven over USB and optionally locally via the AUX inputs (§2.2). |
| FR-29 | Learn MUST reject a sample set that is too noisy, or that lands within the tolerance of an existing button, and say why. |
| FR-30 | Learn MUST record `learned_at_rail_mv` — the **+3V3** rail measured during learn (§6.3) — so runtime classification can renormalize if that rail moves. Classification itself runs on the normalized ratio `n` (§6.3), which is already rail-invariant; this field exists so the app can display absolute millivolts and so a genuine 3V3 fault (a sagging regulator, not an alternator) is detectable. |
| FR-31 | The firmware MUST be able to learn with **no app connected**, using AUX1 as the select button plus buzzer/LED prompts (§7.4), because a user may not have the head unit out of the dash. |

### 5.7 Maintenance

| # | Requirement |
| --- | --- |
| FR-32 | The firmware MUST NOT initialize WiFi or BLE during normal operation. They are **maintenance-only** (§8.1). |
| FR-33 | The firmware MUST enter maintenance mode on an explicit request: a config flag, a USB command, or a sustained AUX1 hold (§8.2). |
| FR-34 | In maintenance mode the firmware MUST expose BLE provisioning compatible with the Espressif provisioning app, and serve the minimal web page of §8. |
| FR-35 | The firmware MUST support firmware update over **WiFi** and over **USB**, both from a user-supplied file and by checking the project's git releases (§9). |
| FR-36 | Any update path MUST verify a **SHA-256** checksum before committing, and MUST refuse an unverifiable image. |
| FR-37 | Any update path MUST use A/B partitions with rollback, and MUST mark the new image valid only after the application has reached a confirmed-healthy state (§9.4). |
| FR-38 | Maintenance mode MUST time out and return to normal operation, so the device cannot be left unable to serve button presses. |

### 5.8 Safety

| # | Requirement |
| --- | --- |
| FR-39 | The KEY line MUST NEVER be left driving a phantom button press. On any reset, fault, brownout or disconnect, the output MUST return to the safe idle state of §6.7. |
| FR-40 | The firmware MUST be resilient to a watchdog reset: the post-reset state MUST be safe **before** it is useful. |
| FR-41 | The firmware MUST NOT brick on a failed OTA; a bad image MUST roll back automatically. |
| FR-42 | The firmware MUST continue to serve button presses with the USB link down, the app absent, and no WiFi. |

---

## 6. Behavior specification

### 6.1 Startup sequence (order is normative)

This order is a safety requirement, not a style choice. The DAC's power-on state
is the only thing standing between the user and a stuck key while the firmware
boots — see §6.7.

```
 1. Reset vector → ROM bootloader
 2. MCP4728 EEPROM powers the DAC up in its SAFE state      (hardware, no firmware needed)
 3. App starts; earliest code:
      a. configure the DAC pins, drive ~LDAC appropriately
      b. VERIFY the DAC is in the safe state (read back)     ← FR-13
 4. Configure the KEY-line sink FET gate to OFF (Q4/Q6)
 5. Bring up the ADC, calibrate it from eFuse, begin sampling
 6. Load Config from NVS; validate; fall back to defaults on any failure
 7. Measure the rail and the head unit's idle level → decide gain mode
 8. Start the classification state machine
 9. ONLY NOW: bring up USB CDC, and announce `hello`
10. Stay in normal mode. Do NOT touch WiFi/BLE.
```

**Steps 3–4 happen before step 9 on purpose.** If USB came up first, there is a
window where the device is addressable but the output is not yet safe.

### 6.2 Gain mode selection

Gain mode is chosen from `gain_policy`: `AUTO` (default), `FORCE_5V` (gain 1.82),
or `FORCE_3V` (gain 1.00).

**The exact gain is 1.82, not 1.812** — `R58/R61 = 82 k/100 k = 0.82`, so
`1 + 0.82 = 1.82`. The firmware must use the ratio, not a rounded decimal:

```
V_DAC_setpoint = (V_KEY_target + 0.82 · V_ADJ) / 1.82
DAC code       = round(V_DAC_setpoint · 4096 / 3.300)
```

`AUTO` measurement, with the output released (FET off, line floating):

1. Sample `/SENSEn` while idle. The divider is an exact ÷2, so
   `V_KEY_idle = 2 × V_SENSE`.
2. **Envelope check.** `V_KEY_idle` outside **1.80–5.20 V** means **no head unit**
   (off, absent, or a wiring fault) → take the `NoHeadUnit` path (§6.8). Do not
   classify against a stale measurement.
3. **Guard band.** If `V_KEY_idle` falls in **2.6–3.4 V**, **do not guess** — the
   two ranges are indistinguishable there. Stay on the current mode and re-measure
   after the head unit has settled.
4. `V_KEY_idle ≥ 3.4 V` → **5 V range**, gain 1.82.
5. `V_KEY_idle < 2.6 V` → **3 V range**, gain 1.00, `V_ADJ` tracking the signal
   channel's code.

**The asymmetry is deliberate, and it is the safety argument:** the only
dangerous mistake is **over-ranging a 3 V head unit**, because gain 1.82 on a
3 V system can command above its rails. Under-ranging a 5 V unit merely wastes
dynamic range. Therefore:

- **Default to gain 1.82** whenever the measurement is absent or ambiguous. It
  works for 3–5 V units at a cost of only ~2.5 mV of DAC-referred error.
- **Upgrade to gain 1.00 only on positive evidence** (`V_KEY_idle < 2.6 V`).

`V_KEY_idle` is **the level the head unit pulls its own KEY line to with no
button pressed** — the single most important measured number in the system, and
exactly what the 2022 design never did.

**Gain mode is re-evaluated, not latched:** on head-unit power change
(`/VBUS_VALID` transitions, rail changes) and periodically while idle.

**Command targets must stay inside `[min_ladder, V_KEY_idle − 0.20 V]`** so the
sink FET is never asked to drive above the line's own resting level — above that
point the servo can only turn `Q4` off, which is the release behavior, not a
command.

### 6.3 The ladder transfer function

This derivation is what justifies "learn, do not assume". Per channel — and note
the topology, which §2.4 establishes and which an earlier revision of this section
got backwards:

```
   +3V3 ── R15/R16 10k (pull-up) ──┬── J2 connector node ── R1/R2 10k ── ADC pin
                                   │          │                            │
                                   │   ladder (vehicle-specific)      C3/C4 100nF
                                   │          │                      D4/D5 clamp
                                   │     common → GND                  (protection)
```

**The ladder's common is tied to GND, and a button shunts the node it sits at to
that common.** So the **connector node** is a divider between **+3V3 (through
`R_pullup`)** and **GND (through the ladder's resistance to common)**:

```
V_pin = 3.3 · R_ladder / (R_ladder + R_pullup)
```

**`R1`/`R2` do not appear in this expression, and that is not an oversight.**
Verified against the netlist: `R1` runs from the connector node to the ADC pin,
and the ADC input is high-impedance, so `R1` carries no DC current and drops no
DC voltage. `V_pin` = `V_connector`. `R1` and `C3` form the **RC anti-alias
filter**; `R1`'s job is to bound the current into the clamp and give `C3`
something to work against, not to divide. (An earlier revision of this section
included `R_series` in the divider, which double-counted it.)

Idle — no button — is the **maximum** `R_ladder` (the whole chain), so **idle is
the high level**, and the pin never approaches the +12 V rail or the ADC's 2.9 V
ceiling.

Three consequences that shape the firmware:

1. **`V_pin` depends on the +3V3 rail, not on the vehicle's 12 V rail.** The 12 V
   system still matters indirectly — it is what the steering-pad ladder is
   referenced to in some vehicles, and it sets the head unit's own KEY-line idle
   — but it is **not** the divider's high side here. The rail that matters for
   classification is the regulated 3V3, which is far more stable than an
   alternator. This is a *weaker* rail-dependence than an earlier revision of this
   section claimed, and it is why §2.4's "measure the ladder, do not assume its
   values" is the load-bearing rule.
2. **The mapping is monotonic and compresses toward the *top*.** As `R_ladder`
   falls toward 0 (a button shorting straight to common), `V_pin` falls toward 0.
   Sensitivity `dV/dR = 3.3·R_pullup / (R_ladder + R_pullup)²` is *highest* at low
   `R_ladder` and falls as `R_ladder` grows — so buttons whose ladder resistances
   are large (i.e. near the idle end) produce pin voltages only a few tens of
   millivolts apart, while buttons near common are well separated. This is why
   `mv_tolerance` is derived from the *measured gap* (§3.4), why the ADC's
   accuracy ceiling (§6.5) matters, and why the bring-up measurement (§10.6) must
   check that the **idle-adjacent** buttons are still resolvable.
3. **The pull-up and the ladder values must be chosen together.** `R_pullup` is
   10 kΩ as built, but **whether that spreads the buttons adequately across the
   ADC's range depends on the ladder's actual resistances**, which are vehicle-
   specific and are *measured* at bring-up (§10.6), not assumed. If the measured
   spread is too small — most likely among the idle-adjacent buttons, per
   consequence 2 — the fix is to change `R15`/`R16`. This is a rework item, and
   the reason this measurement is early in the bring-up order.
4. **There is also a *ceiling* constraint on `R_ladder`, and it is the one that
   bites first.** Idle is the high end of the divider, so it is the *worst case*
   for the ADC's 2.9 V calibrated limit:

   ```
   V_idle = 3.3 · R_ladder_idle / (R_ladder_idle + R_pullup)  ≤  2.9 V
   ⇒  R_ladder_idle  ≤  R_pullup · 7.25
   ```

   With `R_pullup` = 10 kΩ that means **the idle ladder resistance must be
   ≤ ~72.5 kΩ**, or idle reads *at* the 2.9 V ceiling and the whole range is
   clipped from the top — silently, since a clamped idle looks like a valid
   reading. A steering-pad ladder built from 10 kΩ steps (say 8 buttons → ~80 kΩ
   idle) **would sit right on that edge.** So the bring-up measurement (§10.6)
   must record the idle resistance and confirm it is comfortably under the bound;
   if it is not, `R15`/`R16` get **smaller**, not larger. This is the opposite of
   the naive "raise the pull-up for more separation" instinct and is exactly why
   the measurement gates the decision.
   `MilliVolt`'s declared range (§3.2, 0–2900) is the ceiling this expresses.

> **Two corrections to `DESIGN.md` §4.1, both load-bearing.** That prose says the
> 10 kΩ is a pull-up *to 12 V* (the netlist shows `R15`/`R16` go to **+3V3**), and
> it says the ladder is *pulled to 12 V when idle* (it is a series chain whose
> **common is tied to GND**, so idle is simply the high end of the divider). The
> first was already noted here; the second is the more serious, because it inverts
> which end of the range a press moves toward.

**The decode still uses a normalized ratio**, which is what makes it robust to the
3V3 rail's own tolerance and to any residual series-resistance variation:

```
n = V_ADC / V_ADC_idle          measured now
n_learned = V_learned / V_learned_at_idle   recorded at learn time
match button k  ⟺  |n − n_learned[k]| < tolerance_n[k]
```

Because both the numerator and denominator scale with the 3V3 rail, **`n` is
invariant to that rail.** This removes the class of "works at idle, drifts as the
rail moves" bugs and means runtime classification needs no separate renormalization
step.

Absolute millivolts are still stored and displayed (§3.2) because they are what a
human compares against a datasheet — but **classification runs on `n`.**

`AUTO` gain mode plus the measured `V_KEY_idle` is the mechanism; a fixed voltage
table is the anti-pattern.

### 6.4 Temperature compensation

Two independent effects, and the firmware must not conflate them:

- **The ladder itself drifts.** Vehicle switch contacts and any series elements
  change with temperature. This is a real but *second-order* effect, and it is
  what `RT1` is intended to compensate.
- **The ADC and its reference drift.** The S3's ADC has a temperature
  coefficient; eFuse calibration is done at a nominal temperature.

**Honest position:** the size and sign of the ladder's own drift is not known for
this vehicle and cannot be known until the board is on a car in real temperature
conditions. Therefore:

- `temp_c_at_learn` is recorded per button (§3.4) and `temp_comp_enabled` is
  carried through the config, so a correction is **computable** later.
- **No correction is implemented in v1, and this is stated plainly rather than
  claimed as a zero-valued one.** There is no coefficient field, no correction
  function, and no test of one; the two recorded halves are the inputs such a
  function would consume. An earlier revision of this paragraph said the
  correction was "present, wired, and recorded" and "testable", which was not
  true, and it mattered -- that sentence is exactly what would have satisfied
  FR-17 on review. See open item N-9.
- The NTC is read and reported regardless, so a bring-up session can *measure*
  the drift and set a coefficient, rather than guessing one now.

FR-17 is therefore **not satisfied** and is recorded as such, rather than
papered over with a made-up coefficient or a claim of a path that does not
exist.

### 6.5 The servo and the software trim loop

The output stage is **already a closed-loop servo in hardware** (§2.3). The
firmware's job is to command a target, not to re-implement the loop.

**Verified numbers, from the netlist:**

| Quantity | Value | Derivation |
| --- | --- | --- |
| Integrator time constant | **~10 ms** | `R46` 100 k × `C24` 100 nF |
| Analog loop bandwidth | **~16 Hz** | `1/(2π·10 ms)` |
| Analog settling | **tens of ms** | several time constants |
| S3 ADC linear range at 12 dB | **0–2.9 V** | S3 ceiling — see below |
| S3 ADC INL / DNL | **±8 / ±4 LSB** | Espressif's own comparison data |
| Post-calibration full-scale error | **−30…0 mV** | after curve-fitting calibration |

**ADC calibration is mandatory and per-chip.** Use the **curve-fitting** calibrated
scheme (`adc_cali_create_scheme_curve_fitting()`, read via
`adc_cali_raw_to_voltage()`), which is **factory eFuse-backed and unique to each
chip**. Never compute `raw × 3300 / 4095` — that ignores both the eFuse
correction and the S3's 2.9 V ceiling, and would be wrong by hundreds of
millivolts at the top of the range.

`adc_cali_create_scheme_curve_fitting()` returns `ESP_ERR_NOT_SUPPORTED` if the
eFuses are missing (e.g. some third-party module batches). **The firmware must
handle that case explicitly** — fall back to a documented linear approximation
*and report that it did*, rather than silently mis-scaling every reading.

The board is designed so the ADC never saturates (§2.3), so the top-of-range
non-linearity is never reached in normal operation.

**The trim loop is a supervisor, not a controller.** This is stated as a
prohibition because the obvious wrong design is attractive and dangerous:

> **Do NOT run a fast software PI loop on `/SENSEn` around the hardware
> integrator.** Two integrators in one loop, plus ADC noise injected through the
> second, is an oscillator. The hardware loop already regulates; a software loop
> of comparable speed fights it.

The correct design:

| Property | Value | Why |
| --- | --- | --- |
| Function | **Open-loop DAC code is the primary command** | The servo already regulates |
| Trim rate | **1–2 Hz** | Two decades below the 16 Hz loop — comfortably stable |
| Trim step | **±1 LSB per update** | Moves nothing fast enough to ring the loop |
| Deadband | **±3 LSB** | Absorbs ADC noise; without it the trim jitters the line |
| When it runs | **Only in a steady commanded state** | Never during a transition |
| Total authority | **Bounded cap** on deviation from the open-loop value | A wiring fault cannot drive the output to an extreme |
| Sense sample rate | **≤ 100 Hz**, 16–64 oversamples averaged | Keeps sampling far below the loop bandwidth |

The trim's purpose is to null *static* error — DAC offset and gain error,
`R58`/`R61` tolerance, `R36` leakage — not to track dynamics.

**Setpoint ramping.** Slew the DAC code at roughly **1–2 V/ms** rather than
stepping. A stepped command can saturate the integrator, which then has to unwind;
that adds tens of milliseconds of delay and overshoots the KEY line.

**Hold time.** Hold the settled code for the head unit's recognition time —
**≥ 100–200 ms**; the 2022 design's `KEY_SEND_DURATION_MS = 200` is a sound
starting point — and confirm the line has returned toward idle before the next
command.

**Stability caveat for bring-up.** The dominant *unknown* plant pole is the head
unit's own pull-up resistance (unknown, plausibly 1 k–100 k) times the harness
capacitance (100 pF–10 nF), which lands in the tens of kHz — well above the 16 Hz
loop, so the analog loop is stable as built. But **a long harness can slow it and
ring.** Do not shrink `C24` without measuring, and bench-verify overshoot and
settling on real hardware (§10).

**v1 posture, given no board yet:** open-loop command with the trim loop present
but **disabled by default** until its gain is measured on hardware. This satisfies
FR-19 (the path exists, is bounded, and is testable) without shipping a loop tuned
against a guess.

### 6.6 The press → action path (normative ordering)

```
  raw samples ─► filter ─► classify ─► gesture ─┬─► LOCAL action (binding)  ── FIRST, always
                                                └─► USB `event` report       ── SECOND, best-effort
```

**The head unit is gesture-blind, and that is the whole reason this device
exists.** Its SWC input reads a voltage and recognizes key windows. It has no
concept of a "double press" or a "long press" — those are *ours*, computed from
the ladder switch being closed for a particular duration. So the adapter's job is
not to forward a press; it is to **convert a gesture into one clean, gesture-blind
key event**:

| wheel input (the head unit cannot see it) | what the head unit receives |
| --- | --- |
| tap | one pulse at key A's voltage |
| double tap | one pulse at key **B**'s voltage — a *different* command |
| hold | one pulse at key **C**'s voltage, or a USB command to the app (§3.6) |

Three rules follow, and they are normative:

1. **The output is not driven until the gesture resolves.** Because a button's
   `DOUBLE` and `LONG` bindings normally name *different* voltages, driving key A
   at press time and key B at resolution would make the head unit act twice — a
   phantom press, which is the hazard §6.7 exists to prevent. The delay is not a
   latency compromise; it is what makes the conversion unambiguous.
2. **Every drive is a bounded pulse**, held for `send_duration_ms` and then
   released. A gesture becomes *one key event*, never a held line. `LONG` is
   **not** a held key: a long press is only the ladder switch closed past the
   threshold, and what the head unit is given is a single pulse of the level that
   the `LONG` binding names.
3. **How long the resolve takes is per button, and is derived from that button's
   own bindings** — the adaptive rule:

   | the button binds | the wait before driving |
   | --- | --- |
   | neither `DOUBLE` nor `LONG` | none — drive as soon as the press is classified |
   | `LONG` only | until release, or until the long threshold passes |
   | `DOUBLE` (with or without `LONG`) | until the double-press window closes after release |

   A button that binds only `SINGLE` has no ambiguity to resolve and must not
   inherit the double-press window's latency. This is a per-button property, not a
   per-device one.

`SINGLE` is therefore not "delayed by design" — it is delayed by exactly the
ambiguity that its button actually has, and no more.

**The local action runs first and unconditionally.** The app is an *enhancer*,
not a dependency. If the USB link is down, the app has crashed, or the head unit
is rebooting, every `OUT_VOLTAGE` binding still works. This is FR-42, and it is the
difference between a product and a toy.

`APP_*` actions are reported to the app and are the app's business; their failure
is reported back as a `nack`/`event` outcome but never blocks an `OUT_VOLTAGE`.

### 6.7 The safe idle state — the central safety property

**Statement of the hazard:** `Q4` sinks. If the DAC commands a low code, the
servo drives the KEY line low, which the head unit reads as a button held down —
possibly *forever*, from the driver's point of view, with the radio doing
something the driver cannot stop. During boot, a reset, or a fault, the DAC's
state is not under firmware control.

**The mitigation is three-layered:**

1. **Hardware default (primary).** `U4`'s EEPROM is programmed so channel A/C
   (signal) power up at **full scale** and channel B/D (gain) power up
   **powered down**. Full-scale signal with the 1 kΩ gain pulldown is a command
   *above* the head unit's idle voltage → the servo drives the gate low, `Q4`
   turns off, and the line floats. **A dead firmware is a safe firmware.**
   This is why §3.7's default `idle_dac_code` is `4095`, not `0`.
2. **Boot ordering (§6.1).** The firmware verifies the safe state before it
   brings up anything that could accept a command.
3. **Firmware discipline.** Every fault path — watchdog, brownout handler, USB
   disconnect, config corruption, update failure — returns to the safe idle state
   as its first action.

**This inverts the usual intuition and must not be "fixed" later:** *idle is a
high command, not a low one,* because the output only sinks.

### 6.8 Failure behavior matrix

| Fault | Detection | Response |
| --- | --- | --- |
| Watchdog reset | Boot reason from RTC | Safe idle FIRST (§6.1), then resume; log and buzz the reset reason |
| Brownout | Boot reason | Safe idle; hold off enabling the output until the rail is stable; log |
| Config corrupt / bad checksum | Read-time checksum | Defaults; **loud** buzzer pattern; report `config_state: defaults` over USB |
| Config from newer schema | `schema_version` check | Defaults; report clearly, do not attempt to interpret |
| Head unit disappears (VBUS off) | `/VBUS_VALID` | Release the KEY line immediately; report `vbus_present: false`; keep classifying |
| USB link drops | 10 s silence | Keep serving local bindings; report nothing (there is no one to report to) |
| Ladder out of range | Range check | Report `UNKNOWN`, emit no gesture, do not guess. A reading **above the idle reference** is the out-of-range case (a short to 12 V): the channel goes `kFault`, the KEY line releases, and `LED_STAT` latches `blink` (FR-4). The indication is LED-only — no `FAULT_*` pattern names this subsystem (N-10) |
| Rail collapse | Rail measurement | Report out-of-range; do not classify against a stale rail |
| OTA image bad | SHA-256 mismatch | **Do not commit**; keep running the current image; report the failure |
| OTA image boots then faults | No health confirmation | Roll back to the previous slot (§9.4) |
| I²C to DAC fails | NACK / timeout | Retry with backoff; if persistent, release the line and report a fault; **never drive a guessed code** |

### 6.9 Default behavior — the transparent pass-through

FR-25 exists so the device is useful before it is configured, and so a
config-loss event degrades to "the steering wheel works like stock" rather than
"the steering wheel does nothing".

With no config there are **no learned windows to classify against**, so the
pass-through cannot be "present the same key value" — there is no table mapping a
level to a key. What it can do, and does, is **map the incoming RATIO onto the
head unit's own idle**:

```
wheel_ratio   = level_mv / wheel_idle_mv          (the wheel's live idle)
output_key_mv = head_unit_idle_mv · wheel_ratio   (spec 6.2 step 1's measurement)
```

This is the same normalization §6.3 already uses for the configured case, so a
press maps to the *same key* on the head unit even though **the wheel's ladder and
the head unit's ladder need not have the same resistances** — which is the whole
reason not to copy the incoming millivolts across. The two idles are
deliberately distinct quantities: the wheel's idle is the ratio's denominator, the
head unit's idle is what the ratio is applied to. Using the output's safe-idle
level (~5200 mV, full scale) as the denominator would scale every press against
the wrong range and push low buttons into the 1.80 V output floor, i.e. onto a
different key.

A press is "clearly off idle" (`> 300 mV` from the wheel's idle), because with no
config there is no learned window to compare against. Feedback patterns are not
played: there is nothing configured to resolve against.

The user gets a working steering wheel immediately, and the app is an *upgrade*,
not a prerequisite. This is also the fallback if learning was never done.

**With no usable ladder reference, pass-through is DISABLED rather than guessed**
— a fabricated denominator would map every press to a voltage nothing defined.
FR-13 is unaffected: this changes *what* is served, never *when* the output
becomes safe.


---

## 7. User feedback: the buzzer and LED grammar

Two LEDs and a buzzer are the only feedback on the board, and they carry the
whole no-app experience — including the programming UX the user wants carried
forward. The grammar must therefore be **learnable and unambiguous**, not
decorative.

### 7.1 What the hardware can actually do

| Output | Drive | Capability |
| --- | --- | --- |
| `BZ1` buzzer | `Q3` low-side FET from `/BUZZ` (IO13) | **On/off gating only.** |
| `D6` `/LED_STAT` (IO47) | `R7` 1 k, active high | On/off. Software-PWM possible. |
| `D12` `/LED2` (IO14) | `R26` 1 k, active high | On/off. Software-PWM possible. |

**This is a capability reduction from the 2022 design**, and two facts make it
definitive rather than provisional:

- `BZ1` is a **Huaneng `TMB12A05` — an *active*, self-driving electromagnetic
  buzzer with a built-in oscillator at a fixed ~2.4 kHz.** The tone is set by the
  part. The old code's `pwm_set_freq_khz()` melody grammar **cannot be
  reproduced**; gating the supply is the entire vocabulary. (An active buzzer
  driven at a fixed DC level is also why a PWM duty-based "volume" control is not
  meaningful in the way the old code assumed.)
- **Both LEDs are green 0805 (`GREEN`)**, so a *colour* grammar is impossible.
  The LEDs must be distinguished by **blink pattern and position**, not hue.

Accordingly the grammar is built from **rhythm on one fixed tone**, plus two
independently-blinking LEDs. This is a genuinely different design constraint, and
it is stated up front so the implementation does not fight it. It is also a more
legible grammar than pitch — beep *count* and *pattern* survive road noise and a
driver's divided attention better than a melody does.

### 7.2 The buzzer grammar — rhythm as the language

Patterns are named, not ad-hoc, so they can be referenced from config and tested.

```
pattern   := pulse("on_ms", "off_ms") , repeat , gap_ms
```

| Pattern | On/off (ms) | Reps | Meaning |
| --- | --- | --- | --- |
| `BOOT_OK` | 60/60 | 1 | Power-on self-test passed |
| `BOOT_DEGRADED` | 60/60 | 3 | Booted but with a fault (see `FAULT_*`) |
| `BOOT_ERROR` | 500/200 | 2 | Cannot serve output; needs attention |
| `KEY_ACCEPTED` | 25/0 | 1 | A gesture was recognised and an action taken |
| `KEY_UNKNOWN` | 120/80 | 1 | A press was seen but not recognised (unlearned) |
| `PROGRAM_ENTER` | 40/40 | 2 | Entering programming mode |
| `PROGRAM_STEP` | 40/40 | 1 | **One** step deeper into a menu — see below |
| `PROGRAM_SAVED` | 40/20 | 4 | Setting stored |
| `PROGRAM_EXIT` | 200/0 | 1 | Programming finished |
| `PROGRAM_CANCEL` | 300/100 | 1 | Programming abandoned, nothing saved |
| `LEARN_PROMPT` | 100/100 | 1 | Waiting for the user to press a button |
| `LEARN_OK` | 40/30 | 2 | That button learned and accepted |
| `LEARN_REJECT` | 300/80 | 2 | Sample rejected (§7.4) — will be repeated |
| `FAULT_DAC` | 500/300 | 3 | I²C/DAC fault |
| `FAULT_CONFIG` | 500/300 | 4 | Config corrupt; defaults loaded |
| `FACTORY_RESET` | 800/200 | 3 | Everything erased |
| `OTA_START` / `OTA_OK` / `OTA_FAIL` | 400/0 · 150/100 · 80/40 | 1 · 2 · 3 | Long single / double / harsh triple |

The OTA row is the only one with no numbers in the 2022 table, and its prose says
"rising double" — **which this hardware cannot produce.** §7.1 fixes the buzzer's
tone at ~2.4 kHz with on/off gating only, so a *rising* interval is not
representable. `OTA_OK` is therefore a plain double with a longer second pulse
(150/100 ×2), which is what "rising" can mean once pitch is unavailable: the
pulses grow in *length*, not in frequency.

**Design rules:**

- **`PROGRAM_STEP` is one rep, and callers repeat it.** §7.4's fallback menu
  announces the *n*-th button as "*n* beeps", which reads as a conflict with this
  table's `Reps = 1`. It is not: the pattern is defined as a single 40/40 pulse,
  and the caller emits it *n* times with the pattern's own `gap_ms` between
  repeats. Defining the pattern with `Reps = n` would make the rep count a
  runtime parameter of a pattern, which the grammar
  (`pattern := pulse(on, off), repeat, gap`) does not express — `repeat` is a
  property of the named pattern, not an argument. The same rule covers
  `PROGRAM_ENTER`'s 2 reps, which are baked in and not caller-varied.
- **`KEY_ACCEPTED` is deliberately the quietest and shortest.** The user hears it
  hundreds of times a drive; the diagnostic patterns are long and loud so they
  are unmistakable and rare. If feedback were uniform, a fault would be
  indistinguishable from normal operation.
- **No routine pattern may exceed ~2 s**, because the buzzer is non-blocking
  (FR-21) and a long pattern would still be playing over a subsequent event.
  **The fatal patterns are the stated exception, bounded at ~3 s instead**:
  `FAULT_DAC` (2.1 s), `FAULT_CONFIG` (2.9 s) and `FACTORY_RESET` (2.8 s) exceed
  2 s as the table above defines them. That is deliberate and it is the same
  argument as `KEY_ACCEPTED`'s, read the other way: these three must be
  *unmistakable*, and they are rare enough that nothing follows them to be
  delayed. A 2 s rule applied to them would shorten the one pattern the user
  most needs to hear. The bound is stated as ~3 s so the table has a check it
  actually passes.

- `buzzer_level` (`OFF` / `QUIET` / `NORMAL` / `LOUD`) scales duty or suppresses
  entire classes: `OFF` silences everything except `BOOT_ERROR` and `FAULT_*`.

### 7.3 The LED grammar — state, not events

LEDs are **continuously readable state**, complementing the buzzer's **transient
events**. They must answer "is this thing OK?" at a glance from the driver's seat.

The orchestrator owns this channel. It sets **breathing** at boot and **solid**
when a host opens the USB port (`SetUsbConnected`), and the learn wizard and the
identify flash borrow it while they run. **A fault latches `blink` and wins over
both normal states** — a device that is faulted is not OK at a glance, whatever
the link is doing, and a connect mid-fault must not repaint the lamp green.
`blink` clears only on a reboot, because a wiring fault or a collapsed rail is a
hardware condition that does not fix itself.

| `LED_STAT` (D6) | Meaning |
| --- | --- |
| Off | No power / not running |
| **Solid** | Running, output safe, USB connected, config valid — the all-good state |
| Slow breathe (1 Hz) | Running normally, **no USB** (app not connected) |
| Fast blink (5 Hz) | **Fault** — see the buzzer `FAULT_*` for which |
| Double-flash burst | Maintenance mode active (BLE/WiFi) |
| Alternating with LED2 | Learn mode active, awaiting a press |

| `LED2` (D12) | Meaning |
| --- | --- |
| Off | Idle, no recent key activity |
| Flick on gesture | A gesture was recognised (mirrors `KEY_ACCEPTED`) |
| Solid | A key value is currently being presented (**the line is driven**) |
| Long pulse (0.5 s) | Gain mode changed, or the head unit was (re)detected |

`LED2`'s "solid while driving" state is a genuine diagnostic: the user can see
that the adapter is holding a key, which distinguishes "the adapter is doing
something wrong" from "the head unit is ignoring it".

Both LEDs are the same colour (green, §7.1), so **nothing above relies on hue** —
`LED_STAT` is the *state* channel and `LED2` is the *activity* channel, and their
patterns are distinct by rate and rhythm.

### 7.4 The learn wizard

Teachable without a phone, because the user may not have the head unit out of the
dash. Driven either by the Android app (primary, with live graphing) or by
AUX1 + buzzer/LED prompts (fallback, FR-31).

**Per-button learn loop, with the fallback prompts:**

```
 1. Enter learn      BEEP PROGRAM_ENTER · LED_STAT alternate, LED2 off
 2. Select button    BEEP PROGRAM_STEP (n beeps = nth button) · user presses AUX1 n times
 3. Prompt           BEEP LEARN_PROMPT · LEDs alternate
 4. User holds the physical steering-wheel button
 5. Sample           ≥ N samples over ≥ T ms; compute mean, spread, rail voltage
 6. Validate         ┌ reject & beep LEARN_REJECT, return to 3 if:
                     │   · spread > noise_limit            (too noisy)
                     │   · level within tolerance of an existing button (ambiguous)
                     │   · level at/near idle              (button not actually pressed)
                     │   · out of ADC range                (fault)
                     └ accept  → BEEP LEARN_OK, store LadderButton
 7. More buttons?    yes → 2 ;  no → 8
 8. Exit             BEEP PROGRAM_EXIT · LED_STAT solid
```

**Rejection reasons are spoken aloud as distinct rhythms**, not a single generic
failure, because "it didn't work" is not actionable and the user is doing this
blind, holding a button with one hand.

**`UNKNOWN` handling (FR-12):** a press that matches no learned window is
reported as `event{button: null}` and beeps `KEY_UNKNOWN`. It is **never** guessed
at — the failure mode of a wrong guess is the radio doing something the driver
did not ask for, which is worse than doing nothing.

### 7.5 The programming UX (carried forward from the 2022 design)

The user's stated good part: *"hold down the button and then double/single/long
press to set a function, buzzer sounds that escalate to indicate modes."*
Preserved as a first-class flow, implemented properly this time (the old
implementation was stubbed — see §2 of the plan).

**Trigger: hold `AUX1` for ≥ 1.5 s.** This is a deliberate change from the
obvious choice of the BOOT button:

- **BOOT (`IO0`) is recessed** behind a Ø5 hole in the lid, and is a *strapping
  pin* (§2.2) — a hold-at-power-on means ROM download mode, not a user action.
- **AUX1 is a real, reachable analog input** on the `J5` terminal with its own
  conditioning (§2.4). The user can wire a momentary button to it and reach it.
- It costs nothing to support the app path in parallel.

```
PROGRAMMING MODE   (trigger: AUX1 held ≥ 1.5 s, or the app requests it)
  1. Enter:      BEEP PROGRAM_ENTER — §7.2's 40/40 ×2, NOT a shave-and-a-haircut
                 LED_STAT does the same 2-pulse cadence
  2. Target:     the user presses the physical button to program
                   SWC1's buttons → 1 beep slot, SWC2's → 2, AUX1–3 → 3/4/5
                   (beep COUNT identifies the slot — there is no pitch)
                 LED2 flick on press so the user sees it register
  3. Gesture:    the user then performs the gesture to bind:
                   single press  → SINGLE   (1 beep)
                   double press  → DOUBLE   (2 beeps)
                   hold ≥ long   → LONG     (3 beeps)
                 ← this escalating count IS the "buzzer sounds that escalate"
  4. Confirm:    BEEP PROGRAM_SAVED — §7.2's 40/20 ×4
  5. More?       return to 2; exit with AUX1 held ≥ 1.5 s again
                 BEEP PROGRAM_EXIT
```

**The 2022 "shave-and-a-haircut" cadence is gone, and the reason is the same one
that removed pitch.** This block previously specified a 3-short-plus-1-long
`PROGRAM_ENTER` and a "2 equal beeps" save confirmation. Neither is in §7.2's
table: `PROGRAM_ENTER` is 40/40 ×2 there, and the save confirmation is
`PROGRAM_SAVED` (40/20 ×4). Two homes for one pattern is how a firmware
implementation and a test come to disagree about what the device plays, so the
table wins — it is the section that exists to define the patterns, and §7.5 is a
description of a *flow* that consumes them.

**Every pattern named in this flow is now a §7.2 row.** That is the check worth
applying to any future edit here: if a step names a pattern, the pattern must
exist in the table above with the timings this step implies.


**The beep count is the menu depth**, which is how a fixed-pitch buzzer conveys
"how deep am I" — the old design used rising pitch for this, and pitch is not
available (§7.1). Counting is arguably clearer under road noise anyway.

The full action library is far too large to cycle through by beeping. **The
on-device flow therefore edits a small, high-value subset** — "present this key
value", "present the neighbouring key's value", "release", "do nothing" — and the
Android app is the interface for the long tail (`APP_INTENT`, `APP_LAUNCH`, …).
This is the honest division of labour: beeps for the three things a driver wants
at the roadside, an app for the rest.


---

## 8. Maintenance mode: BLE provisioning and WiFi

**Governing requirement (user's words):** *"Normal use in the car won't
necessarily be on wifi, but it would be for maintenance/upgrade-type things."*
And: *"use the espressif ble app to configure wifi and other settings so you can
put it on wifi to update firmware."*

That is a precise brief, and it settles the architecture:

### 8.1 WiFi and BLE are not initialized in normal operation

FR-32 is a **hard rule**, and it is the single biggest power, RAM and attack-
surface decision in the firmware:

| | Normal mode | Maintenance mode |
| --- | --- | --- |
| WiFi stack | **Not started** | Started on entry, stopped on exit |
| BLE stack | **Not started** | Started on entry, stopped on exit |
| RAM held for radio | **None** | Whatever the stacks need, transiently |
| RF emissions in the cabin | None | None when idle |
| Attack surface | USB only | USB + BLE + WiFi, time-limited |

This matters because the device sits on a car's electrical system and its only
job is to pass button presses. A WiFi stack that is idle-but-initialized still
costs heap, still can panic, and still has a radio on. Keeping it off is both the
lower-risk and the lower-power choice, and it directly serves the user's "won't
be on wifi in the car" statement.

**Consequence for the flash budget:** the WiFi/BLE **code** still occupies flash
(unless built as a separate maintenance-only image, rejected in §9.6), so the
partition sizing in §9.2 accounts for it even though normal mode never runs it.

### 8.2 Entry and exit

Entry is explicit and multi-modal, so it works with or without the app:

| Trigger | Notes |
| --- | --- |
| USB command `maintenance_enter` | Primary, from the Android app |
| Config flag on next boot | For a user who wants it up immediately after flashing |
| **AUX1 held ≥ 3 s** | The no-app fallback — deliberately longer than the 1.5 s programming hold so the two gestures cannot be confused |
| Reset-reason + no-config | First-ever boot with no config offers provisioning |

The BOOT button is **not** a maintenance trigger: it is recessed behind a Ø5 lid
hole and is a strapping pin, so a hold-at-power-on would mean ROM download mode,
not a user action (§2.2, §7.5). AUX1 is reachable and is not a strapping pin.

The two holdings are deliberately distinct and nested:
**1.5 s = programming**, **3 s = maintenance** — the shorter is a subset of the
longer, so holding too long to program escalates cleanly into maintenance rather
than into an undefined state.

Exit happens on:
- an explicit `maintenance_exit` command,
- **timeout: 5 minutes of inactivity** (FR-38) — a device left unable to serve
  button presses because someone opened a web page is unacceptable,
- successful OTA completion (reboot anyway),
- or an explicit `maintenance_exit` from the console.

**On exit, the radio stacks are fully de-initialized and their memory freed**,
and the device returns to normal mode. `LED_STAT` stops the maintenance
double-flash.

### 8.3 BLE provisioning

**Espressif's unified provisioning with a BLE transport**, which is what makes
the Espressif provisioning app work — the user's explicit request.

| Aspect | Choice |
| --- | --- |
| Component | ESP-IDF `wifi_provisioning` + `protocomm`, BLE transport |
| Host | **NimBLE** rather than Bluedroid — materially smaller flash and RAM, which matters on 4 MB/no-PSRAM (§9.2) |
| Security | **Sec1** with a Proof-of-Possession |
| PoP source | **Derived from the device, shown to the user.** The board has no display and no sticker — see below |
| Device naming | Advertised name includes a short device id, so multiple units are distinguishable |

**The PoP problem, stated honestly.** Sec1/SRP6a needs a secret the user can
supply out-of-band. The board has no screen and no printed label to carry a QR
code, and the enclosure spec explicitly designs the silkscreen *away* in favour
of windows. So a per-device printed PoP is not available.

Options, in order of preference:

1. **Derive a PoP from the chip's MAC and show it in the Android app** once the
   device is connected over USB. The app already has a trusted channel; it can
   read the PoP over USB and display it for entry into the Espressif app. **This
   is the recommended path** and it keeps the secret per-device and non-trivial.
2. **A fixed PoP set at provisioning time.** Simpler, but any device is
   provisionable by anyone in radio range during the window. Acceptable only
   because the maintenance window is 5 minutes and opt-in — but it should be a
   conscious acceptance, not a default.
3. **Sec0 (no security).** Rejected as a default; offered only behind an explicit
   `allow_insecure_provisioning` flag for bench use.

**Config over the provisioning link:** `protocomm` supports **custom endpoints**,
so the same BLE session that receives WiFi credentials can also carry
device-specific data. The spec exposes the maintenance page's API (§8.4) over a
custom endpoint rather than inventing a second BLE protocol. The `Config` blob
itself is small; the full config sync is USB's job (§4), and BLE carries only
what is needed to get onto WiFi and check for updates.

### 8.4 The minimal web page

Served only in maintenance mode. **It is not a config UI** — the Android app
owns configuration. Its entire job is:

```
GET  /                 → one page: device status, WiFi setup, firmware update
GET  /api/status       → fw version, device id, uptime, config state, WiFi state
POST /api/wifi         → SSID + passphrase (the BLE app's job; provided for browsers)
POST /api/ota/upload   → multipart firmware upload (the WiFi OTA path)
POST /api/ota/check    → check git releases for a newer version (§9.5)
POST /api/ota/pull     → download and install a release asset by URL
POST /api/reboot       → reboot into the new image
```

**Security:** the maintenance page is reachable on whatever network the device
joins, so it is **not unauthenticated**. It requires a token — derived and
displayed the same way as the BLE PoP — and the page is served over the device's
own AP **until** it joins a network, then on the joined network with the token
required. No default password, no "admin/admin".

### 8.5 The two maintenance entry points, compared

| | BLE provisioning app | Web page |
| --- | --- | --- |
| Gets the device onto WiFi | **Yes — this is its purpose** | Possible, secondary |
| Firmware upload from a file | No | **Yes** |
| Firmware pull from git releases | No | **Yes** |
| Works with no WiFi yet | **Yes** | Yes (device AP) |
| Works from a phone | **Yes** | Yes |
| Works from the head unit | No (no BLE provisioning app) | Yes |

They are complementary, not redundant: **BLE gets you connected, the web page
gets you updated.** The Android app's USB path (§9.3) is the third and, in the
car, the most convenient.


---

## 9. Update paths

**Requirement (user's words):** *"The android app should also be able to push an
update via usb. Both methods should allow pushing a file to update and also
checking the git repo release and pulling down firmware via that method."*

So there are **three** update routes and **two** acquisition modes each. This
section specifies all of them on one shared core, because they differ only in
transport.

```
                     ┌──────────────────────┐
   file  ──────────► │                      │
                     │   shared update core │ ──► verify SHA-256 ──► write inactive
   git release ────► │  (§9.4, transport-   │                        slot ──► set boot
                     │   agnostic, tested)  │                        ──► reboot ──► health
                     └──────────────────────┘                        ──► confirm|rollback
                            ▲          ▲          ▲
                            │          │          │
                       USB (app)   WiFi (web)  (BLE carries creds only)
```

**Design rule:** the checksum, slot-writing, rollback and health-confirmation
logic exists **exactly once** and takes a byte stream. Transports are thin
adapters. A second implementation of the verification path is how one route ends
up less safe than the others.

### 9.1 There is exactly one artifact: the application image

The coop_controller precedent updates two things — an application binary and a
LittleFS filesystem holding its web UI. **SWC deliberately has only one.**

| Artifact | Contains | Update frequency |
| --- | --- | --- |
| **`firmware.bin`** | The application **and the embedded maintenance web page** | Rare |

The maintenance page is a handful of static files (§8.4). **Embedding them in the
application image** rather than serving them from a filesystem partition:

- **Frees the entire filesystem partition** for the app slots — ~320 KB, which
  matters a great deal against §9.2's budget.
- **Removes a whole OTA path.** There is no `assets.bin`, so there is no second
  artifact to version, checksum, upload, roll back, or get out of sync with the
  firmware that is meant to serve it.
- **Removes a failure class.** The coop project had to invent an NVS
  backup/restore dance precisely because flashing its filesystem wiped the user's
  settings. With no filesystem, that problem cannot occur.
- The page is small enough that this costs nothing: one HTML file, one CSS file
  and a little JavaScript, all compressible.

**The config is still not part of the image.** It lives in NVS and survives
updates — that is the point. A firmware update must never cost the user their
button mapping.

### 9.2 Partition layout

Constraint: **4 MB flash, two OTA slots, and an app that links WiFi + BLE +
USB + TLS + the embedded web page.** The app must be big enough to hold all of
that.

```
# Name,      Type, SubType,  Offset,   Size,     Notes
nvs,         data, nvs,      0x9000,   0xC000,   48 KB — config + provisioning creds (§3.8)
otadata,     data, ota,      0x15000,  0x2000,   A/B boot selector + rollback state
phy_init,    data, phy,      0x17000,  0x1000,   RF calibration
#            0x18000–0x1FFFF reserved (32 KB): keeps app0 64 KB-aligned
app0,        app,  ota_0,    0x20000,  0x1E0000, 1,966,080 B = 1920 KB — the running image
app1,        app,  ota_1,    0x200000, 0x1E0000, 1920 KB — the update target
coredump,    data, coredump, 0x3F0000, 0x10000,  64 KB — crash forensics
```

**App partitions must be 64 KiB-aligned.** IDF's `gen_esp32part.py` sets
`ALIGNMENT[APP_TYPE] = 0x10000`, and it **rejects the whole table** otherwise:

```
Partition app1 invalid: Offset 0x208000 is not aligned to 0x10000
```

An earlier revision of this table put `app1` at `0x208000` (app0 + 0x1E8000).
`0x208000 % 0x10000 = 0x8000`, so **the firmware did not build at all** — the
error was found by actually running `pio run -e esp32s3` (plan Task 1), not by
inspection, because the sum checked out. Both slots are now `0x1E0000` = **1920
KB**, and `0x3E0000–0x3EFFFF` is left reserved so `coredump` keeps its specified
64 KB at `0x3F0000`.

**Arithmetic, checked exactly:** `0x20000 + 2 × 0x1E0000 = 0x3E0000`; `+ 0x10000`
(coredump) = `0x3F0000`; `+ 0x10000` (coredump size) = **`0x400000` = exactly
4 MiB.** Both app slots are **1,966,080 bytes = 1920 KB.**

**The app budget is therefore 1920 KB, not 1952 KB.** That is 32 KB less than the
first revision assumed, and it tightens §9.6's fallback trigger — the size gate in
§10.5 and in CI is **1920 KB**.

This is **generously above** the reference layouts: IDF's own
`partitions_two_ota_large.csv` — the table Espressif's `advanced_https_ota`
example uses for 4 MB parts — gives **1700 KB** per slot, and the IDF
`wifi_prov_mgr` provisioning example's app partition is only ~1.4 MB. **1920 KB
is larger than both**, and that gap is affordable precisely because §9.1 removed
the filesystem.

**Why NVS is 48 KB and not 24 KB:** the whole `Config` is stored per slot for
atomicity (§3.8), and **two slots plus the sequence key share the partition.**
A config with two full channels of learned buttons and a few dozen bindings is
realistically **10–15 KB of JSON**, which **exceeds a single NVS value** — an NVS
value is capped at `ENTRY_SIZE × (ENTRY_COUNT − 1)` = 32 × 125 = **4000 bytes**
on this IDF version, a hard limit, not a tuning knob. So a slot is written as
**chunked NVS keys** per §3.8's measure-then-decide rule, and 48 KB is sized so
two chunksets fit with headroom. **§10 requires the real serialized size to be
measured, not estimated.**

**Is 1920 KB enough for the app?** This is the load-bearing question and §10
requires a **measured** answer. Reference points:

- coop_controller builds an ESP32 WiFi + web + JSON + TLS app to ~1.47 MB
  (83.4 % of its 1.75 MB slot) **without** BLE.
- Adding BLE costs real bytes, but **NimBLE rather than Bluedroid** — a benchmark
  reported NimBLE cutting flash by ~167 KB and heap by ~8.5 KB, and another
  reported ~656 KB vs Bluedroid's ~1.1 MB. **Bluedroid would not fit; NimBLE is
  not optional here.**

**This remains the highest-risk budget in the project**, and it is an explicit
gate: `pio run -t size` is checked at every milestone, and if the release build
does not fit with NimBLE, the fallback is §9.6. The plan makes this an early
measurement, not a late surprise.

### 9.3 Route A — update over USB from the Android app

The in-car path, and the one the user asked for first. Works with no WiFi, no
BLE, and no user interaction with a web page.

```
  ota_begin  { size, sha256, version }
  ota_chunk  { offset, data_b64 }            ← repeated, acked
  ota_end    { }                             ← verify, mark valid, offer reboot
```

**Requirements:**

- **Chunked with per-chunk acks**, so the app can show real progress and resume
  a partially-transferred image rather than restarting a 1.75 MB push.
- **SHA-256 computed incrementally during the write** and compared at `ota_end`.
  Never buffer the whole image in RAM (there is no PSRAM) and never trust a
  size-only check.
- The device MUST reject an image claiming the wrong size, or any chunk with a
  gap or an overlap.
- **The device keeps serving button presses during the transfer.** The update
  writes to the *inactive* slot; normal operation continues on the active one.
  This is a real advantage over the naive "reboot to a bootloader" approach and
  it matters because the car may be driving.
- On `ota_end` success the device offers a reboot; it does **not** reboot on its
  own while a key value is being presented.

### 9.4 Route B — update over WiFi via the maintenance page

1. Device is in maintenance mode and on WiFi (§8).
2. User opens the page and either uploads a file or asks it to **check for
   updates**.
3. For an upload, the page streams the file to `/api/ota/upload`, which feeds the
   **same shared core** as Route A.
4. For a check/downpull, see §9.5.

### 9.5 The git-release scheme

Modelled directly on the coop_controller implementation the user built and
approved (`docs/ota-update-system.md`), which is the reference design. Same
shape, adjusted for this project.

**Published per release (GitHub Releases):**

```
firmware.bin            the application image (includes the embedded web page)
firmware_merged.bin     full-flash image, for first-time bench flashing
version_manifest.json   the update descriptor
```

**The manifest** — the contract between CI and the device:

```jsonc
{
  "latest_version": "1.2.0",
  "channel": "stable",
  "firmware": {
    "version": "1.2.0",
    "url": "https://github.com/<owner>/<repo>/releases/download/v1.2.0/firmware.bin",
    "size_bytes": 1543210,
    "sha256": "…"
  },
  "assets": null,
  "min_from_version": "1.0.0",
  "release_date": "2026-09-18T00:00:00Z",
  "changelog": "…"
}
```

**Check procedure (on device, in maintenance mode, or proxied by the app):**

1. Fetch the manifest. **Verify TLS against a pinned CA certificate — not
   `setInsecure()`.** The coop project shipped with certificate validation
   disabled and flagged it as a known gap; **SWC must not repeat that.**
2. Semver-compare `latest_version` against the running version.
3. If newer, **verify `sha256` and `size_bytes` before writing anything.**
4. Honor `min_from_version`: if the running version is older, the update is not
   offered (it would need a staged migration), and the user is told why.
5. Download to the inactive slot and confirm.

**Where the check runs, and why it matters:** the ESP32 may have no WiFi in the
car, so the **Android app should also be able to perform the check over its own
internet connection** and then push the resulting file over USB. That turns
"update the adapter" into something the user can do in a driveway with the head
unit on a phone hotspot. This is the practical superset of the user's two
requests, and it is why the manifest is a published, static, verifiable artifact
rather than something only the device can interpret.

**`channel`** (`stable` / `beta`) is included in the manifest from v1 so a beta
path can be added without a firmware change.

### 9.6 Rejected: a separate maintenance-only firmware image

Splitting "normal" and "maintenance" into two images would shrink the main app
below the 1.75 MiB constraint entirely and eliminate the radio code from the
production image. Rejected because it makes firmware updates two-phase (flash
maintenance image → update → flash back), requires a way to store and restore
the original, and is materially more complex for a two-person-scale project. **It
remains the documented fallback if §9.2's measured budget fails.**

### 9.7 Release automation

Mirrors the coop project's proven pipeline:

```
push tag v*.*.*  ─► CI: build firmware ─► run the FULL test suite
                 ─► compute SHA-256 ─► generate version_manifest.json
                 ─► create the GitHub release with all artifacts
```

**A release MUST fail if any test fails.** This is how the "verify, don't assume"
requirement is enforced without a human remembering to check.

### 9.8 Safety properties of the update system (FR-36, FR-37, FR-41)

| Property | Mechanism |
| --- | --- |
| Never install an unverifiable image | SHA-256 checked incrementally during write; mismatch → discard, keep running the old image |
| Never brick on a bad image | A/B partitions; a new image must **confirm health** before the boot pointer is moved |
| Never lose the user's config | Config lives in NVS, untouched by any update path |
| Never drive a stuck key during an update | The active image keeps running and keeps the safe idle state; the reboot into the new image goes through §6.1's boot sequence |
| Never offer an incompatible jump | `min_from_version` in the manifest |

**Health confirmation (FR-37)** — the mechanism, stated explicitly because it is
the difference between a recoverable and an unrecoverable mistake:

1. The new image boots and runs §6.1 **through step 8** — safe idle established,
   config loaded, classification running.
2. Only then does it call the bootloader's *mark-valid* API to cancel the
   pending rollback.
3. If the image faults, resets, or watchdog-trips **before** reaching step 2 — the
   bootloader counts a failed attempt and rolls back to the previous slot.
4. Bootloader attempt counting is configured with a small limit (e.g. 3) so a
   boot-looping image cannot spin forever.

**The critical subtlety:** mark-valid must happen **after** the output is safe
*and after* the device has proven it can do its job — not merely after `main()`
starts. An image that boots but cannot drive the DAC is not healthy, and
confirming it would strand the user with a bricked-but-"valid" device.


---

## 10. Build and verification strategy

The rule for this project: **nothing is "done" because it compiles.** Every
requirement above is either (a) covered by an automated test, (b) covered by a
scripted on-device procedure with pass/fail thresholds, or (c) explicitly listed
in §12 as unverifiable and why. §11 is the matrix that proves the coverage is
complete.

### 10.1 Repository layout

Everything lives under `code/` in the PCB repo. The firmware is a PlatformIO
project; the Android app is a Gradle project beside it; the contract between them
is one generated header so the two can never drift.

```
code/
├── platformio.ini                 # firmware envs: native, esp32s3, esp32s3-ota
├── partitions.csv                 # §9.2, exactly
├── sdkconfig.defaults             # the non-default knobs, checked in
├── src/                           # thin: main.c + wiring only
├── lib/                           # one directory per module, each unit-tested
│   ├── HAL/          IHAL.h, EspHal.{h,cpp}, MockHAL.{h,cpp}
│   ├── Analog/       AdcReader, LadderDecode, CalibrationCurve
│   ├── Output/       DacMcp4728, GainPolicy, ServoLoop
│   ├── Gesture/      PressClassifier, GestureStateMachine
│   ├── Bindings/     ActionLibrary, BindingResolver
│   ├── Feedback/     BuzzerGrammar, LedGrammar
│   ├── Config/       ConfigStore (A/B NVS), ConfigCodec
│   ├── Link/         Ndjson, UsbCdc, CommandRouter
│   ├── Maintenance/  BleProvisioning, WebPage, MaintenanceMode
│   └── Update/       OtaUsb, OtaWifi, ReleaseCheck, ImageVerify
├── test/                          # Unity, runs ON the device
├── test_native/                   # GoogleTest, runs ON the desktop
├── tools/                         # host-side scripts (release, contract gen)
├── contract/                      # the single source of truth for the wire types
│   └── swc_contract.h             # GENERATED, checked in, CI-diffed
└── android/
    ├── app/src/main/java/...      # Compose UI + usb-serial + provisioning
    ├── app/src/test/              # JVM unit tests
    └── app/src/androidTest/       # instrumented tests
```

**This tree is the binding target structure.** The module names above are the
contract: `AdcReader`, `DacMcp4728`, `BleProvisioning`, `UsbCdc`, `LadderDecode`,
`CalibrationCurve`, `GainPolicy`, `ServoLoop`, `PressClassifier`,
`GestureStateMachine`, `ActionLibrary`, `BindingResolver`, `BuzzerGrammar`,
`LedGrammar`, `ConfigStore`, `ConfigCodec`, `Ndjson`, `CommandRouter`, `WebPage`,
`MaintenanceMode`, `OtaUsb`, `OtaWifi`, `ReleaseCheck`, `ImageVerify`. An earlier
plan revision named several of these inconsistently and created no task for
`UsbCdc`, `DacMcp4728`, `BleProvisioning` or `AdcReader`; the plan is rewritten
against this list, not the other way round. `src/` stays thin — wiring only.

**CI workflows live at the repository root, not under `code/`.** GitHub Actions
reads `.github/workflows/` from the repo root only, so a workflow placed at
`code/.github/workflows/` is never executed. In this repo the firmware and
Android workflows therefore belong at the PCB repo's own root:

```
<repo root>/
└── .github/workflows/
    ├── firmware.yml               # test → build → size gate → artifacts
    └── android.yml                # test → assembleDebug → APK artifact
```

Each workflow must `working-directory: code` (or `code/android`) for its steps,
since the projects live one level down.

**Why the split between `src/` and `lib/`:** PlatformIO runs `lib/` tests from
`test_native/` against the host compiler with no ESP32 toolchain involved, so the
pure logic (ladder decode, gesture timing, gain policy, NDJSON parse, config
codec) is testable in milliseconds with no hardware. Only the HAL implementation
and the RTOS plumbing require the device.

### 10.2 The HAL seam

`IHAL` is the only interface between logic and silicon. Every module above
`lib/HAL/` takes an `IHAL&` and is therefore host-testable.

```c
// lib/HAL/IHAL.h  (frozen — this is the binding interface, not a sketch)
typedef struct {
    int  (*adc_read_mv)(void *ctx, AdcChannel ch);      // calibrated millivolts, §2.3
    void (*dac_set_code)(void *ctx, DacChannel ch, uint16_t code);
    void (*dac_power_mode)(void *ctx, DacChannel ch, DacPowerMode m);
    void (*dac_ldac)(void *ctx, bool assert);
    void (*gpio_write)(void *ctx, GpioPin pin, bool level);
    bool (*gpio_read)(void *ctx, GpioPin pin);
    void (*buzzer_on)(void *ctx, bool on);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    int  (*nvs_get)(void *ctx, const char *key, void *out, size_t len);
    int  (*nvs_set)(void *ctx, const char *key, const void *in, size_t len);
    void (*reboot)(void *ctx);
    void *ctx;
} IHAL;
```

The four channel/pin enums are part of the same frozen interface, and they are
enumerated here so the seam has one authority rather than a header that gets
invented per task. `IO4`/`IO5`/`IO6` and `IO8`/`IO9` are `A-in` in §2.2, so the
AUX inputs and the KEY senses are **ADC channels, not GPIOs**.

```c
typedef enum { ADC_CH_SWC1, ADC_CH_SWC2, ADC_CH_TEMP,
               ADC_CH_AUX1, ADC_CH_AUX2, ADC_CH_AUX3,
               ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2,
               ADC_CH_COUNT } AdcChannel;          // eight, §2.2
typedef enum { DAC_CH_KEY1, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2,
               DAC_CH_COUNT } DacChannel;          // four, no spare, §2.3
typedef enum { DAC_POWER_NORMAL, DAC_POWER_GND_1K, DAC_POWER_GND_100K,
               DAC_POWER_GND_500K } DacPowerMode;  // the MCP4728 has no Hi-Z
typedef enum { GPIO_LED_STAT, GPIO_LED2,
               GPIO_BOOT, GPIO_VBUS_VALID, GPIO_COUNT } GpioPin;
```

**`GpioPin` carries raw pins only.** The buzzer (`/BUZZ`, IO13) and the MCP4728
`~LDAC` (IO48) are deliberately **absent**, because each already has its own
member above — `buzzer_on` and `dac_ldac`. They are *semantic* lines: the buzzer
is a rhythm grammar (§7.2) and `~LDAC` is DAC sequencing (§2.3), so a bare pin
write cannot express the contract. Listing them here as well would give two
routes to one physical line, and a mock would have to store the same state twice
— which is exactly how a driver ends up driving the line one way while the tests
observe the other.

**This interface is frozen as of this revision.** An earlier revision labelled it
"shape, not final", which made every module above `lib/HAL/` a moving target and
let the plan and the spec drift on names — `IHal` in the code block against `IHAL`
in the prose, C enums vs `enum class`, `SENSE1`/`SENSE2` as GPIOs when §2.2
defines them as ADC inputs. Those were all symptoms of the interface not being
authoritative.

**The freeze is what makes the seam real:** the enums (`AdcChannel`, `DacChannel`,
`DacPowerMode`, `GpioPin`) are **C enums with explicit `ADC_CH_*` / `DAC_*` /
`GPIO_*` constant names**, not C++ `enum class`, because `IHAL` is a C struct and
the ESP-IDF side of the seam is C. Every module takes `IHAL&`; the reference is
passed in rather than fetched from a singleton, so there is no `Interface()`
accessor on the interface itself.

The **mock** does expose `InterfaceRef()`, and that is not a contradiction: a
test owns a `MockHal` object and must hand *its* `IHAL&` to the unit under test,
so the accessor belongs to the harness, not to the seam. The earlier revision
labelled both names drift symptoms, which left the plan with ~51 call sites and
no sanctioned way to obtain the reference from a mock.

Changing this interface is now a **spec change**, not an implementation detail: it
requires a spec edit, a regeneration of `contract/` (§10.2 above), and a plan
revision. That is the intended cost — the seam is the one place where a mistake
propagates to every task at once.

**`now_ms`/`now_us` are in the HAL on purpose.** Every timing rule in §7 — the
500 ms double-press window, the 750 ms long-press threshold, the 200 ms send
duration, the 5-minute maintenance timeout — is a *tested* rule, and the only way
to test a timing rule without sleeping is to make the clock an input.
`MockHAL` advances a synthetic clock, so the entire gesture grammar is exercised
deterministically in microseconds of wall time.

**Contract generation.** `tools/gen_contract.py` reads §3 and §4 and emits
`contract/swc_contract.h` (firmware) and, via `tools/gen_contract_kotlin.py`, the
matching Kotlin data classes. CI runs both and fails if the checked-in output
differs — so a change to a field name in the spec surfaces as a failing build
rather than as a runtime parse failure in a car.

### 10.3 Framework and platform pinning

| Setting | Value | Why |
| --- | --- | --- |
| `framework` | `espidf` | Not Arduino. The reported crash class (§1) is Arduino-only |
| `platform` | **pioarduino's fork, pinned to an exact release tag** — `https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip` | See below — this is not a preference, it is forced |
| `board` | the DOIT ESPS3-32-N4 definition, or a custom `board_*.json` | Must match the ordered module: 4 MB flash, no PSRAM, USB-Serial-JTAG on IO19/IO20 |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | y | Console on the ROM USB-JTAG peripheral |
| `CONFIG_TINYUSB_CDC_ENABLED` | y | The app link on TinyUSB CDC — **two separate USB endpoints, deliberately** (§4.1) |
| `CONFIG_BT_ENABLED` + `CONFIG_BT_NIMBLE_ENABLED` | y (NimBLE) | Bluedroid does not fit; §9.2 |
| `CONFIG_ESP_ADC_CAL_*` | curve-fit, per-chip | §2.3; raw-ADC accuracy claim depends on this |
| `CONFIG_MBEDTLS_HARDWARE_AES` | **n** | Avoids the mbedTLS GCM/DTLS bug that caused the user's TLS crashes; §1 |
| `CONFIG_ESP_SYSTEM_PANIC` | `gdbstub` in dev, `reset` in release | A coredump partition exists (§9.2) for post-mortem |

**Why the platform is forced (verified 2026-09-18, not assumed).** Stock
PlatformIO's `espressif32` registry platform is at **7.1.3** and its
`framework-espidf` package tops out at **4.60100.0 — ESP-IDF 4.6.1**. There is
**no IDF 5.x in the stock registry at all**: the published `framework-espidf`
versions are 3.4.x, 3.5.x and 4.6.x, and that is the whole list. IDF 4.6.1 is
older than the mbedTLS/GCM bugfix that motivated this decision, so the stock
platform would silently reintroduce the exact TLS defect the project exists to
avoid.

`pioarduino/platform-espressif32` **55.03.311** ships
`framework-espidf` from `esp-idf v5.5.5`, which is the version this project
targets. It also happens to be the same fork this machine already uses for the
Arduino work, so the toolchain is proven here.

**Pin to the release tag, never the rolling `stable` zip.** The rolling zip's
framework spec drifts, which triggers pioarduino's reinstall path, and **that
path crashes under Python 3.14** (`exists(None)` in `safe_framework_cleanup`).
This machine runs Python 3.14.7, so the pinned tag is required, not merely
advisable. A pinned tag gives a deterministic package spec and a clean install.

A **custom board JSON is likely required** — the stock `esp32-s3-devkitc-1`
definition has PSRAM enabled and a different flash size, both of which would
produce a build that lies about the target.

### 10.4 The test pyramid, and what belongs at each level

**Level 1 — native unit tests (`test_native/`, GoogleTest).** No hardware, no
RTOS, milliseconds. This is where the correctness of the *rules* lives:

- **`LadderDecode`** — the ratio-normalized transfer function of §6.3, against
  synthesized ADC traces: exact resistors, ±1 % resistors, a **+3V3 rail sweep
  (3.14–3.47 V)**, ±temperature skew. **The assertion is behavioural:** the same
  physical button classifies identically at 3.14 V and at 3.47 V. That single
  test is the entire justification for the ratio-normalization design choice.
  *(Not an 11–14.8 V vehicle-rail sweep: no vehicle-rail term exists in the
  transfer function, and no reading above 2.9 V is producible at all — §6.3.)*
- **`CalibrationCurve`** — the curve-fit polynomial and the attenuation
  conversion; a table of (raw, expected_mv) pairs at the 2.9 V ceiling (§2.3).
- **`GainPolicy`** — the 1.82/1.00 selection, the 2.6–3.4 V guard band, the
  asymmetric default. Feeds §6.2's envelope and asserts the output never leaves
  1.80–5.20 V for any legal input.
- **`ServoLoop`** — convergence, overshoot bound, and settling time against a
  plant model of the op-amp integrator. Includes the degenerate cases: no
  head-unit load, an open circuit, a short.
- **`PressClassifier` / `GestureStateMachine`** — every gesture in §3.4 with a
  synthetic clock: single, double, long, double-then-long, the 500 ms boundary
  at exactly 499/500/501 ms, the 750 ms boundary at 749/750/751, debounce, and a
  press that never releases (must not hang the link).
- **`Ndjson`** — framing, partial reads, split frames across a read boundary, a
  frame larger than the buffer, malformed JSON, and a `seq` gap (must be
  *detected and reported*, not silently tolerated).
- **`ConfigCodec`** — round-trip of every field in §3, a truncated blob, a
  corrupted blob with a bad CRC, and a blob from a **newer** `schema_version`
  (must refuse, not misparse).
- **`ConfigStore`** — the A/B dual-slot scheme of §3.8 against a `MockNVS` that
  can fail a write at an arbitrary byte offset, simulating power loss mid-write.
  **The assertion is** that after any injected failure, the store reloads the
  *previous* good config, never a torn one.
- **`ReleaseCheck`** — the manifest parse and version comparison of §9.5,
  including a downgrade attempt, a `min_from_version` refusal, and a manifest
  pointing at a URL for a different board.
- **`ActionLibrary` / `BindingResolver`** — every action kind in §3.6 resolves;
  an action with a data payload round-trips it; an action whose kind is not in
  the table is rejected with an error rather than silently dropped. (There is no
  numeric action id to be unknown — §3.6 defines an action as its `kind` plus
  params, so the rejection is on the kind and on an `OUT_VOLTAGE` that carries no
  `key_mv`.)

**Level 2 — on-device tests (`test/`, Unity).** Runs on the real board, asserts
what only silicon can answer. Deliberately small and mostly *measurement*, not
logic:

- **ADC linearity and calibration** — a sweep against a bench supply across
  0–2.9 V, asserting the curve-fit error stays inside the §2.3 budget. This test
  *produces a number*, which is then recorded in the spec. It is a calibration
  check, not a pass/fail on the driver.
- **DAC monotonicity** — every one of the 4096 codes is monotonic in the
  measured output; no missing codes. Catches a wiring or I²C-order fault that
  desktop tests cannot see.
- **Gain-mode envelope** — drive the DAC across its range in both gain modes and
  assert the measured output stays in 1.80–5.20 V and switches cleanly at the
  guard band. §6.2's numbers are only true if this passes.
- **USB enumeration** — both the CDC interface and the console enumerate
  simultaneously on one cable, and the console does not corrupt the app link.
- **NVS round-trip and power-loss** — write a config, cut power mid-write
  (hardware-interruptible power switch on the bench), assert boot delivers the
  old config.
- **OTA A/B rollback, for real** — flash a deliberately-faulting image
  (compiles, panics at startup), assert the bootloader rolls back and the device
  comes up on the old image. **This must be tested with a genuinely broken
  image**, not a mocked failure, or it proves nothing.

**Level 3 — Android tests.** JVM unit tests for the protocol codec, the config
model, and the update-state machine (all host-only, fast). Instrumented tests
for the USB permission flow, the Compose screen state, and — where the emulator
permits — the provisioning client against a mock peripheral. **What cannot be
tested without the physical head unit is listed in §12**, not quietly skipped.

**Level 4 — system/bench tests.** Scripted, with the board wired to a bench
supply, a resistor ladder in place of the steering wheel, and a head-unit
emulator (a resistor load plus a scope on the output). These are the tests that
answer the questions the user actually cares about:

- Every learned button, pressed 100 times each, at three **+3V3 rail** voltages
  (3.14, 3.30, 3.47 V) — **zero misclassifications**, and every press produces
  exactly the expected output level. *(The rail that feeds the ladder is +3V3,
  not the vehicle's 12 V — §6.3.)*
- The output level for each button, measured, versus the head unit's own
  documented ladder windows — the end-to-end pass/fail that matters.
- Simultaneous-press and rapid-alternation behaviour per §6.
- 72-hour soak with randomized presses, asserting no watchdog reset, no heap
  exhaustion, and a bounded worst-case latency.

### 10.5 Build gates (these fail the build, they are not advisory)

| Gate | Command | Threshold |
| --- | --- | --- |
| Native tests | `pio test -e native` | 100 % pass, no skips |
| Device tests | `pio test -e esp32s3` | 100 % pass on the bench board |
| App fits the slot | `pio run -e esp32s3 && pio run -t size` | ≤ 1920 KB (§9.2) — **the highest-risk gate** |
| Free-heap headroom | runtime assertion in the device test | ≥ 20 % free at worst-case steady state |
| Config fits NVS | `ConfigCodec` size assertion: worst case > **4000 B per value** (so the chunked path is exercised, not dead code), and **two slots + `cfg_seq` ≤ 48,384 B** of entry space at **2,112 B per 2048-byte chunk** — counting NVS's metadata and `BLOB_IDX` entries, not just the payload |
| Contract in sync | `tools/gen_contract*.py` then `git diff --exit-code` | No diff |
| Android builds | `./gradlew assembleDebug test` | Clean, tests pass |

**If the app does not fit 1920 KB**, the documented fallback is §9.6's separate
maintenance image — decided now, so a size blowout is a known trade, not a
mid-project emergency. The gate exists so that is discovered in CI, on day one,
rather than the week the boards land.

### 10.6 Bring-up plan for the ordered board

The board arrives in a few days; the firmware is written ahead of it. That
ordering creates a specific risk — **code that has never met silicon** — so
bring-up is a scripted sequence, and each step's result is *recorded*, not
eyeballed. Nothing later is trusted until the step before it passed.

1. **Power and identity.** 5 V bench supply, current draw sane, AMS1117 rails
   correct, module enumerates on USB-Serial-JTAG. Confirm the flash size and
   absence of PSRAM the build assumed. *If this disagrees with the board JSON,
   stop and fix the board definition — every later measurement is void.*
2. **I²C and the DAC.** Scan the bus, find the MCP4728 at its strap address,
   write a mid-code, measure with a meter. Confirms §2.5's wiring and that
   `LDAC` behaves.
3. **The ADC ladder — and the `R15`/`R16` decision.** With a resistor ladder in
   place, sweep the **+3V3 rail 3.14 → 3.47 V** and record the idle and
   per-button readings. **Fit the real calibration here** — this is where the
   §2.3 numbers become true for the actual board.
   Two gates ride on this measurement, and both must be recorded before the
   PCB is considered final:
   - **The ladder ceiling.** Confirm `R_ladder_idle ≤ R_pullup · 7.25`
     (≈72.5 kΩ at the 10 kΩ `R15`/`R16`; §6.3 consequence 4). If it is over,
     `R15`/`R16` go **smaller**, not larger.
   - **The ceiling headroom.** The nominal 2835 mV idle reaches the 2900 mV ADC
     ceiling at only **+2.3 %** of rail, so confirm the real idle is not already
     clipping at nominal 3.3 V.
4. **The output stage and gain.** Measure the output envelope in both gain
   modes, verify 1.82 and 1.00, verify the guard-band switch, verify the 1.80 V
   floor and 5.20 V ceiling. Update §6.2 if reality disagrees — and it may, on
   the first board.
5. **Servo dynamics.** Step the DAC, scope the output, measure overshoot and
   settling. Tune §6.5's constants to the *measured* plant, and if the measured
   plant contradicts the modelled one, the model is what changes.
6. **Feedback.** Buzzer gate and the two LED channels, both cadences of §7.
7. **Timing.** The gesture boundaries under a real, jittery RTOS — the native
   tests prove the *rule*, this proves the *implementation meets the rule* with
   real scheduling latency.
8. **NVS and OTA.** The dual-slot write with a real power cut, then a real A/B
   update with a real broken image. Both are called out in §10.4 for the same
   reason: a mocked version of either test is worthless.
9. **System bench tests** (§10.4 level 4) — the ones that decide whether the
   thing actually works in the car.

**Every step produces a number that goes back into this spec.** The spec is the
living contract; if bring-up contradicts it, the spec is wrong and gets fixed in
the same commit as the code.

### 10.7 What "done" means

A requirement is done when: its §11 row names a test, that test exists, that
test fails before the implementation and passes after, and — for anything in
§10.6 — the measured number is recorded. The user's own review (the punch list
that drove this design) is the final acceptance, not the ERC or the test suite.

---

## 11. Traceability matrix

Every requirement maps to at least one test, and every test names the file it
lives in. A requirement with no test is a gap; a test with no requirement is
either a gap in the requirements or dead weight. §12 lists the requirements
whose tests cannot be fully automated, and why.

Test location key: **N** = `test_native/` (GoogleTest, host), **D** =
`test/` (Unity, device), **B** = bench/system (§10.4 level 4), **A** =
`android/app/src/test|androidTest` (JVM/instrumented).

| FR | Verified by | Location | The assertion that actually decides it |
| --- | --- | --- | --- |
| FR-1 | Non-blocking acquisition test | N + **B** | **N:** idle ticks are silent and cheap, and no HAL call in the poll path blocks (the HAL seam takes no locks and the CDC write is a FIFO push that returns the count accepted). **B:** the cadence under a real 20 ms host stall — an earlier revision of this row claimed a host test with injected stalls, and no such test exists |
| FR-2 | `CalibrationCurve` + ADC linearity | N + **D** | **N:** endpoints map exactly, monotonic across the full raw range, never above the 2.9 V ceiling for this attenuation, midscale within 25 mV of half-scale, and a blank eFuse falls back to the linear curve *and reports which source it used*. **D (not yet run):** the curve-fit *accuracy* against a bench reference — it needs the real eFuse, so it cannot be a host test, and an earlier revision of this row claimed a host test for it |
| FR-3 | Filter settling test | N | Step response settles in < `debounce_ms`, and a 20 ms press is not attenuated below the detection threshold |
| FR-4 | Fault-injection tests | N + D | Open input and a short-to-rail each release the KEY line and latch `LED_STAT` blink rather than classifying as a button. **Not** a `FAULT_*` buzzer: no such pattern names this subsystem (N-10) |
| FR-5 | Live-sample stream test | N + **D** | **N:** `ladder_sample` is emitted while a learn run is open and stops when it closes, and the frame carries the run's channel. **D (not yet run):** the ≥ 20 samples/s cadence and its latency — that is a property of the poll loop's real rate, which the host clock cannot show |
| FR-6 | Classification + hysteresis test | N | A level inside the window's outer edge twice in a row does not re-trigger; each learned button classifies across the **+3V3** tolerance band (3.14–3.47 V, §6.3), and an idle-adjacent button — the worst case for separation, per §6.3 consequence 2 — still resolves |
| FR-7 | Gesture tests | N | Each of SINGLE/DOUBLE/LONG fires exactly once for its stimulus |
| FR-8 | Injected-clock suite | N | The entire §3.4 gesture set runs with zero wall-clock sleeps |
| FR-9 | Dual-channel concurrency test | N + B | Two simultaneous presses produce two independent, correct events |
| FR-10 | Long-press timing test | N | `LONG` fires at 750 ms ± 1 tick, **before** release |
| FR-11 | Gesture-exclusivity tests | N | LONG emits no SINGLE; DOUBLE's second press emits no SINGLE |
| FR-12 | Unlearned-press test | N + B | A never-learned level yields `event{button:null}` + `KEY_UNKNOWN`, never a guess |
| FR-13 | Boot-sequence test | N + **D** + B | **N:** `SafeIdleEstablished()` is true immediately after `Boot()` and the DAC has been written before anything else runs (the tests assert this on every construction, including the pass-through and no-config paths). **D:** the same ordering measured on the real output pin before USB enumerates |
| FR-14 | Gain-policy tests | N | `GainPolicySelect` implements AUTO per §6.2's two-sided rule — a 3 V line measures low and takes gain 1.00, 5 V takes 1.82, the ambiguous guard band and an **absent** measurement both default to 1.82 (the safe direction) — and a forced mode is honoured. `Boot` passes the measured idle into it per channel |
| FR-15 | Output-command test | N | The bound action's key is driven **only after the gesture resolves** (`TheOutputIsNotDrivenWhileTheGestureIsUndecided`), and the line returns to the idle code afterwards. The **hold duration** is a property of the injected clock, asserted by the gesture tests, not by a wall-clock measurement here |
| FR-16 | Release-state test | N + **D** | **N:** release writes a code **above** the head unit's measured idle — if the configured idle code cannot reach above it, the firmware substitutes full scale rather than leaving Q4 conducting. **D:** that the resulting line is genuinely high-Z and the head unit sees its own idle |
| FR-17 | Temp-comp test | — | **Absent.** The traceability row claimed a test for a correction that does not exist; a coefficient must be measured before either can be written (N-9) |
| FR-18 | Clamp test | N | An over-ceiling code is clamped and logged; an out-of-envelope write never reaches the DAC |
| FR-19 | Trim-loop tests | N (+ B when enabled) | **N:** converges on a 3 % gain error within the code budget, never moves more than `max_step` per update, stops inside the deadband, does not oscillate when handed a measured overshoot, and respects the total code budget. **The loop is DISABLED in v1** (spec §6.5: open-loop until its gain is measured on hardware), so these prove the implementation and not the running system; enabling it and confirming no ADC-noise injection is a bring-up step |
| FR-20 | Grammar tests | N + D + B | Every pattern in §7.1/§7.3 produces the documented drive sequence |
| FR-21 | Feedback-nonblocking test | N + D | A key press during an in-flight buzzer pattern is still served on time |
| FR-22 | Level-setting tests | N | Each level, including fully-off, suppresses the right classes and nothing else |
| FR-23 | Atomic-persist test | N + D | Power loss at any byte offset yields the previous good config, never a torn one |
| FR-24 | Corrupt/newer-config test | N | Corrupt CRC and newer `schema_version` each fall back to defaults **and** signal audibly |
| FR-25 | No-config pass-through test | N | With empty config, a press drives a key at the wheel's ratio against the head unit's idle (not a copy of the wheel's volts), and no usable ladder reference disables pass-through rather than guessing |
| FR-26 | Config-validation test | N + A | Invalid config → `nack`, and a read-back proves the old config is intact |
| FR-27 | Export/import test | N + A | Full config JSON round-trips byte-identically through export → import |
| FR-28 | Learn-mode tests | N + D + B | Measured level, tolerance and rail are stored and match the bench instrument |
| FR-29 | Learn-rejection tests | N | Noisy and too-close-to-existing samples are each rejected **with the correct distinct reason** |
| FR-30 | Rail-renormalization test | N | A button learned at 3.3 V classifies correctly at 3.14 V and 3.47 V (±5 % regulator tolerance); the ratio `n` is unchanged across that sweep. A **separate** fault test asserts that a 3V3 sag to ≤20 % of the learned value is reported as a rail fault, not as idle. **Note:** this is a *+3V3* sweep, not the 11–14.8 V vehicle-rail sweep an earlier revision specified — no vehicle-rail term exists in the transfer function (§6.3). |
| FR-31 | Headless-learn test | D + B | A full learn completes with no USB host attached, driven by AUX1 + buzzes |
| FR-32 | Radio-absent test | D + B | In normal mode, current draw and heap show WiFi/BLE never initialized |
| FR-33 | Maintenance-entry tests | N + D | Each of the three triggers enters maintenance; each exits correctly |
| FR-34 | Provisioning test | D + A | The **real Espressif provisioning app** completes provisioning against this device |
| FR-35 | Dual-path update tests | D + B | A file update and a git-release update each succeed over WiFi **and** over USB |
| FR-36 | Checksum-refusal test | N + D | A corrupted image is refused; the device keeps running the old image |
| FR-37 | Rollback test with a real bad image | D + B | A genuinely faulting image rolls back automatically (§10.4) |
| FR-38 | Maintenance-timeout test | N + D | 5 minutes of inactivity returns to normal mode and serves a press again |
| FR-39 | Reset-safety test | D + B | On reset, watchdog, brownout and USB-disconnect, the measured KEY line is idle, **not** driving |
| FR-40 | Watchdog-recovery test | D + B | After a forced WDT reset, safe idle is re-established before any key can be served |
| FR-41 | Brick-resistance test | D + B | Repeated failed updates never leave an unbootable device |
| FR-42 | Headless-operation test | B | Full button function with USB down, no app, no WiFi — the normal in-car case |

**Coverage statement:** 42 of 42 requirements have an automated or scripted
test. Zero requirements are covered only by inspection. The six that need
physical hardware — FR-16, FR-34, FR-39, FR-40, FR-41, FR-42 and the bench
portions of FR-6, FR-9, FR-12, FR-19, FR-31, FR-35, FR-37 — are blocked on the
ordered board and are called out in §12 as the critical path.

---

## 12. Open items, risks, and accepted limitations

### 12.1 Open items needing a decision or a measurement

| # | Item | Needed by | Blocking? |
| --- | --- | --- | --- |
| N-1 | **The exact DOIT ESPS3-32-N4 flash/RAM configuration** — confirm 4 MB flash, no PSRAM, and the USB-Serial-JTAG pin map, then fix the board JSON | Immediately | **Yes** — every build depends on it |
| N-2 | **The head unit's actual ladder resistor values.** The *input* side is now settled (§2.4/§6.3: series chain, common to GND, pulled up to +3V3 by `R15`/`R16`, no 12 V term), but the **actual resistance values are vehicle-specific and unmeasured**, so how well the buttons spread across the ADC range is unknown. If the spread is poor — most likely for the idle-adjacent buttons (§6.3 consequence 2) — the fix is to change `R15`/`R16` | Before bench tests | Yes, for **input** calibration; also gates §6.2's output envelope indirectly |
| N-3 | **App-fits-in-1920 KB** — unresolved until the first full BLE build with NimBLE. The §9.6 fallback is the answer if it does not | First CI run | No (fallback exists) |
| N-4 | **The real MCP4728 I²C address strap** on this board — read from the schematic/silkscreen, not assumed | Before step 2 of bring-up | Yes, for bring-up |
| N-5 | **Whether the integrator's measured plant matches the modelled one.** §6.5's servo constants are a model until step 5 of bring-up | Bring-up | No, but constants change |
| N-6 | **Android: the head unit's Android version and whether it is rooted/a system app.** Determines if launching apps from background needs the launcher role or the overlay permission (§3.6, Android BAL) | Before Android work | Yes, for the app's action library |
| N-7 | **Long-term availability of the DOIT module.** It was chosen for JLCPCB Economic eligibility; if it goes away, the fallback changes the board JSON and possibly the pin map | Not urgent | No |
| N-8 | **The first `pio run -e esp32s3` against pioarduino 55.03.311.** The platform/IDF pairing is verified from the published manifests, but the ESP-IDF branch of this fork has not been exercised on this machine yet — the Arduino branch has | **First task** | **Yes** — if it does not build, the framework decision is reopened |
| N-9 | **FR-17's temperature compensation is not implemented.** `temp_c_at_learn` and `temp_comp_enabled` are recorded, but there is no coefficient field, no correction function, and no test. Needs a measured drift coefficient from a bring-up session, then a field plus the correction applied to the learned windows | After a bring-up measurement | No — v1 is correct without it, since an uncompensated ladder is the status quo |
| N-10 | **A ladder or rail fault has no audible indication.** Spec 7.2's `FAULT_*` patterns each name a subsystem (`FAULT_DAC` is the I2C/DAC path, `FAULT_CONFIG` a corrupt config) and none fits an out-of-range ladder or a collapsed rail, so the firmware blinks `LED_STAT` and stays silent. A user with `led_level` set to 0 therefore gets no fault indication at all. Needs either a `FAULT_INPUT` pattern or an explicit decision that an unlit-LED fault is acceptable | Before the board arrives | No |

### 12.2 Risks, ranked by expected damage

| # | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R-1 | **The app does not fit 1920 KB with BLE + WiFi + OTA + web UI** | High — forces the §9.6 split image | Gate it in CI on day one (§10.5); the fallback is designed, not improvised |
| R-2 | **A wrong config drives a wrong key in a moving car** | High — could be genuinely dangerous | Ratio-normalized decode, hysteresis, never guessing `UNKNOWN`, safe idle on every fault path, and §10.4's 100-press × 3-voltage misclassification test |
| R-2b | **The steering-pad ladder's button spread is too tight to classify** once the real resistances are measured — the failure mode §6.3 consequence 2 predicts, and worst at the idle-adjacent end | Medium–High — degrades the headline feature, not safety | Measure the ladder early in bring-up (§10.6, N-2) **before** trusting `R15`/`R16`; the fix is a pull-up value change (rework), and `mv_tolerance` derives from the measured gap (§3.4) so a tight-but-usable spread still works |
| R-3 | **The output stage misbehaves on the real head unit** (impedance, bias, the 1.80 V floor) | High | Real-load bench test with a head-unit emulator; the envelope is asserted, and the spec gets corrected by measurement |
| R-4 | **First firmware meets silicon only at bring-up** | Medium–High | The bring-up plan of §10.6 is ordered so each step gates the next; nothing is trusted on the strength of a compile |
| R-5 | **Android BAL: cannot launch apps from background** | Medium — could kill a headline feature | targetSdk 34 to avoid BAL hardening; default-launcher role is the real fix, overlay permission the fallback; **needs N-6 to resolve** |
| R-6 | **USB CDC + USB-Serial-JTAG coexistence on the head unit's host stack** | Medium | Tested explicitly in §10.4 level 2; the console can be disabled in release if the host chokes |
| R-7 | **NVS wear over the device's life** | Low | Dual-slot A/B with staged writes; config writes are rare, and only on user action |
| R-8 | **BLE provisioning UX friction** (the Espressif app's own flow) | Low | Keep the device's own config as the source of truth; BLE only ever sets WiFi credentials |
| R-9 | **The pioarduino IDF-5 platform does not build cleanly here** | High if it happens — it removes the reason ESP-IDF was chosen | N-8 tests it in the *first* task, before any real code exists. Fallback order: (a) a different pinned pioarduino tag, (b) a hand-installed IDF 5.x via `esp-idf` directly with `pio run` driving `idf.py`. What is **not** a fallback is stock PlatformIO — that is IDF 4.6.1, which reintroduces the TLS bug |

### 12.3 Accepted limitations (stated, not hidden)

- **The buzzer cannot change pitch.** It is an active magnetic buzzer at a fixed
  ~2.4 kHz (§7.1). All feedback is rhythm, count and duration. Any future design
  that wants pitch must change the part.
- **The LEDs are one colour.** Both are green (§7.3). The grammar uses position
  and pattern, never hue.
- **No DAC Hi-Z.** The MCP4728 has no high-impedance output state; "release" is
  implemented as *command-above-idle* plus the sink FET turning off (§6.7). A
  future board revision with a different DAC could change this.
- **No PSRAM means no large buffers.** TLS for OTA, the web page, and BLE all
  fit in internal RAM or they do not ship. This is a hard constraint, not a
  preference.
- **The Android side cannot be fully tested without the head unit.** What that
  means concretely: the USB permission flow, the background-launch behaviour, and
  the real system-UI interaction are **not** covered by the automated suite, and
  the spec says so rather than implying coverage it does not have.
- **The 2022 Pico design's programming UX is preserved in spirit, not in code.**
  Its gesture-recognition functions were stubs (`return true`) with no
  persistence (§2 of the plan). The *interaction* — hold, then single/double/long
  to assign, with escalating buzzer feedback — is carried forward and implemented
  properly; the implementation is new.

### 12.4 What would change this design

Recorded so a future reader knows which decisions were contingent:

| If this turns out true | Then this changes |
| --- | --- |
| The head unit needs a current-mode, not voltage-mode, ladder | The entire output stage model (§6.2, §6.5) — a real redesign, not a tuning change |
| The board ships with a different DAC without Hi-Z too | Nothing — the current design already assumes no Hi-Z |
| 4 MB proves unworkable with BLE | §9.6's separate maintenance image becomes the primary design; normal mode drops the radio code entirely |
| The head unit's Android blocks background launch outright | The "launch app / send intent" feature degrades to a foreground-only feature, or the app becomes a launcher — **this is the single largest UX risk in the project** |

---

## Appendix A: the 2022 design, and what was kept

| Aspect | 2022 (Pico + digital pot) | This design | Verdict |
| --- | --- | --- | --- |
| Output element | Digital pot | 12-bit DAC + op-amp integrator servo | **Replaced** — the stated reason the old design "didn't work well" |
| Ladder decode | 1/3-of-range heuristic over 0..24 | Ratio-normalized windows, learned per vehicle | **Replaced** |
| Gesture recognition | Stubbed (`check_is_double_press_key` / `check_is_long_press_key` both `return true`) | Deterministic, clock-injected state machine (§3.4) | **Replaced** — the old code could not have worked |
| Persistence | None | Dual-slot A/B NVS with CRC (§3.8) | **Added** |
| Programming UX (hold, then single/double/long, escalating buzzer) | Present in intent | Preserved as first-class (§7.5) | **Kept** — the user's stated good part |
| Android link | Custom serial driver | NDJSON over TinyUSB CDC (§4) | **Kept, standardized** |
| Timings | `MAX_DOUBLE_PRESS_OFF_MS 500`, `MIN_LONG_PRESS_MS 750`, `KEY_SEND_DURATION_MS 200` | Same defaults, now configurable and tested | **Kept** |
