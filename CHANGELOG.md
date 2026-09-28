# Changelog

Revision history for the SWC Steering Wheel Controls Adapter.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This is a hardware project, so "version" is the board revision; the project is
not yet ordered and carries no assigned revision letter (`REV ${REVISION}` on
the F.Fab block is still an unresolved text variable).

The **firmware and Android app are versioned separately** from the board, and
their releases are published as GitHub Releases (see "Software releases" below) —
a release is what the device's OTA check (spec §9.5) and the app fetch from
`releases/latest/download/version_manifest.json`, so it is a protocol artifact,
not just a tag.

## Software releases

### [1.0.1] — 2026-09-28

**Fix: "check for updates" now works in production.** The WiFi OTA release fetch
(FR-35) had never succeeded against the real GitHub release host — it was only ever
proven against a Cloudflare tunnel. Three independent, host-dependent reasons, all
found and fixed (defect N-92):

- **The release repository was private.** GitHub serves release assets to
  anonymous clients only on public repos, so the device and the app both received
  HTTP 404 (neither sends a credential). The repository is now **public**.
- **The redirect target's CA was not pinned.** A GitHub release URL 302-redirects
  to `release-assets.githubusercontent.com`, signed by Let's Encrypt (ISRG Root
  X1) — a different CA than `github.com` (Sectigo E46). The firmware pinned only
  the latter, and `esp_http_client` reuses its certificate across the redirect, so
  the fetch died on the redirect hop. `ReleaseCa.h` now pins **both** roots.
- **The HTTP request buffer was too small.** The redirect URL carries a long
  signed query string, so the request first line is ~900 bytes — over IDF's
  512-byte default, which refuses it ("Out of buffer"). The check path set no
  buffer size; it now matches the install path (`buffer_size_tx = 1024`).

**Diagnostics**: `OtaWifiLastError()` now reports *why* a release check failed
(the transport error and HTTP status) instead of the single, uninformative "the
release manifest could not be read" — the ambiguity that hid all three causes.

**Guards** (CI-wired, mutation-tested): `check_release_ca.py` asserts the pin's
count, both subjects and both expiries; new `check_ota_buffer.py` requires every
OTA HTTP config to size its TX buffer.

**Verification**: 612/612 native tests; 71 tools tests; all 11 static gates; device
build 71.9 % of the app slot. **Proven on the DUT** with the production URL
compiled in (no override): `POST /api/ota/check` → *"an update is available: 1.0.0
(1410368 bytes)"*, the exact size of the published asset.

### [1.0.0] — 2026-09-28

The first release with the complete **no-app** feature set: a head unit can be
given single, double and long functions on every steering-wheel button with no
phone attached.

**Firmware** (version reported in `hello` = the release tag):

- **Headless gesture programming — the 2022 Pico flow, restored.** Hold `AUX1`,
  perform the gesture on a learned wheel button (tap = single, two taps = double,
  hold = long), and the adapter **holds that gesture's voltage on the KEY line**
  while the head unit's own "set key function" screen learns it; releasing `AUX1`
  releases the line. Previously the output was suppressed during the hold, so
  there was nothing on the line to program — the "it doesn't work" the user
  reported. (Spec §7.5; defect N-89.)
- **An unbound gesture now presents its own gesture's voltage**, not the button's
  single level for all three gestures. This is what makes three head-unit
  functions per button possible with no app: the head unit is gesture-blind, so
  it can only tell the gestures apart by a different voltage. The levels come from
  a predefined ascending table (`Output/GestureLevels`) mapped as a fraction of
  the output band, so they are correct on both 5 V and 3 V head units. (Spec §6.6
  rule 4; defect N-89.)
- **`key_click_enabled` setting** (default **off**): an optional per-press
  acknowledgement beep, enabling it in the app. Normal switch operation is silent
  by default. An explicit `BUZZ` action and `KEY_UNKNOWN` still play.

**Android app** (`com.oetsolutions.swc` 1.0.0): a **Key click** switch on the
Bindings screen for `key_click_enabled`, matching the firmware setting.

**Documentation**: a new end-user **[MANUAL.md](MANUAL.md)** (wiring, the on-device
learn, programming and binding, the app, firmware updates, feedback reference,
troubleshooting).

**Verification**: 612/612 native tests; the rig driver's 10/10 host tests; the
Android JVM suite; the device build at 71.7 % of the app slot; and every static
gate. The board itself was re-flashed with this image and reports `config_state:
ok` / `output_safe: true`. The "three distinct levels reach the head unit" half
is covered by the host suite only — reading a driven level on the bench needs the
per-channel loopback wire, which is not fitted here (defect N-90).

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
