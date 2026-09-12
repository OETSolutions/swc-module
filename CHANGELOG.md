# Changelog

Revision history for the SWC Steering Wheel Controls Adapter.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This is a hardware project, so "version" is the board revision; the project is
not yet ordered and carries no assigned revision letter (`REV ${REVISION}` on
the F.Fab block is still an unresolved text variable).

## [Unreleased] — 2026-09-11

Full redesign of the 2022 RP2040 reference board. Design complete and routed;
**not yet ordered**. See [DESIGN.md](DESIGN.md) for the architecture and
[MANUFACTURING.md](MANUFACTURING.md) for the order.

### Added

- **ESP32-S3-WROOM-1-N4** (`U3`) replacing the Pi Pico module, with native
  USB-CDC and USB-JTAG.
- **MCP4728 quad 12-bit I²C DAC** (`U4`) on +3V3 with VREF = VDD — the ESP32-S3
  has no DAC of its own, and 8 bits would have been too coarse.
- **Closed-loop integrator servo** output stage (`U6` quad op-amp): the DAC
  drives an inverting integrator whose summing node sets
  `V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ`. Gain is a resistor ratio
  (82 k / 100 k → 1.812), and `V_ADJ` shifts the offset, giving runtime range
  switching that the 2022 design did not have.
- **Auto-ranging** — the KEY line is sensed through a 1 MΩ / unity-gain buffer
  and an exact ÷2 divider (`R54`/`R50`), so firmware can learn the head unit's
  own idle voltage at startup.
- **Temperature compensation** — `RT1`, a 10 kΩ B3380 NTC on the exposed right
  edge.
- **Three auxiliary inputs** (`J5`, `R23`–`R25` 1 kΩ + 10 kΩ pull-ups,
  BAT54S-clamped) for extra buttons or programming functions.
- **`J1` 12 V input** making external power optional — the board runs from USB
  VBUS alone.
- **Per-source protection**: `F1`/`F2` PPTCs and `D2`/`D3` SS34 OR-ing diodes,
  so a fault on one source cannot back-feed the other.
- Test points `TP1`–`TP8`; buzzer `BZ1` on +5 V via `Q3`; status LEDs `D6`/`D12`.
- `tools/` direct-write scripts, `AGENTS.md`, `CLAUDE.md`, `DESIGN.md`,
  `MANUFACTURING.md`, this changelog.

### Changed

- **Input conditioning** for a 12 V-idle factory ladder: 10 kΩ series, BAT54S
  clamp to +3V3/GND, 100 nF filter, 10 kΩ pull-up — so a bare switch-to-ground
  button works as well as a ladder.
- **USB.** `J4` USB-C through `U8` (USBLC6-2SC6) and 22 Ω series resistors
  `R11`/`R12` to the module's native USB. Ordering is connector → clamp →
  series R → IC.
- **Power.** `U1` XL1509-5.0 buck (`L1` 47 µH, `D7` freewheel) and `U2`
  AMS1117-3.3; `D2`/`D3` diode-OR at +5 V.
- **C1/C2 bulk input caps re-sourced** from `C3151829` (ROQANG RVT1H101M0607) to
  **`C46550415`** (jieerrui JVJ50V100M6x8). Same 100 µF/50 V ±20 %,
  2000 hrs @105 °C, `SMD,D6.3xL7.7mm` part in the identical `CP_Elec_6.3x7.7`
  footprint, so **no layout change** — it adds a published 140 mA @120 Hz ripple
  rating the ROQANG never listed, and widens operation to −55…+105 °C. The
  motive was JLCPCB's **`PCBA Type`** field: the ROQANG part is `Standard Only`
  and forced the order onto Standard PCBA.
- **`U3` MCU module re-sourced** from `C2913197` (Espressif ESP32-S3-WROOM-1-N4)
  to **`C49164655`** (DOIT `ESPS3-32-N4`), also to clear a `Standard Only`
  flag — every Espressif ESP32-S3 WROOM-1/‑1U/MINI-1 variant is `Standard Only`.
  The DOIT module is a verified clone of the WROOM-1: 18 × 25.5 × 3 mm, all 41
  pins identical by number and function (IO19/IO20 = USB D−/D+, `RXD0`/`TXD0` =
  IO44/IO43, EPAD = GND), and the same land pattern, so it fits
  `RF_Module:ESP32-S3-WROOM-1` with **no PCB change**. Trade-offs: third-party
  single source (645 stock), DOIT's own FCC ID, and no PSRAM option that is
  Economic-eligible. With both flags cleared the order should qualify for
  Economic PCBA — see [MANUFACTURING.md §3](MANUFACTURING.md#3-pcba-type--the-economicstandard-decision).
- **Connectors onto the board edge**: all four screw terminals on the left edge,
  USB-C on the top-left edge. The 2022 design's separate per-channel terminals
  were consolidated into one input (`J2`) and one output (`J3`), each with a GND
  pin.

### Fixed

- **USB D+/D− corridor re-routed.** An earlier "provably un-routable" conclusion
  was wrong: it came from a BFS that treated every SMD pad as an obstacle on
  **both** layers, when an F.Cu pad never blocks B.Cu. With that corrected and
  placement treated as the free variable it is, the pair routes cleanly and is
  **via-free on F.Cu**. `/VBUS` moved out of the corridor and was widened to
  0.50 mm. Board DRC went 50 → 19 violations and unconnected 27 → 0; the 27 were
  collateral from the rip's bounding box, and all were restored.
- **Silkscreen pass.** All 108 reference designators re-placed against a
  DRC-accurate obstacle model, and 140 labels now sit at 0° or 90° with no
  text-over-text overlap. Four defects in the placement tooling were found and
  fixed — most notably a sign error in the rotation transform that had been
  mirroring every obstacle of a ±90° footprint, and the discovery that unprinted
  F.Fab `Value` fields and tented vias were being treated as hard obstacles when
  neither can ever be flagged.
- **`J4` (USB-C) orientation** corrected to `(at 19.65 48.206 270)` with its 3D
  model set to `rotate (0 0 180)` / `offset (0 −1.07 0)`, so the shell lands on
  the footprint's own F.Fab body instead of hanging off the board.
- **`RT1` re-placed** to the exposed right edge — the four board corners are
  unusable, each carrying an M5 mounting hole with a 5.55 mm-radius courtyard.
- **BOM regenerated from the schematic.** The archived CSV predated the
  output-stage rework and listed `R50`/`R51` as 13.7 k; they are **10 k**, which
  is what makes the sense divider an exact ÷2.
- **Documentation corrected against the netlist.** Several confident earlier
  notes were wrong: `U4` runs on **+3V3** (VREF = VDD, 0–3.3 V FS), so there is
  **no I²C level shifter**; the op-amp is an **integrator servo**, not an
  open-loop buffer; and there are **five** BAT54S positions
  (`D4`/`D5`/`D8`/`D9`/`D10`), not three.
- **Board renders added to the front page.** `SWC_Top.png` and `SWC_Bottom.png`
  (3616 × 1936) show the current design top and bottom; the README previously
  had no image of this board at all.

### Removed

- **Digital potentiometer** — the 2022 design's core weakness; not precise
  enough, and it could not auto-range. Replaced by the DAC + servo.
- **Pi Pico module** — replaced by the ESP32-S3 module.

## [2022] — baseline

The original RP2040 reference design, archived under `old/` (`swc_module_pcb.*`
plus a per-block sheet set: `power`, `mcu`, `usb`, `lora`, `sensors`,
`ssr_switches`, `ac_switching`, `light_control`, `door_motor_drive`). Reference
only — not input to the current design. Its failures are the reason this board
exists: digital pot, no auto-ranging, no temperature compensation, module build
rather than a custom PCB.

## Accepted limitations

Deliberate, not defects:

- **The ESP32 antenna overhangs the top board edge by 5.900 mm.** No antenna
  cutout is needed as a result — the keepout lies off-board except for a 0.25 mm
  sliver — but the overhang **must be supported by the enclosure**. A fully
  seated module would need roughly 108 mm of board. Accounting in
  [DESIGN.md §4.12](DESIGN.md#412-the-esp32-antenna).
- **`hole_to_hole` on the `/VBUS` vias**, 0.2408 mm apart — 0.0092 mm under the
  0.25 mm rule. Accepted because nudging one shifts the whole VBUS elbow and
  re-opens a clearance violation elsewhere, which is a worse trade than a 9 µm
  hole-spacing warning on a hand-assembled board.
- **`lib_footprint_mismatch` on `J4`, `U3` and `R60`** — deliberate local edits
  to stock footprints. These are currently undone by any footprint-library
  resync; making them durable is the first open item below.
- **`silk_edge_clearance` on `J4` and `U3`** and **`silk_over_copper` on `BZ1`'s
  "(+)"** — excluded by design, geometrically impossible to clear.

## Open

1. **Project-local footprint library for `J4`, `U3` and `R60`** — makes their
   local edits durable and clears the three mismatch warnings. Blocked: the
   kicad MCP server has no footprint-library tool and nothing writes a
   `.kicad_mod`, so it needs either hand-written files (forbidden) or a new
   script under the `tools/` exception. Detail in
   [DESIGN.md §7](DESIGN.md#7-open-items).
2. **4-layer vs 2-layer at order time** — see
   [MANUFACTURING.md §5](MANUFACTURING.md#5-ordering-decisions).
