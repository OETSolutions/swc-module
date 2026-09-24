# Design document — SWC Steering Wheel Controls Adapter

Engineering description of the board: requirements, theory of operation,
component choices, and the reasoning behind them. For what the project is and
how to build it, see [README.md](README.md). For sourcing and fabrication, see
[MANUFACTURING.md](MANUFACTURING.md). For how to work on the files, see
[AGENTS.md](AGENTS.md).

- [1. Requirements](#1-requirements)
- [2. Prior design](#2-prior-design)
- [3. Block diagram](#3-block-diagram)
- [4. Theory of operation](#4-theory-of-operation)
- [5. Design rules and stackup](#5-design-rules-and-stackup)
- [6. Key design decisions](#6-key-design-decisions)
- [7. Open items](#7-open-items)

## 1. Requirements

The design brief, in full:

- ESP32 module; USB-C to the head unit (the ESP32 enumerates as a USB CDC
  serial device and takes commands).
- Two identical channels: ladder → ADC → translate → DAC → head-unit KEY input.
- The DAC range must sit **within the head-unit input range sensed at startup**.
- The output must be **isolated / high-impedance when idle**, so the head-unit
  KEY line floats to its own Vcc when not driven.
- Single, double and long-press translation, with a temperature sensor to
  compensate for ladder drift over the cabin temperature range.
- Screw terminals on the board edge for power, inputs and outputs; USB-C on an
  edge.
- JLCPCB assembly, stock parts, Economy/Basic tier where possible; **Extended
  parts only if there is absolutely no alternative.**
- Professional schematic and layout; clean ERC and DRC.

## 2. Prior design

This is a redesign of a 2022 RP2040 board. Its sources are archived under
`old/` and are reference only. Its specific failures — the reason this board
exists — were:

- it used a **digital pot**, which was not precise enough (this board uses a
  12-bit DAC with a servoed output instead);
- **no auto-ranging** to the head unit's sensed input voltage;
- **no temperature compensation**;
- it was a Pi Pico module build, not a custom PCB.

## 3. Block diagram

Two identical channels, **SWC1** and **SWC2**. Every part below is per channel
unless noted; channel 2 mirrors channel 1 with the `C`/`D` halves of the DAC and
quad op-amp.

```text
  SWC1  J2.3 ─R1 10k─┬─ /SWC1_ADC ─────────► U3 ADC1
  SWC2  J2.2 ─R2 10k─┴─ /SWC2_ADC ─────────► U3 ADC1
        (R15/R16 10k pull-ups to +3V3 · D4/D5 BAT54S clamp · C3/C4 100nF)

  AUX1  J5.4 ─R23 1k─┬─ /AUX1_F ───────────► U3 ADC
  AUX2  J5.3 ─R24 1k─┤   /AUX2_F
  AUX3  J5.2 ─R25 1k─┴─ /AUX3_F
        (R17/R18/R19 10k pull-ups to +3V3 · D8/D9/D10 BAT54S · C15–C17 100nF)

  U3 ESP32-S3 ══ I2C, 3.3 V, R5/R6 10k pull-ups ══ U4 MCP4728  (VDD = +3V3,
       │                                              VREF = VDD, 0–3.3 V FS)
       │                                              ┌ A ─ signal, ch 1
       │                                              ├ B ─ gain mode, ch 1
       │                                              ├ C ─ signal, ch 2
       │                                              └ D ─ gain mode, ch 2
       │
       └─ per channel:  U4.VOUTx ─R46 100k─► U6x− ─(C24 100nF fb)─► U6x out
                                          U6x+ ◄─ R58 82k ─ V_buf
                                              ◄─ R61 100k ─ V_ADJ (VOUTB/D)
                        U6x out ─R52 1k─► Q4 gate ─► Q4 drain = KEY line
                        KEY ─R36 1M─► U6y buffer ─┬─► R54 10k / R50 10k ─► /SENSEx ─► U3 ADC
                                                   └─► R58 (feedback above)

  12 V ─ J1 ─ F1 PPTC ─► /+12V_SW ─► D1 SMBJ18A ─► U1 XL1509-5.0 ─► L1 47µH ─┐
                                                  (D7 SS34 freewheel)         │
  USB VBUS ─ J4 ─ F2 PPTC ─► /VBUS_FUSED ─► D3 SS34 ─┐                        │
                                                     ├─► +5V ─► U2 AMS1117 ─► +3V3
                              /+5V_BUCK ─► D2 SS34 ──┘
  NTC RT1 on /TEMP_ADC · buzzer BZ1 via Q3 · LEDs D6 / D12 · VBUS detect /VBUS_VALID
```

## 4. Theory of operation

### 4.1 Inputs

**SWC1 / SWC2.** The steering-pad ladder is a **series resistor chain whose
common is tied to GND**; each button shunts a different point in the chain to
that common, so a press pulls the input **down**. Idle (no button) is therefore
the *high* state, set by the **10 kΩ pull-up** (`R15`/`R16`) to +3V3. Each input
takes a **10 kΩ series resistor** (`R1`/`R2`) into a **BAT54S** clamp
(`D4`/`D5`) to +3V3/GND (protection only, not the operating point), a 100 nF
filter (`C3`/`C4`), and that pull-up. The pull-up is what lets a bare
switch-to-ground button work as well as a ladder. Both SWC lines land on the
ESP32-S3's ADC inputs.

> **Correction (2026-09-18).** This paragraph previously said the ladder is
> "pulled to 12 V when idle", which is wrong and inverts the direction a press
> moves the input. Verified against `Tundra_SWC_steeringpadswitch.bmp` in the
> Android_Stereo_Apps working notes and with the board's owner. The ladder
> resistances are vehicle-specific, so **`R15`/`R16` may need adjusting to spread
> the buttons adequately across the ADC range** — measure before assuming.
> The firmware spec's §2.4/§6.3 carry the corrected derivation.

**AUX1–AUX3.** On the 4-pin `J5` terminal, intended for extra buttons or
programming functions. Identical conditioning but with a **1 kΩ series
resistor** (`R23`–`R25`) rather than 10 k, the same BAT54S clamp
(`D8`–`D10`), 100 nF (`C15`–`C17`) and 10 kΩ pull-ups (`R17`–`R19`). All three
sit on ADC-capable module pins.

All three terminals that carry signals — `J2` (inputs), `J3` (outputs) and `J5`
(AUX) — include a **GND pin**, so each cable carries its own reference.

### 4.2 The ESP32-S3 and its analog ceiling

Two facts drive the whole analog design, and both are worth stating explicitly
because they have been gotten wrong before:

- **The ESP32-S3 has no DAC at all.** The classic ESP32's 8-bit DAC did not
  carry over to the S3. This is why the external DAC is not optional.
- **The S3's calibrated ADC ceiling is 2.9 V**, not 3.1 V, at the 12 dB
  attenuation setting. Every divider on the board is scaled against 2.9 V.

The module's native USB provides both the CDC interface to the head unit and
USB-JTAG for debugging.

### 4.3 DAC

`U4` is an **MCP4728** quad 12-bit I²C DAC. It runs from **+3V3** with
**VREF = VDD**, so its full-scale output is 3.3 V (1 LSB = 0.806 mV) — it is
*not* the 4.096 V internal-reference mode, which would require VDD ≥ 4.096 V.
Because the DAC is on 3.3 V, it shares the ESP32's I²C bus **directly**, with
`R5`/`R6` 10 kΩ pull-ups to +3V3 — there is **no level shifter**. A level
translation stage would be needed only if the DAC ran on 5 V; it does not. Its
LDAC pin is driven from a GPIO (`R13` 10 kΩ pulldown) and its `RDY/BSY` is left
unconnected.

The four channels are paired per signal channel: **A = signal and B = gain mode
for channel 1; C = signal and D = gain mode for channel 2.**

### 4.4 Output servo

**The output is a closed-loop servo, not an open-loop buffer.** Per channel,
the DAC voltage enters `U6A`'s inverting input through `R46` (100 kΩ), with
`C24` (100 nF) as feedback — an **integrator**. At DC the integrator forces no
current through `R46`, so `U6A−` equals the DAC voltage, and the op-amp forces
`U6A+` to match. `U6A+` is the summing junction of `R58` (82 kΩ, from the
sensed KEY-line voltage `V_buf`) and `R61` (100 kΩ, from the gain-mode DAC
output). Solving gives the load-bearing relation:

```text
V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ
```

So **the gain is set by the resistor ratio alone, and `V_ADJ` shifts the
offset** — it does not change the gain. `U6A`'s output drives the sink FET `Q4`
gate through `R52` (1 kΩ), with `R48` (100 kΩ) holding the gate low if the
op-amp is unpowered.

**The gain-mode channel is a range selector.** `R58/R61` = 82 k/100 k gives a
gain of 1.812, which is what lets a 3.3 V DAC drive a 5 V head unit. The mode is
chosen by what the `V_ADJ` channel does:

| Head unit | `V_ADJ` channel | Mechanism | Gain |
| --- | --- | --- | --- |
| **5 V range** | powered down (`PD1:PD0 = 01`) | 1 kΩ path to GND — current flows in `R58` | **1.812** |
| **3 V range** | normal, tracking the signal channel's code | `V_ADJ = V_DAC` — no current in `R58` or `R61` | **1.000** |

This is a defined path rather than a leakage one, which is the point: the
MCP4728 has **no high-impedance state** — all three of its power-down modes are
pull-downs to GND at 1 kΩ, 100 kΩ or 500 kΩ. The 1 kΩ power-down is exactly
what the 5 V mode wants, so a DAC channel can be used as a switch without any
leakage error. A series MOSFET was considered and rejected — see
[§6](#6-key-design-decisions).

Two consequences worth knowing:

- **The real ceiling is the op-amp rail, not the gain.** `U6` runs on +5 V, so
  `V_buf` cannot exceed ~4.98 V. Gain 1.812 advertises 6.0 V but the KEY line
  can only be actively servoed to about 5 V; the extra gain is *release margin*,
  putting the release command safely above idle rather than on the boundary.
- **The ADC can never saturate.** See [§4.5](#45-sense).

### 4.5 Sense

The KEY line is buffered by `U6B`, wired as a unity-gain follower (`U6.6` tied
to `U6.7`), reading through `R36` (1 MΩ) — so the sense path presents a picoamp
load to the head-unit line and never loads it. The buffer output then feeds two
places: back into the servo as `V_buf`, and through a **10 kΩ / 10 kΩ divider**
(`R54`/`R50`) into `/SENSE1` with a 100 nF anti-alias capacitor (`C22`).

The divider is an **exact ÷2**, so `V_SENSE = V_KEY/2`. Because the op-amp rail
binds first (V_buf ≤ ~4.98 V), the sense node can never exceed ~2.49 V — safely
*under* the S3's 2.9 V calibrated ceiling. **The op-amp rail protects the ADC
for free, and the ADC can never saturate.**

Measuring `V_SENSE` at startup, with the output released, is how the firmware
learns the head unit's own idle/float voltage and so auto-ranges its output — one
of the capabilities the 2022 design lacked ([§2](#2-prior-design)).

### 4.6 Idle is inherently high-impedance

Releasing the line needs no special mode. `Q4` **only sinks**: it pulls the KEY
line down to ground. Commanding a target *above* the head unit's idle voltage
makes the servo drive the gate low, `Q4` turns off, and the line floats up to the
head unit's own pull-up through only `R36` (1 MΩ) into the follower's input.

Power-up safety follows: with the DAC at zero the loop would servo the line to a
stuck press, so the firmware programs the MCP4728's EEPROM so that channel A
resets to full scale — the maximum command — and the gain-mode channel resets
powered-down. That yields a 5 V-range command that releases both 3 V and 5 V head
units.

### 4.7 Power

Both a 12 V feed and USB VBUS can power the board, and — importantly — **the
12 V feed is optional**. Diode-OR'd at +5 V, either source alone is sufficient;
if both are present the higher one supplies the rail.

- **12 V path:** `J1` → `F1` PPTC (1.5 A) → `/+12V_SW` → `D1` SMBJ18A TVS →
  `U1` XL1509-5.0 buck with `L1` (47 µH); **`D7` (SS34) is the freewheeling
  diode** on the switch node.
- **USB path:** `J4` VBUS → `F2` PPTC (1.0 A) → `/VBUS_FUSED` → **`D3` (SS34)**.
- **Buck output:** `/+5V_BUCK` → **`D2` (SS34)**.
- `D2` and `D3` are the OR-ing diodes. Each source therefore has its own PPTC
  and its own Schottky, so a fault on one cannot back-feed through the other.
- **3.3 V:** `U2` AMS1117-3.3 from +5 V.
- Bulk 100 µF/50 V capacitors sit on the +12 V and buck-output nodes.

`/VBUS_VALID` is a divider (`R56`/`R57` 10 kΩ/10 kΩ with 100 nF `C26`) off the
fused VBUS, so the firmware can tell whether USB power is present.

### 4.8 USB

`J4` is a 16-pin USB-C receptacle feeding the ESP32-S3's **native USB**, which is
**Full-Speed / Low-Speed only — the S3 has no High-Speed PHY**. D+/D− pass
through `U8`, a `USBLC6-2SC6` ESD array, then through 22 Ω series resistors
`R11`/`R12`, then to the module.

The ordering is deliberate: **connector → clamp → series R → IC**. The transient
is diverted into the TVS *before* the resistor, leaving the resistor to limit
only the residual current into the ESP32 pin. The USBLC6-2 is flow-through
(pins 1↔6 and 3↔4 are the same channel), which is why both ends of each channel
sit on the connector-side net. `U8` is also powered from `/VBUS`, so the clamps
are referenced to the actual USB rail.

The pair is length-matched, though at Full-Speed no impedance target is owed —
USB 2.0's 90 Ω ±15 % applies to High-Speed only. It costs nothing, and the pair
is via-free on F.Cu. On the 22 Ω value itself, see
[§6](#6-key-design-decisions).

### 4.9 Thermal sensing

`RT1` (a 10 kΩ B3380 NTC, 0603) sits from `/TEMP_ADC` to GND, with `R29` (10 kΩ)
to +3V3 and `C19` (100 nF). It compensates the ladder for temperature drift, so
it must measure the *environment*, not the board: it is placed at **(68.5, 120.0)**
on the exposed right edge, roughly 33 mm from the nearest heat source, where case
ventilation reaches it, and its divider trace is kept out of thermal gradients.

The four board corners — the obvious place for an edge sensor — are unusable:
each carries an M5 mounting hole whose courtyard is a 5.55 mm radius circle.

### 4.10 Peripherals

- **Buzzer `BZ1`** on +5 V, switched low-side by `Q3` (2N7002) from `/BUZZ`
  through `R27` (100 Ω), with `R28` (100 kΩ) holding the gate low. `D11` (SS34)
  is its freewheeling diode. `BZ1` is a 5–15 V part, so it cannot run from 3V3.
- **LEDs:** `D6` (`/LED_STAT`, via `R7`) and `D12` (`/LED2`, via `R26`).
- **Buttons:** `SW1` (BOOT → module strapping pin) and `SW2` (RESET → EN).
- **Test points:** `TP1`–`TP6` on module I/O, plus `TP7`/`TP8` on the UART
  `TXD0`/`RXD0`. All eight are bare through-hole pads with no purchased part.
  `TP7`/`TP8` are **the console**: the production console is on UART0 (GPIO43/44,
  off the USB PHY), so it stays readable after TinyUSB takes the PHY for the app
  link (N-16, fixed 2026-09-25) — clip a UART adapter (`115200 8N1`) between them
  and a ground pad.

### 4.11 Interfaces

| Ref | Function | Pins | Position |
| --- | --- | --- | --- |
| `J1` | 12 V DC in | GND, +12 V | left edge |
| `J2` | SWC input | GND, SWC2, SWC1 | left edge |
| `J3` | Output to head unit | GND, KEY2, KEY1 | left edge |
| `J5` | Auxiliary inputs | GND, AUX3, AUX2, AUX1 | left edge |
| `J4` | USB-C | — | top-left edge |
| `SW1` / `SW2` | BOOT / RESET | — | — |
| `TP1`–`TP8` | Test points | — | bottom edge, B.Cu |

All four screw terminals are CUI TB007 series on 5.08 mm pitch, on the left board
edge so wire entry runs along the edge; USB-C sits on the top-left edge. The
brief allowed for separate per-channel SWC/output terminals; the two channels
instead share one input (`J2`) and one output (`J3`) terminal, each with a GND
pin.

### 4.12 The ESP32 antenna

The module overhangs the top board edge by **5.900 mm**, at the antenna end. This
is a deliberate acceptance decision, not an oversight, and it means no antenna
cutout is needed: the stock 48 × 21 mm keepout lies off the board except for a
0.25 mm sliver, so there is no copper under the antenna because there is no board
under the antenna.

| | board mm |
| --- | --- |
| Board top edge (`Edge.Cuts` y = 31.000, x 18…68) | 31.000 |
| `U3` module body (F.SilkS outline) | y 25.100 … 50.950 |
| `U3` pads (62), centres | y 32.740 … 50.500 |
| `U3` pads (62), copper edges | y 32.290 … 51.250 |
| `U3` antenna keepout (F.CrtYd stub, x 22.219 … 70.219) | y 10.250 … 31.250 |

The consequence — and the reason the board is 102 mm tall — is that the overhang
**must be supported by the enclosure**. A fully-seated module would need roughly
108 mm of board, and the pad row at y 32.740 is already only 1.740 mm from the
edge, so the module cannot simply be moved down. The two `silk_edge_clearance`
warnings on `U3` are the module outline being drawn across the edge it
intentionally crosses.

## 5. Design rules and stackup

From `SWC.kicad_pro`:

| Rule | Value |
| --- | --- |
| Minimum clearance | 0.150 mm |
| Minimum track width | 0.150 mm (default routed width 0.200 mm) |
| Copper-to-edge clearance | 0.300 mm |
| Hole-to-hole | 0.250 mm |
| Via | 0.6 / 0.3 mm (secondary 0.8 / 0.4) |

Four copper layers — F.Cu / In1.Cu (power) / In2.Cu (power) / B.Cu — with five
zones. Board outline 54.00 × 102.00 mm, 1.6 mm thick, rounded corners r ≈ 2.83 mm.

## 6. Key design decisions

| Decision | Rationale | Alternative rejected |
| --- | --- | --- |
| External 12-bit DAC | The ESP32-S3 has **no DAC at all** (the classic ESP32's 8-bit DAC did not carry over), and 8 bits would be too coarse anyway | On-chip DAC — does not exist on the S3 |
| DAC on +3V3, VREF = VDD | VDD must be ≥ 4.096 V for the internal-reference mode, so that mode is invalid here; a 3.3 V DAC also shares the ESP32's I²C bus directly | 5 V DAC + I²C level shifter — adds parts, buys nothing |
| **No I²C level shifter** | Both the ESP32-S3 and the MCP4728 are on 3.3 V, on the same bus, with `R5`/`R6` pull-ups. There is no voltage domain to translate | Level shifter — only needed if the DAC ran on 5 V |
| Closed-loop integrator servo | Regulates the KEY line against the sensed `V_buf`, so output is precise and repeatable; the 2022 design's digital pot was not | Open-loop buffer — no load regulation |
| Gain set by `R58/R61` at the summing node, offset by `V_ADJ` | Gain and offset separate cleanly: `V_KEY = (1+R58/R61)·V_DAC − (R58/R61)·V_ADJ` | Resistor change per head-unit range — not switchable at runtime |
| **`V_ADJ` driven by a DAC channel, not a GPIO** | The MCP4728 has no Hi-Z state but does have a *defined 1 kΩ* power-down, which is exactly what the 5 V range wants | Series MOSFET — off-state leakage appears at the KEY line multiplied by `R58`, and a 2N7002's `I_DSS` roughly doubles per 10 °C |
| ÷2 sense divider **after** the buffer | `V_SENSE ≤ 2.49 V`, under the S3's 2.9 V ceiling, so the ADC can never saturate; being after the buffer means it never loads the head-unit line | Divider at the input — would load the KEY line |
| 22 Ω USB series resistors | Follows ST's own USBLC6-2 application circuit (Fig. 14) and the brief, at a value that is provably harmless because the S3 never runs High-Speed. **Not an Espressif requirement** — the ESP32-S3-DevKitC-1 v1.1 runs its data lines straight to the module with no series resistors at all | Omit them — also valid; kept because they cost nothing (Basic part) and follow vendor topology |
| Connector → clamp → series R → IC ordering | Diverts the transient into the TVS *before* the resistor, leaving the resistor to limit only residual current into the pin | Series R before the clamp |
| ESP32 antenna overhangs the top edge, no cutout | The keepout lies off-board except a 0.25 mm sliver, so there is simply no copper under the antenna | Antenna cutout / slot — unnecessary |

## 7. Open items

**A project-local footprint library for `J4`, `U3` and `R60`.** Their local
edits are currently undone by any footprint-library resync or KiCad upgrade —
the same failure mode that hit the `J4` 3D model. A project-local
`footprints/<lib>.pretty` (already the pattern used for `logo.pretty`) makes the
edits durable *and* clears the three `lib_footprint_mismatch` warnings. It is
not done because there is no sanctioned way: the kicad MCP server has no
footprint-library tool and no tool writes a `.kicad_mod` at all, so it needs
either hand-written files (forbidden) or a new script under the `tools/`
exception. See [AGENTS.md](AGENTS.md).

The other open question — **4-layer vs 2-layer at order time** — is a purchasing
decision, not a design one, and lives in
[MANUFACTURING.md §5](MANUFACTURING.md#5-ordering-decisions).
