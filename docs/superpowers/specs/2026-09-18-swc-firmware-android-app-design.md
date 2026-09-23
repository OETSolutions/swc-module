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

   **"Tracking the signal channel's code" means `V_ADJ = V_DAC` on EVERY code
   write, not just at gain selection.** Selecting the mode only sets the ADJ
   channel's *power mode* (its code field is 0 in that write); the tracking
   relationship is a property of the two channels carrying the *same code*, so
   each KEY-line write must be paired with the matching ADJ write. Writing the
   power mode alone leaves `V_ADJ = 0` and the amplifier delivering 1.82× — the
   over-range direction §6.2 calls the only dangerous one — while every
   power-mode assertion still reads correct. (Found 2026-09-22: the firmware
   selected tracking mode and never wrote ADJ's code, so a 3 V head unit was
   driven at 1.82× and no test noticed because none read ADJ's code.)

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

  **The recessed BOOT pin is a *recovery* path, not the ONLY way into the ROM
  download loader** (corrected 2026-09-24, N-80). An earlier revision stated
  that entering the loader was a power-on/BOOT-pin event with no software path,
  and that was used to justify refusing `boot_target: "bootloader"` (§4.3). The
  ESP32-S3 ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`RTC_CNTL_OPTION1_REG`
  bit 0) on every reset, and that bit lives in the RTC domain — it survives
  `esp_restart()` but not a power cycle. So the firmware can *request* the loader
  (`IHAL::reboot_to_download`), which is what makes the device flashable over its
  own USB cable without opening the enclosure. BOOT remains the way in for a
  blank board or firmware that predates this feature, which is the honest
  bootstrap boundary `tools/dev_flash.sh` reports rather than hides.

#### 2.5.1 The MCP4728 write frame, and why `~LDAC` is never pulsed

The DAC is driven by the **Multi-Write command** (DS22187E §5.6.2, Figure 5-8),
chosen because its single channel-select field addresses exactly one output per
transaction and, with `UDAC = 0`, that output updates on the transaction's final
ACK — **no `~LDAC` pulse is required.** The frame is **three bytes**:

| Byte | Bits | Meaning |
| --- | --- | --- |
| 0 | `0 1 0 0 0 DAC1 DAC0 UDAC` | command type `C2:C1:C0 = 010`, channel select, `UDAC` |
| 1 | `VREF PD1 PD0 Gx D11 D10 D9 D8` | reference, **power-down** (`PD1:PD0`), gain, code high nibble |
| 2 | `D7 … D0` | code low byte |

`VREF = 0` (VDD reference) and `Gx = 0` (×1) always; the gain mode lives entirely
in `PD1:PD0`, per §2.3. The bytes are built by `DacFrame::EncodeSet`
(`lib/HAL/DacFrame.h`) rather than inline in `EspHal.cpp`, **because `EspHal` is
the one `lib/` file the host build excludes** — a byte layout written inside it
is checked by no test. An earlier revision made exactly that mistake and emitted
a **four-byte** frame (an MCP4725-shaped one, command in its own byte, `PD1:PD0`
at bits 5:4), so on the wire the device read the channel-select byte as the
command byte and never addressed channels B/C/D: **no output was ever written.**
Found 2026-09-22; the host `DacFrameTest` now pins every byte to Figure 5-8.

**`~LDAC` (IO48) is therefore never asserted by the firmware.** It is configured
as an output and held HIGH at boot (so the `R13` 10 kΩ pulldown cannot latch at
an arbitrary time during start-up), and the `dac_ldac` HAL member exists for the
deferred-latch path — but with `UDAC = 0` on every write, each output latches on
its own ACK and no pulse is needed. `~LDAC` additionally has a *required*
High→Low transition mid-frame to programme the I²C address bits (§5.6.7), which
this design does not do: the address is the strap default `0x60` (§12.1, N-4).
Deferring the latch to a shared `~LDAC` pulse is explicitly rejected in §2.3
consequence 1 — it would apply both channels' new values at one instant and
destroy the intermediate state the servo depends on.

---

## 3. The data model

This is the **initial layout of the entities**, and it is the contract between
all three components. The firmware persists it in NVS, the Android app edits it
and sends it over USB, and the web page only ever touches the small subset in
§3.6. Every field is defined once, here.

### 3.1 Entity map

```
Config ──┬── schema_version, device_id, updated_at_ms
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

**`source` is a NUMBER, and it is a raw ADC channel index, not a name.** The
worked example below writes `"source": 0` and that is literal: both codecs
(`ConfigCodec` and the app's `ConfigJson`) encode and decode it as an integer, and
`0` is `ADC_CH_SWC1`. This paragraph exists because the example previously wrote
`"source": "LADDER_3V3"`, which **no codec accepts** — decoding that form fails
outright, so the spec's own example config was undecodable for a second,
independent reason beyond the `gain_mode` one. Nothing reads `source` at runtime
today; it is carried for round-trip fidelity and reserved for a future
source-selection feature, so a name would have been a vocabulary invented for a
consumer that does not exist.

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

**`Channel.enabled` gates only whether this channel's learned ladder is
CLASSIFIED, and it defaults to `true`.** It is not a binding gate, and it is not
an opt-in for the output. The distinction matters because the two gates are easy
to confuse:

- `Binding.enabled = false` (§3.5) means "this binding does not match at all", so
  a lower-priority source may claim the gesture. That check belongs to the
  binding and lives in `BindingResolve`.
- `Channel.enabled = false` means "this channel has no learned ladder to compare
  against — do not try to classify its input". The default config's channels are
  named and plausibly idle-referenced but have `count = 0`, which is precisely
  that state. Because `channel_count` (§3.8) decides how many channels are
  SERVICED at all, `enabled = false` is not the way to express "this input is
  absent" — omitting the channel from `channel_count` is.

Gating the binding resolve on `Channel.enabled` instead is a defect with a silent
and severe symptom, and it was live in this firmware until 2026-09-21. A config
written by `ConfigDefault` — which is what a fresh device replies to `config_get`,
and what a headless learn is applied to — has every channel disabled. The
resolver then returned "not found" for EVERY binding, so `SystemOrchestrator`
took §6.6 rule 4's unbound branch and presented the button's own level as though
a stock wheel. A user who had bound `vol_up SINGLE → 2400 mV` got the pass-through
default for that button instead: **a different key voltage, with `KEY_ACCEPTED`
feedback, and nothing anywhere reporting a problem.** It never surfaced as "no
output", which is what made it invisible — the device looked like it was working.

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
- **Resolution takes the FIRST match, and a binding's POSITION is therefore its
  precedence.** `BindingResolve` scans the top-level `bindings` array in order and
  returns on the first binding whose `enabled`/`channel`/`button`/`gesture` all
  match; a later binding for the same triple is never reached. This matters most
  for the `ANY` wildcard, which matches from either channel: an `ANY` binding that
  sits ahead of a channel-specific one for the same `(button, gesture)` wins on
  BOTH channels, and the specific binding is dead. **The app's dispatch must
  mirror this exactly** — it runs the app-side actions of the binding the device
  resolved, not of every binding that matches, or it fires more than the device
  did (two launches for one press). A writer that appends a specific binding must
  therefore place it *ahead* of any surviving `ANY` binding for the same triple,
  or the edit saves, is reported as applied, and does nothing.

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
| `BUZZ` | `pattern` (named, §7.2 spelling — enumerator name without the `k`) | Firmware | Local audible confirmation. **Replaces the default `KEY_ACCEPTED`**, because there is one buzzer and `Play` replaces rather than queues; a pattern name this build does not know falls back to `KEY_ACCEPTED` rather than leaving the press silent |
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

**Integer fields carry integers.** Every field the model declares as an integer
is refused by the codec if it arrives with a fractional part — not truncated.
Truncation is the same wrong-value-accepted class as a wrap: `long_press_ms`
sent as `750.9` would otherwise be stored as `750`, a config the device accepts
and then behaves differently from what was sent. The bound is applied before the
cast (magnitude for `uint32_t`/`uint64_t`, `0`–`3` for the levels via
`ConfigValidate`), and `config_patch` (§4.3) applies the identical rule so the
two writers of a config give one answer to "what is a legal value".

**The bounded timing scalars are bounded in RANGE, not only in width.**
`debounce_ms` and `send_duration_ms` refuse zero; the gesture timings are ordered
against each other; `send_duration_ms` and `maintenance_timeout_ms` each refuse
anything above their ceiling (`kSendDurationMaxMs`, 10 s; `kMaintenanceTimeoutMaxMs`,
1 hour, `ConfigValidate`). The magnitude check alone is not a range check, and both
ceilings guard a hazard the `uint32` maximum (~49.7 days) would create: a
`send_duration_ms` that large is the KEY line DRIVEN for a phantom press the user
cannot release (FR-15/FR-39), and a `maintenance_timeout_ms` that large is "never
closes" — the device-left-unable-to-serve state FR-38 exists to prevent. The lower
end matters too for `maintenance_timeout_ms`: a zero makes the close test
(`now - last_activity >= timeout`) true on the tick the window opens, so
maintenance appears to work and instantly closes. None is a value a user can mean,
so each is refused rather than clamped, the same answer `debounce_ms` gives.

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
  "updated_at_ms": 1700000000000,
  "settings": {
    "debounce_ms": 25,
    "double_press_off_ms": 500,
    "long_press_ms": 750,
    "send_duration_ms": 200,
    "gain_policy": "AUTO",
    "buzzer_level": 2,
    "led_level": 2,
    "temp_comp_enabled": true,
    "maintenance_timeout_ms": 300000
  },
  "aux": [
    { "id": "aux1", "source": 1, "mv_center": 100, "mv_tolerance": 1600 }
  ],
  "channels": [
    {
      "name": "SWC1",
      "enabled": true,
      "ladder": {
        "source": 0,
        "idle_mv": 2835,
        "buttons": [
          { "id": "vol_up", "name": "Volume Up", "mv_center": 1430, "mv_tolerance": 120,
            "learned_at_rail_mv": 3300, "temp_c_at_learn": 23.5, "sample_count": 200, "confidence": 0.98 },
          { "id": "vol_dn", "name": "Volume Down", "mv_center": 1785, "mv_tolerance": 120,
            "learned_at_rail_mv": 3300, "temp_c_at_learn": 23.5, "sample_count": 200, "confidence": 0.97 },
          { "id": "next", "name": "Next Track", "mv_center": 2145, "mv_tolerance": 110,
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
      "enabled": true, "actions": [ { "kind": "APP_LAUNCH", "package": "com.spotify.music" } ] },

    { "id": "b4", "channel": "SWC1", "button": "next", "gesture": "LONG",
      "enabled": true, "actions": [
        { "kind": "APP_INTENT",
          "action": "com.oetsolutions.swc.ACTION_NAVIGATE",
          "data": "geo:40.7608,-111.8910?q=Home" }
      ] }
  ]
}
```

**This example is decoder-verified, and it was not before.** `tools/check_spec_example.py`
decodes it with the firmware's own decoder as part of CI. An earlier revision of
this block described a config **no codec would accept**, in six independent ways:
settings were nested under a `device` object the codec has no field for (spec
§3.1's entity map has no such node); five settings names (`single_press_ms`,
`double_press_gap_ms`, `long_repeat_ms`, `release_margin_mv`,
`usb_protocol_version`) appear nowhere in the code; the feedback levels were
written `"NORMAL"` where the codec's enum is numeric; `updated_at_ms` was missing
and is required (`aux` was missing too, but that one is optional — checked, since
the difference matters to anyone reading this as a template); the channel was
keyed `id` where the codec writes `name`; and `b4` still carried `extras`/`flags`,
which §3.6 had already removed as over-budget. The example is the spec's most concrete assertion about
the wire, so a stale one is worse than none: it is what a reader copies.

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
- **The HAL's NVS return contract is fixed and stated once (IHAL.h).**
  `nvs_set` returns **0 on success, nonzero on failure**; `nvs_get` returns the
  **byte count on success, -1 when the key is absent or the caller's buffer is
  too small**. This is the one part of `IHAL` `EspHal` and `MockHal` must agree on
  exactly, because `EspHal` is the only `lib/` file the host build excludes — no
  native test compiles it, so a divergence is invisible until it runs on
  hardware. It had diverged in **two** places at once: `HalNvsSet` returned the
  byte count where every caller tests `!= 0` (so **every device save looked like a
  failure** — a config or a headless learn could never persist), and `HalNvsGet`
  read a 2048-byte chunk through a 16-byte buffer (see the read-width rule above).
  `tools/check_hal_contracts.py` pins both shapes statically, and
  `test/test_hw/TestEspHal.c` asserts the same contract on real silicon.
- **A chunk is read back at FULL CHUNK WIDTH, never at the header's width.**
  `nvs_get_blob` does **not** truncate: when the caller's buffer is smaller than
  the stored value it returns `ESP_ERR_NVS_INVALID_LENGTH` (and *sets the length
  out-param to the real size*), which is **not** `ESP_ERR_NVS_NOT_FOUND`, so the
  HAL's "absent → -1" mapping turns it into a plain read failure. Reading chunk 0
  into a 16-byte header buffer is therefore a read failure for **every** config,
  not just large ones — `ReadSlot` must read chunk 0 into the full slot buffer
  (the caller's buffer is already sized to the whole blob, so no per-call chunk
  buffer is needed on the device's small stack) and then take the header from it.
  This defect class is invisible to the native suite unless the mock reproduces
  IDF's strictness: a truncating mock makes the same code pass on the host and
  fail on the device. `MockHal::NvsGet` therefore mirrors `nvs_get_blob` — a
  stored value larger than the caller's buffer is an error.
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

**Both may NOT be active at once on this board — only ONE internal PHY exists.**
Espressif's own documentation is unambiguous: for the ESP32-S3, "both controllers
share a single internal PHY, allowing only one to operate at a time", and "by
adding an external PHY, it is possible to enable the simultaneous operation of
both USB-OTG and USB-Serial-JTAG" (ESP-IoT-Solution, *USB PHY/Transceiver
Introduction*; ESP-IDF *USB Host* — both verified 2026-09-20). This board has **no
external PHY**, so **the two are mutually exclusive.**

**What the ESP-IDF documentation says must be read exactly, because it is
counter-intuitive and it decides the design:** "If there is a need to develop a
USB-OTG application using the USB Host Driver or the TinyUSB protocol stack,
**during protocol stack initialization, the USB-PHY connection will
automatically switch to USB-OTG**" (same source). So calling `tinyusb_driver_install`
— which `UsbLinkStart` does unconditionally at boot — **takes the PHY away from
USB-Serial-JTAG.** The consequence is not a lost convenience:

- The console is gone at runtime, so the `ESP_LOG*` output the firmware is full of
  goes nowhere, including `UsbLinkStart`'s own "tinyusb driver install failed" —
  the message that reports the link failing is itself unreadable.
- **ROM download mode stops using USB-Serial-JTAG.** Espressif: "In USB-OTG mode,
  if users wish to utilize the download functionality of USB-Serial-JTAG, they need
  to manually boot into download mode." Whether an automatic reset still enters ROM
  download over the internal PHY is a **bring-up question**, not a settled fact —
  and it is the difference between "reflash by unplugging and replugging" and "open
  the enclosure and hold BOOT + RESET", which matters because §2.2 recesses the BOOT
  button behind a Ø5 lid hole. Verify it against the board (§10.6 step 1); the eFuse
  route (`USB_PHY_SEL`) is one-way and must not be burned to find out.

**The requirement the design actually keeps is the important one**, and it holds
regardless: the app protocol must never share a CDC interface with debug output, so
a stray `printf` can never be parsed as a protocol frame. Today that is satisfied by
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` with the console gone once TinyUSB installs.
The two ways to restore an observable console are a **UART0 console on unused
GPIO43/44** (needs header pins or test pads on a respin) or an external PHY (≥6
GPIOs, worse). Neither is in the current design — so **a production build has no
console**, and §10.5's free-heap gate cannot be instrumented on-device as written.

**This is a hard requirement:** the console must never be configured onto the
TinyUSB CDC port in a production build. §10 tests it — and that test, as written,
asserts a state (both enumerate at once) this board cannot reach.

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

**A committed config takes effect immediately, with no reboot.** "Committed"
means both halves: it is *persisted* AND it is the config the running device now
classifies against. The device has exactly one config, and after a successful
`config_end` (or `config_patch`) that one is the new one — so the next press is
classified, bound and gestured against what the user just saved, not against
what was loaded at boot. Deferring to the next boot is not a permissible reading
of the above: the app adopts the config it pushed as the device's live state the
moment the `ack` arrives, so a stale running config makes the app and the device
disagree about the bindings the user is looking at, and the user sees a saved
binding do nothing until they power-cycle a device the app called configured.

The refresh is therefore part of `config_end`'s commit, not a separate
operation: the same re-derivation `Boot` does for the running config — the
classifiers, the gesture machines, the timings, the feedback levels and the
per-channel output derivation — runs again over the newly committed config before
the `ack` goes out. This is the same rule the headless learn already follows
(§7.4 commits a profile and the device uses it on the next tick). A config the
device *refused* changes nothing, running or stored: rejection leaves the
previous config intact in both places.

### 4.3 Frame types

| Direction | `type` | Payload | Notes |
| --- | --- | --- | --- |
| FW → App | `hello` | `fw_version`, `hw_id`, `protocol_v`, `caps[]` | Sent on connect. **Not on request:** the sole producer is `CommandRouter::OnConnected()`, driven by the host opening the port (DTR asserted); `ping` answers with a `status`, not a `hello` (§4.3's reply row, and §4.5's negotiation reads the envelope `v`, not `hello.protocol_v`) |
| FW → App | `event` | `channel`, `button`, `gesture`, `t_ms`, `level_mv`, `idle_mv` | **The core event.** Fired on every classified gesture. `idle_mv` is the **LIVE** idle the classification normalized `level_mv` against (§6.3's `V_ADC_idle`), so the app reproduces the device's ratio instead of comparing absolute millivolts (N-25) |
| FW → App | `status` | `vbus_present`, `gain_mode`, `config_state`, `output_safe`, `uptime_ms`, `tx_dropped`, `rx_overflows`, `temp_c`, `heap_free` | Periodic + on change. `temp_c` is the last good NTC reading as a decimal (the config codec's `AddTenths` unit), **`null`** when nothing has been measured — never a fabricated `0.0`; `heap_free` is the HAL's free-heap figure at emit time |
| FW → App | `status` (reply) | the fields above **plus `for_seq`** | The answer to `ping`; a `for_seq` names the frame being answered |

**`caps[]` names what this build can ACTUALLY do, and must never over-advertise.**
It is a capability claim a client branches on, so it is a promise, not a
description of the roadmap. It advertised `ota` while the dispatcher nacked every
`ota_*` frame as `not_implemented` (N-14), so a client trusting `caps[]` would
offer an update flow that can never succeed. A capability entry is added in the
same change that makes the corresponding frames work, and the host suite asserts
`caps` against the dispatcher's own behaviour so the two cannot drift apart.

**The periodic `status` carries no `for_seq`; the `ping` reply does.** A
`for_seq` names the frame a reply is answering, so only a reply may carry one.
The periodic keepalive answers nothing, and the firmware used to fill its
`for_seq` from its own outbound counter (`seq_sent_`) — which made an
unsolicited `status` look like the answer to whichever command happened to share
that number. Because the app resolves a pending request on a matching `for_seq`
and both counters start near zero, a keepalive landing between a refused
`config_chunk` and its own `nack` could complete the waiting app first and report
the refusal as **success** — the "config the device is not running" lie the nack
exists to prevent. The reply is distinguishable from the keepalive by exactly
this field, so an app must not treat a bare `status` as an ack.

**`config_state` is the CONFIG's state, and `output_safe` is the output's.** They
were the same field, derived from `SafeIdleEstablished()`, so a device whose
config had fallen back to defaults — the exact case §6.8 requires be reported —
answered `"ok"`, and the fault had no name the app could read. `config_state` is
one of `ok` / `none` / `recovered` / `defaults` (`none` is FR-25's supported
pass-through device, deliberately NOT `defaults`), and `gain_mode` is the mode the
device actually resolved rather than a hardcoded word. The app reads `config_state` and raises a named warning on the link screen for the two fault values (`defaults`, `recovered`); `none` is not a fault.

**`config_state` describes the config the device is RUNNING, so a successful commit returns it to `ok`.** The boot load is how the value is *derived*, not what the field is *about*: `defaults` and `none` both say "the config in force is not the user's", and a `config_end`, `config_patch` or `learn_commit` that persists a validated config puts the user's config in force. Leaving the field latched at the boot value makes the app tell a user who has just re-programmed the device that it "lost its configuration ... program it again" — the report asserting something the device is no longer doing, which is the class of lie this field exists to prevent. The same rule governs the LED: §7.3's reboot-only latch is stated for a *hardware* fault, and a corrupt config is not one, so remedying it clears the blink while a wiring fault latched alongside it keeps blinking.
| FW → App | `ladder_sample` | `channel`, `level_mv`, `n` | Streamed **only during learn mode** |
| FW → App | `maintenance` | `active`, `pop`, `token`, `page_url`, `ble_name`, `ble_failures` | §8.3 option 1's delivery path for the two per-device secrets: the BLE Proof-of-Possession and the web page's token are derived from the MAC and shown **over the trusted USB link**, because the board has no display and no printed label to carry either. Sent on connect and on every change of the window's state, so an app that joins an already-open window still learns it. Every field is empty when `active` is false, so a stale secret cannot be read off a closed window; `ble_failures` is **0 when THIS window's radio came up and nonzero when it did not** — a window-scoped state, not a running total, so an app may branch on it first without a past failure making a later working window claim its radio is down (N-81). It is what lets the app say the radio is not up instead of showing a setup page that is not there |
| FW → App | `ack` | `for_seq`, `ok` (+ `mv_center`, `mv_tolerance` on `learn_commit`'s ack) | Every command is acked. **No `err`:** an ack is `ok:true` and every failure is a `nack`, so an `err` here would name a field no firmware writes (the N-22 shape). `learn_commit`'s ack carries the **derived window** (`mv_center`/`mv_tolerance`, §3.4) because that derivation IS the answer the learn flow exists to return |
| FW → App | `nack` | `for_seq`, `err`, `detail` | Explicit failure, with a machine-readable code |
| FW → App | `log` | `level`, `msg` | Diagnostic line. `level` is a word (`INFO`/`WARN`), matched by the app like `gesture`. **Not gated by a flag:** the one producer is FR-18's clamp warning, which marks a config value the device refused to drive as written, and a warning the user can switch off is a warning they will never see. The app shows received lines on the link screen, bounded to the newest few |
| FW → App | `link_gap` | `channel`, `button`, `gesture`, `expected_seq`, `got_seq` | An inbound frame's `seq` skipped ahead, so a frame was lost. Fire-and-forget, like `event`. **The app must report it** rather than drop it: a lost `config_chunk` or `learn_commit` is otherwise indistinguishable from a command the device refused, and the user's only clue that their cable is dropping bytes |
| App → FW | `config_get` | — | Request the whole config (replied as a chunked run, §4.2) |
| App → FW | `config_begin` / `config_chunk` / `config_end` | total_len+crc32; offset+data; sha256 | The chunked transport that carries **both** `config_get` and `config_set` (§4.2) |
| App → FW | `config_patch` | `path`, `value` | Single-field change, cheaper and less racy — fits one line. The path vocabulary is the **`settings.*` scalars** (`debounce_ms`, `double_press_off_ms`, `long_press_ms`, `send_duration_ms`, `buzzer_level`, `led_level`, `maintenance_timeout_ms`); a path outside that set is refused `unknown_path` rather than approximated. Values follow the same range rule as the chunked codec (`ReadU32`/`ReadU8`), so the two writers of a config give one answer to "what is a legal value" |
| App → FW | `learn_start` / `learn_stop` | `channel`, `button_id` | Drive the learn wizard (§6.4) |
| App → FW | `learn_commit` | `channel`, `button_id`, `name` | Accept the streamed samples as this button. **The device records every `ladder_sample` it streams**, so a single `learn_commit` after the stream commits those samples — they are the accumulator, not a count. (Recorded 2026-09-21: the stream was emitted and never stored, so one `learn_commit` always answered `learn_rejected: too_few_samples` and the flow as specified could not succeed.) |
| App → FW | `maintenance_enter` / `maintenance_exit` | — | Enter/leave maintenance mode (§8.2) |
| App → FW | `test_key` | `channel`, `key_mv`, `hold_ms` | Bench/production test of the output stage |
| App → FW | `identify` | `pattern` (`flash`/`buzz`) | `flash` borrows LED_STAT for the burst; `buzz` sounds the buzzer only. The two patterns do **different** things — a user at the wheel wants the buzz, one looking at the box wants the flash — so routing both to one "flash and buzz" action is a defect (an accepted field that is ignored). An unknown pattern is refused |
| App → FW | `reboot` | `boot_target` (`app`, `bootloader`) | Reset the device. **Both targets are honoured and they select DIFFERENT destinations** (N-80). `app` is an ordinary restart (`IHAL::reboot` → `esp_restart()`). `bootloader` restarts into the ROM USB download loader (`IHAL::reboot_to_download`), so a peer can flash the device over its own cable with no BOOT press — the developer affordance the bench rig needs. A revision refused `bootloader` as unimplementable on the belief that entering the loader was a power-on/BOOT-pin event (§3.2); that is wrong for the ESP32-S3, whose ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`RTC_CNTL_OPTION1_REG` bit 0) on **every** reset — the bit is in the RTC domain, so it survives a software reset but not a power cycle, which is exactly the lifetime wanted. IDF's own `esp_usb_console_before_restart` writes the same bit for its `REBOOT_BOOTLOADER`, and esptool's `ESP32S3ROM.hard_reset` clears it before resetting, so a device asked for the loader is not stranded there. Any other target is refused `bad_target` — an accepted field whose value changes nothing, reported as success, is the shape this refusal exists to prevent. The **app deliberately does not send `bootloader`** (a user-facing app has no business dropping a device into a flash loader); it is a host/bench affordance, reached by `tools/dev_flash.sh` |
| App → FW | `ping` | — | Liveness; FW answers `status` |
| App → FW | `time_sync` | `epoch_ms`, `tz_offset_min` | So timestamps and OTA checks are meaningful |
| App → FW | `ota_begin` / `ota_chunk` / `ota_end` | size/sha256; offset+data; — | USB OTA (§9.3) |

`event` is deliberately **fire-and-forget and never acked by the app**: a button
press must not be held hostage to the app being responsive. The firmware acts on
the local binding first and tells the app second (§6.6).

**`test_key` carries all three of its fields, and `hold_ms` is bounded.** A
handler that reads a frame's `key_mv` while ignoring `channel` and `hold_ms` is
worse than one that refuses them: the app's own test button then measures a
different output, for a different time, than the user asked for, and reports
success. `channel` selects the output (it was hardcoded to 0); `hold_ms` is
bounded at 1000 ms (a hold is time the OUTPUT is driven, so an unbounded value
pins the KEY line, and `now + hold_ms` is a wrap primitive); `hold_ms` = 0 means
the default. Each refusal is a `nack: bad_param`.

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
  Going down stops the periodic status and **discards the link-scoped runs** — a
  half-received `config_set` (§4.2) and an open learn stream (§7.5) are both link
  state, and neither survives a link that has gone quiet. This is what makes an
  interrupted transfer recoverable: the app that died mid-`config_set` leaves the
  device refusing every later config until the cable is pulled, unless silence
  reaps it. The first frame of the next connection re-arms the link, so the reap
  is stateless.
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
| FR-29 | Learn MUST reject a sample set that is too noisy, or that lands within the tolerance of an existing button, and say why. **And it MUST reject a set whose level the classifier would call IDLE** — the learn gate's idle band is the SAME constant the classifier uses (`kIdleMarginPermille`, §6.3's ±30 ‰), not a local copy. A learn gate narrower than the classifier's commits a **dead button**: `LEARN_OK` is reported, the profile persists, and the button never fires because `LadderClassify` returns `kIdle` for its own centre. Two constants for "what counts as idle" is the defect. |
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

**Gain mode should be re-evaluated, not latched:** on head-unit power change
(`/VBUS_VALID` transitions, rail changes) and periodically while idle.

> **v1 LATCHES it, deliberately — see N-20.** `gain_mode_` is resolved once in
> `EstablishSafeIdle` and not re-derived, so the sentence above describes the
> target rather than this build. A runtime re-selection needs a debounce on
> `ADC_CH_KEY_SENSE1` first, because a press's own transient on that pin would
> otherwise re-select gain mid-key, and choosing that debounce is a measurement
> on the real ladder. The safety posture is unaffected: the initial selection is
> the conservative one (1.82 unless the line measures below 2.6 V), and latching
> cannot move a channel toward the dangerous over-range direction.

**Command targets must stay inside `[V_OUT_floor, V_KEY_idle − 0.20 V]`** so the
sink FET is never asked to drive above the line's own resting level — above that
point the servo can only turn `Q4` off, which is the release behavior, not a
command. `V_OUT_floor` is §6.2's own output floor, **1.80 V** — the DAC envelope's
low end, below which the servo has no authority — so the band is the set of values
the servo can actually reach *and* that sit below the line's rest.
(`GainPolicyClampCommand` is the one home for this band; an earlier revision wrote
the lower bound as a bare `min_ladder` that no code defined — see N-32.) When
`V_KEY_idle − 0.20 V` falls below the floor the band is **empty** and there is no
command to make: the device releases and reports the press rather than driving a
level outside the band, because a clamped-to-the-floor target would be a guess
FR-12 forbids.

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

> **`V_ADC_idle` is the LIVE idle, not `learned_idle_mv`.** The denominator must
> be maintained at runtime (seeded at Boot from the live rail idle and re-adopted
> as the rail moves), because a denominator pinned to the *learned* idle is the
> learn-time rail — so `n` drifts with the rail's own ±5 % deviation instead of
> cancelling it. Two concrete failures follow, both found 2026-09-22:
>
> 1. **An idle-adjacent button is lost at the band edge.** `next` at 757 ‰ (the
>    profile's most idle-adjacent button, and FR-6's named worst case) drifts to
>    ~792 ‰ at +5 % rail — just outside its ±39 ‰ window — so a real press is
>    reported `UNKNOWN`.
> 2. **A healthy rail is mis-reported as a fault.** At +5 % the live idle is
>    ~1050 ‰ of the learned idle, above §6.3's +30 ‰ idle margin, so
>    `LadderClassify` returns `kFault` — the "short to a supply" case — on a rail
>    that is merely high. The same inertness disables the rail-sag check below.
>
> The orchestrator seeds the denominator from the live reading **only when that
> reading is within the ±5 % rail tolerance of the learned idle** (950–1050 ‰), so
> a button held at power-on — far below the rail — cannot become the reference.
> It then re-adopts settled readings within `[1000−60, 1000+30]` ‰: wide enough to
> follow a gradual rail move, narrow enough to reject a step to a press or a
> short. The FR-30 sag check then works as written, because its reference argument
> (`V_ADC_idle`) and `profile.learned_idle_mv` are now genuinely different values.

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
- The NTC is **NOT** read in this build, and this line used to claim it was
  ("The NTC is read and reported regardless"); that was false and is corrected
  here. `ADC_CH_TEMP` is mapped to `ADC_CHANNEL_6` in `EspHal.cpp` and is read by
  **nothing**: every `adc_read_mv` call site names SWC1/SWC2, AUX1 or KEY_SENSE1/2,
  and both `LearnSession::AddSample` callers pass a literal 0 for
  `temp_tenths_c` (`kTempNotMeasuredTenths`, `SystemOrchestrator.cpp`). There is
  also no NTC-to-temperature conversion anywhere in the tree -- no B3380/
  Steinhart routine, no divider inversion -- so the raw millivolts would not be a
  temperature even if they were read. A bring-up session therefore cannot
  *measure* the drift from the device as shipped: it would need to read the NTC
  off the board directly, or wait for the sampling path of open item N-67. FR-1's
"sample ... the NTC continuously" is unmet for the same reason.

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

**The report is a boot-degraded condition, not a silent log.** A blank eFuse is the
same class as a recovered config (§6.8): the device runs, but with knowledge it did
not have. It is announced as `BOOT_DEGRADED` (§7.2) — the one boot signal a user
at the bench can hear with no host attached — alongside the init-time device log
that names the cause. It is deliberately NOT a `status` field: §4.3's payload is
fixed, and a fault the user can only read over a link fails the same way a
silent fallback does for the no-app install the spec is built around. (An
app-visible `log` frame would need a link, and no link exists at boot; the
device console log is the init-time record.)

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
   per-device one — **and "per button" is load-bearing, not a figure of speech:**
   the bindings consulted for a press are the ones whose `button` field names the
   button *being pressed*. Scanning the channel instead (any `DOUBLE` anywhere on
   it) adds the double-press window to every press on that channel and can emit a
   `LONG` for a button that binds neither. The implementation reads the pressed
   button's own bindings each tick, because the button is not known until the
   level is classified.

   **A `DOUBLE` is the SAME button pressed twice inside the window.** Two
   *different* buttons pressed in quick succession are two separate presses: the
   first resolves as its own `SINGLE` and the second starts a fresh press. Folding
   them into one `DOUBLE` would send the *second* button's double-press command
   while swallowing the first button's press entirely — the driver taps `vol_dn`
   then `next` and the radio acts on `next`'s `DOUBLE`, a command nobody asked for.
   That is the wrong-command hazard rules 1 and the §6.7 safe-idle property exist
   to prevent, so the button identity is part of the double's definition, not an
   afterthought.

`SINGLE` is therefore not "delayed by design" — it is delayed by exactly the
ambiguity that its button actually has, and no more.

**4. A recognised button whose gesture is not bound PASSES THROUGH.** "Unbound"
is `BindingResolve` returning `found == false` (§3.5) — no `Binding` in the
top-level table names this `(channel, button, gesture)`. The device then behaves
as a **stock wheel**: it presents that button's own level, one bounded pulse by
ratio (§6.9's mapping), and reports `event` as usual. It is the same rule for
`SINGLE`, `DOUBLE` and `LONG`, because the head unit is gesture-blind — it cannot
tell them apart, so all three present the same button, which is exactly what the
unmodified wheel does.

This is the default the 2022 design shipped (`lookup_single/double/long_press_val`
always mapped the key's own value; only `program_alt_key` overrode it), and it is
what makes the no-app product work at all. **A fresh device learned by AUX1 alone
has learned windows but an empty binding table** — `ConfigDefault` ships
`binding_count = 0`, and every runtime binding otherwise comes from a config the
Android app pushes. So a learned button with no app-pushed binding reaches here.
Before this rule was stated, such a press resolved to nothing: the line was
released and `KEY_UNKNOWN` played. The learned ladder **never drove a key**, and
the failure was invisible because the learn itself beeped `LEARN_OK`.

**Two things this rule is NOT**, because both would be the guess FR-12 forbids:

- It does not apply to a level matching **no learned window** at all. That is
  still FR-12's unknown: reported with a null `button`, released, and
  `KEY_UNKNOWN` played. The window matched first; the binding is what is absent.
- It does not apply when there is **no usable head-unit idle** to map the ratio
  onto. A fabricated denominator would land every press on a key nothing defined,
  so the device releases rather than drives — the same direction §6.9 takes.

`enabled: false` on a binding is different from absent and is handled before this
rule: a disabled binding is *not a match*, so the gesture falls through to this
pass-through default, exactly as `BindingResolver.h` describes ("a disabled
binding lets a lower-priority binding match" — and the stock wheel is that
lower-priority source).

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
| Head unit disappears (VBUS off) | `/VBUS_VALID`, or `V_KEY_idle` outside §6.2's 1.80–5.20 V envelope | Release the KEY line immediately; report `vbus_present: false`; keep classifying. **This response does NOT latch.** A head unit may sleep, suspend, or reboot at any moment (§4.4) and come back, so the indication must clear by itself when the line returns — `LED_STAT` keeps its normal state and no `FAULT_*` sounds. The reboot-only latch of §7.3 belongs to the **ladder's** own out-of-range (FR-4), which is a wiring condition. Folding the envelope test into that latch leaves the lamp blinking forever the first time the radio powers down, reporting a fault the device is not in |
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

**`wheel_idle_mv` is PER CHANNEL.** §3.4's two SWC inputs are independent wheels
(FR-9), and their idles need not match — so each channel's ratio is taken against
*its own* captured idle. A single device-wide reference makes one channel's idle
the other's press threshold: two channels idling 465 mV apart (a different wheel
ladder) drive a phantom key on the lower-idle channel with nothing held, and a
disconnected input reading ~0 does it unconditionally. Capturing and healing the
reference per channel is the same rule FR-3's filter and the classifier already
follow. Consequently **"disabled rather than guessed" is per input**: a channel
whose own ladder was unreadable at Boot serves nothing while its healthy sibling
still passes through — one dead wheel must not disable the other.

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
representable. The grammar (`pattern := pulse(on_ms, off_ms), repeat, gap_ms`)
expresses a **uniform** pulse train — `BuzzerGrammar::StepsFor` returns one
`{on_ms, off_ms, reps}` per pattern and the playback modulates that single
interval — so there is no way to make the second pulse longer than the first, and
an earlier revision of this paragraph claiming OTA_OK's second pulse is "longer"
described a shape the encoder cannot emit. `OTA_OK` is therefore a plain
**uniform** double (150/100 ×2): "rising" is dropped and the double is
distinguished from OTA_START's long single (400/0 ×1) by its longer *total*, not
by an intra-pattern interval change.

**Design rules:**

- **`PROGRAM_STEP` is one rep, and callers repeat it.** §7.5 announces the
  *n*-th gesture with "*n* beeps" (the count IS which gesture was heard), which
  reads as a conflict with this
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

**The one exception is the config-fault blink, which a successful commit clears.**
The reboot-only rule above is stated for hardware conditions, and a corrupt config
is not one: it is remedied by writing a valid config, which is exactly what `§4.2`
makes `config_end`/`config_patch`/`learn_commit` do. Keeping the blink latched
after a commit would have the device still signalling "not OK" over a config it is
successfully running — the same false report `config_state` is fixed against. A
hardware fault latched alongside it is a separate fact and keeps blinking; that the
two share a lamp is the reason the state word, not the LED, is what names the fault.

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

**The AUX1 + buzzer/LED path below is the PRODUCTION path, and the only one that
ships (FR-31).** Hold AUX1 to enter, then do the gesture on the input you are
programming, then release AUX1 — no phone, because the user may not have the head
unit out of the dash. **The Android app has no learning screen** (a hard product
requirement, and the reason this is stated first rather than as a footnote), so an
app-driven learn has no UI. That is a deliberate choice, not a gap: the learn
interaction is a hold-and-press on the device itself, which needs no graphing to
complete, and an app screen that could drive it would be a second, divergent write
path for the one operation whose failure mode is a persisted config.

**The protocol still carries the app-driven learn frames** (`learn_start` /
`learn_stop` / `learn_commit`, §4.3; `ladder_sample`, FR-5), and the firmware
implements them so a host and the bench tooling can drive a learn. What the
shipping app does NOT have is a screen that uses them. The two paths share one
commit implementation (`CommandRouter::HandleLearnCommit` and the wizard both
build a `LadderProfile` and hand it to the same apply-and-save), so a config they
can disagree about is not a hazard — but the app is not the way a user is expected
to learn a button, and an earlier revision of this paragraph calling the app path
"primary, with live graphing" and the AUX1 path a "fallback" was the exact
inversion of both the requirement and the code (see N-47).

**Per-button learn loop, with the fallback prompts:**

```
 1. Arm learn        BEEP PROGRAM_ENTER · LED_STAT alternate, LED2 off
                     (AUX1 held past 1.5 s — spec 7.5)
 2. Name the input   NO menu and NO selection. The wizard watches every input
                     (SWC1, SWC2, AUX2, AUX3) and the one that LEAVES ITS IDLE
                     is the one being programmed — exactly how the 2022 firmware
                     named its key (`is_key_pressed()`, spec 7.5).
 3. Prompt           BEEP LEARN_PROMPT · LEDs alternate (the input is named; the
                     level is being measured)
 4. User holds the physical button on that input
 5. Sample           ≥ N samples over ≥ T ms; compute mean, spread, rail voltage
 6. Commit           On the AUX1 RELEASE (spec 7.5): BEEP LEARN_OK (or LEARN_REJECT
                     if a gate refuses — step 5's validation), store the button
                     into that input and persist.
 7. More inputs?     repeat from 1 (each programming action is its own hold).
 8. Idle             BEEP PROGRAM_EXIT · LED_STAT solid
```

**The input is named by WHICH LINE MOVED, not by a menu — this is the load-bearing
correction to this loop.** An earlier revision counted AUX1 presses to pick the
*n*-th button slot (step 2 above), and `SystemOrchestrator::learn_channel_` was a
constant `0` read in five places and assigned nowhere — so on a two-channel
install every headless-learned button landed on SWC1 and SWC2 was learnable by no
shipping means (the app has no learning screen, so there was no fallback; open
item N-23). The user's own description of the 2022 interaction settles it:
*"You just hold the aux1 button and then do the gesture on whichever swc_in switch
you're working with. No complicated menu or selecting which one, just do it. It
can easily be detected by seeing which one changes from idle."* It can, and the
2022 firmware did exactly that — `is_key_pressed()` set `key = KEY1`/`KEY2` from
whichever input's average had left its baseline. One hold therefore programs ONE
input; the slot-numbering menu is gone, and a new button takes the next free
`swc<ch>_bt<n>` derived from the channel's own ladder.

**The user's own gesture during a learn is NOT delivered to the head unit.**
While the learn is armed, no press drives the radio — the 2022 firmware guarded
every transition to `SEND_KEY_VALUE` on `!is_program_button_pressed`, and the
reason is the same here: the user holds AUX1 to program, so a button pressed
during that hold is being TAUGHT, not driven. Delivering it would be the
phantom-key hazard FR-39 exists to prevent, arriving from the one path whose whole
purpose is "nothing is being pressed for real".

**An AUX input (AUX2/AUX3) is a different KIND of learn target.** It is a switch,
not a ladder, so its learn records the switch's own window — a centre plus a wide
half-rail tolerance — written into that input's `aux[]` config entry rather than
appended to a ladder. A switch shorting to ground reads near 0 mV, which
`LadderProfileIsValid` refuses as a `mv_center`, so the switch path bypasses the
`LearnSession` ladder gates and applies the classifier's own at-idle margin
(`kIdleMarginPermille`) directly: a light touch that barely left idle would
otherwise store a centre the classifier calls idle, and the switch would be dead
with `LEARN_OK` feedback.

**A learn ADDS to the channel's ladder; it never replaces it, and re-learning a button corrects that entry in place.** Normative, because the failure was total and silent:

- **The learn's output is assigned over the channel's ladder.** `ApplyLearnedProfile` does `config_.channels[ch].ladder = profile`, so a session that began from an empty profile DELETES every button the channel already had. Measured before this was fixed: a user who learned three buttons and then re-entered the wizard to re-measure **one** of them ended with **one** button, and the other two gone — with `LEARN_OK` feedback and nothing anywhere reporting a loss. The session must therefore be SEEDED with the channel's current ladder, which the caller supplies (the wizard holds no `Config`).
- **Re-learning a button replaces that entry rather than appending a second one.** Two buttons sharing an id is rejected nowhere — `ConfigValidate` does not check id uniqueness — yet `BindingResolve` and `BindingsForButton` both look a binding up by `strcmp` on the id, so a duplicate makes "which window does this binding mean" ambiguous between two different voltages.
- **The entry being re-learned must be EXCLUDED from the neighbour set.** Because the seed contains the button being corrected, and a re-measure lands inside that button's own old tolerance by definition, leaving it in makes the ambiguity gate (`too_close_to_existing`) fire on the button being fixed — the user holds exactly the button they asked to fix and is told it is too close to **itself**, so a button could never be corrected. With no slot menu there is no id to match on, so the exclusion is by **VOLTAGE**: the measured level selects the seeded entry whose window contains it, and that entry is dropped from the neighbour set. `LearnSession` compares voltages only and takes the already-adjusted set; the voltage-to-id decision stays with the caller (the id is the *outcome* of the match, not its input — a new button is named from the channel's own ids).
- **A re-learn on a moved rail must REBASE the seeded buttons into the live frame.** A `LadderProfile` carries ONE `learned_idle_mv`, while every button's `mv_center`/`mv_tolerance` are ABSOLUTE millivolts measured at that one rail (§3.4), so the profile is consistent only while all its buttons share a frame. A learn measures the new button on the **live** rail (§6.3) and stamps the live idle as the denominator, so a seed whose rail has since moved would otherwise hold two frames under one denominator — and this is silent, exactly like the failures above: nearest-centre matching still returns *something*, so the press lands in whichever window it now falls inside and the **wrong button fires**. Measured through the real learn path: two buttons 200 mV apart learned at 2835 mV, one re-learned at 2693 mV (a −5 % regulator deviation, inside the documented band) — pressing the other reported the 794 ‰ button when the press was at 864 ‰, and its own window was no longer reachable. Both the seeded sibling set the gates compare against and the profile that gets stored must be rebased; `LadderProfileRebase` is the one home for the transform, and it scales `mv_center` AND `mv_tolerance` so every permille window is unchanged to within one permille — the quantization of writing whole millivolts, which can tip only a profile whose centres are a few millivolts apart and is therefore unreachable from a real learn, whose tolerance derives from the measured spread and the nearest-neighbour gap.

**A learn's write must not fall back to defaults for a config that is merely UNREADABLE.** `ConfigLoadResult` distinguishes the four cases for exactly this reason, and a write path that collapses them loses data:

- **`kNoConfig`** — nothing was ever stored. Defaults are correct: that is the pass-through device a learn is meant to configure, and `config_get` already answers with `ConfigDefault()` for it.
- **`kFellBackToDefaults`** — something IS stored and could not be read (both slots corrupt, a CRC failure, a schema this build cannot interpret). Substituting defaults here and then SAVING destroys the user's entire config — bindings, the other channel, every setting — and reports success. Measured before this was fixed: three bindings replaced by zero from a single `learn_commit`, with an `ack`. The correct behavior is to **refuse and report** (`nack: config_unreadable`); the fault is the thing the user needs told.
- **`kRecoveredFromBackup`** — the older slot is intact. Using it is correct, and `Load` has already done so.

The same rule holds for **every** path that reads-modifies-writes a config, and
`config_patch` was the second place it was violated — an earlier version of this
sentence claimed that path was already correct, and it was not. It refused an
invalid *result*, which is a different check from refusing to build the patch on
an unreadable *input*: `if (Load != kLoaded) c = ConfigDefault()` made a patch
over a corrupt config write defaults plus one field. Measured: three bindings to
zero, with an `ack`. It now refuses `config_unreadable` exactly as a learn does.

**Rejection reasons are spoken aloud as distinct rhythms**, and MUST NOT be reported as persisted unless it was (`store.Save` returned true).** Both are normative, not implementation notes, because both fail *silently and late*:

- **A profile the validator refuses loses the whole config.** `ConfigStore::Save` encodes and writes; it does **not** validate (the `config_end` comment claiming `ConfigEncodeBlob` refuses an invalid config was wrong, and is corrected there). The validator runs at the end of every `ConfigDecodeBlob`/`ConfigDecodeJson` — i.e. on the way *in*, never on the way out — so an invalid profile is written happily and then, at the next boot, `ConfigStore::Load` refuses the slot, falls back to defaults, and the user loses **every button they ever taught**, reported only as a corrupt config. Reachable in practice rather than theoretically: `mv_tolerance`'s floor (below) runs *after* its cap, so a noisy learn can push a window past the cap and into its neighbour. The check belongs where the profile is chosen, against the profile the button would *complete* — `LadderWindowsAreDistinguishable` (§3.4) is the shared predicate, so the validator and the learn cannot drift. The rejection is `LEARN_REJECT`/`too_noisy`.
- **A failed save reported as success is a broken promise.** "Persisted" means the bytes are in NVS, so it must be `store != null && Save(config)` — not `store != null`. A full NVS, a write error, or a store that refused the config all report LEARN_OK otherwise, and the user finds the button gone after a power cycle with nothing having warned them.

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

**Read that sentence literally, because it describes the 2022 interaction and not
a modal menu.** In the 2022 firmware the modifier was the BOOTSEL button, read
once per poll (`is_program_button_pressed = bootsel_button_is_pressed()`); the
user **held the modifier**, performed the gesture on a wheel key, and released.
The state machine reached `PROGRAM_ALT_KEY` when a wheel key's double- or
long-press completed *while the modifier was held*, and called
`program_alt_key(curr_key_val, curr_key, is_double_press)` — capturing **(level,
button, gesture)** in one operation. The user's own restatement is exact:
*"You just hold down the button, do the gesture, and then release the button."*

**On this board the modifier is AUX1, not BOOT**, for the reasons below; the
interaction is otherwise carried forward unchanged.

**Why AUX1 replaces BOOT as the modifier:**

- **BOOT (`IO0`) is recessed** behind a Ø5 hole in the lid, and is a *strapping
  pin* (§2.2) — a hold-at-power-on means ROM download mode, not a user action.
- **AUX1 is a real, reachable analog input** on the `J5` terminal with its own
  conditioning (§2.4). The user can wire a momentary button to it and reach it.

```
PROGRAMMING (modifier: AUX1 held)
  1. Hold AUX1.                    BEEP PROGRAM_ENTER · LED_STAT alternate
  2. Press the wheel button to     its gesture is now being assigned
     program, and while STILL      (the button must be learnable — §7.4 —
     holding AUX1 do the gesture:   and its id is what the binding names)
       single press → SINGLE   (1 beep)   ┐
       double press → DOUBLE   (2 beeps)  ├ each press re-beeps PROGRAM_STEP,
       hold ≥ long  → LONG     (3 beeps)  ┘ the count IS the gesture
  3. Release AUX1.                 BEEP PROGRAM_SAVED · (the binding is written)
  4. Repeat from 1 for more, or do nothing — there is no modal state to leave.
```

**The escalating beep count is the gesture**, which is how a fixed-pitch buzzer
(§7.1) conveys which of the three the device heard — the count is the menu depth,
and counting is clearer under road noise than the 2022 design's rising pitch.

**There is no modal wizard, and that is the substantive correction this section
needs.** An earlier revision of §7.5 described a modal `PROGRAMMING MODE` entered
by an AUX1 hold and left by "AUX1 held ≥ 1.5 s again". That flow was unreachable
as written and collided with §7.4: `LearnWizard::kEnterHoldMs` is 1500 ms and the
maintenance window opens on the same hold reaching `kMaintenanceHoldMs` (3000 ms),
so a *second* 1.5 s hold can never fire before maintenance escalates — one 1.5 s
value cannot serve both entry and exit. The held-modifier interaction has no such
problem: it is **stateless from the user's side**, so there is nothing to enter,
nothing to leave, and nothing for the maintenance hold to collide with.

**What a freshly-assigned gesture binds to is the pass-through default (§6.6 rule
4), not a special case.** The on-device flow's job is to *learn the button*; what
its gestures then DO is a `Binding`, and a button with no binding presents itself
as a stock wheel. So a user who learns a button on AUX1 alone gets a working
button immediately, and binds a *different* action later (from the app, §3.6) —
which is the honest division of labour: the beeps handle "teach this button", and
the app handles the long tail of actions no fixed-pitch buzzer can cycle through.

**The full action library is far too large to cycle through by beeping**, so the
on-device flow does not attempt it. `APP_INTENT`, `APP_LAUNCH` and the rest of
§3.6's kinds are the app's interface; the device's own contribution to the no-app
case is the pass-through default above.

**Every pattern named in this flow is a §7.2 row.** That is the check worth
applying to any future edit here: if a step names a pattern, the pattern must
exist in the table above with the timings this step implies.

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
| USB command `maintenance_enter` | Primary, from the Android app. **Wired** |
| **AUX1 held ≥ 3 s** | The no-app fallback — deliberately longer than the 1.5 s programming hold so the two gestures cannot be confused. **Wired** |
| Config flag on next boot | For a user who wants it up immediately after flashing. **Wired** — `settings.maintenance_on_boot`, consumed and persisted by `Boot` so the window opens on the ONE boot the user asked for (N-13) |
| Reset-reason + no-config | First-ever boot with no config offers provisioning. **Not reachable in this build: nothing reads the reset reason** (N-13) |

The BOOT button is **not** a maintenance trigger: it is recessed behind a Ø5 lid
hole and is a strapping pin, so a hold-at-power-on would mean ROM download mode,
not a user action (§2.2, §7.5). AUX1 is reachable and is not a strapping pin.

The two holdings are deliberately distinct and nested:
**1.5 s = programming**, **3 s = maintenance** — the shorter is a subset of the
longer, so holding too long to program escalates cleanly into maintenance rather
than into an undefined state.

Exit happens on:
- an explicit `maintenance_exit` command,
- **timeout: `maintenance_timeout_ms` after the last activity** (FR-38; default 5
  minutes) — a device left unable to serve button presses because someone opened
  a web page is unacceptable. **The activity source now exists** (N-15 resolved): the
  HTTP server bumps the clock on every request, so a window being used is not
  reaped mid-task. The length is a setting, bounded to `(0, 3600000]` ms and
  changeable live via `config_patch`, so "5 minutes" is only its default.
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
POST /api/ota/upload   → raw image body + X-SWC-Sha256 / X-SWC-Size headers (the WiFi OTA path)
POST /api/ota/check    → check git releases for a newer version (§9.5)
POST /api/ota/pull     → download and install a release asset by URL
POST /api/reboot       → reboot into the new image
```

Every endpoint requires the token, in the `X-SWC-Token` header (or the `token`
query parameter on the page's first load, which cannot send a header before the
script has loaded).

**`/api/ota/upload` carries a raw body, not a multipart form.** The shared gate
(§9.4) needs the declared size and SHA-256 *before* it writes a byte, so both
travel in headers — `X-SWC-Size` and `X-SWC-Sha256` — and the body is the image
itself. The page computes the digest in JavaScript (a small SHA-256, since
`crypto.subtle` is undefined outside a secure context and this page is plain
HTTP on the device's own AP). A multipart form cannot convey a field the
firmware reads without a multipart parser on the device, and removing that
parser removes the one input-handling routine in this flow with no other
consumer.

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
2. Semver-compare `latest_version` against the running version. The comparison is
   **semver 2.0.0 rules 10 and 11**, and the two are easy to half-implement: rule
   11 puts a pre-release *below* its release (`1.3.0-rc1 < 1.3.0`), and rule 10
   makes **build metadata (`+suffix`) irrelevant to precedence — on a pre-release
   as well as on a bare release** (`1.3.0-rc+build == 1.3.0-rc`). Metadata that is
   stripped only when it leads the string is the half-done version: `1.3.0-rc+b`
   then keeps `+b` inside the compared pre-release and reads as *newer* than
   `1.3.0-rc`, so the check offers an update to the same version (or, reversed,
   never offers a genuinely newer one).
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

> **Both workflows were created under `code/.github/workflows/` and moved to the
> root on 2026-09-23 (N-33).** This passage is the spec's own statement of the
> rule, and the plan repeated it, yet Task 22's "Files" block named
> `code/.github/workflows/…` and the trees were written exactly there — so **no
> gate in §10.5 had ever run**: not the native suite, not the contract-sync diff,
> not the size gate, not `check_stack_usage`, not the Android JVM suite. The
> workflows' own steps were written with `working-directory: code` and
> `git diff -- code/contract …`, i.e. paths that only resolve from the repo root,
> which is what confirms the placement was an error rather than a decision. A CI
> gate that never executes is the same class as the device-only blind spots
> (N-17, N-18, N-30): everything it guards was verified by hand and reported as
> covered. The move is the whole fix — the workflow contents needed no change
> beyond their own paths' now being correct.

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
- **USB enumeration** — the CDC interface enumerates on the cable and carries the
  app protocol. **Do NOT assert that the console enumerates at the same time**: the
  S3 has one internal PHY and installing TinyUSB moves it to USB-OTG, so the
  Serial-JTAG console is gone once the link is up (§4.1). The real checks are
  (a) the CDC interface carries a well-formed frame, and (b) **whether a
  reset still enters ROM download mode over USB** — which is a question, not a
  given, and the one that decides how the board is reflashed in the car.
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
| App fits the slot | `pio run -e esp32s3` then `tools/check_size.py` | ≤ 1920 KB (§9.2) — **the highest-risk gate** |
| Free-heap headroom | runtime assertion in the device test | ≥ 20 % free at worst-case steady state |
| Config fits NVS | `ConfigCodec` size assertion: worst case > **4000 B per value** (so the chunked path is exercised, not dead code), and **two slots + `cfg_seq` ≤ 48,384 B** of entry space at **2,112 B per 2048-byte chunk** — counting NVS's metadata and `BLOB_IDX` entries, not just the payload |
| Contract in sync | `tools/gen_contract*.py` then `git diff --exit-code` | No diff |
| Cross-language codec agreement | `tools/crosscheck_config.sh` | The app's `contract/swc_sample_config.json` decodes with the firmware's own decoder, validates, and re-encodes **byte-identically** (exit 0). This is the only gate that can see a Kotlin/C codec divergence: each side's own round-trip test compares that side to itself |
| Android builds | `./gradlew assembleDebug test` | Clean, tests pass |
| App limits match the firmware | `tools/check_app_limits.py` | Every count, string width and timing ceiling read from BOTH sides and equal. Its absence let the firmware grow a `send_duration_ms` ceiling the app never got, so a save the app accepted was nacked at decode with the field unnamed |
| App validator parity at rule level | (open — see N-40) | The rule set, not just the numeric limits |
| Device tests actually LINK | `pio test -e esp32s3 --without-uploading --without-testing` | The device suite had been a multiple-definition error that nothing compiled, so the whole "D" column of the coverage matrix was unrunnable |
| HAL contract | `tools/check_hal_contracts.py` | The NVS + ADC-read contract the host build cannot compile (EspHal is excluded from `native`) |
| Task ownership | `tools/check_task_ownership.py` | Every protocol handler on the poll task; USB callbacks on none |
| Stack budget | `tools/check_stack_usage.py` | `sizeof(Config)` by value on a task stack is a guaranteed boot panic |
| sdkconfig keys | `tools/check_sdkconfig_keys.py` | Every key the firmware reads exists in `sdkconfig.defaults` |
| Every frame has a handler somewhere | `tools/check_frame_handlers.py` | All three directions: every `fw2app`/`both` frame is referenced by an app source outside the generated contract; every `app2fw`/`both` frame is in the router's `IsKnownCommand`; every accepted name reaches a real dispatch branch. A frame emitted and never handled is a message sent into a void — `link_gap` sat in that state for a revision (N-46) |
| Contract example still decodes | `tools/check_spec_example.py` | The §3.7 worked example round-trips through the real codec |

**If the app does not fit 1920 KB**, the documented fallback is §9.6's separate
maintenance image — decided now, so a size blowout is a known trade, not a
mid-project emergency. The gate exists so that is discovered in CI, on day one,
rather than the week the boards land.

**The gate must measure the PRODUCTION image, and it must be able to tell.** A
PlatformIO device build and a device test build (`pio test -e esp32s3`) write to
the SAME `.pio/build/esp32s3/firmware.bin`, so whichever ran last is what the gate
sees. CI's ordering — build, run the test link, then size — therefore measured the
**test** image (287 KB) instead of the production one (386 KB): it under-reported
by ~99 KB and would pass a production image ~99 KB over its slot. The two images
are distinguishable without running anything (the test image carries the Unity
runner and none of the app's own symbols), so `check_size.py` refuses to report a
number unless the artifact it read is a production image — a gate that cannot tell
what it measured is a gate that can pass a wrong answer. The CI ordering is also
fixed so the production build is the last word before the measurement.

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
   write a mid-code, measure with a meter. Confirms §2.5's wiring and that the
   code **latches on the frame's own ACK** — `UDAC = 0`, so no `~LDAC` pulse is
   asserted (§2.5.1), and a mid-code that appears at the pin proves both the
   three-byte frame and the latch behaviour on real silicon.
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
| FR-1 | Non-blocking acquisition test | N + **B** | **N:** idle ticks are silent and cheap, and no HAL call in the poll path blocks (the HAL seam takes no locks and the CDC write is a FIFO push that returns the count accepted). **B:** the cadence under a real 20 ms host stall — an earlier revision of this row claimed a host test with injected stalls, and no such test exists. **Note (2026-09-24):** this row covers both clauses now. The first clause -- "sample both ladder channels **and the NTC** continuously" -- was unmet until this date (`ADC_CH_TEMP` was read by nothing, and there was no NTC conversion; see N-67, now closed). `SystemOrchestrator::SampleNtcTenthsC` reads the channel and `Ntc::NodeMvToTenthsC` inverts the schematic's divider (+3V3 -> R29 10k -> node -> RT1 10k B3380 -> GND) and the B3380 model; the result reaches `temp_c_at_learn` on BOTH learn paths. Asserted by `NtcConvert` (8 tests, against the datasheet model) and `SystemOrchestrator.TheNtcIsConvertedAndItsTemperatureReachesTheLearnRecord`. **Fully satisfied 2026-09-24 on the bench:** the learn path alone did NOT meet "continuously" -- a device merely serving the wheel never read `ADC_CH_TEMP`, so `status.temp_c` was permanently `null` (measured: `null` at `uptime_ms` > 1.1 M). `Tick` now samples on a fixed ~1 Hz cadence (`kNtcSampleIntervalMs`; 1 Hz because the NTC tracks thermal drift, 100 Hz would add an ADC conversion to the 10 ms loop for no information), and the DUT reports `temp_c: 26.7` -- a real room temperature -- where it was `null`. Pinned by `TheNtcIsSampledInNormalOperationNotOnlyDuringALearn`, mutation-tested by dropping the call and by sampling once without rescheduling |
| FR-2 | `CalibrationCurve` + ADC linearity | N + **D** | **N:** endpoints map exactly, monotonic across the full raw range, never above the 2.9 V ceiling for this attenuation, midscale within 25 mV of half-scale, and a blank eFuse falls back to the linear curve *and reports which source it used*. **D (not yet run):** the curve-fit *accuracy* against a bench reference — it needs the real eFuse, so it cannot be a host test, and an earlier revision of this row claimed a host test for it. **Note (2026-09-21):** the N cells above describe `CalibrationCurve`, and they passed while `EspHal` — which the native build excludes — created the eFuse handle and then converted with the fixed linear line instead of `adc_cali_raw_to_voltage()`, so the host tests exercised a curve never applied on the device. The device now uses the eFuse value when the handle exists and the linear fallback only for blank-eFuse parts; the accuracy of that path is still bench-only |
| FR-3 | Filter settling test | N | Step response settles in < `debounce_ms`, and a 20 ms press is not attenuated below the detection threshold. **Note (2026-09-21):** this row proves the filter *can* settle within `debounce_ms`; it does not prove the classifier waits for `Settled()` before reading a value. As written, `ServiceChannel` classifies on `Value()` unconditionally (the `Settled()` gate is unread outside its own test). Whether that matters is a bench question — the classifier's own `debounce_ms` is expected to cover the single mixed tick a real press produces — so this is recorded, not changed |
| FR-4 | Fault-injection tests | N + D | Open input and a short-to-rail each release the KEY line and latch `LED_STAT` blink rather than classifying as a button. **Not** a `FAULT_*` buzzer: no such pattern names this subsystem (N-10) |
| FR-5 | Live-sample stream test | N + **D** | **N:** `ladder_sample` is emitted while a learn run is open and stops when it closes, and the frame carries the run's channel. **D (not yet run):** the ≥ 20 samples/s cadence and its latency — that is a property of the poll loop's real rate, which the host clock cannot show |
| FR-6 | Classification + hysteresis test | N | A level inside the window's outer edge twice in a row does not re-trigger; each learned button classifies across the **+3V3** tolerance band (3.14–3.47 V, §6.3), and an idle-adjacent button — the worst case for separation, per §6.3 consequence 2 — still resolves |
| FR-7 | Gesture tests | N | Each of SINGLE/DOUBLE/LONG fires exactly once for its stimulus |
| FR-8 | Injected-clock suite | N | The entire §3.4 gesture set runs with zero wall-clock sleeps |
| FR-9 | Dual-channel concurrency test | N + B | Two simultaneous presses produce two independent, correct events. **VERIFIED on the bench 2026-09-24** (`tools/bench_ladder.py`): the rig drives SWC1 then SWC2, and each press is reported on its OWN `channel` (0 then 1) — a press on one ladder cannot be resolved as the other |
| FR-10 | Long-press timing test | N | `LONG` fires at 750 ms ± 1 tick, **before** release |
| FR-11 | Gesture-exclusivity tests | N | LONG emits no SINGLE; DOUBLE's second press emits no SINGLE |
| FR-12 | Unlearned-press test | N + B | A never-learned level yields `event{button:null}` + `KEY_UNKNOWN`, never a guess. **VERIFIED on the bench 2026-09-24** (`tools/bench_ladder.py`): a press at a level clear of every learned window produced `event{button:null,gesture:"NONE",level_mv:…}` — the device reports the unrecognised press rather than guessing |
| FR-13 | Boot-sequence test | N + **D** + B | **N:** `SafeIdleEstablished()` is true immediately after `Boot()` and the DAC has been written before anything else runs (the tests assert this on every construction, including the pass-through and no-config paths). **D:** the same ordering measured on the real output pin before USB enumerates |
| FR-14 | Gain-policy tests | N | A channel whose `output.gain_mode` is `AUTO` defers to `settings.gain_policy`, and `Boot` passes that channel's own measured idle into the selection — end to end, both the 3 V and the 5 V side, plus a concrete channel mode overriding the policy. `GainPolicySelect` implements AUTO per §6.2's two-sided rule — a 3 V line measures low and takes gain 1.00, 5 V takes 1.82, the ambiguous guard band and an **absent** measurement both default to 1.82 (the safe direction) — and a forced mode is honoured. `Boot` passes the measured idle into it per channel |
| FR-15 | Output-command test | N | The bound action's key is driven **only after the gesture resolves** (`TheOutputIsNotDrivenWhileTheGestureIsUndecided`), and the line returns to the idle code afterwards. The **hold duration** is a property of the injected clock, asserted by the gesture tests, not by a wall-clock measurement here |
| FR-16 | Release-state test | N + **D** | **N:** release writes a code **above** the head unit's measured idle — if the configured idle code cannot reach above it, the firmware substitutes full scale rather than leaving Q4 conducting. **D:** that the resulting line is genuinely high-Z and the head unit sees its own idle. **DONE 2026-09-24 on the bench** by `tools/bench_output.py`: the driver board watches the DUT's own KEY output through the loopback (DUT J3.1 → driver J2.1), a `test_key` pulse pulls the line to the commanded level (2500 commanded → 2465 observed), and on release the line returns to the DUT's own 10 kΩ pull-up level (3167 mV) — the FET off, the line high-Z, not held down |
| FR-17 | Temp-comp test | — | **Absent.** The traceability row claimed a test for a correction that does not exist; a coefficient must be measured before either can be written (N-9) |
| FR-18 | Clamp test | N | An over-ceiling code is clamped and logged; an out-of-envelope write never reaches the DAC |
| FR-19 | Trim-loop tests | N (+ B when enabled) | **N:** converges on a 3 % gain error within the code budget, never moves more than `max_step` per update, stops inside the deadband, does not oscillate when handed a measured overshoot, and respects the total code budget. **The loop is DISABLED in v1** (spec §6.5: open-loop until its gain is measured on hardware), so these prove the implementation and not the running system. **The bring-up enable was RUN 2026-09-24** (`SWC_BENCH_TRIM_LOOP`): it exposed a real defect — the enabled loop moved the output ~11 mV (one `max_step`) in the WRONG direction, because it was fed the sense reading taken BEFORE the command (the line's idle), not the code it wrote. **Fixed, and the fix is CONFIRMED ON SILICON 2026-09-24** (the loop is now serviced from `Tick` after settling, reading the DRIVEN line; pinned + mutation-tested, native 592→593 — N-84). Re-measured on the rig, the enabled settled-level static error (−29/−33/−6 mV at 2500/2200/2800) is **no worse than the disabled open-loop baseline** (−37/−33/−10 mV) and marginally better, so the wrong-direction step is gone; the enabled repeat spread was 0–9 mV, i.e. **no ADC-noise injection**. What still needs the loop ENABLED to be observable is a continuous gain-convergence figure (the loop only runs while a pulse is on the line, so the settled level per pulse is the observable). The loop stays DISABLED in the shipped build either way |
| FR-20 | Grammar tests | N + D + B | Every §7.1/§7.3 pattern produces its documented drive sequence (N), and the ORCHESTRATOR drives the right one: `LED_STAT` solid/breathe/blink per state, and **LED2 solid while the KEY line is driven** — §7.3's diagnostic that tells “the adapter is doing something wrong” from “the head unit is ignoring it”, which was unreachable because nothing but the learn wizard ever set LED2. D+B confirm the patterns on real pins |
| FR-21 | Feedback-nonblocking test | N + D | A key press during an in-flight buzzer pattern is still served on time |
| FR-22 | Level-setting tests | N | Each level, including fully-off, suppresses the right classes and nothing else |
| FR-23 | Atomic-persist test | N + D | Power loss at any byte offset yields the previous good config, never a torn one |
| FR-24 | Corrupt/newer-config test | N | Corrupt CRC and newer `schema_version` each fall back to defaults **and** signal audibly |
| FR-25 | No-config pass-through test | N | With empty config, a press drives a key at the wheel's ratio against the head unit's idle (not a copy of the wheel's volts), each channel against its OWN captured idle, and no usable ladder reference disables pass-through for that input rather than guessing |
| FR-26 | Config-validation test | N + A | Invalid config → `nack`, and a read-back proves the old config is intact |
| FR-27 | Export/import test | N + A | Full config JSON round-trips byte-identically through export → import |
| FR-28 | Learn-mode tests | N + D + B | Measured level, tolerance and rail are stored and match the bench instrument. **VERIFIED on the bench 2026-09-24:** the app-path learn (`learn_start` → stream → `learn_commit`) stored a button at `mv_center: 1816, mv_tolerance: 120` from a rig-held 1840 mV level, and the headless learn stored `swc1_bt1` at a level matching the rig. (Getting this to run required the two harness fixes `bench_ladder.py` documents — the released node floats above the ADC ceiling, and a rig status read releases its DAC.) |
| FR-29 | Learn-rejection tests | N | Noisy and too-close-to-existing samples are each rejected **with the correct distinct reason** |
| FR-30 | Rail-renormalization test | N | A button learned at 3.3 V classifies correctly at 3.14 V and 3.47 V (±5 % regulator tolerance); the ratio `n` is unchanged across that sweep. A **separate** fault test asserts that a 3V3 sag to ≤20 % of the learned value is reported as a rail fault, not as idle. **Note:** this is a *+3V3* sweep, not the 11–14.8 V vehicle-rail sweep an earlier revision specified — no vehicle-rail term exists in the transfer function (§6.3). |
| FR-31 | Headless-learn test | N + D + B | A full learn completes with no USB host attached, driven by AUX1 + buzzes, and the taught button then WORKS (classifies and presents, §6.6 rule 4). `test_system`'s `AHeadlessLearnStoresAButtonAndItClassifiesImmediately` and `AnUnboundGesturePresentsTheButtonRatherThanDoingNothing` cover the host half. **VERIFIED on the bench 2026-09-24** (`tools/bench_ladder.py`): the stateless AUX1 interaction (hold AUX1, press SWC1, release AUX1; no app, no menu) taught a new button `swc1_bt1` at the rig's level, and a subsequent press on it classified as `swc1_bt1` with no reboot (FR-31b) |
| FR-31b | No-app-button test (the unbound pass-through) | N + D + B | **A user with no app binds nothing and still has a working button.** §6.6 rule 4: a recognised but unbound gesture presents the button's own level (§6.9's ratio), so a fresh device learned by AUX1 alone drives a key with no config ever pushed (N-19, resolved 2026-09-21). `test_system` covers it on the host; the board portion confirms it end to end |
| FR-32 | Radio-absent test | N + D + B | In normal mode, current draw and heap show WiFi/BLE never initialized. **The N half is new** (N-81): `check_maintenance_radio.py` statically pins the shape that keeps the radio windowed — no stack is brought up without a teardown step that frees exactly it, so a resource added to the bring-up cannot silently leak past the window. **MEASURED on the DUT 2026-09-24:** `heap_free` in normal mode is `135,764`; entering maintenance drops it to `8,740` (the radio is ~132 KB); exiting returns it to `135,764`. **Four consecutive enter/exit cycles give the IDENTICAL two numbers** (`8,740` / `135,764` every time), so the teardown is exact and there is no leak. The residual ~5.7 KB below the pre-maintenance baseline is N-82's deliberate trade: `FREE_BT` keeps the BTDM pool reserved for the boot rather than releasing it irreversibly |
| FR-33 | Maintenance-entry tests | N + D | Three of §8.2's four triggers are wired and tested. The USB command and the 3 s AUX1 hold open the window directly; the **config flag on next boot** is `settings.maintenance_on_boot`, consumed and persisted by `Boot` — an absent field decodes as `false`, so a config written before the field existed still loads (N-13 resolved). The remaining one, reset-reason + no-config, is **not**: it needs a reset-reason source `IHAL` does not expose, and a device with no config already reaches pass-through, so it is a redundant path rather than a missing one |
| FR-34 | Provisioning test | D + A | **The real Espressif provisioning protocol completes provisioning against this device.** The radio is up on hardware (N-15/N-82, bench 2026-09-24): the AP broadcasts as `SWC-A1B2` (RSSI −39 dBm), the `maintenance` frame carries a MAC-derived PoP (`<PoP>`) and page token. **DONE 2026-09-24 on the bench** by `tools/bench_prov.py`, which runs Espressif's own reference client `esp_prov.py` — the protocol the phone app wraps (its macOS BLE client is `bleak`, so the same reference client runs on this bench unchanged): a **PoP-authenticated Sec1 session established**, `CmdSetConfig` returned **`status: 0x0`**, `CmdApplyConfig` returned **`status: 0x0`**, the device did **not** reset across the exchange (uptime climbed), and a **WRONG PoP was refused** (no session; the SRP6a client proof rejected — the security property). The WiFi *join* is not FR-34: it needs a real AP and is FR-35's dependency; the tool drives a non-existent SSID by default and names the join as the only remaining step. (The phone-app handshake itself is the same protocol; running it needs a phone, but no step of the protocol is left unexercised by this client.) |
| FR-35 | Dual-path update tests | D + B | **D is now runnable: both network paths exist** (N-15 resolved). `MaintenanceRadio.cpp` serves all six §8.4 endpoints: `/api/ota/upload` streams a raw body through `OtaUsb`'s gate, and `/api/ota/check` + `/api/ota/pull` route through `OtaWifi` → the same `OtaBegin`/`OtaChunk`/`OtaEnd`. The token gate, the routing and every API body are host-tested (`MaintenanceHttp`, 15 cases). What still needs the board: the `esp_http_server`/`esp_wifi`/NimBLE bring-up, the flash write, and a real HTTP fetch |
| FR-36 | Checksum-refusal test | N + D | A corrupted image is refused; the device keeps running the old image. **The refusal is host-tested** (`test_native/test_update/OtaUsbTest.cpp`); "keeps running the old image" is the part that needs the board, since the host build refuses to commit at all |
| FR-37 | Rollback test with a real bad image | D + B | A genuinely faulting image rolls back automatically (§10.4). **DONE 2026-09-24 on the bench:** `tools/bench_rollback.py` builds a REAL panicking image (`-D SWC_BENCH_PANIC_IMAGE`, a NULL dereference as the FIRST statement of `app_main`, so it cannot reach `esp_ota_mark_app_valid_cancel_rollback`), pushes it over the real USB-OTA path, commits, and reboots into it. Verified twice, once reading the otadata directly: the pending entry is **`ABORTED`** and the previous entry is **`VALID`**, and the device serves frames again on the old image with `config_state: ok`. **The POSITIVE path is also proven (2026-09-24):** pushing the same GOOD image over USB OTA and rebooting left otadata's newest entry **`VALID`** — `app_main` marks valid only when `OutputVerified()`, so a healthy image is confirmed rather than rolled back, and the DAC read-back that gate depends on agreed with the real MCP4728 (N-21's board half) |
| FR-38 | Maintenance-timeout test | N + D | The configured `maintenance_timeout_ms` (default 5 minutes) of **inactivity** returns to normal mode and serves a press again. **The activity source now exists** (N-35 resolved with the radio): `MaintenanceRadio.cpp`'s HTTP server counts every request, and `main.cpp` bumps the window's clock when that count moves. The mode's half is host-tested (`NoteMaintenanceActivityKeepsAWorkingUserInTheWindow`); the wiring is device-only and its proof is the bench. **VERIFIED on the DUT 2026-09-24:** with `maintenance_timeout_ms` patched to 6000, the window entered on `maintenance_enter` and closed ~5.5 s later with NO activity (the `maintenance` frame flipped `active` true→false), returning to normal mode. The activity-renewal half is not host-observable (it needs a real HTTP request, which the Espressif app provides) |
| FR-39 | Reset-safety test | D + B | On reset, watchdog, brownout and USB-disconnect, the measured KEY line is idle, **not** driving. **The reachable member — a software `reboot` — is VERIFIED 2026-09-24** by `tools/bench_output.py`: the loopback reads the released (pull-up) level before the reset and again after the device re-enumerates, so the reset never leaves the line driving (the boot sequence establishes safe idle before any key, the code path WDT/brownout resets share). The brownout and USB-disconnect reset sources cannot be commanded from the host and remain unproven by name |
| FR-40 | Watchdog-recovery test | D + B | After a forced WDT reset, safe idle is re-established before any key can be served. **Verified for a software reset 2026-09-24** (`tools/bench_output.py`): after `reboot`, `status` reports `output_safe: true`, the loopback is released, and a key is served again — so safe idle is re-established before any key. A forced **WDT** reset specifically is not host-commandable and remains unproven by name |
| FR-41 | Brick-resistance test | D + B | Repeated failed updates never leave an unbootable device. The failure modes are covered on the bench: a CORRUPT image is refused before any commit (`tools/bench_ota_corrupt.py`, running slot untouched) and a COMMITTED image that panics is rolled back by the bootloader (`tools/bench_rollback.py`). Neither leaves an unbootable device |
| FR-42 | Headless-operation test | B | Full button function with USB down, no app, no WiFi — the normal in-car case. **VERIFIED in part on the bench 2026-09-24** (`tools/bench_ladder.py`): the device classifies and reports presses with no app and no radio, and re-serves its link after a host has never attached — `uptime_ms` climbs across a run (no reboot to serve a press), which is the observable that survives the host being absent. The full "USB down" case (a press served while the CDC link is closed) is the same code path the host half exercises with no link object |

**Coverage statement:** 41 of the 42 requirements have an automated or scripted
test. The exception is **FR-17 (temperature compensation), which has no test
because it has no implementation** — §6.4 states plainly that it is "not
satisfied", and its own row above carries `—` rather than a test. (An earlier
revision of this statement read "42 of 42 … Zero requirements are covered only by
inspection", which contradicted the FR-17 row three lines above it and §6.4; the
count is corrected here rather than left as a claim the document itself refutes.
Open item N-9 owns the implementation.) The ones that need
physical hardware — FR-16, FR-34, FR-39, FR-40, FR-41, FR-42 and the bench
portions of FR-6, FR-9, FR-12, FR-19, FR-31, FR-35, FR-37 — are now **partly
discharged**: the board is on the bench (2026-09-24) and FR-9, FR-12, FR-16,
FR-28, FR-31/31b, FR-32, FR-34, FR-37, FR-38, and the software-reset members of
FR-39/FR-40 have been measured on it (see the rows above and
`code/docs/HANDOFF.md`'s bench-status table). FR-19's bring-up enable was RUN and
found+fixed a real defect (N-84), and the FIXED build is now silicon-confirmed
(2026-09-24): the enabled settled-level error is no worse than the disabled
baseline, with no noise injection. Still hardware-gated: FR-35 (a real HTTP OTA
fetch — needs a published release and a live network), the loop's continuous
gain-convergence figure (the loop only runs while a pulse is on the line, so a
per-pulse settled level is its observable), and FR-39/FR-41's brownout/WDT
reset-source cases. §12 carries the critical path.

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
| N-11 | **Four of the seven app-side action kinds are not implemented:** `KEYCODE`, `MEDIA`, `VOLUME`, `SYSTEM` (spec §3.6). `ActionRunner` runs `APP_LAUNCH`, `APP_INTENT` and `APP_RAW`; the bindings screen offers every kind in the generated enum, so a user can bind one and will see "not implemented in this build" rather than nothing. Key injection and screen control need root, an accessibility service or a system-app install — see N-6 | Before Android work | Yes, for those four kinds |
| N-12 | **The app's release check (spec §9.5) does not exist.** §9.5 gives the app the check over its own connection because the ESP32 may have no WiFi in the car; the app had no manifest client and no `INTERNET` permission. **RESOLVED 2026-09-24.** The app now performs the full §9.5 path. `update/ReleaseManifest.kt` parses the nested manifest and decides with a Kotlin `Semver` that is a faithful mirror of the firmware's `ReleaseCheck`/`SemverCompare` (verified by differential test: identical on 19 version pairs, rules 10 and 11 included). `update/ManifestFetcher.kt` fetches the manifest and downloads the image over HTTPS (`HttpURLConnection`, no new dependency; `INTERNET` declared in the manifest). `AppViewModel.checkForUpdates` fetches, decides, and maps the outcome to the screen (`UpToDate`/`Newer`/`NotNewer`/`TooOldToUpgradeFrom`/`Failed` — the floor and ahead-of-release cases are their own states, not folded into `UpToDate`); `installAvailableUpdate` downloads the image and verifies `size_bytes` then `sha256` BEFORE pushing it over USB (spec step 3), so a corrupted or substituted image never occupies the link. The USB-OTA progress display is where `ImageVerifyBytesSoFar` was intended (N-72) and the check is the one path the app can run with no board. Tested: 17 `ReleaseManifestTest` cases + 6 `AppViewModelTest` install/check cases + 3 screen cases; 9 mutations verified caught (5 in the parser/semver, 4 in the install path). The firmware-side `ReleaseCheck` is otherwise still reachable only over WiFi, which needs the radio (N-15) | Before Android work | No — the device can check over WiFi, so the feature existed one path down |
| N-13 | **Two of §8.2's four maintenance triggers are unreachable.** The USB command and the 3 s AUX1 hold are wired and tested. The **config flag on next boot** needs a `DeviceSettings` field that does not exist, so nothing can set it; the **reset-reason + no-config** trigger needs a reset-reason source, which `IHAL` does not expose either. Both `MaintenanceTrigger::kConfigFlag` and `kNoConfigAtBoot` are therefore defined and tested in isolation but never produced by the device. A user whose car has no WiFi and no working AUX1 has no way into the window. **PARTIALLY RESOLVED 2026-09-24: the CONFIG-FLAG half is now wired.** `DeviceSettings` gained `maintenance_on_boot`; `SystemOrchestrator::Boot` consumes it -- clearing it in memory, persisting the clear when the config in force is durable, and calling `EnterMaintenance(kConfigFlag, now)` -- so spec 8.2's "config flag on next boot" is reachable from a pushed config. The consume is the load-bearing part: without it the flag would reopen the window on EVERY boot for the life of the config, an unbounded maintenance window, which is exactly the state FR-38 forbids. The field is OPTIONAL on the wire (an absent `maintenance_on_boot` decodes as `false`), so a config written by an older firmware still loads rather than reading as corrupt. The reset-reason half remains open: `IHAL` still exposes no reset reason, and a device with no config already reaches pass-through, so that trigger is redundant rather than missing. Pinned by 4 orchestrator tests (`AStoredConfigFlagOpensTheWindowOnTheNextBoot`, `AConfigWithoutTheFlagDoesNotOpenAWindow`, `TheConfigFlagIsSpentOnceAndDoesNotReopenOnTheNextBoot`, `AConfigFlagBootWithoutAStoreStillOpensTheWindowOnce`), a codec back-compat test (`AConfigWrittenBeforeFR33sTriggerStillDecodes`), and the app's mirror (`a config from before FR33s trigger decodes with the trigger off`); all mutation-tested | Before the board arrives (reset-reason half) | No — the primary trigger works from the app, and the AUX1 hold is the documented no-app fallback, so one of the two no-tool paths is live |
| N-14 | **USB OTA (spec 9.3) — WIRED 2026-09-24; the on-device flash write remains board-gated.** `OtaUsb` implemented the whole run (verify gate, chunk accumulation, commit) while `CommandRouter` nacked `ota_begin`/`ota_chunk`/`ota_end` as `not_implemented`, so no host could start one — the commit `30c00df` claimed "both OTA paths" but described one working path and one library. **The router is now wired:** `HandleOtaBegin`/`HandleOtaChunk`/`HandleOtaEnd` are thin adapters over `OtaUsb` (the same three functions the WiFi path calls — spec 9's one-gate rule), the same range-first/`1e999`→`+inf` ordering the config transport uses, a `gap` refusal for any chunk whose offset is not the next expected byte (spec 9.3 requires gaps and overlaps be rejected), and `hello`'s `caps` now advertises `"ota"` — guarded by `HelloAdvertisesOtaOnlyIfTheDispatcherImplementsIt`, which asserts the string against the dispatcher rather than a second hardcoded copy. `kAppSlotBytes` (1920 KiB) is the one image-size bound shared by the router, the WiFi path and `check_size.py`. Nine new router tests cover begin/chunk/end, the gap, the bad hash, the oversize and the inf size, and the truncated-run refusal; the host suite cannot run the `ESP_PLATFORM` flash write, so `OtaEnd` on the host reports `result:not_supported` rather than claiming an install (asserted). **What still needs the board:** the `app_update`/`esp_partition` calls themselves — `esp_ota_begin`/`esp_ota_write`/`esp_ota_set_boot_partition` — must be exercised on a real device, and the first live test should be a deliberately CORRUPT image so the refusal is proved before the write is trusted | The on-device flash write: with the board | No — the WiFi path exists, so the feature was already one path down; the USB reachability is now closed |
| N-15 | **`MaintenanceMode` was pure state and nothing started the radio — RESOLVED 2026-09-24.** `BleProvisioning`/`WebPage` were written and unit-tested but called by nothing, so a maintenance window opened and lit `LED_STAT` with no NimBLE, no provisioning endpoint and no web server — and §8.2's "on exit the radio stacks are fully de-initialized" had nothing to de-initialize. **The radio is now wired.** `lib/Maintenance/MaintenanceRadio.cpp` (a THIRD device-only TU, excluded from the host build beside `EspHal.cpp`/`UsbLink.cpp`) brings up NimBLE + `wifi_provisioning` (BLE transport, **Sec1** with the MAC-derived PoP), the WiFi driver in AP+STA (so the page is reachable on the device's own AP before it joins a network, then on the joined network), and `esp_http_server` serving the compiled-in page plus all six §8.4 endpoints. `main.cpp`'s poll loop drives it on the `MaintenanceActive()` TRANSITIONS, and `MaintenanceRadioStop()` frees every stack it created — one flag per resource, so a bring-up that bailed out partway frees exactly what exists (FR-32). **The routing, the token gate and the response bodies live in `MaintenanceHttp`** (host-tested, 15 cases: the 401-on-every-path gate, the near-miss token, 404-vs-index, JSON 404 under `/api/`, the four status facts, the temperature's integer split, SSID escaping, per-endpoint actions, method policing, and the reply-size refusal) — a `esp_http_server` handler with the gate inlined would have put a SECURITY property in a file no host build compiles. **The upload is a real verified install**, on a raw body with the digest and size in two headers: `crypto.subtle` is undefined outside a secure context and the page is plain HTTP on the AP, so the page carries a small SHA-256 whose correctness is pinned against the published NIST vectors by extracting it from the served asset and running it (a wrong digest rejects every VALID upload, pointing the user at their firmware file). Both network-update endpoints route through the shared `OtaWifi` → `OtaBegin`/`OtaChunk`/`OtaEnd` gate, so no path is weaker than another (spec 9.4). `ble_failures` is reported so a window whose radio failed is distinguishable from a working one. **The two per-device secrets reach the user**: §4.3 gained a `maintenance` frame (spec 8.3 option 1) carrying `pop`/`token`/`page_url`/`ble_name`, emitted on connect and on every change, all-empty when closed; the app renders the PoP and the page URL on the maintenance card, and the AUX1-opened window is now visible to the app (it sent no frame before). **Board-gated:** FR-34's end-to-end provisioning needs the real Espressif app and a radio, and the AP/HTTP bring-up itself cannot be exercised on the host | After the board arrives | Yes, for FR-34 — the provisioning test needs the real Espressif app and a real radio |
| N-16 | **A production build has no readable console, and reflash-by-USB is unverified.** The S3 has ONE internal USB PHY shared by USB-Serial-JTAG (the console) and USB-OTG (TinyUSB CDC, the app link), so installing TinyUSB at boot moves the PHY and the console goes dark -- Espressif: "both controllers share a single internal PHY, allowing only one to operate at a time" and "during protocol stack initialization, the USB-PHY connection will automatically switch to USB-OTG" (verified 2026-09-20). Consequences: every `ESP_LOG*` line the firmware emits is unreadable at runtime, including `UsbLinkStart`'s own install-failure warning; and whether a reset still enters ROM download mode over USB is unverified, which decides whether reflashing needs the recessed BOOT button (§2.2) to be pressed. §4.1 previously claimed "both may be active at once", which this board cannot do. The console can be restored with a UART0 console on unused GPIO43/44 (needs header pins or test pads on a respin), or an external PHY (≥6 GPIOs). Decide which before finalising the PCB | **Before the board is finalised** for the hardware question; the download-mode check is §10.6 step 1 | Yes — it changes the pinout or the bring-up procedure |
| N-17 | **The eFuse calibration path was unreachable until 2026-09-21, and its accuracy is still unverified.** `EspHal` created the curve-fitting handle and then converted every reading with `AdcRawToMilliVolts`, whose two branches return the same straight line — so the per-chip polynomial was computed and discarded, and `cali_degraded` reported healthy while the device used the linear scale §2.3 forbids. `adc_cali_raw_to_voltage` was called nowhere. Fixed: the eFuse value is now used when the handle exists, the linear line only for blank-eFuse parts. **Also fixed 2026-09-23, the REPORT half of §3.2's "fall back *and report that it did*":** the fallback was logged on the console but never announced the way the spec (and this project's own comments in `main.cpp` and `EspHal`) claimed — no `BOOT_DEGRADED` was ever played, because `IHAL` carries no calibration accessor and the orchestrator's boot pattern came only from the config load, so a blank-eFuse device booted silently. `SystemOrchestratorCreate` now takes the flag (`EspHalCalibrationIsDegraded()`) and `Boot` folds it into the `BOOT_DEGRADED` pattern. **Still open:** the resulting absolute accuracy against a bench reference (the spec's `D` marker on FR-2) — measure it before trusting learned windows, because a systematic millivolt offset shifts every button centre equally and would look like a correctly-learned ladder | With the board | No — v1 is correct without it, and the fallback already worked; but the *verification* is what FR-2's row cannot yet claim |
| N-18 | **FR-3's settle gate is unread.** `AdcReader::Settled()` reports whether the median burst was a clean level or a mixture of old and new, and two code comments assert that classification waits for it — but `ServiceChannel` classifies on `Value()` unconditionally (`SystemOrchestrator.cpp`), and classification reads `Value()` unconditionally. (`Settled()` IS read now — `ServiceChannel` gates the idle-reference re-adoption on it — but that is not classification, so the substance of this item is unchanged; an earlier revision of this row said `Settled()` was "referenced only by its own unit test", which the re-adoption gate has since made untrue.) FR-3's matrix row proves the filter *can* settle within `debounce_ms`; it does not prove the classifier waits. Expected to be harmless, because the classifier's own `debounce_ms` (25 ms) covers the single mixed tick a real press produces — but "expected to be harmless" is a bench question, and gating classification changes when a key latches, so do not change it without a measured press on the real ladder | With the board | No — recorded for completeness, not a known fault |
| N-19 | **§7.5's no-app flow needed a rewrite, not a new subsystem — the fix is §6.6 rule 4 (resolved 2026-09-21).** The original §7.5 described a *modal* `PROGRAMMING MODE` (AUX1 hold to enter, "AUX1 held ≥ 1.5 s again" to leave) that both collided with §7.4 and was unreachable as written: `kEnterHoldMs` is 1500 ms and maintenance opens on the same hold reaching 3000 ms, so a second 1.5 s hold could never fire. The 2022 interaction the user described is *stateless* — hold the modifier (AUX1, standing in for the 2022 BOOTSEL), perform the gesture on a wheel key, release — so §7.5 is rewritten to that and **§6.6 gains rule 4: a recognised but UNBOUND gesture passes through** (presents the button's own level by ratio, §6.9), which is the 2022 default (`lookup_*_press_val`) and makes a headless-learned button work with no app and no config. Implemented in `SystemOrchestrator::PresentLevel` + the `found == false` branch; tests in `test_system` (`AnUnboundGesturePresentsTheButtonRatherThanDoingNothing` and siblings). **Residual, minor:** the beep COUNT for which gesture was heard (1/2/3 `PROGRAM_STEP` pulses) is described in §7.5 and plays from the learn wizard's own prompt path, but there is no distinct `PROGRAM_SAVED` on a *binding* write, because the on-device flow writes no `Binding` — the pass-through default needs none. Whether the device should also be able to author a `Binding` headlessly (rather than only present the button) is a v2 question, not a v1 requirement | Before the board arrives | **No — resolved.** The core no-app requirement (learn + use a button with no app) now holds |
| N-20 | **§7.3's LED2 *long pulse* is implemented and never emitted.** `Led2Pattern::kLongPulse` has a correct cadence (0.5 s on, 1.5 s off, `LedGrammar.cpp`) and a zero-emitter grep in `lib/`, `src/` and `test_native/`. The two triggers §7.3 names both fail to occur: **gain mode is chosen once, in `EstablishSafeIdle`**, and nothing re-derives `gain_mode_` afterwards, so there is no "gain mode changed" transition to signal; and there is no head-unit **(re)detection** event either — `head_unit_idle_mv_` is written once during boot and never re-evaluated, while the per-tick check is an envelope test (`rail_fault`) that deliberately does *not* report. The other three LED2 states are live: `kFlick` (learn wizard), `kSolid` (driving), `kOff`. **Not fixed here, because both candidate triggers are behavior changes that need a measurement**: a runtime gain re-selection would need a debounce on `ADC_CH_KEY_SENSE1` (else a press's own transient re-selects gain and the LED stutters), and a head-unit transition would need `head_unit_idle_mv_` promoted to a live re-measurement. A v1 decision is available and cheap: DROP the row. | With the board | No — LED_STAT carries the state, and LED2's diagnostic value is its *driving* state, which works |
| N-21 | **The DAC fault path is three-quarters absent, and two doc-comments assert otherwise.** §6.8's I²C row promises four things — *retry with backoff*, *latch*, *release the line*, *never drive a guessed code*. Only the last holds. **No read-back:** §6.1 step 3b and FR-13 require *verifying* the DAC reached the safe state, and `EspHal` calls no `i2c_master_receive`/`transmit_receive`/`probe` at all — the MCP4728's multi-read (command `0x50`) makes this implementable, it is simply not written. **No retry and no latch:** `IHAL.h` and `EspHal.cpp` both state that `dac_set_code` 'retries with backoff internally and latches a fault on persistent failure'; neither is true — it is one synchronous `i2c_master_transmit` and on failure it only `ESP_LOGE`s. **No `FAULT_DAC` emitter:** §7.2 gives the pattern the I²C/DAC meaning and nothing plays it, because `dac_set_code` returns `void` so no caller can learn of a failure. **A fix is an interface change, not a local patch** — `dac_set_code` returning `esp_err_t` (or a HAL fault accessor) is what makes 'report a fault' reachable, and host-testing needs a MockHal injection hook beside `FailNextNvsWrite`. **Fixed 2026-09-24.** All three absent pieces now exist and the two false comments are gone. **(1) Read-back:** `IHAL` gains `dac_read_code`, `EspHal` issues the MCP4728 Read Command (`i2c_master_receive`, 24 bytes) and `DacFrame::DecodeReadCode` owns the byte layout (`buf[6n+2] | ((buf[6n+1] & 0x0F) << 8)` per channel A–D, from DS22187E Fig 5-15, cross-checked against Adafruit's driver). `SystemOrchestrator::VerifySafeIdleIdleCodes` runs it per channel at the end of `EstablishSafeIdle` and compares against the code just written. **(2) Retry + latch:** `DacRetry.h` holds the policy (3 attempts, 1 ms then 2 ms — short deliberately, because the whole retry must fit inside the 200 ms key pulse), applied to both `HalDacSetCode` and `HalDacPowerMode`; a persistent failure latches the HAL's `dac_faulted`. **(3) FAULT_DAC:** `ReportDacFault` plays it once per boot (an edge — the latch is permanent, so a per-tick check would re-`Play` it forever) and latches `hw_faulted_`. `Tick` calls it whenever the HAL latch is set, so a write that fails on the KEY path is reported too, not only the boot one. **(4) The comments** in `IHAL.h`, `EspHal.cpp` and `DacRetry.h` now describe what the code does. **A mismatch RELEASES rather than driving the read value** — the read value is what the check just declared untrustworthy, so driving it is §6.8's forbidden "guessed code"; the firmware re-asserts the idle code, which IS the released state (§6.7). **A wrong VALUE needed a second latch:** the HAL's `dac_faulted` is set by a failed I2C *transaction*, and a mismatch is a successful one, so `OUT` gained `dac_verify_failed_` — without it FR-37's gate could not see the case the read-back exists to find. Pinned by 4 `DacFrame` read/round-trip tests, 4 `DacRetry` tests, and 5 orchestrator tests (`BootReadsBackTheSafeIdleCodeOnEveryChannel`, `AReadBackMismatchReportsAFaultAndDrivesTheWrittenCode`, `AFailedReadBackReportsAFault`, `TheDacFaultIsReportedOnceNotEveryTick`, `ADacWriteFailureOnTheKeyPathIsReported`); mutation-tested three ways (dropping the read-back call, driving the read value, and removing `dac_verify_failed_` from the gate — each fails). The `ack` contract row gained `result` (`ota_end`'s outcome word), caught by the gate self-test. **Board half now PROVEN on the bench 2026-09-24:** a GOOD image pushed over USB OTA and rebooted left otadata's newest entry **`VALID`**, and `app_main` calls `esp_ota_mark_app_valid_cancel_rollback` *only* when `OutputVerified()` is true — which requires the read-back to have returned the exact idle code on the real MCP4728 (`!dac_verify_failed_ && !dac_faulted`). So the 24-byte Read-Command response is parsed correctly against silicon, not just against a fixture | Fixed 2026-09-24 | No — the pre-fix failure direction is believing a write that did not land, and nothing drove a guessed code |
| N-22 | **§4.3's `status` row named three fields no firmware emits, and one field the app reads is never sent.** `rail_mv`, `temp_c` and `heap_free` have no producer anywhere (grep-confirmed) — and the app's `Frames.STATUS` branch reads `rail_mv` into the ladder screen's idle display, which is therefore dead. Worse, that field is the **+3V3 rail** (§3.4 `learned_at_rail_mv`), not the wheel's idle KEY level, and `LadderScreen` divides every band by it — so if the field were ever sent, every band would be placed against ~3300 mV when the real idle is ~2835. The two quantities are conflated in the name and the label. **The APP half is fixed (2026-09-21):** the app no longer reads `rail_mv` (the idle it needs already comes from the config, via `ladder.learned_idle_mv`), and it now reads `config_state`, which the firmware emits and the app previously ignored — so §6.8's corrupt-config fault finally has the name the app can read. **NOTE (2026-09-22): the claim that `learned_idle_mv` is "the idle it needs" is WRONG for the ladder view** — §6.3 requires the LEARN-TIME denominator only at learn, and runtime classification uses the LIVE idle. The full consequence is recorded as N-25; the correct repair needs a wire field that does not exist yet. **The firmware half remains open**, because it is a decision plus a board gate rather than a repair: the open questions are (a) whether `status` should carry a live idle at all and under which name, and (b) whether a RUNTIME rail measurement path even exists — FR-30 measures the rail only *during learn*, and a runtime one is board-gated. Resolved in this pass and removed from the row: `config_state` now carries the config's state (`ok`/`none`/`recovered`/`defaults`) with `output_safe` split off, and `gain_mode` reports the resolved mode instead of a hardcoded word. **RESOLVED 2026-09-24 (firmware half).** The two fields that CAN be measured now have real producers, and the third is retired: (a) `temp_c` is the last good NTC reading (`SystemOrchestrator::LastNtcTenthsC`, filled by `SampleNtcTenthsC`) written as a DECIMAL in the config codec's `AddTenths` unit, and **`null` when nothing has been measured** — the sentinel is 0 and 0 °C is a legal temperature, so a bare `0.0` would be the same lie the sentinel exists to prevent; (b) `heap_free` comes from a new `IHAL::heap_free` (`esp_get_free_heap_size` on the device), read at emit time rather than cached; (c) **`rail_mv` is deliberately NOT emitted and is removed from the row** — `LadderDecode.h` records that this board has NO rail sense channel (`AdcChannel` carries SWC1/SWC2/TEMP/AUX1-3/KEY_SENSE1-2, none of them +3V3), so a runtime `rail_mv` could only be a fabricated constant, and the quantity the app actually needs is the **live idle**, which is a different field on a different frame — see N-25, now also resolved. Both new fields are asserted to have producers by `test_frame_field_lists_match_the_router`, which reads the router's own emit site, so the row cannot drift back to the phantom-field shape | Fixed | No — the config state is now truthful, which was the part with a wrong answer rather than a missing one |
| N-23 | **RESOLVED 2026-09-24 — the headless learn could only ever teach channel 0.** `SystemOrchestrator::learn_channel_` was read in five places (the idle sense-channel choice, the channel's ladder seed, the wizard tick, the commit, and the link's sample stream) and **assigned nowhere** — a constant `0` — so a no-app user on a two-channel install who held AUX1 got every learned button on SWC1, and SWC2 was learnable by no shipping means (the app has no learning screen, so the fallback the row once named did not exist — see N-47). **The fix is the 2022 design, and it is not a new gesture: there is NO channel selector and NO slot menu at all.** The user's restatement is the specification: *"You just hold the aux1 button and then do the gesture on whichever swc_in switch you're working with. No complicated menu or selecting which one, just do it. It can easily be detected by seeing which one changes from idle."* The 2022 firmware did exactly that — `is_key_pressed()` named its key by whichever input's average left its baseline — and the redesign does the same: `LearnWizard` now takes a `LearnInputs` list (every input's `wire_channel`, ADC pin, LIVE idle, ladder-vs-switch kind, id), and `Detect` names the target by the one whose reading left its idle by `kDetectMv`. One hold programs exactly one input, SWC1 and SWC2 alike, and AUX2/AUX3 are switch targets with their own window path. The `learn_channel_`/`learn_idle_mv_` members are deleted; the target is the wizard's own `target_wire_channel_`, produced by detection and consumed by `ApplyLearnedResult`. **Two coupled defects fell out of the same rewrite and are fixed with it:** (a) the button being *taught* would otherwise also drive the radio while AUX1 is held — the 2022 firmware guarded every `SEND_KEY_VALUE` on `!is_program_button_pressed` for exactly this reason, and both `ServiceChannel` and `ServiceAux` now suppress output while the wizard is armed; (b) the former slot-menu re-learn matched the entry to replace by **id**, which a no-menu flow cannot name, so the match is now by **VOLTAGE** (the seeded entry whose window contains the measured level) and a new button takes the next free `swc<ch>_bt<n>` | Fixed | No — and the severity the row recorded (SWC2 unlearnable by any shipping means) is closed |
| N-25 | **The app's live-ladder view cannot classify the way the device does, because the live idle never reaches it.** §6.3 is explicit that classification runs on the normalized ratio against `V_ADC_idle` "measured now" — and the firmware implements exactly that (`SystemOrchestrator::idle_reference_mv`, seeded at Boot and re-adopted within `[1000-60, 1000+30]`‰). The app instead compares ABSOLUTE millivolts (`LadderScreen.matched()`: `abs(live - mvCenter) <= mvTolerance`) against `ladder.learned_idle_mv` from the config — **the learn-TIME rail, which is the denominator §6.3 explicitly forbids**. N-22 asserted this was "the idle it needs"; it is not, and that claim is wrong. **Consequence:** when the rail drifts inside its ±5 % tolerance, a real press is reported "Classified as: nothing" while the device classifies it correctly and fires the right key — the screen reports the adapter as broken when it is working, which is precisely the diagnosis it exists to make (§4.3's notes). It is also reachable in the OTHER direction: `mv_center` is an absolute pin voltage, so at a MOVED rail a reading in the app's absolute band can fall outside the device's ratio window, so the app can mark "matched" a button the device did not fire. Nothing else in the app renormalizes. **Why not fixed with the other N-22 work:** the app has no source for the live idle. `learned_idle_mv` is the only idle on the wire (`config_get`), and §4.3's `status` carries no live idle — `rail_mv` has no producer, and adding one is a spec decision plus a board gate (a runtime rail measurement path does not exist; FR-30 measures the rail only during learn). The repair is therefore a §4.3 decision — carry the live idle (or the event's own ratio) in `event`/`status`/`ladder_sample` — followed by an app change to classify on the ratio, exactly as `LadderDecode.cpp` does. Until then the app's match indicator is advisory and may disagree with the device on a rail that has moved. **RESOLVED 2026-09-24.** The wire decision is the event's own denominator: `event` now carries **`idle_mv`**, set from `IdleReferenceMv(channel_index)` for a wheel and `kNominalRailMv` for an AUX switch (`GestureEventRecord::idle_mv`). It is the same value `ServiceChannel`/`ServiceAux` handed the classifier, so the app's ratio is a copy of the device's decision rather than a second derivation. The app's `LadderUiState` gained `liveIdleMv` and `matched()` now reproduces `LadderClassify`: the reading is normalized against `liveIdleMv` and each window's `mv_center`/`mv_tolerance` against the learned `idleMv`, then the nearest centre within its half-width wins. The two denominators are deliberately different — that is the rail-cancelling construction §6.3 describes — and using one for both was the bug. The match is quantized to permille exactly as the device's is, so the app's band edge is now the device's band edge (a ±120 mV window at the 2835 mV fixture is ±42 ‰, i.e. ±119 mV, which is what the boundary test asserts). Both halves are pinned: the firmware by `ARecognizedGestureIsReportedAsAnEventWithTheLearnedButtonId` and `AnAux2PressDrivesKeyChannelZeroAndIsReportedAsItsOwnInput` asserting the event's idle equals the reference the classifier used, and the app by `a live idle lets the match reproduce the device on a moved rail`, which drives vol_up's ratio at a +5 % rail where the old absolute test reported "nothing" | Fixed | No — the device's own classification was always correct and unaffected; this was the app's diagnostic fidelity |
| N-24 | **BOTH of the transport's loss counters were counted and reported to nobody — the outbound TX-buffer drop and the inbound staging-ring overflow (fixed 2026-09-23).** `UsbCdc::Send` returns false and increments `dropped_` when its buffer is full, and the class's own doc-comment says a silent drop "is the failure this class exists to prevent, so it must be observable" — but `DroppedFrames()`'s only reader in the whole tree was a unit test. The production call site is `RouterOutSinkThunk`, which discards `Send`'s return (`LinkWiring.cpp`), so nothing could surface the count: not a `log`, not a `status` field, not an LED. **The audit that fixed it found the twin:** `RxOverflows()` — the RX staging ring refusing staged input — was in the same state and did not even carry the observability claim, and **it is the more dangerous half**: an inbound overflow means a frame the app SENT was discarded before the firmware parsed it, so the command fails with no nack and no error at all. From the app's side that is indistinguishable from a device that ignored the request — the exact confusion `link_gap` was added to remove, one layer down (spec 4.3 emits `link_gap` only for the app-to-device direction, because the router can see a `seq` gap there; a frame dropped before parsing leaves no seq to notice). **Why the count could not simply be returned:** the sink type is `void`, so a refused frame fails entirely inside the transport and nothing returns to the caller that asked for it; the router's only channel to a user is a frame it EMITS. The fix therefore reports both counters in the `status` body (`tx_dropped`, `rx_overflows`), on every status including the periodic keepalive — which is the app's one guaranteed look at the link without asking. **Fixed:** `CommandRouter::SetLossCounters(UsbCdc*)` holds the transport by pointer, called once in `LinkBind` beside the sink and flush wiring; `EmitStatusBody` reads both counters into the frame; the app stores them in `LinkUiState` (`deviceTxDropped`, `deviceRxOverflows`) and renders a card that names WHICH direction lost frames, because the two have different fixes. Pinned by `CommandRouter.StatusReportsTheTransportsLossCounters` (driven through a REAL `UsbCdc` filled to refusal, wired the way `LinkBind` wires it — so the test fails if the wiring call is dropped, and the property under test is REACHABILITY, not the counter's correctness) and by `AppViewModelTest.the device's own loss counters reach the screen` plus `a status without the loss counters leaves them alone` (absent must mean UNCHANGED, not reset to zero — zeroing would erase the evidence on the next 2 s keepalive). **All three mutation-tested:** removing the `SetLossCounters` call fails the router test at the `tx_dropped` assertion, and removing the app's field storage fails both app tests. The contract row for `status` gained both fields, so the existing both-directions `test_frame_field_lists_match_the_router` now pins the schema against the emit site. Note the practical bound on `tx_dropped`: `kTxCapacity` is two maximum frames and the poll loop drains every tick, so it needs a sustained stall rather than a normal burst | Fixed | No — a dropped frame is a degraded-link signal, not a safety one. The inbound half is the one that can make a user-visible operation fail silently, which is why it was fixed in the same sweep rather than deferred |
| N-26 | **`AUX1`–`AUX3` are declared bindable inputs and nothing in the firmware services them.** §3.5 and §3.1 define `BindingChannel` as `SWC1\|SWC2\|AUX1\|AUX2\|AUX3\|ANY`, `Config` carries an `aux[3]` table (`AuxButtonConfig`, spec §3.1), §3.7's worked example binds `{"channel":"AUX1","button":"aux1","gesture":"SINGLE"}`, the codec and `ConfigValidate` both accept such a binding, and the Android model exposes `AUX1`/`AUX2`/`AUX3` as first-class channels. **But no path reads them**: `SystemOrchestrator` services only `channels_[0..channel_count_-1]` (the two SWC ladders); `ADC_CH_AUX2`/`ADC_CH_AUX3` are named only by `EspHal`'s channel map and are never read; `config.aux[]` is read only inside the codec; and `BindingResolve` — which does consult `aux[i].id` to decide whether a binding *names a real input* — is never *invoked* for an `AUX*` channel, because nothing produces a gesture event from an AUX input. (`BindingResolver.cpp`'s own comment points the reader at the `aux` table as if AUX were handled.) **Consequence:** an app that binds a gesture to `AUX1` — a natural thing to do for the physical programming button — gets a config the device accepts with an `ack`, lists as applied, and never acts on. The one AUX behaviour that IS wired is AUX1's *learn* role (FR-31, §7.4/§7.5) — the 1.5 s / 3 s holds — which is a separate mechanism, not a binding. No test gives false confidence: none asserts an AUX binding fires. **Why this is an open item and not a repair:** the fix is a spec decision (does an AUX press classify through the existing `PressClassifier` against `aux[].source`, and does it run the FULL gesture grammar with its own `DeviceSettings` timings, or only SINGLE?) plus a new per-input servicing loop and a hardware measurement — not a local patch. A v1 decision is available and cheap: state that AUX1–3 bindings are accepted-and-inert in v1 (AUX1 is the programming modifier), and drop them from the app's channel picker, so the app cannot offer a binding the device silently ignores | With the board | No — the SWC ladder path (the product's primary input) is unaffected, and an install that binds nothing to AUX loses nothing |
| N-27 | **The app does not implement §4.4's keepalive or its own liveness detection.** §4.4 makes the app responsible for two things: "app sends `ping` if it has seen nothing for 5 s; firmware replies `status`", and the app-side half of "after 10 s of silence the link is down". The firmware implements its side (`CommandRouter::NoteSilenceIfStale`, `kLinkSilenceMs = 10000`; the router sends `status` every 2 s). **RESOLVED 2026-09-24.** `SwcClient` now owns both halves. `SilenceTick()` is the mechanism: it is a no-op until a frame has armed the clock (the two last-seen stamps are `Long.MIN_VALUE`, not `0` — a zero initialiser makes `now - last` a huge positive number on the first tick and declares a fresh client's link down before any device has been asked anything); it emits one `ping` once the idle passes `kPingIdleMs` (5,000 ms, stamped on the ATTEMPT so an unanswered ping is not a flood); and past `kLinkSilenceMs` (10,000 ms, the firmware's own value and the same `>=`) it raises the new `LinkState.SilenceExpired`, which the view model maps to `LinkProblem.SilenceExpired` and the Link screen shows as **"Not responding"** with physical guidance rather than a retry. `DriveLiveness()` is the loop; it is driven from the activity's lifecycle (a periodic `delay` loop in `AppViewModel.init` would hang every `advanceUntilIdle()` in the virtual-clock suite). Recovery is a frame: `handle()` clears `SilenceExpired` exactly as it clears `Failed`, so an unplugged-then-replugged device revives without a reconnect, and a version mismatch is deliberately NOT overwritten. Garbage on the wire is not a liveness signal (a malformed line throws before the stamp). Pinned by 9 `SwcClientTest` cases against a hand-moved clock, a `DriveLiveness` case on the scheduler's own clock, a `AppViewModelTest` mapping case, and 2 `LinkScreenTimeoutTest` render cases; 4 mutations verified caught (threshold raised, sentinel zeroed, revival removed, ping-stamp removed). **The sibling half of §4.5 remains:** the app once set `VersionMismatch` and then kept transmitting — that is fixed at `sendLocked`/`sendAndAwait`/`getConfig` | Fixed 2026-09-24 | No — the device's own behaviour and the user's key presses are unaffected; only the app's connection indicator could go stale |
| N-28 | **`updated_at_ms` is declared, round-tripped, and never given a value by either side.** The codec requires it (`ReadU64`) and the decoder-verified §3.7 example carries a real value, but no production path assigns it: the firmware never writes it (grep-confirmed — the only assignment is a test's fixed `1700000000000`), and the app's encoder writes back whatever it decoded (or `0` for a fresh `Config()`), so a config that has been edited many times still reports the timestamp it arrived with, or `0`. It is therefore a field with the N-22 shape — carried, decoded, validated, and never meaningful — and its name (`updated`) promises a fact nothing produces. This is also **why §3.1's entity map previously named a `created_at` that does not exist**: the map described an intent (create/update stamps) that the model only half-implemented, and the fix here corrects the map to the one real field rather than inventing the missing one. **Why this is an open item and not a repair:** giving it a value is a decision — the firmware has no RTC without a `time_sync` (§4.3), so a device-authored stamp would be an uptime or a boot count, not a wall-clock time; and the app can only stamp edits it originates. Either choice is a spec change plus a producer on the writer(s), not a local patch. **No consequence today:** nothing reads it — not the firmware, not the app, not the web page — so the only cost is an unused field in a 1,409-byte blob | With the app's next work session | No — no reader exists, so a stale stamp cannot mislead anyone yet; the field costs bytes, not correctness |
| N-29 | **The firmware runs only a binding's FIRST action, while §3.5 and the app run the whole ordered list.** §3.5 is explicit that a binding's `actions` are "executed in order, each independently failable", and names the product's core case as one binding carrying *both* halves — "emit the factory key press **and** tell the app". The plan agrees (Task 13 is where "the multi-action runner" belongs). The APP implements its half correctly: `AppViewModel.runAppSideAction` iterates `binding.actions` and runs every one that is app-owned, skipping the kinds the firmware owns. But the firmware stops at the first: `BindingResolve` returns only `actions[0]` (`out.action = b.actions[0]`), and `SystemOrchestrator`'s execute path acts on that one action and only in its `OUT_VOLTAGE` branch — every other kind, and every action past the first, falls to the `else` that merely `ReleaseKey`s. **Consequence:** a binding stored as `[OUT_VOLTAGE, APP_LAUNCH]` (the exact combination §3.5 says the list exists for) drives the key correctly but the firmware never signals the second half; worse, a binding stored as `[APP_INTENT, OUT_VOLTAGE]` (app action first) drives NO key at all, because `actions[0]` is not `OUT_VOLTAGE` and the `else` releases — the press silently does nothing on the wire the user is watching, while the app still fires its own half from the `event`. **Reachability is narrow but real:** the app's editor always builds single-action bindings (`actions = listOf(action)`), and `ConfigDefault` ships none, so this needs a hand-authored or `config_patch`-written config with two actions — legal, accepted by `ConfigValidate` (`kMaxActionsPerBinding` is 2), and decoded without complaint. **Fixed 2026-09-24:** the plan's Task 13 runner now exists. `BindingResolve` returns a `ResolvedBinding` carrying the whole ORDERED list (`action_count` + `actions[]`), refusing the binding if ANY action is un-executable, and `SystemOrchestrator::RunBindingActions` walks it: the `OUT_` family and `BUZZ` are the firmware's column (§3.6's "Executed by" table) and execute, while an app-owned kind is SKIPPED — neither executed nor treated as a release — which is what spec 3.5 requires ("a failed app-side action must never prevent the hardware key press") and what makes `[APP_INTENT, OUT_VOLTAGE]` drive its key. The order-dependency the open item flagged is resolved by the list's own order: the `OUT_VOLTAGE` pulse is set on `key_released_at_ms` and the release is NOT issued by the runner, so a later action cannot cut it short; only an explicit `OUT_RELEASE` or `NONE` releases. `BUZZ` REPLACES the default `KEY_ACCEPTED` (one buzzer, `Play` replaces not queues, §7.2) and an empty command band still suppresses the acknowledgement and plays `KEY_UNKNOWN`. Pinned by `ABindingWithTwoActionsRunsBothInOrder`, `AnAppActionFirstStillDrivesTheKeyAfterIt` and `AnAppActionAfterTheLevelDoesNotReleaseTheKey` (the last mutation-tested against a skip→release regression), plus two resolver tests for the ordered list and the any-position refusal | Fixed 2026-09-24 | No — the reachable pre-fix set was hand-written configs, and the fix only ADDS the actions that were being dropped |
| N-30 | **The head-unit-gone envelope latched a permanent hardware fault (regression, fixed 2026-09-23).** §6.2 step 2 routes a `V_KEY_idle` outside 1.80–5.20 V to the `NoHeadUnit` path, and §6.8's head-unit-gone row says "keep classifying"; N-20's own note records that the per-tick envelope check "deliberately does *not* report". Commit `817367bd` ("Report the fault FR-4 detected…") folded that envelope test into FR-4's fault branch, so `SystemOrchestrator::ServiceChannel` called `ReportFault()` on it — and `ReportFault` sets `hw_faulted_`, which §7.3 latches until reboot. **Consequence:** because §4.4 says the head unit "may sleep, suspend, or reboot at any moment", the FIRST time the radio powers down (or the user pulls the car's radio fuse, or a 3 V unit's own line drifts out of envelope) `LED_STAT` began blinking forever — a reboot-only indication of a fault the device was not in, over a condition that is normal and self-clearing. It was reachable on the enabled path and on the disabled-channel path, which has the same envelope test and no ladder classifier to reach FR-4 with. **Fixed:** the two conditions now share the RELEASE but not the LATCH — `ladder_fault` (`level == kFault`) reports and latches, `head_unit_gone` releases and resets only — and both variables are named for what they measure so the distinction is visible at the call site. Pinned by two new host tests (`AHeadUnitThatGoesAwayReleasesButDoesNotLatchAFault`, `ADisabledChannelDoesNotLatchAFaultWhenTheHeadUnitGoesAway`), each mutation-tested by re-adding the latch. The two tests that had used an out-of-envelope sense line as their "hardware fault" fixture (`ApplyConfigKeepsAHardwareFaultLatched`, `IdentifyDoesNotRepaintALatchedFault`) were switched to FR-4's genuine ladder fault (a reading above the idle reference); their intent was unchanged and their old fixture was the defect | Fixed | No — a latched fault was the wrong indication, not a safety failure; the release itself was always correct and is unchanged |
| N-31 | **A config push that removed a channel left that channel's KEY line driven (fixed 2026-09-23).** `Tick` runs `ServiceChannel` only for `channels_[0..channel_count_-1]`, and EVERY release path lives inside that per-channel service: the pulse timeout (`key_released_at_ms`), the ladder fault, and the head-unit-gone release. `ApplyConfig` re-derived `channel_count_` from the incoming config but released nothing first, so a channel the new config no longer names was never visited again — a `test_key` pulse or a resolved action still in flight when the apply landed held its key until the next reboot. **That is FR-39's phantom-key hazard exactly** (§6.7: "possibly *forever*, from the driver's point of view"), reached from an ordinary app action: `channels` is a list the user edits, so a two-channel install reduced to one is a normal save. It is also self-concealing — `LED2` keeps showing "a key value is currently being presented" (§7.3), which reads as the diagnostic working, not as the fault. **Fixed:** `ApplyConfig` releases every channel index between the new count and the old one BEFORE it adopts the new `channel_count_`, while `idle_code_`/`gain_mode_` still hold the values the line was actually driven under (`EstablishSafeIdle` only re-derives the channels the new config keeps, so the old pair is the correct release level for a removed one). Pinned by `RemovingAChannelReleasesItsDrivenKey`, mutation-tested by deleting the release | Fixed | No — reachable only through a config push that shrinks the channel list while a key is in flight, and the fix is a release, not a behaviour change |
| N-32 | **§6.2's command band — "Command targets must stay inside `[min_ladder, V_KEY_idle − 0.20 V]`" — was implemented nowhere, and the accessor that claimed to be its reference returned a different quantity (fixed 2026-09-23).** The sentence is normative and appears twice (§6.2 and the plan's gain-policy section), and it states a *physical* requirement rather than a preference: "so the sink FET is never asked to drive above the line's own resting level — above that point the servo can only turn `Q4` off, which is the release behavior, not a command." The only bound in the drive path was FR-18's, which clamps to the **output envelope** (1800–5200 mV) and knows nothing about the head unit's own idle. So a `key_mv` sitting *between* the measured `V_KEY_idle` and the 5200 mV ceiling was accepted by `ConfigValidate` (which refuses only `key_mv == 0`), encoded, acked, persisted — and then drove the radio to **nothing at all**, because the output only sinks: the FET was already off and the line stayed at its rest. The user sees a saved binding, a green config, an acked press, `LED2` reporting a key presented (§7.3), and no key at the radio — indistinguishable from a broken adapter, and the exact "clamped level still reaches the radio as a key" property FR-18's comment asserts. **The second half of the defect is a false claim in the code.** `SystemOrchestrator::IdleKeyMv()` had **zero callers** and a doc-comment reading "This is the `V_KEY_idle` of spec 6.2 and the reference every command is bounded against" — but it returned `idle_key_mv_`, which is `GainPolicyKeyMvForCode(mode, idle_code)`: the voltage of the **idle DAC code**, i.e. the level the firmware *commands* (spec 6.7 — idle is a high command, near the 5200 mV ceiling), not the level the head unit *pulls its own line to* (spec 6.2 step 1's `2 × /SENSEn`). It was also channel 0's alone, while §6.2 samples `/SENSEn` per channel and the two head-unit inputs are independent. A "written but never read" accessor whose comment describes a bound that does not exist is how this gap stayed invisible: the symbol census found the dead accessor, and the accessor's own comment named the missing feature. **Fixed:** `GainPolicyClampCommand(target, head_unit_idle_mv, *clamped)` implements the band as a single home (`[kOutputFloorMv, idle − kCommandHeadroomMv]`, `kCommandHeadroomMv = 200`), returning **0 when the band is empty** — a head unit idling below `kOutputFloorMv + 200` leaves no reachable level below its own rest, and 0 is the same "absent" the rest of the output code uses, so the caller releases and plays `KEY_UNKNOWN` rather than driving a level it cannot justify (FR-12's direction). The configured drive path clamps through it before FR-18's envelope clamp; the empty case also suppresses the tail's default `KEY_ACCEPTED`, because a press that drove no key must not be acknowledged as if it had. `IdleKeyMv(channel)` now returns `head_unit_idle_mv_[channel]` — the actual `V_KEY_idle` — and the dead `idle_key_mv_` member and its single writer are gone. Pinned by `GainPolicyClampCommand` unit tests (above the idle, in band, below the floor, empty band, no head unit) and an orchestrator test that a `key_mv` above the measured idle drives no key and plays `KEY_UNKNOWN`, each mutation-tested by deleting the band | Fixed | No — it cost a *dead* press, not a wrong one: the failure direction was silence rather than a phantom key, so nothing dangerous was driven. It was reachable from any app-written binding, though, and `test_key` was deliberately left outside the band: it is the bench/production check OF the output stage (FR-18's FR row, §4.3), it already refuses anything outside the 1800–5200 mV envelope, and its whole purpose is to measure the servo across that envelope. Gating it on the head unit's idle would make the bench unable to reach the output's own limits. That exemption is now stated here rather than left implicit |
| N-33 | **Both CI workflows were placed where GitHub Actions never reads them, so not one gate in §10.5 had ever executed (fixed 2026-09-23).** The workflows were created at `code/.github/workflows/{firmware,android}.yml`. The git repository root for this project is the **PCB repo root** (`git rev-parse --show-toplevel` reports it, and `code/` is not a repo of its own), and GitHub Actions reads `.github/workflows/` **from that root only** — a path this spec already states, normatively and in its own words: "a workflow placed at `code/.github/workflows/` is never executed". The plan repeated the rule at its top ("CI workflows live at the repository root, not under `code/`") and then contradicted it in Task 22's own *Files* block, and the trees were written to the Task-22 path. **Consequence:** every §10.5 gate — the native suite, the contract-sync diff, `check_spec_example`, `check_hal_contracts`, `crosscheck_config.sh`, `check_app_limits`, the device-suite link, the size gate, `check_stack_usage`, `check_task_ownership`, `check_sdkconfig_keys`, and the whole Android JVM suite — ran in no workflow at all. Each was nonetheless reported as covered (the coverage statement, which claimed "42 of 42" until N-41 corrected it), because every one of them passes when a human runs it locally; the defect is not that a check was wrong but that nothing ran it. It is the same class as the device-only blind spots (N-17, N-18, N-30) one layer up: a guard that does not execute is indistinguishable from a guard that passes, except in the one case it exists for. **The tell was visible and is worth naming:** the workflows' own steps were written `working-directory: code` and `git diff -- code/contract …` — paths that resolve ONLY from the repo root — so the file contents already assumed the location the file did not have, and R-1's "caught on day one in CI" guarantee (the size gate, the whole point of Task 22) had no mechanism behind it. **Fixed:** both files moved to `.github/workflows/` at the repo root, with no content change needed beyond their paths' now being correct; the plan's Task 22 *Files* block and its `git add` line were corrected to the root paths. Verified by `git rev-parse --show-toplevel` (PCB repo root), the absence of any other `.github/` in the repository, and the workflows' internal path prefixes, which all resolve from the root | Fixed | **Yes if CI is the release gate** — nothing here is unsafe on the device, but R-1 (app does not fit the OTA slot) and every contract-drift class the `contract-sync` step exists for would have been discovered on the boards' arrival or never. Until the workflows are pushed, the local commands in this spec's verify loop ARE the gate |
| N-34 | **The oversize-error frame stamped the protocol version as a hardcoded `"v":1`, the one copy `Ndjson.h` claims was removed (fixed 2026-09-23).** `Ndjson.h:27` states, as the *reason* `kNdjsonProtocolVersion` exists, that "the writer previously hardcoded the literal `"v":1` while Task 15 separately wanted a constant of this name, which is the same fact spelled twice" — yet `NdjsonWriter::Write`'s fallback branch (the frame emitted when a body would exceed `kNdjsonMaxFrame`) still wrote the literal, while that same function's normal branch and both `CommandRouter` emit sites used the constant. So the "same fact spelled twice" the header names as fixed had in fact survived in exactly one place. It was invisible for two compounding reasons: the branch is unreachable from any current caller (the largest `body` is 320 bytes at `CommandRouter.cpp:131`, against a 1024-byte cap), and the only test that reaches it — `NdjsonWriter.RefusesToEmitAFrameThatWouldExceedTheMaximum`, driving a 4096-byte body — asserts `h.type == "error"` and never reads the envelope's `v`, so nothing compared the two spellings. **Consequence if a protocol bump ever landed:** the peer dispatch (`CommandRouter.cpp:429`, and the app's `SwcClient.kt:239`) rejects an envelope whose `v` differs, so a bump that missed this copy would send the peer a frame — the frame whose whole job is to keep the link recoverable — stamped with the OLD version, which the peer would refuse or mis-handle. The failure would appear only under an oversized body on a bumped protocol, i.e. never in normal use, and only after the constant had already been trusted to be the single home. **Fixed:** the branch now formats `kNdjsonProtocolVersion`, and the oversize test asserts `h.v == kNdjsonProtocolVersion` (not `== 1`, so the assertion is tied to the constant), mutation-tested by bumping the constant to 2 AND reverting the branch to the literal — the test then fails | Fixed | No — unreachable from every current caller, and no behaviour changes when it is reachable (the field's value is identical while `kNdjsonProtocolVersion == 1`). It is fixed because a "single home" that is not single is the defect the surrounding comment exists to prevent, and because the next protocol bump is exactly when it would silently matter |
| N-35 | **Maintenance mode closes on a FIXED deadline, not the "5 minutes of inactivity" FR-38 and §8.2 specify — and two comments asserted the inactivity behaviour that does not exist (fixed 2026-09-23).** FR-38 reads "Maintenance mode MUST time out", and §8.2 names the close as "**timeout: 5 minutes of inactivity** — a device left unable to serve button presses because someone opened a web page is unacceptable"; the FR row says "5 minutes of inactivity returns to normal mode". `MaintenanceMode` has the machinery for that (`last_activity_`, bumped by `NoteActivity`, measured by `ShouldTimeout`), but **no production code path ever calls `NoteActivity`**: the only wrapper is `SystemOrchestrator::NoteMaintenanceActivity`, which itself has zero callers, so `last_activity_` is written once, by `Enter`, and the window is a fixed deadline from entry against `timeout_ms`. `Tick`'s comment claimed the close was already "after 5 minutes of inactivity", and `MaintenanceMode.h`'s `NoteActivity` comment claimed it kept "a user actively working in the web UI or typing a PoP" from being "kicked out mid-task" — both describing a call site that does not exist. **The activity input cannot be wired in this build:** the events that would bump it are an HTTP request and a PoP entry, and the web server, radio and provisioning session are exactly the device-only work that does not exist yet (N-15), so there is no source to call from. **Direction is safe but the gap is real:** a fixed deadline is STRICTER than inactivity (it closes no later than inactivity would), and closing early serves a press again sooner, which is the state FR-38 actually cares about — an unbounded window is the failure it forbids. So no user is left unable to serve input; the cost is a user mid-provision being closed out at the deadline rather than being kept in. **Fixed:** both comments now state the fixed-deadline behaviour, name the gap, and point here, rather than asserting an inactivity close that nothing implements — the same "a comment that describes the missing feature is how the gap stayed invisible" shape as N-32 and N-20. **The two USER-FACING strings that made the same claim were not fixed here and were found separately as N-39** — this row fixed the code's description of itself, not the UI's description of the device | RESOLVED 2026-09-24 (see the appended note) | No — a fixed deadline cannot leave the device unable to serve input (it is the safe direction), and the activity source is device-only work; the residual is a mid-provision close-out, which lands with the radio | **RESOLVED 2026-09-24, with the radio (N-15):** the activity source now EXISTS. `MaintenanceRadio.cpp`'s HTTP server counts every request it serves (`MaintenanceRadioRequestCount`), and `main.cpp`'s poll loop bumps the window's clock (`SystemOrchestratorNoteMaintenanceActivity`) whenever that count has moved. A count rather than a flag, because the server task runs concurrently with the poll loop and several requests can land between two ticks. So the close is now what §8.2 and FR-38 say -- `maintenance_timeout_ms` of INACTIVITY -- and a user typing a WiFi password is no longer reaped at the entry deadline. The existing host test `NoteMaintenanceActivityKeepsAWorkingUserInTheWindow` covers the mode's half; the wiring is device-only and its proof is the bench.
| N-36 | **A learn with no idle reference committed a profile the config validator refuses — persistently, losing the user's entire config at the next boot (fixed 2026-09-23).** `LearnSession::Commit` used the session's `learned_idle_mv_` for its RATIO gates but **substituted a nominal 2835 mV when it was zero** (`const int idle = (learned_idle_mv_ > 0) ? learned_idle_mv_ : 2835`), and the `prospective` validity check substituted yet another value (`: 1`). With a nominal idle standing in, every gate passed — the level read as "pressed" and "steady" — and `Commit` returned `kNone`. But the callers stamp the profile's denominator from the SESSION's own value (`LearnWizard.cpp:348` and `CommandRouter::HandleLearnCommit`'s `lp.learned_idle_mv = session_.LearnedIdleMv()`), so they wrote `learned_idle_mv = 0`, which `LadderProfileIsValid` refuses outright (`learned_idle_mv <= 0`). The chain is the same one N-32's sibling (the noisy-tolerance gate, seventh audit) closed for the WINDOW: `Commit` reports LEARN_OK, `ApplyLearnedProfile` applies it in memory, `ConfigStore::Save` writes it **without validating**, and the next boot's `ConfigDecodeBlob` ends by calling `ConfigValidate`, refuses the whole config, and `ConfigStore::Load` answers `kFellBackToDefaults` — so the user loses every learned button, binding and setting, reported only as a corrupt config. **Two compounding reasons it stayed invisible:** the only commit-time validity check validated `prospective.learned_idle_mv`, which the substitution had set to a POSITIVE value, so it could not see the zero the caller would actually store; and the property test meant to catch exactly this (`EveryCommittedProfileIsAcceptedByTheConfigValidator`) built its profile from a fixture whose `learned_idle_mv` was a fixed 2835 and never stamped the session's `LearnedIdleMv()` — so it validated the FIXTURE's idle, not the committed one. **Reachable in practice** from the app-driven learn (`RecordLearnSample` passes `sys_->IdleReferenceMv(ch)` straight into `AddSample`, and that is 0 for a channel whose live idle is unreadable); the headless path already guards its own `Tick` on `learn_idle_mv_ > 0`, so it is the app path that reaches it. **Fixed:** `Commit` refuses with a new specific reason, `kNoIdleReference` (`"no_idle_reference"`, FR-29's actionable-reason rule), when its reference is absent, and the two substitutions are gone — the ratio gate and the `prospective` check now both use `learned_idle_mv_`, the single value the caller will store. Pinned by `ALearnWithNoIdleReferenceIsRefusedNotCommitted` and a property test, `ACommittedProfileAlwaysCarriesTheIdleItsCallerWillStore`, that sweeps the idle over BOTH a fresh and a seeded channel and asserts what a caller would actually store (`learned_idle_mv = LearnedIdleMv()`) passes `LadderProfileIsValid`. Both were mutation-tested by restoring the original code exactly — the gate absent AND both substitutions present — and both then fail; the property test was corrected during mutation-testing after it was found NOT to fail, because the seeded fixture made the `prospective` gate refuse incidentally | Fixed | No — the failure direction is a lost config reported as corrupt, not a wrong key driven; but it is persistent (the config is unrecoverable from the user's side) and reachable from the ordinary app learn, so the fix is a refusal at the point of decision |
| N-37 | **`APP_LAUNCH` / `APP_INTENT` were dead for every app the visibility rules hide, because a PackageManager QUERY was used as a launch gate (fixed 2026-09-23).** `ActionRunner` decided a launch by asking about the package: `launchPackage` did `getLaunchIntentForPackage(pkg) ?: return AppNotInstalled(pkg)`, and `sendIntent` returned `NoHandler` when `queryIntentActivities(intent, 0)` came back empty. Since Android 11 (API 30) those queries are **filtered by package visibility** — the app targets 34 — so an installed app the user bound a button to is invisible to the query unless it is automatically visible or named in a `<queries>` element. The manifest declared **no `<queries>` at all** (grep-confirmed: the string appeared nowhere in the repo). **The sharp edge:** STARTING an activity does *not* need visibility — Android's own docs: "you can start another app's activity using either an implicit or explicit intent regardless of whether that app is visible to your app" (`training/package-visibility/automatic`) — so the query was the ONLY thing failing, and the null it returned ABORTED the action before the launch that would have worked was ever attempted. Every `APP_LAUNCH` binding was therefore dead for any app outside the automatically-visible set, and every `APP_INTENT` whose implied action was not automatically resolvable was reported as "no handler"; the user's fix ("install the app" / "check the action string") was wrong in both cases. **Two independent homes, both fixed.** (1) `AndroidManifest.xml` now declares `<queries>` with the `MAIN`/`LAUNCHER` intent signature — the exact intent `getLaunchIntentForPackage` resolves — so a launcher app is discoverable again; an `<intent>` signature rather than `<package>` because the target is the user's runtime choice (any package, any action), and `QUERY_ALL_PACKAGES` is a Play-policy-gated permission unsuitable for a utility app. (2) `ActionRunner` no longer treats a null/filtered query as evidence: `launchPackage` falls back to an EXPLICIT `MAIN`/`LAUNCHER` intent with `setPackage` (which needs no visibility) and lets `ActivityNotFoundException` be the "not installed" evidence, and `sendIntent` attempts the launch and maps `ActivityNotFoundException` to `NoHandler`. `SecurityException` still maps to `Blocked` unchanged, so spec 3.6's BAL outcome is preserved. **Why no test caught it:** no test constructed an `ActionRunner` at all (only `ActionAppViewModel`'s injected function was exercised), so the whole class was untested; Robolectric uses the merged manifest and does not enforce visibility filtering, so even a context-based test would not see the filtering itself. Pinned by a new `ActionRunnerPackageVisibilityTest` (5 cases) that models the production asymmetry directly — a `ContextWrapper` whose `startActivity` succeeds over a package manager where the query returns NULL — plus a manifest assertion that `<queries>` names MAIN/LAUNCHER. Mutation-tested both halves: restoring the `?: return AppNotInstalled` line fails `a filtered package query does not stop the launch` (and the Blocked case), and deleting the `<queries>` block fails the manifest test | Fixed | No — the failure direction is an app-side action that does not fire, never a wrong key or a safety path; but it silently disabled a headline spec 3.6 feature (`APP_LAUNCH`/`APP_INTENT`) for most targets |
| N-38 | **The app's link-failure recovery erased a failure the very frame that raised it had reported, whenever the link was already failed (fixed 2026-09-23).** `SwcClient.handle` ends by clearing a stale failure so one torn line cannot freeze the UI: the guard was `val failedBeforeThisFrame = _state.value is LinkState.Failed` taken BEFORE the dispatch, then `if (failedBeforeThisFrame && _state.value is LinkState.Failed) _state.value = Connected`. That cannot distinguish a STALE failure from one the current frame just raised — both read as `LinkState.Failed` — so a frame that BEGAN on a failed link AND wrote a new failure erased its own error and the link reported `Connected`. The frame that does both is `config_begin` with an out-of-range `total_len`: it is well-formed as a frame (so it passes the malformed-line check), and `beginInboundConfig` refuses the length INSIDE the dispatch. **Consequence:** after any earlier malformed line, a device offering an impossible config length produced a **healthy-looking link** while `getConfig` was answered with the unchanged local model — the app would show the user's old buttons as current, and the "device offered a N-byte config" error named nothing. The same shape reaches every in-dispatch failure (`endInboundConfig`'s short run / crc32 / sha256 / decode failures) on a link that was already failed. **Root cause of the blind spot:** the existing test, `a config error is not erased by the frame that reported it`, exercised only the case where the link was HEALTHY before the failing frame, so `failedBeforeThisFrame` was false and the branch never ran — the "must survive its own frame" requirement was asserted on the one path that could not violate it. **Fixed:** the guard now captures the state by IDENTITY (`val stateBefore = _state.value`) and clears only when the dispatch left it UNCHANGED (`stateBefore is LinkState.Failed && _state.value === stateBefore`). Every failure path assigns a fresh `LinkState.Failed(...)` (all 10 sites, grep-confirmed), so a changed reference means "this frame wrote a state" and it is left alone; an unchanged reference means the failure predates the frame and is recovered. Pinned by a new `a config error is not erased when the link was ALREADY failed` (a malformed line, then an out-of-range `config_begin`) — mutation-tested by restoring the equality-based guard, which fails it | Fixed | No — the failure direction is the app showing a stale config and a healthy link where the truth is a rejected transfer; the device is unaffected, but it is the same "detected but not reported" class the recovery rule exists to prevent |
| N-39 | **Both user-facing maintenance strings told the user an "of inactivity" behaviour the firmware does not implement, and pinned a literal "5 minutes" the config is free to change (fixed 2026-09-23).** N-35 found this exact gap in the *code comments* — spec 8.2 and FR-38 describe the close as "5 minutes of inactivity", while `MaintenanceMode` closes on a FIXED deadline from entry because nothing calls `NoteActivity` — and fixed the two comments. **It did not touch the two strings a user actually reads**, which made the same false claim: `LinkScreen.kt` rendered *"It returns to normal by itself after 5 minutes of no activity"*, and `assets/index.html` (compiled into `WebPageAssets.h`) rendered *"Maintenance mode ends automatically after 5 minutes of inactivity"*. The app's wording ("no activity") happens to describe what the firmware does, which is what made it read as correct; the web page's ("inactivity") does not. **Second, independent error in the same sentence:** "5 minutes" is only `maintenance_timeout_ms`'s DEFAULT. The field is a validated setting bounded to `(0, kMaintenanceTimeoutMaxMs]` — 3600000, one hour — and `CommandRouter`'s `config_patch` can change it live (N-35's own neighbour fix added that path), so a device configured for 20 minutes was still described to its user as 5. **Consequence:** the maintenance card and the provisioning page are the two places a user standing at the device looks, and both told them the window would stay open while they worked. The user that N-35 says is closed out mid-provision — someone typing a Wi-Fi password — is reading one of these two strings when it happens. **Fixed:** the card now derives its copy from the device's own `maintenance_timeout_ms` (carried on `LinkUiState`, sourced from the config in `AppViewModel.onConfig`), stated as how long the window LASTS rather than as "of inactivity"; `describeTimeout` rounds UP, because telling a user the window is shorter than it is makes them rush and longer is how they are cut off. The web page is a static asset with no access to the config, so it drops both the "inactivity" claim and the literal, and says the window closes on "its configured timeout". **Pinned by** `WebPageFind.TheTimeoutCopyDoesNotClaimAnInactivityBehaviourTheFirmwareLacks` and the new `LinkScreenTimeoutTest` (3 cases: the rounding, the sub-minute floor, and the rendered card naming a 20-minute config rather than 5) — both mutation-tested by restoring each string, which fails the corresponding assertion | Fixed | No — the firmware behaviour is unchanged and remains the safe direction; the defect is that two screens described a behaviour the device does not have, which is the N-35 shape one layer out (N-35 fixed the code's description of itself; this is the UI's description of the device) |
| N-40 | **The app's local config validator is a hand-maintained mirror of `ConfigValidate` and had silently fallen four rules behind it, with no CI guard on the rule level.** **RESOLVED 2026-09-24.** `ConfigJson.problems()` now mirrors the firmware's remaining rules: `learned_idle_mv` range `(0, kAdcCeilingMv]`, `mv_center` range `[1, kAdcCeilingMv]`, `LadderProfileIsValid`'s derived rule that `mv_tolerance`'s ratio be positive, `LadderWindowsAreDistinguishable`, `BindingNamesARealInput`, and the two unchecked string widths (`device_id`=`kDeviceIdLen`, `channel.name`=`kChannelNameLen`). The ratio predicate (`ladderRatioPermille`) is duplicated with the firmware's ROUNDED division and pinned by a boundary test (a truncating copy would refuse a legal profile the device accepts — verified by mutation). The rule-level guard the row asked for is `check_app_limits.py` grown by five constant pairs (channel-name width, device-id width, ADC ceiling, ladder button max, image cap) PLUS a differential harness: 55 mutated configs were run through the firmware's `ConfigValidate` and the app's `problems()`, and the two verdicts agree on all 55 (30 reject / 25 accept). Pinned app-side by 7 new `ConfigCodecTest` cases; 7 mutations verified caught | Fixed 2026-09-24 | No — latent (the app only edits bindings today), but a config the device refuses is now caught before Save |
| N-41 | **The §11 coverage statement claimed "42 of 42 requirements have an automated or scripted test. Zero requirements are covered only by inspection" while the table three lines above it and §6.4 both said otherwise (fixed 2026-09-23).** FR-17's own row carries `—` in its test column, glossed "**Absent.** The traceability row claimed a test for a correction that does not exist"; FR-17's requirement row (§5.3) says "**NOT IMPLEMENTED in v1**"; and §6.4 ends "FR-17 is therefore **not satisfied** and is recorded as such". The statement nonetheless counted it among the 42 and asserted that ZERO requirements rest on inspection. **Why this is the exact defect class the sentence exists to forbid:** the coverage statement is the document's one-line answer to "is anything taken on faith?", and it was itself the thing taken on faith — a reviewer reading only the summary would have concluded FR-17 was tested, which §6.4's own paragraph says was "exactly what would have satisfied FR-17 on review". A count that contradicts its own table is worse than a smaller, true count, because the table is the evidence and the summary is the claim. **Fixed:** the statement now reads 41 of 42, names FR-17 as the exception with the reason (no implementation, so no test — N-9 owns it), and states the correction rather than silently restating a number. N-33's row, which quoted the old "42 of 42" as evidence that a green-looking suite hid a gate that never ran, is cross-referenced to this correction so the quotation is not mistaken for current text. **No code change and no test:** the defect is in the document's description of its own coverage, which no test can observe — the check is that the count and the table now agree (verified by enumerating the 42 FR rows and finding exactly one with no test column) | Fixed | No — nothing on the device changes; the cost was a review-time false assurance about FR-17, which N-9 already tracks as an open implementation item |
| N-41b | **The same defect in the companion plan's Self-Review (fixed 2026-09-23).** `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md` §Self-Review-1 mapped `FR-17 → Task 13` and closed with "**No gaps.**", while Task 13's body contains no occurrence of "temp", "compensation" or "FR-17" and nothing implements the correction — the row named a task that only records FR-17's *inputs* (`temp_c_at_learn`, Task 9; `temp_comp_enabled`, Task 8). The plan's own test-discipline note, ninety lines above, already listed FR-17 among the requirements that once "had no test at all", so the traceability row contradicted its own document as well as §6.4, §5.3 and §11. Recorded as an extension of N-41 rather than a new item — it is one defect written in two documents, and a second number for it would imply two causes. **Fixed:** the paragraph now states that FR-17 is the one row whose task does not implement it, why the inputs made it easy to assume otherwise, and that 41 of 42 rows map to an implementing task. **No code change and no test**, same as N-41: the claim is about the document's coverage of itself | Fixed | No — review-time only; N-9 remains the implementation item |
| N-41c | **§8.2, FR-38 and the plan's maintenance prose described the window's close as "5 minutes of inactivity" while the firmware closes it on a FIXED deadline, and 5 minutes is only the DEFAULT (fixed 2026-09-23).** N-35 corrected the two *code comments*; the same claim survived in the places a reader and an operator actually see: §8.2's exit list ("timeout: 5 minutes of inactivity") and FR-38's §11 matrix row ("5 minutes of inactivity returns to normal mode"), plus `SystemOrchestratorTest.cpp`'s comment and its failure messages ("5 minutes of inactivity must close the window") and `ConfigFixtures.h`'s comment. (FR-38's own requirement row in §5.3 never claimed inactivity — it says only that maintenance "MUST time out" — so it needed no change.) Two independent errors in one phrase — (a) nothing calls `NoteActivity` in this build (`MaintenanceMode.h:63`, N-35), so the close is timed from `Enter`, not from the last activity; and (b) `maintenance_timeout_ms` is a validated setting (`(0, kMaintenanceTimeoutMaxMs=3600000]`), so a 20-minute device was described as 5. **Why it matters:** FR-38's whole reason for a bounded window is that "a device unable to serve input is unacceptable", and a reader who believes the window renews on activity would size it differently than the firmware runs it. **Fixed:** the spec's §8.2/FR-38 prose now states the fixed deadline and names `maintenance_timeout_ms` as the length; the tests' comments and failure messages say "the configured window" or name the setting. The user-facing copies were already fixed under N-39 (app card, served web page). **No test can observe a comment**, so the check is that no remaining prose in spec or plan asserts an inactivity behaviour — verified by grepping both documents | Fixed | No — behaviour is unchanged and already safe (an early close serves a press sooner, FR-38's stated direction); the cost was a mis-stated bound |
| N-42 | **The headless learn reported LEARN_OK for a button it did not store when the ladder was full, while the app path refused the same condition (fixed 2026-09-23).** `LearnWizard::ServicePrompt`'s append branch was a bare `if (count < kLadderMaxButtons) { store; ++count; }` with **no else**: at 16 buttons and no matching id the measurement was silently dropped, yet the code fell through to `profile_.learned_idle_mv = session_.LearnedIdleMv(); committed_ = true; buzzer_->Play(kLearnOk)`. The caller persists on that flag (`SystemOrchestrator::Tick` → `ConsumeCommitted` → `ApplyLearnedProfile` → `store_->Save`), so LEARN_OK sounded, the ladder was saved back **UNCHANGED**, and `LastLearnPersisted()` returned `true` — spec 7.4 step 6 defines "accept" as "BEEP LEARN_OK, **store LadderButton**", and FR-29 requires a rejection to say why. **The two learn write paths disagreed about one condition:** `CommandRouter::HandleLearnCommit` refuses the same full-ladder case with `Nack("no_space")` and stores nothing, so the app-driven learn was correct and the headless one — the ONLY production path, per the no-learning-screen requirement — was not. **Reachable with no contrivance:** a ladder the app filled to 16 buttons carries APP slugs while the wizard generates its own `swc1_bt<n>`, so a headless selection matches no id and takes exactly this append branch; a seventeenth AUX1 press reaches it too. **Why this is the project's recurring shape:** it is a *silent* failure with *correct-looking* feedback, the same one this document describes for the empty-`button_id` commit and the seedless profile — the user hears success and the button is not there, and nothing distinguishes it from a broken adapter. **Fixed:** the branch is now an else that sets `last_result_ = LearnReject::kNoSpace` plus a new enumerator (wire string `"no_space"`, the SAME vocabulary the app path already nacks with) and plays `kLearnReject`; `committed_` is set only when the button actually landed. Pinned by `LearnWizard.AFullLadderIsRefusedRatherThanReportedAsLearned` (a seeded 16-button ladder of app slugs, a headless learn of a 17th level: asserts the count stays 16, `LastResult() != kNone`, and no slot was overwritten) — **mutation-tested by restoring the silent path, which fails the `LastResult()` assertion.** Note the sibling gap the test also closes: `press_count_` is unbounded, so "17 presses" selects slot 17, which is exactly this branch (the ladder has 16 slots) | Fixed | No — the failure direction is a lost button reported as learned, not a wrong key driven; but it is the user's only learn path on a device with no app, and it is silent, so the fix is a refusal at the point of decision |
| N-43 | **The pass-through press test consumed the ADC's `-1` error sentinel as a reading and drove a phantom key on an unconfigured device (fixed 2026-09-23).** `SystemOrchestrator::ServiceChannel`'s pass-through branch read the ladder with a bare `hal_->adc_read_mv(...)` and fed the result straight into `const bool pressed = (idle - level_now_mv) > kPassThroughPressDeltaMv` (FR-25 / spec 6.9). `IHAL::adc_read_mv` returns **-1 on error — the sentinel is deliberately not 0**, because 0 mV is a legal reading (a button at the ladder's common), so `idle - (-1)` is `idle + 1`: **the largest possible press by this test's own arithmetic.** One failed conversion — an ADC glitch, or a bus fault held across ticks — therefore looked exactly like the wheel pulled hard off idle, and `PresentLevel` drove the mapped key while nothing was pressed: the phantom-key hazard FR-12 and FR-39 exist to prevent, and the reason `AdcReader` **drops** failed conversions rather than averaging them in as zero. **Three other raw reads in this file guard the sentinel** (`SystemOrchestrator.cpp`'s disabled-channel branch, its `head_unit_gone` sense read, and its boot reference capture) and so does `AdcReader.cpp` — **but `LearnWizard` did not, in two places; see below.** The same unguarded value also fed the reference self-heal (`if (level_now_mv > cs.pass_through_idle_mv)`), which the code's own comment says must only ever move the reference **UP** ("no button pulls the ladder UP from true idle"); with `level_now_mv == -1` that test is false, so the heal was inert rather than wrong on this path — but the comparison was reading an error code either way. **Why the class was invisible to every existing test:** the pass-through pulse self-releases after `send_duration_ms`, so a suite that polls for a fixed window and inspects the line at the END sees it back at idle and cannot see the drive at all — the regression test must sample **every tick**. **Fixed:** the read is now guarded by `have_reading = raw_level_mv >= 0`; a failed read holds the previous press state (`pressed = cs.pass_through_pressed`, so neither the rising nor the falling edge fires and the branch neither drives nor releases a key), and the tail's safety releases (a pulse timeout, a rail fault) still run for a channel this build never drove from a press. A failure says "no measurement": treating it as a release would cut a real press short, and treating it as a press invents one. Pinned by `SystemOrchestrator.AFailedLadderConversionDoesNotDriveAPhantomKey` (idle at 2835 mV, then the HAL returns -1 for the whole window; drives are counted on every 10 ms tick and must be zero) — **mutation-tested by restoring the unguarded comparison, which fails on the drive count.** Its first draft asserted only the FINAL code and passed against the broken code — the false negative above. **Two further sites of the same class, found and fixed in the same sweep (2026-09-23):** `LearnWizard::ServiceSelect` fed the raw AUX1 read to the classifier, where -1 becomes a ratio of ~0 permille against the 3300 mV idle — INSIDE AUX1's window (centre 30, half-width 485) — so it classified as **kPressed** and a run of failed conversions walked the slot menu with nobody touching the button; and `LearnWizard::ServicePrompt` computed `off_idle = level_mv - idle_mv`, which turns -1 into a large NEGATIVE excursion, so `(off_idle < -kPromptPressDetectMv)` read as a press AND `AddSample(-1, ...)` latched `out_of_range_seen_`, making one ADC glitch blame the user's wiring for a learn that then got a clean measurement. Both now skip the tick (`if (mv < 0) return;` / `if (level_mv < 0) return;`), pinned by `LearnWizard.AFailedAuxConversionDoesNotCountAsASelectionPress` and `LearnWizard.AFailedLevelConversionDoesNotStartOrPoisonThePrompt`, both mutation-tested. **The audit also enumerated the sites where the sentinel is consumed but NOT dangerous, so they are not re-litigated:** the boot-time sense read and the two per-tick sense reads turn -1 into `measured < envelope` and so resolve to the AMPLIFIED gain default (spec 6.2's stated safe direction) and to `head_unit_gone` → release-and-keep-classifying (a fail-safe, no latch); the two `ServoLoop::Update` call sites receive it into a loop that ships `enabled = false` (FR-19) so the value is inert in v1; and the boot ladder-idle, learn-idle and `ServiceLearn` AUX1 reads already guarded it. The rule this leaves behind: **every raw `adc_read_mv` site must test `>= 0` before the value is used in arithmetic, and the fix for a failed read is to HOLD state, never to resolve a press or a release** | Fixed | No — the failure direction is a phantom key driven with no press, which is the worst direction for this product (an unintended command to the head unit); but it requires a failed conversion, so on a healthy bench it is latent |
| N-44 | **The app derived a Binding `id` as `"${button}-${gesture}"`, which for the ONLY production learn vocabulary exceeded the field width — so the app refused its OWN edit and disabled Save (fixed 2026-09-23).** `AppViewModel.withEdits` built the id for a user-authored triple from the button id and the gesture wire name. `LearnWizard::FormatSlotId` — the headless AUX1 wizard, and per the no-learning-screen requirement the only learn path that exists in production — generates `"swc%d_bt%d"` with both indices 1-based, so slot 10 is `swc1_bt10`. Against `Gesture.SINGLE` (`"SINGLE"`, 6 chars) that derivation is `swc1_bt10-SINGLE` — **exactly 16 characters**, which is `ConfigModel.h`'s `kBindingIdLen`; the field is `char id[16]`, `ReadStr` refuses `n >= width`, and `ConfigJson.problems()` mirrors that as "binding '…': id must be under 16 chars". **The refusal is not a warning.** `AppViewModel.save()` returns early on a non-empty `problems` list and `BindingScreen`'s Save button is `enabled = state.problems.isEmpty()`, so a single such cell in the grid disabled Save for the WHOLE config — every other valid edit was lost behind it — and the message named an id the user never typed and could not change. **Why this is the same silent-foreclosure shape as N-40 and N-42:** the gate is local and truthful, and the thing it refuses is the app's own invention; the user sees "id must be under 16 chars" against a button they only named by pressing it. **Reachable with no contrivance:** ten headless-learned buttons take a channel to slot 10, which is well inside `kLadderMaxButtons` (16), and a re-learn onto a moved rail is expected (spec 6.5). **Fixed:** the id is now spec 3.5's own ordinal slug, `b<ordinal>`, where the ordinal is `kept.size` plus the edits emitted so far — so the ids stay distinct within a save. Truncating the button id to fit was rejected: slot 10's `swc1_bt10` truncated to the last 8 characters reads as `swc1_bt1`, an id that names slot 1, and no length-preserving scheme survives a button id that is itself 15 characters. **The ordinal is safe because nothing reads a Binding `id`:** `BindingResolve` matches on `b.enabled`, `b.gesture`, `b.channel` (or `kAny`) and `strcmp(b.button, button_id)` — the id is never compared — and the app's grid keys on the same `(channel, button, gesture)` triple, so a collision with a device-supplied id retained in `kept` is inert. Pinned by `AppViewModelTest.an edit on a headless-learned button produces an id the device accepts` (a config whose SWC1 ladder holds the real `swc1_bt10` slug; edits its SINGLE cell; asserts `problems` is empty, the save reaches the transport, the binding appears, and its id is under `kBindingIdLen`) — **mutation-tested by restoring `"${button}-${gesture.wireName}"`, which fails at the `problems` assertion.** Sibling recorded, not repaired: the `mv_tolerance`/`mv_center`/`learned_idle_mv` rule-level parity gap is N-40 | Fixed | No — the failure direction is a Save the user cannot make, not a wrong key driven; but Save was disabled for the entire config by one cell, and the app's own validator was the thing doing it |
| N-44b | **The same field's width was a bare `16` literal in the app and the guard that claims to pin "the string widths" pinned only two of them (fixed 2026-09-23).** Found by auditing N-44's own fix. `ConfigJson.problems()` checked the binding id (`b.id.length >= 16`) and the ladder button id (`btn.id.length >= 16`) against **hardcoded literals**, and `check_app_limits.py`'s `PAIRS` carried only `kActionTargetLen`/`kDataPayloadLen` — while that file's docstring says it pins "the counts, the string widths, and the two timing ceilings". **Two constants are involved, not one:** `Binding.id`/`Binding.button` are `char[kBindingIdLen]` (`ConfigModel.h`) but `LadderButton.id` is `char[kLadderIdLen]` (`Analog/LadderDecode.h`) — a different header and a different constant that merely happened to share the value 16, so pinning one to the other by inspection would have hidden a re-tune of either. **Why the omission mattered in both directions:** a firmware width made LARGER than the app's literal makes the app refuse an id the device would store — precisely the N-44 failure, in which the local gate foreclosed the user's own edit and disabled Save for the whole config; a width made SMALLER lets the app send an id the device refuses at `ReadStr`, which nacks the whole save with the field unnamed, the very failure `problems()` exists to pre-empt. **Fixed:** two named Kotlin constants (`K_BINDING_ID_LEN`, `K_LADDER_ID_LEN`) replace the literals and carry the failure-direction rationale; `PAIRS` gained a row for each and grew a per-row C++ header column, because the two constants do not live in the same file. Pinned by `ConfigCodecTest.a valid config refuses an over-width binding id and ladder button id` — which asserts the **boundary** (exactly 16 refused, 15 accepted, for each of the two id fields), since a width mirror is only meaningful at its edge. **Both new guard rows mutation-tested** (tuning `kBindingIdLen` and `kLadderIdLen` to 17 independently each fails the guard, naming the drift), and **the app-side boundary test mutation-tested** (setting `K_BINDING_ID_LEN` to 17 fails the test). **Scope note:** this pins the two id widths only; the wider rule-level parity gap remains N-40 | Fixed | No — no behaviour changes today (all three values are 16); the defect was that a re-tune on either side would have been silent, in the same shape as the `send_duration_ms` ceiling that prompted the guard's existence |
| N-45 | **The app's app-side action failure surface was computed, worded, and then rendered nowhere; the press was silent on both ends (fixed 2026-09-23).** `AppViewModel.actionOutcomes` is a public `StateFlow` whose own doc-comment names spec 3.6's Android BAL limitation (R-5) and states the rule the type exists for: "'the button did nothing' is the outcome `ActionOutcome` exists to prevent". `runAppSideAction` classifies every app-side kind into `Ran` / `Blocked` / `Failed`, words a line for each failure ("Android refused from the background", "app not installed", "this build does not implement <kind>"), and assigns the list to `_actionOutcomes`. **Nothing collected it:** `grep -rn "actionOutcomes" android/app/src/main/` returned exactly three hits, all inside `AppViewModel.kt` (the backing field, the public accessor, the assignment) — no screen and not `MainActivity` referenced it, so the message was produced and dropped at the last step. **Why that is not a cosmetic gap:** spec 3.6 splits the action library, the `OUT_` family belongs to the firmware and everything else to Android, and `SystemOrchestrator`'s execute path `ReleaseKey`s for an app-side kind rather than hold a key with no action behind it. So an app-side action that cannot run is silent on BOTH sides at once — no key on the wire and no message on the phone. The user's only evidence is a button that did nothing, which is verbatim the outcome the whole `ActionOutcome` type was introduced to prevent, reintroduced one layer below its own documentation. **It is the project's recurring diagnostic shape:** N-24 (`UsbCdc::dropped_`, counted and never reported — repaired in the same audit, together with its inbound twin `RxOverflows`, which was worse: an inbound overflow discards a frame the app SENT before parsing, so the command fails with no error at all), N-22 (`rail_mv`, read with no producer), and `ActionRunner` before it had any caller — a diagnostic that terminates inside the class that produced it reports nothing, and no test can see it because every unit of the chain is individually correct. **Fixed:** `actionProblems: List<String>` was added to `LinkUiState` and a red card renders it in `LinkScreen` — its own card, NOT folded into the device-warning list, because those lines arrive from the device over `log` while this happened on the phone, and the fix is a permission or another app rather than the adapter; the card's copy says so explicitly ("The adapter still sent the key press… This is the phone's half of the action (spec 3.6)"). `AppViewModel.init` gained a collector mirroring `actionOutcomes` into `_link`, and the `Frames.EVENT` branch clears the list at the one place a press arrives, so the warning describes the LAST press rather than persisting for the rest of the session (the clear sits at the EVENT branch and not inside `runAppSideAction` because the unrecognized-press path never reaches `runAppSideAction`, and clearing only there would leave a stale warning through every press the device could not attribute to a button). The flow remains the single home for the OUTCOME; the link state carries a copy for rendering. Pinned by extending `AppViewModelTest.an app-side action that cannot run is reported, not swallowed` with a mirror assertion on `vm.link.value.actionProblems` — **mutation-tested by commenting out the collector, which fails exactly that assertion (`actionProblems: []`) while the `actionOutcomes` assertion still passes**, which is the point: the defect was never the classification, it was the last hop | Fixed | No — the failure direction is a warning the user never sees for an app-side action that already did not run; the key press itself is unaffected and the adapter behaves correctly. It matters because R-5 (Android BAL) is a named headline risk whose whole mitigation is telling the user why the button did nothing |
| N-46 | **No gate tied a firmware-emittable frame to an app-side handler, and `link_gap` had sat unhandled for a whole revision behind a green suite (fixed 2026-09-23).** Spec 4.3 is a closed frame vocabulary and the contract generator emits a Kotlin constant for every name, so both sides can always NAME every frame. Nothing checked that the app ever LOOKS at one. `link_gap` was the live instance: declared in `contract_schema.py`, generated into `Contract.kt` as `Frames.LINK_GAP`, and referenced by no branch in `AppViewModel.onFrame` — so a frame the app's own outgoing command was lost (`config_chunk`, `learn_commit`) produced a device-side `link_gap` that the app parsed past, leaving a failed transfer indistinguishable from a device that ignored the request. That handler was added earlier in this audit series; what was missing is the guarantee that the NEXT frame cannot repeat it. **Why the defect class is worth a gate rather than a careful reading:** it is the same shape as three separate findings already recorded here — N-22 (`status` declaring fields with no producer), N-24 (transport loss counters reported to nobody), N-45 (`actionOutcomes` rendered nowhere) — all of them *a message produced on one side and consumed on no side*. The frame axis is the one where the omission is a single line in a `when` block with no other observable symptom: the device emits, the app ignores, nothing logs and nothing fails. **The existing contract-sync gate cannot see it by construction.** `test_generated_header_matches_the_checked_in_copy`, `test_generated_kotlin_matches_the_checked_in_copy` and `test_frame_field_lists_match_the_router` check that the generated copies agree with the schema and that declared FIELDS have producers; all three are satisfied by a frame the app never handles, because the constant existing is exactly what they assert. **Fixed:** a new guard, `code/tools/check_frame_handlers.py`, makes ONE check per way a frame can fail to arrive anywhere. (1) Every `fw2app`/`both` frame in `contract_schema.FRAMES` must be referenced by at least one Kotlin source outside the generated contract file, and a constant must actually have been generated for it (a name with no constant is a generator drift the app could not work around). (2) The inverse direction, audited while writing it: every `app2fw`/`both` frame must appear in the router's `IsKnownCommand` — a frame the app can send that the dispatcher omits is nacked `unknown_type` at runtime, so nothing looks wrong until a user tries the command. (3) Every name `IsKnownCommand` ACCEPTS must reach a real branch in `OnLine`'s dispatch chain rather than falling into the trailing `else` and being nacked `not_implemented`, which names the wrong reason; the audit confirmed all 15 non-OTA accepted commands do dispatch, and `ota_*` is the one deliberate exception (this build nacks it `not_implemented`, which is true rather than a no-op — that is the N-14/§9.5 open item, not a defect). Checks 2 and 3 read `IsKnownCommand` and the dispatch chain out of `CommandRouter.cpp` rather than a second hand-kept list, for the same reason `_router_emitted_fields` reads the router. The schema is what this reads, not `Contract.kt`, because the schema decides whether a frame is on the wire; the generated constant only decides whether the app can name it. It is wired into `.github/workflows/firmware.yml` beside `check_app_limits.py` — a different axis from contract-sync: sync checks the two generated copies AGREE, this checks the app USES what was generated. **The guard is tested by damaging the tree, not by passing on it:** `code/tools/test_check_frame_handlers.py` copies the app sources, renames the one `Frames.LINK_GAP` reference to a mutant, and asserts the guard exits 1 naming `link_gap` — a guard exercised only on a clean tree is a guard nobody has seen work. All three checks were also mutation-tested directly against the live tree: renaming `Frames.LINK_GAP` (check 1), removing `time_sync` from `IsKnownCommand` (check 2), and renaming the `identify` dispatch branch (check 3) each fail with the frame named, then restore clean. Scope note: the check is structural and cannot tell a handler that does nothing useful from one that works; what it pins is that the frame NAME has a branch, which is the part that silently rots | Fixed | No — the one live instance (`link_gap`) was already repaired earlier in this series; this is the guarantee that the next frame added to the vocabulary cannot be emitted into a void |
| N-48 | **`learn_commit`'s new length bound used the wrong pair of constants — the binding/channel widths instead of the ladder's — which is exactly the drift those constants exist to prevent (fixed 2026-09-23).** Found auditing N-24/N-45's own fix. `HandleLearnCommit` gained a guard refusing an empty or over-long `button_id`/`name` before the copy into the stored button: `strlen(btn) >= kBindingIdLen \|\| strlen(name) >= kChannelNameLen`. But both strings are written into **`LadderButton`** (`snprintf` into `out.id`/`out.name`), whose fields are `char[kLadderIdLen]` and `char[kLadderNameLen]` (`LadderDecode.h`). The four constants are all 16 today, so nothing misbehaves now — the defect is latent, and it is the *precise* failure the separation is for: `ConfigModel.h`'s `kBindingIdLen` and `Config.kt`'s `K_BINDING_ID_LEN` both carry a note that a re-tune of one side alone must be caught, and `LadderDecode.h`'s ladder widths are a **different family** kept separate on purpose (the same reasoning `Config.kt`'s `K_LADDER_ID_LEN` states: "keeping them as two constants means a re-tune of one alone is caught rather than silently applied to both"). Bounding a `LadderButton.id` by `kBindingIdLen` re-couples the two: a future re-tune of `kLadderIdLen` (say to 24, to hold longer slugs) would leave the guard still refusing at 16, so a legal id would be nacked — and a re-tune DOWN would let through an id `snprintf` silently truncates, storing a button whose id no longer matches the binding the user wrote. **The direction is the one the guard was added to close.** Fixed: the bound reads `kLadderIdLen`/`kLadderNameLen`, with a comment naming why the pair is the ladder's (the copy is into `out`, not into a `Binding`). Pinned by a new `CommandRouter.ALearnCommitRefusesAnEmptyOrOversizedIdOrName`, which drives four refusals (empty id, empty name, id at the ladder width, name at the ladder width) and then asserts a legitimate id ONE BELOW the width DOES commit — so the test bounds the value rather than proving the field is refused outright, and it fails if the bound is widened or removed. **Mutation-tested:** reverting the guard fails both this test and the pre-existing `ALearnCommitWithAnEmptyButtonIdIsRefused` | Fixed | No — all four constants are 16 today, so the behaviour is identical; the cost is a live re-coupling of two constant families the codebase deliberately keeps apart |
| N-47 | **§7.4 described the Android app as the PRIMARY learn driver "with live graphing" and the AUX1 + buzzer path as a "fallback" — inverting the product requirement and describing a screen that does not exist (fixed 2026-09-23).** The requirement is the opposite and is hard: the app has **no learning screen**, and the AUX1 interaction is the only production learn path (FR-31: the firmware "MUST be able to learn with **no app connected**"). The paragraph's own next sentence calls the AUX1 path "fallback, FR-31" — so it cited the requirement while demoting it, the same self-contradiction shape as N-41's coverage count and N-41c's inactivity wording. **Both halves are checkable and both are false against the tree.** (a) *No app learn exists*: the app sends no `learn_start`/`learn_stop`/`learn_commit` at all — its only send sites are `ping` and `config_get` — and the three constants are generated but referenced by no source (the same 'constant with no user' shape N-46's new guard now pins for the RECEIVE direction; the SEND direction is a UI gap, not a handler gap, so the guard correctly does not flag it). The app's `LadderScreen` handles only the RECEIVE side of `ladder_sample`, so the "live graphing" the paragraph promised is a display that is never fed during a learn — `learn_open_` is only ever set by `HandleLearnStart`, which the app never sends. (b) *The AUX1 path is fully implemented*: `LearnWizard` runs the whole §7.4 loop (enter hold, auto-detected input, prompt, sampled commit) on the device, with no app, and N-19/N-42/N-44 were all fixed against it precisely because it IS the shipping path. **Why the wording mattered rather than being a harmless priority claim:** it was the one place a reader would look to answer "how does a user learn a button?", and it sent them to build or expect an app screen that the requirements forbid. The frames exist in the protocol (they are how the bench tooling drives a learn), so the drift was in the PROSE's claim about the product, not in the code. **Fixed:** §7.4 now states first that the AUX1 path is the production path and that the app has no learning screen, explains WHY that is a deliberate choice (the interaction is a hold-and-press on the device, and a second UI-driven write path for the one operation that persists a config is a risk with no benefit), and says explicitly that the protocol frames remain implemented for host/bench use. It also records that both paths share one commit implementation, so they cannot disagree about the config they write. **No test can observe a paragraph**, so the check is that no remaining prose in the spec or plan calls the app the primary learn driver or promises an app learn screen — verified by grepping both documents | Fixed | No — behaviour is unchanged and already correct (the AUX1 path works); the cost was a reader building or expecting a screen the requirements forbid |
| N-49 | **The link-scoped teardown had drifted between its two callers, and the silence reap left link-scoped state behind that the disconnect path cleared — a half-sent `config_get` reply, and the learn SESSION's samples (fixed 2026-09-23).** Spec 4.4 promises the same thing for both ways a link ends ("after 10 s of silence the firmware considers the link down", and a disconnect): no half-finished work survives. There are two ways a link ends — `CommandRouter::OnDisconnected` (a real transport event) and `NoteSilenceIfStale` (the peer went quiet) — and they had **drifted**: `OnDisconnected` cleared the `config_set` run, the `config_get` reply run, and the learn stream; the silence reap cleared only the first and the third. **Two consequences, both reachable from ordinary app behaviour.** (a) *A half-sent reply resumed after the link was declared down.* `Process()` emits one `config_chunk` per call and gates on `reply_open_` alone — never on `connected_` — so a `config_get` whose reply was in flight when the app went quiet kept streaming the rest of a config into a FIFO nobody drains, filling the 2 KB TX buffer until a genuine reply was refused (`tx_dropped` climbing); on a link that recovered without re-enumerating (`OnLine` clears `link_down_` but not the reply) the stale run resumed into a session spec 4.4 calls stateless. (b) *A session outliving its link.* Only `learn_open_` was cleared, never the `LearnSession` itself, so `learn_start(0)` → stream → link drop → reconnect → `learn_commit(channel 0)` with **no** `learn_start` ran `session_.Commit` on the dead stream's samples and stamped `lp.learned_idle_mv` from the rail measured before the drop — the device acked and persisted a button measured in a session the spec declares discarded. The reap's own comment already ASSERTED the session was link-scoped ("a learn stream belongs to the app session that opened it"), so the prose was right and the code was not — the project's dominant defect class. **Fixed:** one function, `ResetLinkState()`, clears every piece of link-scoped state (the config run, the reply run, and the learn stream AND its samples via `ForgetLearnSession`), and BOTH callers use it — so the two paths cannot drift again. `learn_channel_` gained a `-1` "no session" sentinel, and `HandleLearnCommit` refuses `no_session` when it is set rather than reading the sentinel as a channel and reporting `channel_mismatch` (the wrong, un-actionable reason FR-29 forbids). Pinned by `CommandRouter.SilenceDiscardsAPartlySentConfigGetReply` and `CommandRouter.ACommitAfterTheLinkDroppedHasNoSessionToCommit`, both **mutation-tested** (reverting each half fails its test) — the second's first draft used the fixture's `vol_dn` at a level that collided with an existing button, so the commit was refused whether or not the session survived and the mutation was NOT caught; it now measures a non-colliding 2400 mV under a fresh id, which is what makes it bite. **Three further link/protocol defects found in the same pass and fixed with tests:** (c) `learn_stop` did `(void)root`, so its declared `channel` was accepted and ignored — `learn_stop{channel:1}` closed channel 0's open stream and acked, while the sibling `learn_commit` guards the same field; it now refuses `channel_mismatch` for a stream it is not stopping, and deliberately does NOT clear the session (spec 4.3's flow is start → stream → STOP → commit). (d) `reboot` acked `boot_target:"bootloader"` while both targets ran the identical bare `esp_restart()`, so a peer expecting a serial-flash loader talked to the application port — a target the device cannot honour, reported as success. It was refused `bad_target` and the frame offered one target (§4.3 row corrected). **SUPERSEDED 2026-09-24 by N-80:** the refusal rested on a false hardware claim (that entering the ROM loader was power-on-only), and both targets ARE honourable on the S3 — `bootloader` now routes to `IHAL::reboot_to_download`. The ROUTING this item asked for is what N-80 delivered; what it got wrong was concluding the destination was unimplementable rather than unimplemented. `CommandRouter.RebootSelectsTheDestinationNamedAndRefusesAnyOther` replaces the test named below. (e) `total_len` (config_begin) and `offset` (config_chunk) were range-checked as doubles then truncated by a bare `static_cast<size_t>`, so `total_len:500.9` was acked as 500 and `offset:9.5` satisfied the contiguity test at 9 — the exact "the value that arrived is not the value that was sent" rule `NumToU32` and the codec's `ReadU32` exist to enforce in the same file. Both refuse a fraction now. Pinned by `CommandRouter.LearnStopNamesTheStreamToClose` and `CommandRouter.AConfigRunRefusesAFractionalLengthOrOffset`, both mutation-tested (the third test this item named, `RebootRefusesAnUnimplementableBootloaderTarget`, was superseded by N-80 — see (d) above). **Judged clean in the same pass** (recorded so they are not re-litigated): `UsbCdc`'s SPSC ring and epoch pull-back, `ServiceTx`'s write-advance, `NdjsonReader`'s overflow latch, `Base64Decode` padding, `ConfigStore::ReadSlot` chunk-0 width, the `EmitGesture`/`Nack`/`EmitLog` buffer arithmetic, and `LinkWiring`'s thunk separation | Fixed | No — the reply-resume fills the TX buffer (a refused reply, not a wrong key) and the session leak writes a stale measurement to a config the user asked for; neither drives a key on its own, but both are silent and both are reachable from ordinary app behaviour |
| N-50 | **Three handlers applied `static_cast<int>`/`static_cast<size_t>` to a peer-supplied double BEFORE checking its range — undefined behaviour on a value the wire can carry (fixed 2026-09-23).** Found auditing the N-49 fixes: the range/integrality guards on `total_len`, `offset` and `key_mv` cast the raw `valuedouble` inside the comparison itself, so an out-of-range value reached the cast before the bounds were consulted. **The value is reachable, not theoretical:** cJSON's `parse_number` calls `strtod` and ignores `ERANGE`, so a JSON number literal that overflows `double` arrives as `+inf` — verified against the pinned 1.7.19 parser, `{"total_len":1e999}` parses to `valuedouble == inf`. `HandleTestKey`'s guard was the worst of the three: `mvd != static_cast<double>(static_cast<int>(mvd)) \|\| mvd < -32768.0 \|\| mvd > 32767.0` casts to `int` FIRST, so `key_mv: 1e999` was UB before the range test ran, and the comment above it already CLAIMED the opposite ("bounded BEFORE the int cast") — the prose was right and the code was not, the project's dominant defect shape. **Fixed:** each site now checks the range in the NaN-safe negated form (`!(v >= low && v <= high)`, under which a non-finite value is false and therefore refused) BEFORE any narrowing cast, and casts only once the value is provably in range. Pinned by `CommandRouter.ANumericFieldOverflowingToInfinityIsRefusedNotCast` (drives `1e999` into `key_mv`, `total_len` and `offset` and asserts a `nack`, not an `ack`) — **recorded honestly as a REGRESSION GUARD rather than a mutation-detector: on x86-64 the pre-fix cast-first form ALSO rejected `inf` (the UB happened to produce a value failing the equality test), so reverting the fix does not fail the test (checked, both mutants passed).** What the fix removes is UB a different compiler or optimisation level may exploit — a compiler is entitled to assume a cast is in range and delete the guard. **Audited the same idiom across the whole tree and deliberately left five sites UNCHANGED because they are safe by ordering** (each returns on `v > max` BEFORE casting, and `inf > max` is true; NaN is unreachable through cJSON's digit-only number scan): `ConfigCodec.cpp`'s `ReadU32` and `ReadU64`, `ReleaseCheck.cpp`'s `ReadSize`, and `CommandRouter.cpp`'s `NumToU32`/`NumToU8`/`NumToChannel` — recorded so they are not "fixed" into churn | Fixed | No — the UB is unobservable on the host toolchain (the value is refused either way); it is a latent portability/soundness defect, not a live wrong behaviour |
| N-51 | **FR-37's rollback health-gate could not be false — it was a compile-time constant `true`, so a device that boots but cannot drive the DAC had its pending rollback CANCELLED (fixed 2026-09-23).** `app_main` marked the OTA image valid on `SystemOrchestratorSafeIdle(sys)`, which returns `safe_idle_established_` — a flag **assigned `true` once**, at the end of an `EstablishSafeIdle()` that returns `void` and cannot fail, and never cleared. The condition was therefore unfalsifiable on the device path, and spec §9.8 is explicit that the gate must prove the device "can do its job — not merely after main() starts": "An image that boots but cannot drive the DAC is not healthy, and confirming it would strand the user with a bricked-but-\"valid\" device." That exact case was the one the gate could not detect. **The missing falsifiable signal is FR-13's DAC read-back**, tracked as N-21; this fix supplies the narrower one that the void write already makes available. `IHAL` gains **`dac_faulted`**, a LATCHED accessor set by any failed `i2c_master_transmit` (code *or* power-mode write — a gain-mode write that did not land leaves the output on the wrong gain, which is "cannot do its job" too) and never cleared, per spec 7.3's reboot-only rule. `SystemOrchestrator::OutputVerified()` = `safe_idle_established_ && !dac_faulted()`, exposed as `SystemOrchestratorOutputVerified`, and `app_main`'s mark-valid now reads THAT. **The accessor rather than a return-type change** is deliberate: every `dac_set_code` call site stays void (N-21 still owns the `esp_err_t` fix), and the gate gets a signal it can branch on today. Pinned by `SystemOrchestrator.TheHealthGateCanSayNoWhenADacWriteFails` (a healthy boot is verified; a device whose safe-idle write failed is **not** — and `SafeIdleEstablished()` is asserted `true` in the same breath, to show why it could not be the gate alone), **mutation-tested by restoring `return safe_idle_established_;`, which fails the assertion.** `MockHal` gains `FailNextDacWrite()`, which leaves the previous code in place rather than storing the new one — a failed write landed nothing | Fixed | No — the failure direction is confirming a bricked image, which needs a pending OTA boot AND a dead I2C bus; but it is the one line that decides whether a bad image is recoverable |
| N-52 | **`SemverCompare` compared a fourth version component by DROPPING it, so two different versions read as EQUAL and the update was never offered (fixed 2026-09-23).** The numeric loop ran `for (int i = 0; i < 4; ++i)` where semver 2.0.0 has exactly three (major.minor.patch), and it consumed the dot on the last iteration too. It therefore exited with both pointers still ON a 4th component, and the code after it compares only the pre-release and the build metadata — neither of which a plain `1.2.3.4` has. **Measured on the pre-fix function:** `SemverCompare("1.2.3.4.5", "1.2.3.4.6")` returned **0**. A 4-component version is not strictly semver, but it is exactly what a date- or build-stamped tag produces and the pipeline publishes whatever the maintainer tags, and the failure is silent and in the worst direction: the running device is told it is up to date and never offered the new image — the same "release invisible forever" shape as the lexicographic bug the loop's own comment cites, one field over. **Fixed:** the loop runs three times and consumes the dot only BETWEEN components, then an extension loop compares further dotted components **numerically** (so `1.2.3.9` still sorts below `1.2.3.10` — a text compare would invert that pair) for as long as either side has one, reading an absent component as zero (so `1.2.3` still equals `1.2.3.0`). Pinned by `ImageVerify.SemverComparesAFourthComponentRatherThanDroppingIt`, **mutation-tested by restoring the 4-iteration loop, which makes the pair equal again.** The pre-release/build-metadata block below the loop is unchanged, so all 22 existing semver assertions still hold | Fixed | No — a missing update offer, not a wrong install; nothing is flashed that should not be |
| N-53 | **A device nack or link timeout DISABLED the Save button, leaving a transient failure with no retry (fixed 2026-09-23).** `AppViewModel.save()` wrote its runtime failure — a nack, a timeout, a refused link — into `BindingUiState.problems`, **the same field `BindingScreen` reads as `enabled = state.problems.isEmpty()`**. `ConfigJson.problems()` is a *validation* list; a runtime failure is not a validation failure and is not something the user can fix by editing a cell. The pending edit was still rendered in its cell, so the change *looked* live while the device held the old bindings, and there was no way to retry: the only recovery was to edit an unrelated cell, which re-sent the same config. **Fixed:** `BindingUiState` gains a separate **`saveError: String?`**, rendered in its own "Save failed" card and deliberately NOT gating the button; `save()` clears `problems` on a runtime failure and `saveError` on a validation refusal, so the two can never be confused for one another. Pinned by extending `a refused save is reported and the pending edit is retained` to assert `saveError` is set AND `problems` is empty, **mutation-tested by writing the failure back into `problems`, which fails the new assertion.** The two sibling Android defects found in the same pass are below | Fixed | No — the user cannot retry a transient link failure, and their edit appears live while it is not; no wrong key is driven |
| N-54 | **Two more Android gates that could not fire, and a dead end the retry button could not leave (fixed 2026-09-23).** (a) *"Try again" could not recover a missing device.* `UsbSerialTransport.open()` was called exactly once, from `MainActivity.onCreate`, and nothing ever enumerated again — there is no `ACTION_USB_DEVICE_ATTACHED` receiver. An app opened BEFORE the adapter was plugged in therefore showed "No device found" permanently, and `retry()` was `= connect()`, which sends a `ping`; `UsbSerialTransport.write` returns early when `connection == null`, so the retry **wrote nothing** and re-reported the same problem forever. `SwcTransport` gains `reopen()` (a default no-op for test doubles), `UsbSerialTransport` implements it as `close(); open()`, and `retry()` re-enumerates then connects — so plugging the adapter in and tapping the button now works. (b) *The picker's Apply gate ignored `OUT_VOLTAGE`'s parameter.* `enabled = !kind.needsParam \|\| target.isNotEmpty()`, and `OUT_VOLTAGE.needsParam` is **false** — its parameter is the NUMBER `key_mv`, not the `target` string — so the gate was `!false \|\| ...`, always true. Apply committed `keyMv = 0`, which `ConfigJson.problems()` then refused ("OUT_VOLTAGE needs a key_mv"), disabling Save for the **whole** config with a message naming an ordinal binding id (`bN`) the grid never shows, so the user could not tell which cell to fix. The gate now also requires a whole `key_mv` in `K_KEY_MV_MIN..K_KEY_MV_MAX`, two new app constants mirroring the firmware's `uint16` field and `ValidateAction`'s non-zero rule. (c) *`close()` leaked.* `MainActivity.onDestroy` closed the transport but never cancelled `AppViewModel`'s coroutine scope, so the five collectors and the never-returning `client.run()` held the whole object graph alive; `close()` now cancels a scope the view model **owns** (`ownsScope`, false for an injected test scope). (d) *`checkForUpdates`'s `inProgress` gate never fired* — set `true` then `false` with no suspension between, so a `StateFlow` collector could only ever see `false`; the writes are gone, since the function is genuinely synchronous (it reports that this build has no manifest client, per N-12). Each of (a)–(c) is pinned by a Robolectric/JVM test and **mutation-tested**; (d) is a comment-only honesty fix on a field that is now always false by construction | Fixed | No — (a) blocks the retry path, (b) disables Save with an unactionable message, (c) leaks an activity's worth of state; none drives a wrong key |
| N-55 | **`DacFrame::SelectForChannel`'s "reject the rest" guard could only be TESTED by executing undefined behaviour, so the guard was unsound in fact (fixed 2026-09-23).** The function took a `DacChannel ch` and `switch`ed on it — the correct signature for the callers, which all hold a real `DacChannel`. But that made the refuse-half of its contract **unexpressible without UB**: the only way to exercise `default:` is to hand in a value outside the enumerator range, and *loading* such a `DacChannel` in the `switch` is UB. **UBSan, run over the whole 489-test host suite, reported exactly one runtime error in the codebase and it was this:** `lib/HAL/DacFrame.h:109: load of value 99, which is not a valid value for type 'DacChannel'`, triggered by `DacFrameTest.cpp`'s `(DacChannel)99`. The check therefore existed on paper and was unverifiable — the same class as N-50 (an unchecked cast standing in for a check) and the same lesson as the ADC `-1` sentinel: a defensive branch nothing can legally reach is decoration. **Fixed:** the parameter becomes `uint8_t ch_ordinal` (the channel ORDINAL, not the enum), so the guard and its test are both well-defined; callers that hold a real `DacChannel` name it with an explicit `static_cast<uint8_t>` so the valid path is still compiler-checked, and the test asserts `99` and `static_cast<uint8_t>(DAC_CH_COUNT)` are both refused as plain integers. **Verified by re-running the sanitizer: zero runtime errors across 489 passing tests.** Pinned by `DacFrame.MapsEachHalChannelToItsOutputAndRejectsTheRest`, **mutation-tested by making `default:` write a channel and return `true`, which fails it.** This is the second finding the ASan+UBSan amplifier produced that no existing test could see (N-51's gate was the first, by reasoning rather than by tooling) | Fixed | No — no wrong key is driven; it made a documented guarantee untestable |
| N-56 | **`SemverCompare` accumulated each version component into a `long`, which is 64-bit on the host but 32-bit on xtensa — so an over-wide component overflowed (undefined behaviour) and could INVERT an ordering (fixed 2026-09-23).** The numeric compare ran `va = va * 10 + (*pa++ - '0')` per digit. That is signed-integer overflow past 19 digits on the host and past **9** on the device, where xtensa is **ILP32** (`_Static_assert(sizeof(long)==4)` compiled against the project's own `xtensa-esp32s3-elf-gcc` confirms it), so the SAME version string could compare differently on the device than in the host test that blessed it — and neither was right. **UBSan, over the existing suite plus a direct call, reported it:** `lib/Update/ImageVerify.cpp:151: signed integer overflow: 999999999999999999 * 10 cannot be represented in type 'long'`. **Reachability is not exotic:** `kReleaseVersionLen` is 24, so a manifest the parser ACCEPTS can carry a 23-digit component; a date-stamped build tag (`202609231`) already exceeds a 32-bit `long`; and `SemverCompare`'s own 4th-component extension loop is reached by exactly those tags. **The dangerous direction is a wrap that flips the sign,** because the caller reads the result as an upgrade decision: `SemverCompare("1.2.3.9223372036854775808", "1.2.3.1")` returned **-1** under the old code — the larger version read as the smaller, so the device refuses a real upgrade and (in the other operand order) would accept a downgrade. Measured against the reintroduced-`long` mutant, which is the assertion that now catches it. **Fixed:** a new `CompareNumericRuns` compares two digit runs as numerals of UNBOUNDED width — strip leading zeros, compare digit-count, then digit-by-digit — with no width to exceed, and it is used by the three version components, the extension loop, AND `PrereleaseIdentCompare`'s numeric branch (one home for "compare two numerals", so a 20-digit build number cannot disagree between the two). Pinned by `ImageVerify.SemverComparesComponentsWiderThanAnyIntegerType`, **mutation-tested by reintroducing the `long` accumulate, which fails it at the 2^63 case.** The 18 existing semver assertions still hold (the digit-count rule is exactly what makes `1.2.3.9 < 1.2.3.10` continue to work). Third finding the ASan+UBSan amplifier produced that no test could see | Fixed | No — reachable only from a malformed or date-stamped release tag, and the effect is a refused or mis-ordered update offer, not a wrong image; but it is UB on the shipping target and platform-dependent, so the host suite could never have caught it |
| N-57 | **The learn session's idle reference had TWO policies, so the frame the seeded siblings were rebased into and the denominator `Commit` stamped could be different readings (fixed 2026-09-23).** `LearnSession::AddSample` converts the channel's already-learned buttons out of the frame they were STORED in and into the LIVE one (`LadderProfileRebase`), so the neighbour set and the new measurement are compared in one frame; the SAME idle then becomes the denominator stamped as the profile's `learned_idle_mv`. Those were separate: the rebase took the FIRST sample carrying a usable idle, while `learned_idle_mv_` was overwritten on EVERY sample (last-writer-wins). An idle that drifted during a hold therefore rebased the siblings into one frame and divided the committed ratio by another -- the silent two-frames-in-one-comparison failure the rebase exists to prevent, surfacing as the too-close gate refusing a learn that is fine (measured: siblings at 2835 rebased, ratio at 1500, `kTooCloseToExisting`). The same last-writer-wins also let a later reading of **0** -- a tick where the ADC is unreadable, which is what the `-1` sentinel's callers pass through as 0 -- RESET the denominator, refusing with `kNoIdleReference` a learn that had seen a perfectly good reference at the start. **Fixed** by latching BOTH consumers on one flag (`have_idle_`, replacing the rebase-only `rebased_`): the first sample with a usable idle fixes the frame AND the denominator together, and a later zero is ignored. Reachability today is LATENT -- the single production call site (`SystemOrchestrator.cpp:643-651`) captures `learn_idle_mv_` once at wizard entry and passes that constant every tick, so the drift needs a caller that varies it -- but the module is host-testable and public, the divergence is silent when it happens, and the reset half is reachable from an unreadable tick alone. Pinned by `LearnSession.TheRebaseFrameAndTheCommitDenominatorAreTheSameIdle`, **mutation-tested by restoring the unconditional overwrite, which fails it at the drift case**; the sibling `LearnSession.ARebaseHappensOnTheFirstSampleWithAnIdleNotTheFirstSample` covers the gate-on-idle half and is likewise mutation-tested | Fixed | No — no wrong key is driven; a drifted or unreadable idle refuses or mis-frames one learned button |
| N-58 | **`UsbCdc`'s session flag is maintained for no reader (found 2026-09-23).** `NoteConnected()` sets `connected_` and `NoteDisconnected()` clears it, and both are called from the DTR path in `UsbLink.cpp` -- but the only thing that ever READS the flag is `IsConnected()`, whose sole caller is `UsbCdcTest.cpp`. The host-open latch that actually gates behaviour is `UsbLink`'s own `g_host_open` (that file's comment says so explicitly: a transport-level flag "cannot serve here -- it only tracks the BUS"). So the flag, its setter, and its accessor are dead in production; `NoteConnected` is a call that does nothing observable. Not removed here because the plan lists `IsConnected()` as part of the class's public interface (`docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md`), so deleting it is a plan change rather than a code fix, and the cost of leaving it is one unused bool. Recorded because it is the same "produced, consumed by nobody" shape as N-22/N-24/N-45/N-46, and because a reader seeing `NoteConnected` called on the DTR rise would reasonably believe it is what makes the link "connected" | Open | No — no behaviour depends on it |
| N-59 | **The compiled-in maintenance page was generated by a hook no CI step checked, and `WebPage.h` documented an `#include` it did not have (fixed 2026-09-23).** Spec 9.2 has no filesystem partition, so `code/assets/index.html` is compiled into flash as `lib/Maintenance/WebPageAssets.h`; that header is what `WebPageFind` serves and therefore what a user's browser actually receives. Two independent defects sat on that path. **(a) The generator's output was unchecked.** `extra_assets.py` regenerates it, but it is an `extra_scripts` entry in `[env]` (platformio.ini:34), which the `native` env ALSO inherits -- so a host `pio test` regenerates the header from the source and quietly repairs any drift before the very test meant to catch it. There was no CI step for it either, although the generated *contract* has exactly such a step (firmware.yml, "Fail if the generated contract drifted"), so the two generated artifacts were held to different standards. **(b) `WebPage.h` claimed the opposite of what it does.** Its comment read "The asset struct is completed by including the generated `WebPageAssets.h`. This header deliberately does NOT include it: the generated file is large and changes on every page edit, so pulling it in here would recompile every consumer of the token helpers for a CSS tweak" -- and then included it unconditionally three lines below, because the declarations there cannot compile otherwise. A reader trusting the comment would not know where `WebAsset` comes from, and would believe a deliberate design decision had been reversed by accident. **Why (a) matters beyond tidiness:** the failure mode is a device serving a page that is not the page in the repository -- the user-facing half of the same "a generated artifact nobody re-derives" shape as N-33 (the workflows committed at a path Actions never reads). It is latent today only because the two files happen to agree. **Fixed:** the comment now states the real arrangement and why the include is there; `WebPageFind.TheServedPageIsByteForByteThePageInAssets` compares the served bytes to the asset file (and so also catches a hand edit to the header), and `WebPageFind.EveryTableEntryIsResolvableAndTheRootHasAnAlias` pins the table's aliases, entry lengths and the `/` root alias -- the generator appends that alias by naming `index.html` literally, so a second page would get an entry and no root route with nothing to notice. A CI step regenerates the header on a clean checkout and fails on any diff. **Mutation-tested:** appending a byte to `assets/index.html` (without regenerating) and, separately, editing the header's copy of the title and adding a zero-length table entry each fail the corresponding assertion. **Known residual:** the generator emits one extra trailing newline, because it `split("\n")`s the source and terminates every piece, so a file ending in a newline yields a final empty piece. The test mirrors that behaviour rather than hiding it, so the extra byte is documented by the assertion instead of normalised away; changing the generator would change the served bytes and would need the same test updated | Fixed | No -- latent (the two files agreed), but it is a user-facing artifact with no drift check |
| N-60 | **The `status` frame's `gain_mode` is channel 0's, but the field name and spec 4.3 both read as a device-wide fact -- and the mode is per channel (found 2026-09-23).** `CommandRouter::EmitStatusBody` builds the field from `sys_->ChannelGainMode(0)`, while FR-14 selects the mode **per channel** from `gain_policy` and spec 6.2 samples `/SENSEn` per channel -- so a 3 V channel and a 5 V channel on the same device legitimately resolve to gain 1.00 and 1.82 at the same time, and one scalar cannot describe both. Spec 4.3's own prose asserts the stronger reading: "`gain_mode` is the mode the device actually resolved". The sibling fields on the same frame are all genuinely device-wide (`vbus_present`, `config_state`, `output_safe`, `uptime_ms`, the two loss counters), so the shape of the frame invites the reading that `gain_mode` is too. **The fix that made it worse-looking than it is:** an earlier pass replaced a hardcoded `"amplified"` with `ChannelGainMode(0)` -- a real improvement, and its test (`StatusGainModeReflectsTheOrchestratorsResolvedMode`) pins exactly that -- but the repair chose index 0 silently, so the limitation moved from "always wrong" to "right for one channel, unstated", which reads as finished. **No user is misled today:** no app-side code reads the field (grep-confirmed -- `Config.kt`/`ConfigJson.kt` handle the *config's* `output.gain_mode`, which is a different thing on a different frame), so the cost is a field that cannot carry what its name claims rather than a wrong value on a screen. **Why this is an open item and not a repair:** widening it is a wire change. The candidates are a per-channel array (a new field shape the app must learn), an explicit `gain_mode_ch0`-style rename (a contract change plus both codecs), or dropping the field until a consumer exists -- and each is a spec decision rather than a local patch. It is recorded rather than fixed because the current value is not FALSE, only incomplete, and because the shape recurs: any per-channel quantity put on this device-wide frame has the same problem. **Pinned so it cannot rot silently:** `CommandRouter.StatusGainModeIsChannelZeroOnlyAndTheFrameSaysSo` builds a two-channel device with deliberately OPPOSITE modes and asserts the frame carries channel 0's and exactly one `gain_mode` field, so an emitter change to channel 1, to a device-wide value, or to two fields all fail. Mutation-tested by pointing the emitter at channel 1, which fails the assertion | Open | No -- nothing reads it; the value is channel 0's rather than a device-wide one, which is incomplete, not wrong |
| N-61 | **`MaintenanceTrigger` records WHY the window opened, and nothing in production acts on it -- two of its five values have no emitter and a no-app user cannot tell the triggers apart (found 2026-09-23).** The enum documents its own reason for existing: "Distinct values because the caller's shutdown path differs: a USB command should get an acknowledgement, an AUX1 hold gets a buzzer, and booting with no config is the one case that must explain itself on the LED." None of that is implemented. **Three facts, each grep-confirmed:** (a) `kConfigFlag` and `kNoConfigAtBoot` have NO emitter anywhere in `lib/` or `src/` -- only `kUsbCommand` (`CommandRouter::HandleMaintenanceEnter`) and `kAux1Hold` (`SystemOrchestrator::Tick`'s 3 s hold) are ever stored; (b) **no production code branches on the trigger at all** -- the sole reader in the tree is the accessor `MaintenanceTriggeredBy()`, whose only caller is a test, so the value is written and never consulted; and (c) no frame carries maintenance state (no `maintenance` field in any frame of spec 4.3), so the wire cannot report it either. **Consequence, and why it lands hardest on the no-app path:** the only evidence a no-app user has that the window opened is LED_STAT's double-flash, and that pattern is identical for every trigger -- a user who held AUX1 for 3 s sees exactly what a user sees when the app opened the window, and the buzzer the enum promises for an AUX1 hold is never played. The device cannot say "you are in the setup page because you held the button", which is the one thing a user standing at the car with no phone needs to know. **The comment is fixed; the feature is not.** The enum's doc-comment asserted the per-trigger behaviour as though it existed, so a reader trusted it for a promise the code does not keep -- the same comment-describes-the-opposite-of-the-code shape as N-59(b), and fixed the same way: the comment now states that no production code branches on the trigger, names which two values are unreachable and why (N-13), and says plainly that spec 8.2 does not require the per-trigger feedback. Making the triggers distinct REMAINS an open item, because it is a feature -- a per-trigger acknowledgement (a `BUZZ` on the AUX1 path) plus, if the app is to show it, a `status` field -- and each is a spec addition rather than a fix to existing behaviour. The current state is not WRONG (the trigger is recorded faithfully and the window opens correctly); it is a distinction with no consequence, the same "produced, consumed by nobody" shape as N-22/N-24/N-45/N-58, here on an enum whose own comment names the consumers that do not exist. **Pinned so it cannot rot silently:** `SystemOrchestrator.TheMaintenanceTriggerIsRecordedAndNothingInProductionBranchesOnIt` drives both the USB and the AUX1 path and asserts each records its own trigger (so the recording is faithful) and that the escalation still leaves no learn behind -- mutation-tested by pointing the AUX1 hold at `kUsbCommand`, which fails the assertion. **PARTIALLY RESOLVED 2026-09-24:** `kConfigFlag` is no longer emitter-less -- FR-33's next-boot trigger now produces it (see N-13), so three of the four §8.2 triggers have emitters. `kNoConfigAtBoot` still has none, and no production code branches on the trigger yet | Open | No -- the window opens and closes correctly and the LED does show *a* maintenance state; the cost is that the triggers are indistinguishable to a user and the enum's promised per-trigger behaviour is absent |
| N-62 | **`OtaWifi` says "verified TLS against a pinned CA" in three places and does neither -- the crypto is real but the TRUST ANCHOR is the stock Mozilla bundle, and `MBEDTLS_CERTIFICATE_BUNDLE` is pinned nowhere (found 2026-09-23).** Spec 9.5 requires "**Verify TLS against a pinned CA certificate -- not `setInsecure()`**", and the header, the module's own comments and `OtaWifiCaBundleAttach()` all assert that is what the code does. What the code actually does is `cfg.crt_bundle_attach = esp_crt_bundle_attach`, which attaches IDF's ~200-root **default** bundle. That is materially better than `setInsecure` -- the channel is authenticated and a passive attacker learns nothing -- but it is **not a pinned CA**: any of the ~200 public roots (or any CA that can be coerced into issuing for the release host) can vouch for the manifest and the image. **The module also contradicts itself about which mechanism is in play:** `OtaWifi.h` says "the CA is a compiled-in PEM" and the .cpp's constant is a *name* (`"esp_crt_bundle"`) returned by `OtaWifiCaBundleAttach()`; there is no PEM anywhere in the tree (`grep -r 'BEGIN CERTIFICATE'` returns nothing), so the PEM comment describes a design the code does not implement. **Why this is the N-59(b)/N-61 shape again, one layer down:** the prose describes the security property, and the property is weaker than the prose. The reference project's documented gap is quoted in this very spec as the thing "SWC must not repeat"; using a bundle that includes the CA that the reference project would also accept is a *narrowing* of that gap, not its closure. **Second, independent defect in the same module: the Kconfig keys it depends on are pinned nowhere.** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` defaults `y` and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` is the selected choice, so today the build works -- but both live only in the gitignored generated `sdkconfig.esp32s3`, and `sdkconfig.defaults` names neither. A clean build with the bundle default changed, or a future IDF, silently drops the include path (`components/mbedtls/CMakeLists.txt:117` gates `esp_crt_bundle/include` on that symbol) and the *manifest and image fetch loses its trust anchor* -- the exact class `check_sdkconfig_keys.py` was written for, and the key carrying spec 9.5's requirement is the one missing from its list. **Third: both "checkable" API surfaces are dead.** `OtaWifiCaBundleAttach()`'s header calls it "a checkable fact rather than a comment someone may have deleted" and `OtaWifiLastSessionWasVerified()` exists so "the 'never setInsecure' rule is checkable at runtime", and neither has a single caller in `lib/`, `src/`, `test/`, `test_native/` or `tools/` (grep-confirmed) -- so the guarantee each one documents is asserted by nothing. The whole layer shares N-14/N-15's status: `OtaWifi` is included by no file outside itself, `OtaUsb`'s only non-test includer is `OtaWifi.h`, and `CommandRouter` still nacks every `ota_*` frame `not_implemented`, so no code path can reach the WiFi check at all. **Fixed (the parts that are defects of the CODE):** `ReadSize`'s fractional-`size_bytes` truncation is repaired and pinned (N-63); the `OtaWifi.h` claims are corrected to say what the code does -- the trust anchor is IDF's default bundle, which authenticates the channel but does not pin a CA, and the PEM sentence is removed because no PEM exists. **Left OPEN (a feature, not a comment):** actually pinning a CA (a custom bundle via `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`, or a literal PEM through `cert_pem`), and pinning the two bundle Kconfig keys in `sdkconfig.defaults` beside the other eight -- the latter belongs with the board work, because a custom bundle path is evaluated relative to the project root and wants the release host's real CA | With the board, as one change | No -- `ota_*` is nacked, so no host can reach the WiFi check and the weaker anchor is not on any live path; the defect is that the code's own description of its security property is stronger than the property |
| N-63 | **`ReleaseCheck::ReadSize` accepted a fractional `size_bytes` where all four sibling readers refuse one (fixed 2026-09-23).** `ReadSize` range-checked the value and then cast it with a bare `static_cast<size_t>`, so `"size_bytes": 1543210.9` parsed as 1543210. Every other reader of an integer JSON field in this tree already refuses a fraction, with the rule spelled out in each: `ConfigCodec`'s `ReadU32` ("Truncating 750.9 to 750 is a config the device accepts and then behaves differently from what was sent") and `ReadU64`, and `CommandRouter`'s `NumToU32`/`NumToU8`. `CommandRouter` carries the SAME rule for the same quantity at the size a `config_begin` declares ("a byte count of 500.9 is a caller bug, not 500"). `ReadSize` was the one that cast first and checked nothing -- so two readers of "a byte count off the wire" gave two different answers. **Consequence, and why it is not merely cosmetic:** the truncated value is not a wrong-accepted image (`OtaEnd` compares `written_ != declared_size_` and `ImageVerifyEnd` re-checks the digest, so a mismatched size fails closed), it is a **permanent failure with no diagnosable cause**: the manifest declared a size no real image has, so every download of a *correct* image ended in `OtaResult::kVerifyFailed` from the size mismatch, reported to the user with nothing naming the manifest or the field. A publisher whose CI emitted a float size (a JSON serialiser that renders `1543210.0`, or a hand-edited manifest) would have made the update path fail for every device, forever, with a message pointing at the image rather than the manifest. **Fixed:** `ReadSize` now applies the identical fraction rule as its four siblings, so a fractional size is a `kMalformed` manifest -- refused at the boundary where untrusted input arrives, which is where every other field of this parse is checked. Pinned by `ReleaseCheck.AFractionalSizeIsRefusedRatherThanTruncated`, **mutation-tested** (removing the new check fails the test, confirmed against the live tree) | Fixed | No -- it needs a manifest the release pipeline does not currently publish, and the failure direction is a refused update rather than an installed wrong image; it is real because the failure it produces is silent and blames the image |
| N-64 | **Spec 3.2's "fall back *and report it*" has TWO homes for one fact, and the struct field whose comment claimed to be the report is read by nobody but a test (found 2026-09-23).** `CalibrationCurve.h`'s `CalibrationSource source` carried the sentence "Carrying the source on the struct is what makes 'report' possible; a caller that never checks it is the silent-fallback fault the spec names" -- and **no production caller checks it**: `grep -rn` for readers of `.source` returns exactly two lines, both in `CalibrationCurveTest.cpp:43-44`. The actual report is EspHal's **own parallel bool** `g_state.cali_degraded` (`EspHal.cpp:43,369,530`), which is derived from the same `supported` local on the line above `AdcCalibrationSelect(supported)` and surfaced through `EspHalCalibrationIsDegraded` to `main.cpp` and the BOOT_DEGRADED path. **The two cannot disagree today** -- both are computed from one `supported` value on adjacent lines -- so this is latent, not a live wrong behaviour. It is worth recording because it is the duplicate-home shape this project has now hit repeatedly (the ADC ceiling spelled three times, `config_state` derived from the wrong state, the `pinned CA` that was a bundle), and because the field's comment asserted a load-bearing role it does not have: **a reader trusting it would wire the report through `source` and find nothing consuming it**, or would re-tune `cali_degraded` without `source` and be told by no test. **Fixed (the comment):** `CalibrationCurve.h` now states that the production report is EspHal's flag, that `source`'s sole reader is the test, and that the duplication is the open defect. **Left OPEN:** collapsing the two homes -- EspHal should report `g_state.curve.source` and drop `cali_degraded`, which removes the bool, the second derivation and the divergence risk in one change. It is deferred rather than done here because it edits `EspHal.cpp`, the ONE file the host suite cannot compile, so the change would be unverifiable until the board exists -- the same reason the HAL NVS cluster is gated rather than merely reviewed | With the board | No -- the two derivations agree, so no reading is mis-scaled and no report is missed; the defect is a false comment plus a duplication that a future edit could split |
| N-65 | **Two public accessors shipped with zero readers, and one of them reports a false value before anything has been received (found 2026-09-23).** (a) `CommandRouter::LastSeenSeqReceived()` (`CommandRouter.h:124`) returns `expected_seq_ - 1`; its sibling `LastSeenSeqSent()` is read by `CommandRouterTest.cpp:271,275` and this one by **nothing repo-wide** (`grep -rn` across `lib/`, `src/`, `test/`, `test_native/`, `tools/`). The plan names both as a deliverable (plan:6935) and its own test step exercises only `Sent`, so half the pair shipped as dead API. **The live defect inside it:** `expected_seq_` is initialised to 1 and `seen_any_` gates the update, so before any frame arrives this returns `0` -- a report that "seq 0 was received" when nothing has been. No caller, so no wrong behaviour today; a future caller (a link-quality display, a diagnostic) would read a value that is false exactly in the case it is most likely to be asked. (b) `MaintenanceMode::LastActivity()` (`MaintenanceMode.h:119`) also has no readers -- `ShouldTimeout` reads the member directly -- and it is the accessor that N-35's missing `NoteActivity` call would have made *observable*: with nothing bumping `last_activity_`, it returns the entry time forever, so it is not merely dead but would report a fixed deadline as though it were activity-tracked. Both are the N-58/N-22/N-45 shape (a value produced and consumed by nobody), which is why they are recorded rather than silently deleted: deleting `LastSeenSeqReceived` removes a plan deliverable, and `LastActivity` becomes live the moment N-35's activity source exists. **Fixed:** nothing yet -- the honest action is to record the pair and fold the fix into the work each belongs to: `LastSeenSeqReceived` should either drop its off-by-one-then-anyway-wrong sentinel (return `seen_any_ ? expected_seq_ - 1 : 0` and have a test assert it) or be removed with the plan line; `LastActivity` is correct as written and waits on N-35 | With the link-diagnostics and activity work | No -- neither has a caller, so nothing reads a wrong value; the defect is two dead public surfaces, one of which is false before first use |
| N-66 | **`AdcReader`'s tolerance constant described an inter-window test the code does not perform (fixed 2026-09-23).** `AdcReader.h`'s doc-comment for `kAdcSettleToleranceMv` read "How close **two consecutive windows** must agree before the value is called settled" -- a claim about a *previous* window's value. The class carries nothing between calls: `window_` is its only buffer, `Reset()` zeroes that same buffer, and `AdcReader::Update` computes `hi - lo` across the samples it just collected, then compares to the constant. So the tolerance bounds the SPREAD WITHIN ONE BURST, which is what `AdcReader.cpp`'s own comment (line 63), the `Settled()` doc-comment below it ("False while the last burst was a mixture of old and new levels"), and this spec's N-18 row all say. The header sentence was the only place holding the inter-window reading, and it is the *more natural* thing to assume from the parameter's name -- so a reader auditing whether a genuine level change is reported unsettled would form a false expectation and find nothing to contradict it. **The per-burst meaning is the deliberate one,** and N-18 explains why: a window taken while the input is still moving spans both levels, so its median is a value the input never held; a uniform burst is settled on its own regardless of how large the step BETWEEN bursts was. **Corrected in the comment, not implemented in the code:** making the tolerance inter-window would leave `Settled()` false for one extra tick after every real step and change nothing any consumer depends on -- the lone production reader (`ServiceChannel`'s idle-reference re-adoption at `SystemOrchestrator.cpp:1121`) independently bounds the candidate by the ratio band, so it never needed the inter-window property. This is the N-59(b)/N-61/N-62 shape once more -- prose stronger or different from the code -- and fixed the same way. Same family as [[swc-spec-code-drift]] | n/a (comment) | No -- no behaviour changes and no consumer's correctness depends on the inter-window reading; the cost was a false expectation in the one place a reader looks for the definition |
| N-67 | **FR-1's NTC half was unimplemented, and two comments plus a spec sentence asserted it was done (found 2026-09-23; IMPLEMENTED 2026-09-24).** FR-1 requires the firmware to "sample both ladder channels **and the NTC** continuously"; spec 6.4's honesty paragraph closed with "The NTC is read and reported regardless, so a bring-up session can *measure* the drift and set a coefficient"; and `SystemOrchestrator.cpp`'s own sentinel comment opened by attributing to spec 6.4 a "linear correction with a coefficient defaulting to zero, so that it 'does not change behavior until the user or a bring-up measurement supplies a non-zero coefficient'." **All three are false, and the third quotes a sentence that appears nowhere in this document.** The facts, each grep-confirmed: (a) `ADC_CH_TEMP` is mapped to `ADC_CHANNEL_6` in `EspHal.cpp`'s `AdcPinFor` and is read by **nothing** -- every `adc_read_mv` call site in `lib/` and `src/` names SWC1/SWC2, AUX1 or KEY_SENSE1/2, and the only other tree-wide reference is a `MockHal` accessor test; (b) there is **no NTC-to-temperature conversion anywhere** -- no B3380 routine, no Steinhart-Hart, no divider inversion -- so the raw millivolts would not be a temperature even if read; (c) both `LearnSession::AddSample` callers pass the literal `kTempNotMeasuredTenths` (0), so `temp_c_at_learn` cannot hold a measurement and its field comment in `LearnSession.h` ("`temp_tenths_c` comes from `ADC_CH_TEMP` -- the NTC on the ladder") describes a producer that does not exist. **Consequence:** the field a future compensation is meant to consume is a constant, and a bring-up session cannot measure the drift *from the device* as spec 6.4 promised -- it must read the NTC off the board directly. **This is the dominant defect class once more** (N-59(b)/N-61/N-62/N-65): prose asserting a behaviour the adjacent code does not implement, here in the module whose comment was rewritten *specifically* to stop overstating the compensation story -- the rewrite corrected the correction claim and invented a stronger one for the input. **Fixed:** spec 6.4 now states plainly that the NTC is not read, names the three pieces of evidence, and says the raw value would not be a temperature anyway; §11's FR-1 row records that it only ever covered the non-blocking clause and that the NTC clause is unasserted; `SystemOrchestrator.cpp`'s sentinel comment replaces the fabricated quote with what spec 6.4 actually says and states that the sentinel is 0 in every case because the channel is never converted. **CLOSED 2026-09-24 -- FR-1's first clause is implemented.** The divider values no longer need the board: they are read off `SWC.kicad_sch`, which carries `RT1` (`Device:Thermistor_NTC`, value `10k B3380`) from the `TEMP_ADC` node to `GND` and `R29` (`Device:R`, `10k`) from `+3V3` to that node. So the part is on the LOW side -- the node RISES with temperature -- and the inversion is `R = R_series * V / (VDD - V)`. `lib/Analog/NtcConvert.h` holds it plus the B-constant model (`1/T = 1/T0 + ln(R/R0)/B`), in integer maths because the config has no floating point: `ln` is the expansion `2*(y + y^3/3 + ...)` with `y = (R-R0)/(R+R0)` in 2^16 fixed point, and **thirty series terms** because the conventional eight are +0.8 C wrong at the hot end (thirty bring it under 0.25 C across -40..+120 C; a wider fixed-point scale does not help, the truncation is the error term). `SystemOrchestrator::SampleNtcTenthsC` reads `ADC_CH_TEMP` and converts, and BOTH learn paths now record the result in `temp_c_at_learn` -- the headless wizard (every tick of a prompt) and `CommandRouter::RecordLearnSample` (the app-driven session). A failed read HOLDS the last good value rather than reporting the sentinel, because the ADC returns -1 on error (N-43) and a learn that stored "0 C" from a transient would record a temperature nothing measured -- the exact lie the sentinel exists to avoid. Asserted by 8 `NtcConvert` tests (pinned to the datasheet model, not to this implementation, so a swapped divider side fails) and one orchestrator test that drives a whole learn and checks the committed profile's `temp_c_at_learn` is the CONVERTED value. **Still board-gated:** the conversion is validated against the datasheet B-constant table, not against a thermometer on the bench -- reading one room temperature and comparing is the bring-up step that would close even that. N-9 (the compensation itself, which would consume this) remains open, unchanged **Pinned so the gap cannot stay invisible:** `SystemOrchestrator.TheNtcChannelIsNeverConvertedSoFR1sFirstClauseIsUnmet` drives a full poll loop -- boot, a press, a release, and a whole headless learn -- and asserts `ADC_CH_TEMP` was converted ZERO times while the ladder channel was converted many, so the day the sampling path is added the counter goes non-zero and the probe fails. It is written to PASS today (a PROBE, not a bug) and is **mutation-tested** by adding one `adc_read_mv(ADC_CH_TEMP)` to `Tick`, which fails it. `MockHal` gained `AdcReadCount` for it | Fixed 2026-09-24, **bring-up validation CLOSED 2026-09-24 on the DUT** -- `status.temp_c` reads `26.7` (a plausible room temperature, stable across frames), and the earlier claim that the learn path alone satisfied "continuously" was wrong and is fixed by the ~1 Hz `Tick` cadence | No -- no reading is mis-scaled and no button mis-fires: `temp_c_at_learn` is a recorded input that nothing branches on (the correction does not exist). It was recorded because the requirement was unmet while three separate surfaces said it was met, and it is now met |
| N-68 | **N-49's "no session" sentinel was only half-implemented: the guard was added, the initializer was not — so on a router that had never seen a `learn_start`, a `learn_commit{channel:0}` cleared both guards and reported the wrong reason (fixed 2026-09-23).** N-49's own row states "`learn_channel_` gained a `-1` 'no session' sentinel, and `HandleLearnCommit` refuses `no_session` when it is set". The GUARD is real (`CommandRouter.cpp:1258`, `if (learn_channel_ < 0) { Nack(for_seq, "no_session", ...) }`), and its comment asserts the property the sentinel is supposed to have verbatim: "`learn_channel_` is `-1` when no session exists -- before any `learn_start`, and after a link drop (see `ForgetLearnSession`)." But `CommandRouter.h` declared `int learn_channel_ = 0;`. `ForgetLearnSession` does assign `-1`, so "after a link drop" holds; **"before any `learn_start`" did not**, and the router is constructed before any frame arrives. **Consequence, measured by the new test:** a freshly constructed router that receives `learn_commit{channel:0,...}` with a legal `button_id`/`name` and no prior `learn_start` passed `learn_channel_ < 0` (0 is not < 0), passed the channel-match guard (0 == 0), and reached `session_.Commit` on an empty session — answering `nack: learn_rejected: too_few_samples`, which names a SAMPLE COUNT as the cause of a refusal whose real cause is that no stream was ever opened. That is precisely the "wrong, un-actionable reason FR-29 forbids" N-49 was written to eliminate; the guard was correct and the value it tested never held. The channel-match guard was defeated for channel 0 in the same step, which is the channel an app is most likely to name first. **Why the class is worth recording rather than folded into N-49:** it is the dominant defect shape once more — a sentinel introduced in one place (a guard's comment + one assignment site) and not at the other (the declaration) — and the spec row asserting it worked made it *findable only by reading the declaration*. `learn_open_` cannot substitute as the key (spec 4.3's flow is start → stream → STOP → commit, and a stop deliberately keeps the samples and the channel), which is why N-49 chose the session channel; that choice only works if the sentinel is the initializer. **Fixed:** `learn_channel_ = -1`, with a comment on the declaration naming why the initializer IS the sentinel and why `learn_open_` is not an alternative. The casts at `EmitLadderSample`/`RecordLearnSample` (`static_cast<uint8_t>(learn_channel_)`) stay safe because `learn_open_` is true only after `HandleLearnStart` assigns a real channel — grep-confirmed the sole writer is line 1135 and the only clearer is `ForgetLearnSession`, so the sentinel and a live stream cannot coexist. **Pinned so the value is asserted where N-49 asserted the property:** new `CommandRouter.ACommitWithNoLearnStartEverNamesNoSession` sends exactly that frame to a fresh router and asserts the nack names `no_session` AND does NOT contain `too_few_samples` (a `nack` is produced either way, so a test checking only for `nack` would not bite). **Mutation-tested:** reverting the initializer to `0` fails it, then restores clean. Same family as [[swc-spec-code-drift]] | Fixed | No — nothing was corrupted (an empty session commits nothing) and no key mis-fires; the cost is a specific, actionable reason replaced by a misleading one on an ordinary out-of-order frame, and it made N-49's guard unreachable for exactly the channel an app names first |
| N-69 | **`UsbLinkStart`'s comment said the TinyUSB task runs BELOW the poll loop; it runs ABOVE it -- priority 5 PREEMPTS `app_main`'s 1 -- and the comment's stated reason was therefore impossible (fixed 2026-09-23).** The comment on `tusb_cfg.task.priority` read: "4096 is esp_tinyusb's own default stack; the priority is below the app loop so a USB burst cannot starve the poll loop that drives the KEY line." The value it annotates is **5**, and `app_main` runs at `ESP_TASK_MAIN_PRIO` = `ESP_TASK_PRIO_MIN + 1` = **1** (`components/esp_system/include/esp_task.h:56`, created by `freertos/app_startup.c:85`). Higher number = higher priority in FreeRTOS, so 5 **preempts** the poll loop, not the reverse. The claim is not merely mis-worded but **unachievable as written**: priority 0 is the idle task and `tinyusb_driver_install` rejects priority 0 outright, so no value below 1 exists for the USB task to hold. **The tree already contradicted the comment in two places, which is how the drift stayed invisible:** `tools/check_task_ownership.py`'s own docstring states the truth verbatim — "the **TinyUSB task** (priority 5), while `SystemOrchestrator::Tick` runs on the poll loop in **app_main** (priority 1) — both pinned to core 0, so the callback PREEMPTS the poll loop at any instruction" — and the whole gate exists to keep the callback bodies tiny *because* they preempt. **Why it matters rather than being a comment typo:** it states an incorrect real-time assumption in the one place a maintainer looks to reason about whether a USB burst can delay the key path. The true mechanism is the **callback split**, not the priority: `CdcRxCallback` only memcpys into the SPSC ring and `CdcLineStateCallback` only does an atomic store, so the preemption window is bounded by those two operations and every protocol/orchestrator action runs on the poll task in `UsbLinkService`. A reader who trusted the comment and "restored" a genuinely low priority would add nothing (no such value exists) or, reading it as a design intent, might conclude the split is optional — which is exactly the invariant the gate forbids undoing. Comment corrected to state the real ordering, the fact that 5 preempts, that the priority cannot go below 1, and that the split — not the priority — is what keeps a burst off the key path. Same class as N-59(b)/N-61/N-62/N-66: prose asserting behaviour the adjacent code does not implement | n/a (comment) | No — no behaviour changes and no key mis-fires; the cost is a false real-time assumption in the surface a maintainer consults, contradicted by a guard in the same tree |

| N-70 | **A comment named the OPPOSITE condition as FR-25's pass-through trigger, and claimed an app inference that does not exist (fixed 2026-09-23).** `SystemOrchestrator::Boot`'s comment beside `if (pass_through_ && !any_reference) pass_through_ = false;` read "The app can still infer the mode from `config_state: defaults`, which is the condition FR-25 keys pass-through on." Both halves are false against the tree. (a) **The condition is inverted.** Pass-through is keyed on `ConfigLoadResult::kNoConfig` (`pass_through_ = (result == ConfigLoadResult::kNoConfig)`), which maps to `config_state_ = BootConfigState::kNone` and therefore the wire word **`none`** — spec §4.3 states the distinction normatively: "`none` is FR-25's supported pass-through device, **deliberately NOT `defaults`**". `defaults` is `kFellBackToDefaults`, the *opposite* state: a config that EXISTS and could not be read, which §6.8 requires be reported as a fault. The comment told a reader that a corrupt-config device is the pass-through device and a fresh one is not — backwards, and pointing a reader at the exact pair §4.3 warns must not be collapsed. (b) **The app inference does not exist.** Grep of `android/app/src/main/java/` for any pass-through read (`passthrough\|pass_through\|pass through`) returns nothing: `configState` drives only `configWarning` and a link-screen status word, and the two fault values it warns on are `defaults`/`recovered`. So the comment's justification for why the absence of a link field is acceptable cited a consumer that was never written. **Why it is worth a row and not a silent edit:** it is the [[swc-spec-code-drift]] shape in its purest form — a comment asserting a behaviour, a condition, and a consumer, all three checkable and all three wrong — and the *code* was correct throughout, so no test could fail. The neighbouring header comment (`SystemOrchestrator.h`, `ConfigStateWord`) already stated the `none`-vs-`defaults` distinction correctly, so the file contradicted itself. **Fixed:** the comment now names `kNoConfig` / the wire word `none` as the key, quotes §4.3's "deliberately NOT `defaults`", identifies `defaults` as the opposite (`kFellBackToDefaults`) condition, and records that no app-side pass-through inference exists. Comment-only; no behaviour changes and none was wrong. Pinned by reading, not by a test — no assertion can observe a comment (same limitation as N-47's prose check) | Fixed | No — the firmware's behaviour was already correct; the cost is that the one comment a reader would trust to explain FR-25's trigger named the wrong state and an absent consumer |
| N-71 | **`CJSON_NESTING_LIMIT` was left at its desktop default of 1000, so a single inbound line could recurse the parser ~1000 levels and overflow the 3,584-byte poll task (fixed 2026-09-23).** cJSON calls itself once per JSON nesting level; its own guard is `CJSON_NESTING_LIMIT`, and the comment on the define states why it exists -- "This is to prevent stack overflows." The value, 1000, is sized for a desktop. This device's poll task is `CONFIG_ESP_MAIN_TASK_STACK_SIZE = 3584` B, and **every inbound line is parsed on it**: `NdjsonParseEnvelope` (called from `CommandRouter::Process` via `UsbCdc::DrainRx`) parses BEFORE any envelope or type check, `Process` parses the same line AGAIN for the handlers, and a peer-supplied config is parsed a third time by `ConfigDecodeJson` after `config_end`. **`kNdjsonMaxFrame` (1024 B) is NOT a depth bound:** `"[" * 500 + "]" * 500` is exactly 1000 bytes, so a legal-length line drives ~1000 levels. **Measured, not estimated** (the pinned 1.7.19, `-fstack-usage`): `parse_value` + `parse_object` are 32 B each on xtensa at this project's `-Og` (64 B/level), and ~96 B/level on the host. The in-project prefix `app_main -> UsbLinkService -> Process -> NdjsonParseEnvelope` measures 480 B, so the budget lands at **depth 32 = 2,672 B (fits), depth 48 = 3,696 B (overflows)**. **Consequence:** a peer bug, or any hostile device on the other end of the USB cable, panics the poll task with a ~1 KB line -- a device-only crash that no host test can reproduce (the host stacks are 8 MB) and that `check_stack_usage.py` cannot see either: the tool sums `-fstack-usage` frames along a call graph, a recursive function's frame appears once, and a cycle cannot be summed by a longest-path DP (its own docstring calls the graph "a DAG here"). This is the `swc-device-stack-overflow-config`/N-30 class again, one layer down -- in the PARSER rather than in a by-value `Config`. **Fixed:** `-D CJSON_NESTING_LIMIT=32` in `[env]`, so BOTH builds pin the same value (the device compiles cJSON from the IDF `components/json` copy while the host compiles the `lib_deps` one, and IDF compiles its own component sources with the project's `build_flags`). 32 is ample: the deepest legitimate frame is the config, at depth 6 (`contract/swc_sample_config.json`). A line past the bound is **rejected, not truncated** -- cJSON returns NULL, so the frame is reported `bad_frame`. **Pinned** by `NdjsonEnvelope.RejectsALineNestedDeeperThanTheDeviceStackAllows`, which uses **BALANCED** brackets: the first version used a bare run of `[` and **survived the mutation**, because cJSON rejects unbalanced input as malformed at ANY limit -- balanced nesting is the only input where DEPTH alone is what cJSON objects to. Mutation-tested by restoring 1000, which fails it, then restored clean. Verified in the built objects: the device `parse_array` compares against 31 and the host against 32 (`subs x8, x8, #0x20`). Same family as [[swc-spec-code-drift]] | Fixed | No -- on a well-behaved link nothing reaches depth 32, and the host suite is unaffected; it is real because the crash is deterministic, remote, and invisible to every gate this repo has |
| N-72 | **A plan-promised public accessor shipped with ZERO references repo-wide -- not even its own test -- and a named constant that no code consults (found 2026-09-23).** (a) `ImageVerifyBytesSoFar()` (`lib/Update/ImageVerify.h:42`, `size_t`, "Bytes accepted so far, for a progress display") is referenced by **nothing**: `grep -rn ImageVerifyBytesSoFar` across `lib/`, `src/`, `test/`, `test_native/`, `tools/`, `android/` returns only its declaration and its definition -- no caller, no test, and no frame in the protocol that carries a verify byte count. It is a plan deliverable (plan:7755, alongside `ImageVerifyReset`), so it is not accidental; it was written for the USB-OTA progress display the app is meant to render, and that path is open item N-14 (every `ota_*` frame is nacked `not_implemented`). The USB OTA module has its OWN progress counter -- `OtaUsb::OtaBytesWritten()` over `written_` -- which the tests DO drive, so the live counter is a different one from the accessor named for the purpose. This is the N-58/N-22/N-45/N-65 shape once more (a value produced and consumed by nobody), and the sharper version of N-65: N-65's pair at least has one test-side reader, this has none at all. (b) `kGuardHighMv = 3400` (`lib/Output/GainPolicy.h:18`) has zero references tree-wide. Unlike (a) it is not a missing feature but a NAME for a spec figure -- spec 6.2's 2.6-3.4 V guard band -- whose top edge is deliberately not a threshold: `GainPolicySelect` splits at `kGuardLowMv` alone and folds the band into "amplified" because the two sides of spec 6.2 are asymmetric. It stays as the one home for the 3.4 V figure; what was missing was any statement of that, so a reader could reasonably assume a three-way branch existed. **Fixed:** (b) gained a comment naming it as the band's top rather than a consulted threshold, and pointing at `GainPolicySelect` for why the top side has no threshold; (a) is recorded and left in place -- deleting it removes a plan deliverable that N-14's USB-OTA progress display will consume, and the honest action is the same as N-65's: fold the wiring into that work rather than delete the API. **Judged clean and deliberately NOT recorded** (verified this pass so they are not re-litigated): `OtaUsb::OtaInProgress()`/`OtaBytesWritten()` are read by `OtaUsbTest` and are the USB-OTA surface N-14 owns -- test-only, not dead; `ServoLoop::Settled()` is a documented FR-19/spec-6.5 deferral (the servo ships disabled); `AdcReader::Settled()` DOES have a production reader (`SystemOrchestrator.cpp:1136`) | With the OTA-progress work (N-14) | No -- neither is read, so nothing acts on a wrong value; the cost is one dead plan deliverable and one unlabelled constant a reader could over-trust |

| N-73 | **The Android app's inbound-chunk bounds check did the arithmetic in `Int` BEFORE comparing, so a peer-supplied `offset` near `Int.MAX_VALUE` WRAPPED the sum negative, passed the guard, and killed the link's only consumer with an escaping `IndexOutOfBoundsException` (found/fixed 2026-09-23).** `SwcClient.acceptInboundChunk` guards a `copyInto` against the app's receive buffer with `if (offset < 0 \|\| offset + bytes.size > buf.size)`. `offset` arrives as an arbitrary JSON integer from the peer (`config_chunk`: the device, or any hostile peer on the far end of the USB cable), and `offset + bytes.size` in `Int` arithmetic **wraps**: with `offset = Int.MAX_VALUE` the sum goes negative, the guard reads `false`, and `System.arraycopy` (via `copyInto`) throws `IndexOutOfBoundsException`. **Measured, not inferred:** a standalone JVM program shows `Int.MAX_VALUE + 512` == `-2147483137` and the guard reading false. **Consequence:** the exception escapes `handle()`, escapes the `frames` collector in `run()`, and ends `run()` -- the link's ONE consumer. The app then goes deaf for the rest of the session while the device keeps talking, which is exactly the "app goes deaf" failure `handle()`'s parse guard and `endInboundConfig`'s decode guard both exist to prevent, so the arithmetic has to be safe *before* it is compared. **Fixed:** rewritten as `if (offset < 0 \|\| bytes.size > buf.size - offset)` -- the subtraction cannot overflow for any `offset` the earlier `offset < 0` clause let through, since `buf.size - offset` is at most `buf.size`. Same family as [[swc-cast-before-bounds-ub]] (N-50) and the N-56 platform-width class: a bounds check whose own arithmetic is the defect. | Fixed | No -- it is fixed and the trigger is a malformed/hostile frame rather than a normal one; the row is recorded because the failure was remote, silent, and ended the link's only consumer |
| N-74 | **Two app-side limits were hand-mirrored from firmware with NO guard, while the guard script that exists for exactly this covered 12 sibling limits and read only one of the app's two limit-bearing files (found/fixed 2026-09-23).** `SwcClient.kt` carried `CHUNK_BYTES = 512` and `kWireConfigMaxBytes = 22407` as literals mirroring `kConfigWireChunkBytes` (`lib/Util/Base64.h:37`) and `ConfigMaxSerializedSize()` (`lib/Config/ConfigCodec.h:69`), and `lineCap = 1024` mirroring `kNdjsonMaxFrame` (`lib/Link/Ndjson.h:9`). `tools/check_app_limits.py` is the repo's guard against exactly this drift -- it parses the C++ constant and the Kotlin constant and asserts equality -- but it read **only** `model/Config.kt`, so it checked 12 pairs and none of these three. A firmware bump to any of them (e.g. a larger `kNdjsonMaxFrame` after a protocol change) would have left the app silently wrong with every gate green. This is N-16/[[swc-app-limit-parity-guard]] in a second file: the guard existed, was correct, and did not look here. **Fixed:** the script now also reads `lib/Util/Base64.h`, `lib/Config/ConfigCodec.h`, `lib/Link/Ndjson.h` and `link/SwcClient.kt`; `_cpp_constant` learned the `inline constexpr <type> Name() { return <value>; }` form (how the two firmware ceilings are written) and `_kt_constant` the optional `const` / optional `: type` form; three PAIRS rows added. It now prints 14 `OK` lines including `wire config chunk`, `wire config ceiling` (22407) and `ndjson line cap` (1024). Mutation-tested: changing any app-side literal fails the guard. | Fixed | No -- nothing is wrong today (the three pairs currently agree); the cost is the hazard, a silent app/firmware divergence with no gate watching it |
| N-75 | **The app dropped the firmware's documented "no reading" sample and held the LAST real reading on screen instead, inverting the exact contract `EmitLadderSample` states (found/fixed 2026-09-23).** `AppViewModel`'s `Frames.LADDER_SAMPLE` branch read `if (level > 0) { ... _ladder.value = ... liveMv = level ... }`, so a `level_mv` of **0** fell through and the previous `liveMv` stayed displayed. But 0 is the firmware's deliberate "NO READING" value, and it says so in three places: `CommandRouter::EmitLadderSample` (`lib/Link/CommandRouter.cpp:996-1007`) emits `0` when there is no orchestrator to sample (`sys_ == nullptr`) and its comment states the contract; `SystemOrchestrator::FilteredLevelMv` (`lib/System/SystemOrchestrator.h:248-252`) returns 0 for an unreadable/stale conversion; and that function's own comment is explicit -- *"0 mV is unambiguous: it is below the ladder's floor, so the app renders it as 'no reading' rather than as a real level."* **Consequence:** during a learn, the live graph showed a frozen stale millivolt value while the device was actually reporting that it could not sample at all -- the user would tune a button against a number that was no longer being measured. **Fixed:** `liveMv = if (level > 0) level else null`, so 0 now blanks the reading (rendering as "no reading") instead of being swallowed. Pinned by `AppViewModelTest.a learn stream sample of zero blanks the reading rather than holding the last`, which emits `level_mv` 1900 then 0 and asserts `liveMv` goes 1900 -> `null`; mutation-tested by restoring `level > 0` on the drop path. Same family as [[swc-status-frame-drift]] / the "0 is a value, not an absence" shape. | Fixed | No -- the live graph is a display, not a control input, and the stale value was itself a real past reading; the cost is a silently wrong live display during the one workflow (learn) where the user is watching it |
| N-76 | **The app's maintenance card told the user the device's WiFi was ON and to connect to its setup page, while N-15 records that nothing in this build starts the radio (found/fixed 2026-09-23).** `LinkScreen`'s maintenance card rendered, in the open state, "The device's WiFi is on -- connect to its setup page", and in the closed state an instruction to turn the device's WiFi on. But `HandleMaintenanceEnter` (`lib/Link/CommandRouter.cpp:923-930`) does nothing but call `sys_->EnterMaintenance(kUsbCommand, ...)` and ack, and N-15 records that the BLE/WiFi provisioning and the network update path are not wired -- there is no radio to be on and no page to connect to. The card was the one user-facing surface making the strongest false promise in the app: it directed a user to a network that does not exist. This is the [[swc-maintenance-copy-and-validator-gaps]] (N-39) shape again -- user-facing copy asserting a capability the code does not have -- one layer over, in the app instead of the firmware, and it is the same [[swc-hello-caps-overadvertised-ota]] pattern (a promise the code does not keep). **Fixed:** both branches rewritten to state what maintenance mode actually IS today (the state the provisioning and network-update paths will use once the radio is wired) and to name that this build does not start the device's radio. Pinned by two new tests in `LinkScreenTimeoutTest.kt` (one per state), each asserting the old strings `"The device's WiFi is on"` / `"Turn the device's WiFi on"` do NOT exist; mutation-tested by restoring either string. | Fixed | No -- maintenance mode still works and is still reachable; the cost is a user sent to a WiFi page that was never started, unable to tell a firmware gap from a broken device |
| N-77 | **FR-30 promises `learned_at_rail_mv` exists "so the app can display absolute millivolts and so a genuine 3V3 fault ... is detectable", and the app does NEITHER — it only round-trips the field (found 2026-09-23).** The field has three homes in the tree and all three are inert on the app side: `LadderButton.learnedAtRailMv` (`Config.kt:202`) is declared, `ConfigJson` parses it (`:108`) and re-emits it (`:228`), and that is the complete set of references — grep for `learnedAtRailMv\|learnedAtRail\|railMv` across `android/app/src/` returns exactly those three lines. The firmware side records it correctly (`LearnSession.cpp:281`, `LearnWizard.cpp:278`, `CommandRouter::RecordLearnSample` all pass `kNominalRailMv`), so the value arrives and is durable; nothing reads it. **Consequence:** (a) FR-30's *display* clause is unmet — no screen shows the rail, and no per-button field beyond `mvCenter`/`mvTolerance` is rendered anywhere (`LadderScreen` is the only screen that touches a `LadderButton`'s measurements). (b) FR-30's *detectability* clause is unmet on the app side: no app-side check compares `learned_at_rail_mv` against anything (grep for `sag\|rail.health\|regulator\|0.2\|20 %` in the app finds no rail logic), so the app cannot report a sagging regulator. **What IS covered, so this is not a silent-safety hole:** the firmware's own FR-30 sag check is real and tested — `LadderClassify` returns `kFault` when `idle_mv < learned_idle_mv * kRailHealthFloorPermille / 1000` (200 ‰, `LadderDecode.cpp:44-47`) — so a collapsed rail releases the key and latches blink (`ReportFault`). What is missing is only the *user-facing readout* the spec's parenthetical promises, and the reason it is a row rather than a repair is the same as N-25: the app has no live-rail source, and a per-button rail display is a UI decision (which screen, which units, what a "sag" threshold means to a driver) with no test that could pin it. **Sibling to N-22/N-25:** N-22 fixed the app's *read* of a dead `rail_mv`; N-25 records that the app cannot classify on the live ratio; this row records that the one rail field that DOES reach the app is consumed by nothing. All three are the "produced, consumed by nobody" shape (N-58/N-65) applied to the rail. Recorded, not repaired — a decision, not a bug | With the board (it needs the display decision first) | No — the firmware's FR-30 check runs and is tested, so a rail fault still releases the key and latches; the cost is a spec promise (a display and a detectability claim) the app does not keep |
| N-78 | **Spec §7.2's OTA prose described a pulse shape the buzzer grammar cannot emit — "a longer second pulse … the pulses grow in *length*" — while its own table row and `StepsFor` both give a plain uniform double (found/fixed 2026-09-23).** The paragraph under the §7.2 pattern table read: "`OTA_OK` is therefore a plain double with a longer second pulse (150/100 ×2), which is what 'rising' can mean once pitch is unavailable: the pulses grow in *length*, not in frequency." Two things contradict it. (a) **The table row above it** gives `OTA_OK` as `150/100 · 2` — one `{on_ms=150, off_ms=100}` interval repeated twice, which is two EQUAL pulses, not a longer second one. (b) **The grammar cannot express a non-uniform train at all.** `BuzzerGrammar::StepsFor` returns a single `Step{uint16_t on_ms; uint16_t off_ms; uint8_t reps;}` per pattern (`lib/Feedback/BuzzerGrammar.cpp:16,44`), and the playback in `BuzzerGrammar::Update` modulates that ONE interval `reps` times — so the second pulse's length is not a free parameter; it is the same `on_ms` as the first. "The pulses grow in length" was therefore not merely un-implemented, it was un-representable, and the sentence sent a reader looking for a rise that no pattern can produce. This is the [[swc-spec-code-drift]] / N-59(b) / N-66 family — prose stronger or different from the code it describes — here in the SPEC itself rather than in a source comment, and on the document the table sits in. **Why no test caught it:** no assertion pins `kOtaOk`'s SHAPE. `BuzzerGrammarTest.EverySpecPatternCompletesAndReleasesTheLine` lists `kOtaOk` only among the every-pattern-completes loop, and a grep for a shape/rising assertion on the OTA patterns finds none — the table numbers were already pinned, but the paragraph describing them was not. **Fixed:** the paragraph now states what the grammar produces — a uniform double (150/100 ×2), distinguished from `OTA_START`'s long single (400/0 ×1) by its longer **total**, not by an intra-pattern interval change — and says explicitly that an earlier revision claiming a "longer" second pulse described a shape the encoder cannot emit. The numbers (400/0, 150/100, 80/40) are unchanged and correct; no code or test needed to change, because the code was already right and only the prose was wrong. | Fixed | No — the emitted pattern was always the uniform double the table specifies; the cost was that the one paragraph explaining WHY the pattern was chosen justified it with a shape the hardware cannot make, so a reader auditing OTA feedback would look for a rise that is not there and find nothing to contradict the claim |
| N-79 | **The head-unit-gone envelope check ran while this device was DRIVING a key, so a low commanded level tripped it on the device's own output — flooding the link with duplicate events and making `LONG` unreachable (found/fixed 2026-09-24).** Found on hardware by the two-board bench rig (`code_driver_board/`), not by the host suite: it needs the analog loop closed by a real sense node. §6.2 step 2's envelope test is on `V_KEY_idle` — the line's **resting** level — but both `ServiceChannel` call sites (the enabled path and the disabled-channel path) evaluated it **every tick**, including while a pulse was on the line. While driving, the sense node reads **this device's own output**; there is no other actor on the line. A learned-only button (`ConfigDefault` ships `binding_count == 0`) has `head_unit_idle_mv_ == 0`, so it presents the button's ratio onto the command band's **1800 mV floor**, which is *at* the envelope's low edge and — with the servo's undershoot and the ADC's calibration of that level (commanded 1800 mV read back ~1790 mV) — fell just outside it. `head_unit_gone` then went true **every tick**, the tail did `ReleaseKey` + `gestures.Reset()`, the line floated back up, and the still-held button re-classified on the next tick and re-emitted: measured **26–177 identical `event` frames for a 2 s hold**, and because `Reset()` discarded the press state a held button could never reach `LONG`. A bound `OUT_VOLTAGE` at a higher level emitted exactly one, which is the tell — the fault depends on the commanded level. The user's own reading of the signal is the design intent: `/SENSEn` is a **boot/gain** input (§6.2 steps 1–5), not a per-tick supervision input, and the one per-tick use it had was reading the device's own output back. **Fixed:** the test is now `SystemOrchestrator::HeadUnitGone` (one home for both call sites). It asks "is the reading *consistent with what we are driving*": (1) a **driven** line is judged only on a deep sag — every command clamps to the band floor, so a driven reading at or above `kFaultSagMaxMv` (1600 mV = the floor less a 200 mV margin) is one we produced, while a rail **collapse** drives it far below and still releases (FR-39's phantom-key hazard); (2) a **released** line is judged on the whole envelope *and* the verdict must persist for `kHeadUnitGoneSettleMs` (250 ms), so the settle after a pulse or a rail coming up at boot is not read as an absent head unit. A `-1` conversion failure holds the settle clock rather than counting as out-of-envelope (N-43). `vbus_present` — §6.8's other head-unit-gone signal — was already emitted live from `gpio_read(GPIO_VBUS_VALID)` and is unchanged. **Pinned** by `ADrivenLineAtTheCommandFloorIsNotAHeadUnitGoneFault`, mutation-checked (disabling the sag guard fails exactly it); `ARailSagDuringAPressReleasesTheKey` still holds the deep-sag release. Native suite 503 → 504; device build clean (RAM 44.0%, Flash 19.7%) | Fixed | No — no key mis-fired and the release was always correct; the cost was duplicate link traffic and an unreachable long-press on any learned-only button, both visible only on hardware |
| N-80 | **A deliberately-refused protocol target was refused on a hardware claim that is false for this part, leaving the only way to flash a sealed device a recessed BOOT pin — and the software route, once added, needed a USB bus reset nobody would guess (found/fixed 2026-09-24).** Spec §4.3's `reboot` row offered one target (`app`) and refused `bootloader` as `bad_target`, justified by "entering the ROM download loader is a power-on/BOOT-pin event (§3.2) — not a software call". **That is wrong on the ESP32-S3**: the ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`RTC_CNTL_OPTION1_REG` bit 0) on every reset, the bit lives in the RTC domain so it survives `esp_restart()` but not a power cycle, and IDF's own `esp_usb_console_before_restart` writes exactly that bit for its `REBOOT_BOOTLOADER`. The refusal was honest about the code (`IHAL::reboot` was a bare `esp_restart()`) and wrong about the silicon, and the cost was concrete: this device's app link hands the USB PHY to TinyUSB, which removes the ROM USB-Serial-JTAG that esptool needs, so **while the app runs esptool cannot reach the board at all** (measured: every `--before` mode fails "No serial data received" on the app port, while the same esptool reaches the rig driver on its own cable). Flashing therefore required opening the enclosure and poking `SW1` on every iteration, which is what the user asked to stop happening. **Fixed:** `IHAL` gains `reboot_to_download` (a SEPARATE member, not a parameter — a caller that asked for the app and silently got the loader is the "wrong destination, reported as success" shape the `bad_target` refusal existed to prevent); `EspHal` implements it with `REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` then `esp_restart()`, the IDF idiom; `HandleReboot` dispatches on the target and still refuses anything else by name. `tools/dev_flash.sh` drives the whole flow (frame → loader → esptool write), `tools/dev_push_ota.py` provides the no-loader alternative, and `tools/dev_usb_reset.py` supplies the step that took the bench debugging: **macOS keeps the stale TinyUSB node (`303A:4001`) bound after the reset, so the ROM interface never appears in `/dev` and the device looks wedged — enumerated, silent, unreachable — until a `pyusb` `Device.reset()` forces re-enumeration, after which it appears as `303A:0009` (the S3 ROM's USB-OTG download interface, PID = chip id 9) and esptool connects immediately.** Two further bench-found defects in the same change: (a) the loader write must fill **BOTH** app slots — `otadata` is set to whichever slot a USB-OTA commit wrote, so writing `app0` alone on a device whose last OTA went to `app1` reboots the OLD image and looks like the flash silently did nothing; IDF's `ota_data_initial.bin` is all-`0xFF` so writing it is a no-op esptool skips, which is why the fix writes both slots rather than the selector. (b) Finding the loader port must exclude the **whole** set of pre-existing loader-shaped ports and must not `awk`-`exit` on the first match: the rig driver is often already in a loader and enumerates as `303A:1001`, the identical identity a BOOT-pressed device uses. **Pinned** by `CommandRouter.RebootSelectsTheDestinationNamedAndRefusesAnyOther` (both destinations routed and asserted DISTINCT via separate mock counters, plus the still-refused unknown target), mutation-checked; and `check_hal_contracts.py` gained check 6, which asserts the `EspHal`/`MockHal` `IHAL` member lists agree — a member wired in one and forgotten in the other is a NULL pointer on whichever side was missed, and neither suite can see it because each only compiles one implementation (mutation-tested by dropping `reboot_to_download` from `MockHal`). **Verified end to end on the bench:** `dev_flash.sh --loader` acked, entered the loader, wrote both slots with `--verify`, and rebooted the DUT into the new image over its own cable with no BOOT press; the device answers frames afterward. Native suite 562/562; device build clean (RAM 44.1%, Flash 20.3%). **The bootstrap boundary is real and is reported rather than hidden:** a blank board, or firmware predating this feature (which refuses `bootloader` with `bad_target`), still needs one BOOT press — after which every later flash uses the script. | Fixed | No — nothing mis-fired; the cost was that the only documented flashing path was a physical button on a sealed enclosure, and the software route silently produced a device that *looked* bricked until the USB reset was found |
| N-81 | **The maintenance radio's failure count never reset, so one failed bring-up made EVERY later window report a dead radio — and the file's other window-scoped invariants were asserted by nothing (found/fixed 2026-09-24, second audit pass on N-15).** Three defects, all in `MaintenanceRadio.cpp`, which is device-only: no native test compiles it and the board suite needs the hardware, so every property in it was checkable only by reading it. **(a) The cross-window bug, and the one with a user-visible consequence.** `g_failures` is what the app reads to decide what to write on the maintenance card, and the card branches on `ble_failures > 0` FIRST. Nothing ever cleared it: every failed path did `++g_failures` and no path reset it, so a single failed bring-up — a transient heap shortage, a driver error — left the count standing forever. Every LATER window then rendered "the device is in maintenance mode, but its radio did not come up, so there is no setup page to open" **while `page_url` in the same frame said the page was right there**, and the `SecretRow`s for the PoP and BLE name were suppressed (`maintenanceBleFailures == 0` gated them). The user is told the one thing that would stop them trying, with the working credential in the frame they are looking at — the N-76 shape (user-facing copy asserting a capability the device does not have), inverted. **Fixed:** the field is now a WINDOW state, not a running total — `MaintenanceRadioStop` clears it for every window that closes (including one whose start failed and left nothing to tear down, so the clear is BEFORE the early return, not inside `TearDown`, which a failed start also runs) and every failure path ASSIGNS 1 rather than incrementing. **(b) A reply path that could report a refusal as success.** `SendOutcome` sets the HTTP status line from `o.status`; had it not, a 401/404/500 body would go out under the default 200, and the page's own script gates on `r.ok` — so an unauthenticated request would render as a successful one, which is exactly what token gate exists to prevent. **(c) FR-32's teardown pairing.** `TearDown` frees by a per-resource flag so a partial bring-up frees only what exists; nothing tied the two lists together, so a resource added to the bring-up without a matching flag would leak a live radio stack past the window — the state FR-32's whole point forbids. **Pinned by a new gate, `tools/check_maintenance_radio.py` (+ `test_check_maintenance_radio.py`, 7 cases), because there is no other way to assert any of this:** it reads the flag names from `TearDown` itself, requires each to be set by the bring-up, requires `SendOutcome` to set its status, and requires the count to be cleared on stop and assigned (never `++`/`+=`) on a failure. Every check is mutation-tested against the real source — removing a flag's assignment, dropping the status call, dropping the clear, switching `= 1` to `++` — and each mutation is asserted to be REPORTED, so the gate cannot pass by having checked nothing (the `swc-stack-gate-silent-undertcount` failure). **Also fixed here:** two spec↔code drifts created by the raw-body pivot — §8.4 still said `multipart firmware upload` (the parser was deleted; the body is raw with `X-SWC-Sha256`/`X-SWC-Size`), and FR-38's row still said "a fixed deadline, not inactivity" after N-35 was resolved by this same radio | Fixed | Yes for the app-visible half — a device whose radio failed once told the user there was no setup page for the rest of time, with the page URL in the same frame |
| N-82 | **The maintenance window was ONE-SHOT per boot: a second entry PANICKED AND REBOOTED the device, found on the bench the first time the feature was exercised twice (found/fixed 2026-09-24).** The BLE scheme was initialised with `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`, whose `WIFI_PROV_DEINIT` handler calls `esp_bt_mem_release(ESP_BT_MODE_BTDM)`. **IDF's own contract for that call is one-way** — "Once Bluetooth Controller memory is released, the process cannot be reversed. This means you cannot use the Bluetooth Controller mode that you have released" (`esp_bt.h`) — so the second `wifi_prov_mgr_init` in a boot re-initialised the controller on heap that had already been handed back, and the device rebooted. **Measured, not inferred:** enter → exit → enter restarts the DUT every time (the app link drops mid-command and `uptime_ms` restarts from ~7 s), while the heap figures show the first cycle working perfectly (141,360 → 8,928 on entry → 137,764 on exit, i.e. FR-32's teardown is correct). The defect is invisible to every test in the suite and to a single-cycle manual check: it needs a SECOND trip through the window, and there is no reason a user would expect that to restart their adapter. **Fixed:** `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT`, which releases **classic BT at init** and, at DEINIT, only releases the BLE pool for the schemes that opt into it — so the BTDM pool stays reserved and re-init is safe. **The trade is deliberate and FR-32 is not weakened:** what FR-32 forbids is a radio INITIALIZED during normal operation, and after `deinit` the controller is de-initialized and idle under either scheme — a reserved-but-idle pool is not a radio. Classic BT is still released, which is the memory that matters on a 4 MB/no-PSRAM part (spec 9.2). **Verified on the bench after the fix: three consecutive enter/exit cycles, `uptime_ms` climbing monotonically throughout (12,728 → 54,729 ms, no reboot), the AP broadcasting as `SWC-A1B2` (RSSI −39 dBm, seen by a CoreWLAN scan), and the heap settling at ~135,756 after each exit with no downward drift.** `check_maintenance_radio.py` pins the scheme choice by name, since no host test can: the handler is a macro in an IDF header and the failure is a runtime reboot. **Also fixed in the same session, from the same bench run:** `dev_flash.sh` passed `--verify` to esptool, which **v5 removed** (verification is now unconditional) — the flag made the whole command fail with `No such option '--verify'` *after* the device had been dropped into the ROM loader, leaving the board sitting in the loader with nothing written | Fixed | Yes — a second maintenance entry rebooted the device, and the window is the feature's whole entry point |
| N-83 | **FR-33's config-flag maintenance trigger was the one of §8.2's four that had no setting at all, so a user could not ask the device to come up in setup mode after a flash (found 2026-09-24, closing N-13's config-flag half).** `MaintenanceTrigger::kConfigFlag` existed and was unit-tested in isolation, but nothing could produce it: `DeviceSettings` had no field carrying the flag. **Fixed:** `settings.maintenance_on_boot` (bool), with `SystemOrchestrator::Boot` consuming it — clear in memory FIRST and unconditionally, persist the clear ONLY when a store exists and the config in force is durable, then `EnterMaintenance(kConfigFlag, now)`. **The consume is the whole safety property, not a tidy-up:** spec 8.2 scopes the trigger to the NEXT boot, and the exit path cannot clear a stored field (only a whole-config commit rewrites `settings`), so a flag left set would reopen the window on every boot for the life of the config — an unbounded maintenance window, the state FR-38 exists to forbid. The clear is in memory before the save so a FAILED save costs persistence, not a stuck window (the next boot re-reads the old config and opens once more: a bounded over-trigger, not an unbounded one). The save is gated on `store_ != nullptr` AND a durable `config_state_`, because saving under a config that just fell back to defaults would write the DEFAULTS over the user's config — the read-modify-write collapse this project already paid for once. **The field is OPTIONAL on the wire**, following `key_mv`'s precedent: absent decodes as `false`, so a config written before the field existed still loads (a required field would make every already-configured device read as "no config"); present-but-malformed FAILS the decode rather than silently defaulting. **No stack cost, and that took a deliberate step:** adding a bool to `DeviceSettings` pushed `sizeof(Config)` from 8,912 to 8,920, because the field landed in `DeviceSettings`'s tail padding while `Config`'s own tail padding (7 bytes after `binding_count`) went unused. Grouping the three count bytes (`channel_count`/`aux_count`/`binding_count`) before the `bindings` array absorbs the new field at ZERO size cost, keeping the pinned 8,912-byte figure the stack-safety arithmetic is written against. Safe because nothing treats `Config` as raw memory — it crosses the link as JSON and reaches NVS as a JSON blob. The app mirrors the field (`DeviceSettings.maintenanceOnBoot`), so a round trip through the app cannot silently clear it. Pinned by 4 orchestrator tests, a codec back-compat test, and an app test; all mutation-tested (dropping the persist, dropping the in-memory clear, dropping the whole trigger block, making the decoder field required, and flipping the app's absent-field default to true each fail a named assertion). **A DEVICE-ONLY DEFECT FOUND ON THE BENCH, the reason the first version of this was wrong:** the persist was guarded on `store_ != nullptr`, but `SetStore` is called from `UsbLinkStart`, which runs AFTER `SystemOrchestratorCreate`/`Boot` — so `store_` is null throughout Boot on the real device and the save was SKIPPED EVERY BOOT. Measured on the DUT: a config with the flag set reopened the window on every boot and the stored flag stayed `true`. The host tests missed it because they called `SetStore` BEFORE `Boot`, an order the device never uses — the same device-only blind-spot class as `EspHal.cpp`/`UsbLink.cpp`. **Fixed by using the LOCAL `ConfigStore` the load already built**, and the tests now call `SetStore` AFTER `Boot` to match the device, with the `store_` form mutation-tested to fail them. **Re-verified on the bench: first boot opens the window (`active: true`, PoP `<PoP>`, page URL) AND clears the stored flag to `false`; a second boot reports `active: false` with an empty PoP, so the window is bounded as FR-38 requires.** **Verified: 591/591 native, 199/199 app, the app↔firmware crosscheck still byte-identical** | Fixed | No — the flag defaults OFF, so nothing changes for a device that never sets it; the failure direction that matters is an unbounded window, which the consume prevents |
| N-84 | **FR-19's trim loop, when enabled, moved the output the WRONG WAY — it was fed the sense reading taken BEFORE the command it was meant to trim (found/fixed 2026-09-24 on the bench).** Spec 6.5 ships the trim present-but-disabled "until its gain is measured on hardware", and enabling it is FR-19's remaining board step. Enabling it revealed the defect: `DriveBoundLevelMv`, `TestDriveKeyMv` and `PresentLevel` all called `ServoLoop::Update` with the sense reading captured earlier in the same tick — the line's IDLE level, not the result of the code about to be written. With the loop disabled this was invisible (the call is inert), which is why the host suite and the disabled bench both passed. **Measured:** with a `test_key` pulse at 2200/2500/2800 mV, the static error was consistently ~11 mV WORSE with the loop enabled than disabled — exactly one `max_step` (8 codes x 1.82 x 3300/4096 = 11.7 mV) applied in the wrong direction — with NO added noise (repeat spread 1-5 mV either way), which is the signature of a fixed wrong-direction step rather than a controller. **Fixed:** the loop is no longer serviced at command time at all; `ServiceTrim` runs it from `Tick` AFTER the servo has settled (`kServoTrimSettleMs` 60 ms, then `kServoTrimIntervalMs` 200 ms), reading the DRIVEN line so it trims toward the code it wrote. The interval is a measured-plant retune of spec 6.5's 1-2 Hz: this device commands a BOUNDED pulse (`send_duration_ms`, default 200 ms), so a 1-2 Hz trim would get at most one update per press and could never null a static error; 200 ms is still 5x below the 16 Hz analog loop, which is the stability property the rate protects. A released line is never trimmed (that would move the IDLE code — a held-key hazard), and a `-1` sense read holds rather than trims toward a sentinel (N-43). Pinned by `SystemOrchestrator.TheTrimLoopTrimsTheDrivenLineNotTheCommandTimeReading` (asserts the command-time write is the open-loop code AND the settled trim moves TOWARD the target), mutation-tested by restoring the pre-drive `Update` call, which fails it. `ServoLoop` gained `Config()`/`SetConfig()` and the orchestrator `SetTrimEnabledForTest` so a host test can reach the orchestration the compile-time switch otherwise hides. **The enable switch is `SWC_BENCH_TRIM_LOOP`** (a define, not a config field — enabling the loop is a hardware-characterisation decision, and a config field would be one more way a shipped device runs a loop tuned against a guess). Native suite 592 -> 593. **The FIXED build is CONFIRMED ON THE BENCH (2026-09-24, DUT back on the bus).** `tools/bench_trim.py` re-measured the enabled build against the disabled baseline: with the loop ENABLED the settled-level static error was −29/−33/−6 mV at 2500/2200/2800 mV versus −37/−33/−10 mV with it DISABLED — **no worse, and marginally better**, which is the inverse of the +11 mV wrong-direction signature this defect was found by. The enabled repeat spread was 0–9 mV (no noise injection). The measurement needed the rig to present a wheel-like idle AND the DUT rebooted holding it, or the command band is empty and every pulse acks without driving; `bench_trim.py` now establishes that precondition itself (commit 2ae7cb3). | Fixed | No — the loop ships DISABLED, so no shipped device ran the wrong-direction step; the defect is only reachable once the loop is enabled, which is exactly the FR-19 bring-up step |



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
| Programming UX (hold, then single/double/long, escalating buzzer) | Present in intent | Specified as first-class (§7.5) | **Built (2026-09-21):** §7.5 is rewritten to the stateless held-modifier interaction the user described, and §6.6 rule 4 (unbound → pass-through) makes a headless-learned button work with no app. See N-19 |
| Android link | Custom serial driver | NDJSON over TinyUSB CDC (§4) | **Kept, standardized** |
| Timings | `MAX_DOUBLE_PRESS_OFF_MS 500`, `MIN_LONG_PRESS_MS 750`, `KEY_SEND_DURATION_MS 200` | Same defaults, now configurable and tested | **Kept** |
