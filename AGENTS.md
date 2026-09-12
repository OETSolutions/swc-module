# AGENTS.md — working in this repository

Agent-facing instructions for the **SWC Steering Wheel Controls Adapter**, a
KiCad 10 PCB project. Written for any coding agent (Claude Code, Codex, …).
`CLAUDE.md` is a one-line pointer to this file — **this is the source of truth
for how to work here**; keep it current rather than duplicating it elsewhere.

Read order for a new session: **`README.md`** (what the thing is) →
**`DESIGN.md`** (the engineering description) → **this file** (how to work on
it) → **`tools/README.md`** (the direct-write scripts, in detail).

## Documentation map

Each document owns one thing. Short orientation summaries are fine to repeat —
this file must be usable without loading the others — but **quantitative facts
(board numbers, part counts, violation lists) and revision history get exactly
one home.** Reference, never copy.

| Document | Owns | Audience |
| --- | --- | --- |
| `README.md` | What the project is; front page, repository guide, credits | Anyone arriving |
| `DESIGN.md` | Requirements, block diagram, theory of operation, design rules, key design decisions | Anyone reading the circuit |
| `MANUFACTURING.md` | BOM generation, part list, assembly cost, fabrication output, ordering decisions | Anyone ordering |
| `CHANGELOG.md` | Revision history (Keep a Changelog), accepted limitations, open items | Anyone asking "what changed" |
| `AGENTS.md` (this file) | How to work on the files: rules, toolchain, verify loop, gotchas, current state | Coding agents |
| `CLAUDE.md` | Nothing — a pointer to this file | Claude Code |
| `tools/README.md` | The direct-write scripts in detail | Agents using `tools/` |

**Live board numbers belong in this file's "Current state" section.** The README
carries a one-line headline only — no breakdown, no totals. Never restate the
warning list, footprint/part totals, or DRC counts in the prose documents: that
is exactly how the stale "19 DRC violations" claim survived.

## What this project is

A programmable adapter that sits between an aftermarket Android head unit and a
vehicle's factory steering-wheel buttons. The buttons are an analog resistor
ladder; the head unit expects an analog "KEY" input. The adapter reads the
ladder, interprets button presses (single / double / long-press), and drives a
DAC-controlled current sink to present the head unit with the key resistance it
expects — so buttons can be re-mapped, combined, or given long-press actions the
factory wiring never had.

Two identical channels: **SWC1** and **SWC2**.

It replaces a 2022 RP2040 reference design archived under `old/` — reference
only, not input. That design's failures are the reason this board exists: a
digital pot that was not precise enough, no auto-ranging to the head unit's
sensed voltage, no temperature compensation, and a Pi Pico module build rather
than a custom PCB. [DESIGN.md §2](DESIGN.md#2-prior-design).

## Architecture (as built)

```text
  SWC ladder ×2 ─► J2 ─ series R ─► BAT54S clamp ─► ESP32-S3 ADC
        └─► translate ─► MCP4728 DAC ─► U6 integrator servo ─► sink FET ─► head-unit KEY
                                        └── sense buffer ──► ÷2 divider ──► SENSEn

  12 V ─► F1 PPTC ─► D1 SMBJ18A TVS ─► U1 XL1509-5.0 buck (L1 47µH, D7 SS34) ─┐
  USB VBUS ─► F2 PPTC ──────────────────────────────► D3 SS34 ─┐              │
                                            /+5V_BUCK ─► D2 SS34 ─┴─► +5 V ─► U2 AMS1117 ─► +3V3
  ESP32-S3 native USB-CDC (J4) + USB-JTAG over U8 USBLC6-2SC6, R11/R12 22 Ω series
  NTC RT1 on TEMP_ADC · buzzer BZ1 via Q3 · LEDs D6 / D12 · 3 AUX inputs on J5
```

| Block | Parts | Notes |
| --- | --- | --- |
| MCU | `U3` ESP32-S3-WROOM-1-N4 (C2913197) | native USB device/CDC; **no on-chip DAC** (S3 has none); ADC ceiling 2.9 V |
| DAC | `U4` MCP4728 (C478093) | 12-bit quad, **on +3V3 with VREF = VDD → 0–3.3 V FS** (not the 4.096 V internal-ref mode), EEPROM power-on. A/B = ch 1 signal/gain; C/D = ch 2 signal/gain |
| I²C | `R5`/`R6` 10 kΩ pull-ups | DAC and S3 are both 3.3 V → **no level shifter** |
| Analog out | `U6` TLV9004IPWR quad (C2058050) | **closed-loop integrator servo** (R46 100k + C24 100nF), not an open-loop buffer; gain = 1 + R58/R61 |
| USB ESD | `U8` USBLC6-2SC6 (C7519) | flow-through: 1↔6 and 3↔4 are the same channel; VCC on /VBUS |
| USB series R | `R11`/`R12` 22 Ω (C17561) | **fitted** — connector → clamp → series R → IC ordering |
| Power | `U1` XL1509-5.0, `U2` AMS1117-3.3, `D1` SMBJ18A, `D2`/`D3`/`D7` SS34, `F1`/`F2` PPTC, `L1` 47 µH | +12 V is **optional**; D2/D3 diode-OR at +5 V |
| Sense | `R36`/`R43` 1M → `U6B`/`U6D` follower → `R54`/`R50` divider → `SENSEn` | divider is an exact ÷2 → ≤2.49 V, under the S3's 2.9 V ceiling |
| Inputs | `R1`/`R2` 10k series + `R15`/`R16` 10k pull-up; AUX `R23`–`R25` 1k + `R17`–`R19` 10k | all clamped by BAT54S to +3V3/GND, 100 nF filtered |
| Thermal | `RT1` NTC (C316397) | on the exposed right edge at (68.5, 120.0), away from the buck/LDO/ESP32 |
| Interfaces | `J1` 12 V 2P · `J2` SWC_IN 3P · `J3` SWC_OUT 3P · `J5` AUX_IN 4P · `J4` USB-C | J1/J2/J3/J5 each carry GND; screw terminals on the left edge, USB-C top-left |

Output stage per channel: `U6x` is an **integrator** whose DC solution is
`V_KEY = (1 + R58/R61)·V_DAC − (R58/R61)·V_ADJ`, so the **gain is set by the
resistor ratio** (82 k/100 k → 1.812 to drive a 5 V head unit) and `V_ADJ` only
shifts the offset. `V_ADJ` is the *gain-mode* DAC channel — powered down to its
defined 1 kΩ on a 5 V head unit (gain 1.812), or tracking the signal channel on a
3 V head unit (gain 1.00). The MCP4728 has no Hi-Z state; that 1 kΩ is the point.

Board facts (regenerate, do not hand-maintain):
`mcp__kicad__get_board_info` → **110 footprints, 80 nets, 804 traces, 160 vias,
5 zones, 1.6 mm**. Outline: **54.00 × 102.00 mm**, 4-layer
(F.Cu / In1.Cu / In2.Cu / B.Cu), rounded corners r ≈ 2.828 mm.

## Repository layout

| Path | Contents |
| --- | --- |
| `SWC.kicad_sch` · `SWC.kicad_pcb` · `SWC.kicad_pro` · `SWC.kicad_sym` · `fp-lib-table` · `sym-lib-table` | **The live project. Edit only these.** |
| `3dmodels/` | Vendor STEP models, referenced as `${KIPRJMOD}/3dmodels/…`. **Required.** |
| `footprints/logo.pretty/` | Local `LOGO` footprint. **Required.** |
| `tools/` | Sanctioned direct-write / inspection scripts — see `tools/README.md` |
| `SWC-backups/`, `SWC2-backups/` | Timestamped project zips — the restore path |
| `attic/` | Retired root files, with `MANIFEST.txt` |
| `old/` | Prior revisions: the 2022 RP2040 design (`swc_module_pcb.*` plus a per-block sheet set — `power`, `mcu`, `usb`, `lora`, `sensors`, `ssr_switches`, `ac_switching`, `light_control`, `door_motor_drive`), netlist experiments, prior BOMs |
| `build/` | Review notes and netlist baselines from the rework |
| `_drcref/`, `_mcp-bug-evidence/`, `SWC2_autorange_output/` | Reference boards and MCP bug reproductions |
| `SWC.kicad_prl` | Per-user local settings — regenerated by KiCad, not meaningful |

`SWC.kicad_prl`, `*-backups`, `attic/`, `old/`, `build/`, `_drcref/`,
`_mcp-bug-evidence/`, `SWC2_autorange_output/`, `*.csv`, `*.net`, `*.dsn`,
`*.ses` are git-ignored (see `.gitignore`).

## Non-negotiable rules

1. **Never hand-edit KiCad files** — no reading-and-patching `SWC.kicad_sch`,
   `SWC.kicad_pcb`, `SWC.kicad_sym`, `SWC.kicad_mod`, or `SWC.kicad_pro` in a
   text editor, and never run `kicad-cli` directly. Every change goes through
   the **kicad MCP server**.
2. **Never create a second copy of a live file** — `SWC copy.kicad_pcb` was
   exactly that mistake and it caused a real layout error. Use `SWC-backups/`
   for snapshots.
3. **Re-run the tool scripts after any footprint-library resync.** A resync
   silently reverts 3D-model paths and reference-designator silk positions:
   `tools/addmodels.py` then `tools/pcbfields.py`.
4. **The scripts in `tools/` are the one sanctioned exception** to rule 1, only
   for writes the MCP server cannot express (3D-model offsets, refdes silk,
   `fp_text` moves, via deletion, pad angles, field text size). Prefer an MCP
   tool whenever one exists. Details and the running recipe: `tools/README.md`.
5. **Export/DRC artifacts land in the root by default** — move them to `attic/`
   or `/tmp` rather than letting them accumulate.
6. **Professional schematic and layout; clean ERC and DRC.**

## MCP servers

Configured in `.mcp.json` (Claude Code) and `.codex/config.toml` (Codex) with
identical contents. Both take absolute paths for every parameter.

| Server | Endpoint | Use for |
| --- | --- | --- |
| **kicad** | `uvx --from mcp-server-kicad mcp-server-kicad` — <https://github.com/ProductOfAmerica/mcp-server-kicad> | All schematic/PCB reads and writes, ERC/DRC, BOM/netlist/gerber/Gerber exports, 3D export |
| **pcbparts** | `https://pcbparts.dev/mcp` — <https://github.com/Averyy/pcbparts-mcp> | Component search & stock (`jlc_search`, `jlc_stock_check`), cross-reference (`mouser_get_part`, `digikey_get_part`), `board_search`/`board_get` reference designs, `get_design_rules` |

The kicad server is configured with this project as its default schematic,
board, symbol library, and export directory, so its tools work with no path
argument. Its entry points: `list_schematic_components`,
`place_component`, `wire_pins_to_net`, `run_erc` · `list_pcb_footprints`,
`place_footprint`, `move_footprint`, `add_trace`, `add_via`, `autoroute_pcb`,
`run_drc`, `update_pcb_from_schematic` · `export_gerbers`, `export_bom`,
`export_netlist`. There are ~109 tools; search by intent if the entry point
above is not the one you want.

## Skills

This project was built with the **KiStack** skill set by American Embedded:
<https://github.com/American-Embedded/kistack> (installed globally via
`npx skills add American-Embedded/kistack`). The skills it provides and what
each covers here:

| Skill (installed name) | Use for |
| --- | --- |
| `schematic` (`kicad-schematic`) | Wiring up / modifying the schematic from parts and datasheets |
| `pcb` (`kicad-pcb`) | Placement and layout review, DRC, 3D interference |
| `bom` (`kicad-bom`) | Part resolution, package/rating/availability, BOM fields — pairs with the **pcbparts** MCP |
| `export` (`kicad-export`) | Deterministic ERC/DRC/fab/assembly exports; JLCPCB position-file and BOM formats |
| `gerbers` (`kicad-gerbers`) | Gerber generation and visual (pygerber) review |
| `footprint` / `symbol` | Generating compliant footprints/symbols from the official KiCad library tools |
| `panelize` | KiKit panelization |
| `pcb-product-render` | Blender STEP/GLB product renders |

Also installed and used here: `jlcpcb-component-finder` and
`jlcpcb-bom-generate-from-kicad` for the JLCPCB part/LCSC workflow.

## Build / verify loop

There is no compiler. "Build" means *regenerate and check*:

| Action | Command |
| --- | --- |
| Board health summary | `mcp__kicad__get_board_info` |
| Design-rule check | `mcp__kicad__run_drc` |
| Electrical-rule check | `mcp__kicad__run_erc` |
| Schematic ↔ PCB parity | `mcp__kicad__update_pcb_from_schematic` (dry inspection) / the DRC `schematic_parity` field |
| Netlist | `mcp__kicad__export_netlist` |
| Re-place all silk refdes | `PYTHONPATH=<uv-archive> python3 tools/pcbfields.py SWC.kicad_pcb` |
| Re-attach all 3D models | `PYTHONPATH=<uv-archive> python3 tools/addmodels.py SWC.kicad_pcb` |
| Silk audit (read-only) | `python3 tools/silk.py SWC.kicad_pcb` |
| Board geometry (read-only) | `python3 tools/boardgeo.py SWC.kicad_pcb` |

The `PYTHONPATH` is the kicad MCP server's uv archive (its s-expression parser);
`tools/README.md` gives the current path. The writers self-locate their imports
and are idempotent.

## Geometry & tooling gotchas

These have each cost a full round of rework at least once. Do not re-derive
them — take the constants from the named files.

- **The footprint-local → board transform** has a **positive `sin`** term:
  `bx = x + lx·cos t + ly·sin t`, `by = y − lx·sin t + ly·cos t`. A version with
  the sign flipped was live in `silksearch.py` and mirrored every obstacle of a
  ±90° footprint; only parts at 0/180 came out right, which is why it survived.
  `silk.rot_pt`, `silksearch.rot_pt` and `boardgeo.rot_pt` are all this function
  — import it, don't write a fourth copy.
- **A footprint field's drawn angle is the stored angle alone** — not
  `footprint + stored`. `silk.drawn_angle` is the definition.
- **KiCad applies a STEP model's internal placement.** J4's 3D offset cannot be
  derived from the STEP's `CARTESIAN_POINT`s. The correct model is
  `rotate (0 0 180)` / `offset (0 −1.07 0)`; both earlier notes were wrong.
- **`courtyards_overlap` is a real DRC error** even when only silk is involved,
  and the `silk_over_copper` on C1/C2's can outlines and BZ1's "(+)" pad are
  geometrically impossible to clear — see `tools/README.md` before "fixing"
  either.
- **Sizing a board trim off a pad *centre* overstates the margin by half the
  pad.** Measure copper **edges**.
- **Don't trust prose about the analog topology — read the netlist.** Several
  confident notes in this project's history were wrong: `U4` runs on **+3V3**
  (VREF = VDD, 0–3.3 V FS), so there is **no I²C level shifter**; the op-amp is
  an **integrator servo**, not an open-loop buffer; and there are **five**
  BAT54S positions (`D4`/`D5`/`D8`/`D9`/`D10`), not three. `mcp__kicad__export_netlist`
  and parse the nodes; do not infer from names. In particular the `*_3V3`
  suffixed net names do **not** mean an isolated 3.3 V island — they are the
  module's own GPIO nets.
- **A library resync wipes 3D-model paths and refdes offsets** — see rule 3.

When a geometric claim is in doubt, the tie-breakers, in order, are: the DRC
message text, `mcp__kicad__export_pcb` to SVG, then `get_footprint_bounds`.
Not arithmetic, and not a tool's own summary of itself.

## Current state — 2026-09-11

**This is the authoritative place for live board numbers.** Cite from here; if a
prose document disagrees, this file is newer.

- **DRC: 0 errors, 0 unconnected, 0 schematic/PCB parity errors.** 8 warnings
  are reported: 3 active `lib_footprint_mismatch` (J4, U3, R60 — deliberate
  local edits) and 5 marked *excluded* by design (4 `silk_edge_clearance` on
  J4/U3 where the part must cross the board edge, and 1 `silk_over_copper` on
  BZ1's "(+)").
- **ERC: 3 warnings**, all `unconnected_wire_endpoint`, all ~0.03 mm stubs
  inside KiCad's own library symbols at ~1–3 mm coordinates. Pre-existing.
- The USB D+/D− pair is via-free on F.Cu; `/VBUS` and `/VBUS_FUSED` are 0.50 mm.

The root `SWC-drc.json` and `SWC-erc.json` are **current** — both report on
`SWC.kicad_pcb`, and the DRC figures above come from them. Re-run `run_drc`
rather than quoting a saved report; the superseded `SWC2.kicad_pcb` DRC output
now lives in git-ignored `attic/`.

Revision history and the accepted-by-choice limitations are in
**[CHANGELOG.md](CHANGELOG.md)**.

## Manufacturing

**JLCPCB**, 4-layer, 5-off, **targeting Economic PCBA**; 34 unique LCSC part
types, 96 assembled packages. Full detail — BOM regeneration, part list, the
PCBA-type and assembly-fee analysis, the ordering decisions — is in
**[MANUFACTURING.md](MANUFACTURING.md)**. Do not restate it here.

Three things agents need to know before touching the order:

- **Both `Standard Only` parts have been cleared, so Economic is reachable.**
  JLCPCB decides Economic-vs-Standard assembly by a part's **`PCBA Type`** field
  (`Economic and Standard` vs `Standard Only`), a **separate axis from
  Basic/Extended** that is **not** visible to the `pcbparts` MCP; one
  `Standard Only` part forces the whole order to Standard. `C1`/`C2` moved to
  C46550415 and `U3` to the DOIT `ESPS3-32-N4` (C49164655), a verified
  pin- and land-pattern-identical WROOM-1 clone — **check the part's own JLCPCB
  page, not the tier tables**, and re-confirm before every order. See
  `MANUFACTURING.md` §3.
- The outline is **54 × 102 mm**, above JLCPCB's ≤100 × 100 mm promotional tier,
  so it is area-priced. The `swc-jlcpcb-4layer-size-threshold` memory has the
  exact trim arithmetic if that is ever revisited; **do not trim unprompted**.
- The ~$43 Extended loading line cannot be designed away by substitution — see
  `MANUFACTURING.md`. Do not re-open that analysis.

## Data provenance

`SWC.kicad_pro` carries no title block and the board has no git remote or
LICENSE; `REV ${REVISION}` on the F.Fab block is an unresolved text variable.
The F.Fab block and the `SWC INPUT` / `SWC OUTPUT` silk labels are the only
identity markings on the board.
