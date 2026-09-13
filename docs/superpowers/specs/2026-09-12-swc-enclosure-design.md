# SWC Adapter Enclosure — Design Spec

**Date:** 2026-09-12
**Status:** BUILT — model in `plastic_case/`; see §9.2 for the as-built corrections
**Deliverable:** A parametric FDM-printable two-part enclosure (base + screw-on lid) for
the SWC steering-wheel-controls adapter PCB, generated with FreeCAD, sliced with OrcaSlicer.

---

## 1. Goal

House the `SWC.kicad_pcb` assembly in a rugged, printable, serviceable enclosure that:

1. exposes every user-facing interface (USB-C, 4 terminal blocks, buzzer, 2 LEDs, 2 buttons);
2. keeps the ESP32-S3 antenna **inside** the box, in adequate free space;
3. lets the board's own silkscreen do the labelling;
4. vents around RT1 so the NTC senses ambient, not internal, temperature;
5. prints without supports on the user's printer.

## 2. Confirmed inputs (verified, with provenance)

### 2.1 Live board geometry

| Item | Value | Source |
| --- | --- | --- |
| Outline | **x 16.0–70.0, y 31.0–133.0** = **54.0 × 102.0 mm** | `list_pcb_graphic_items` (Edge.Cuts) **and** the STEP substrate solid (54.000 × 102.000) — two independent confirmations |
| Corner radius | 2.0 mm | `tools/holes.py` (matches the Edge.Cuts R2.0 arcs) |
| Board thickness | 1.5162 mm (nominal 1.6) | Edge.Cuts F.Fab "Board Thickness 1.6062" table; STEP substrate 1.5162 |
| Mounting holes | 4 × Ø5.3 through, at **(22,37), (64,37), (22,127), (64,127)** | footprint list (`MountingHole_5.3mm_M5`) |
| Hole inset | **6.00 mm from both nearest edges, all four** | measured against the verified outline |
| Tallest top-side part | 15.6712 mm above board top | STEP (J2–J5 terminal bodies) |
| Deepest bottom-side part | −4.3988 mm below board top | STEP (BZ1 buzzer back can) |

> **Correction to `tools/holes.py`:** its `OUTLINE = (16.0, 31.0, 79.5, 135.5)` is **stale**
> (that is a 63.5 × 104.5 board). The live outline is 54 × 102. The hole inset arithmetic
> therefore does **not** yield 6.0 mm from that constant. Do not reuse it.
> AGENTS.md's "54.00 × 102.00" is correct. (Its "110 footprints / 804 traces / 160 vias"
> is slightly stale — live is 110 / 841 / 163.)

### 2.2 STEP export frame (calibrated)

`mcp__kicad__export_3d` writes a frame rotated 90° about X, with **y negated**:

```
STEP = ( KiCad_x , −KiCad_y , KiCad_z )
```

with **KiCad z = 0 at the board's TOP face**, +z toward the components, −z toward the
back of the board. Validated by recovering the 54 × 102 substrate exactly and by
matching 4/4 mounting-hole insets.

### 2.3 Component geometry that drives the openings (from STEP solids)

| Ref | Part | Board-frame extents (kx, ky, kz) |
| --- | --- | --- |
| U3 | ESP32-S3 module | module PCB **x 37.22–55.22, y 25.25–50.75**, z 1.60–2.60. **Overhangs the board's front edge (y=31) by 5.75 mm.** |
| U3 | antenna keepout zone | **x 38.72–53.72, y 25.25–31.25** (the part off-board) |
| J4 | USB-C receptacle (HRO TYPE-C-31-M-12) | x 15.98–23.88, **y 43.74–52.68**, z 0.75–4.85. Faces **−x (left)**; housing end-face at kx = 15.98 (just proud of the board edge, so the connector body is essentially flush with the board edge). |
| J1 | TB007-508-02, 2-pole | x 16.86–26.86, y 54.11–64.87, z −1.90…15.60. Wire bores at **y 56.566, 61.500**. Pins reach z −1.90. |
| J2 | TB007-508-03, 3-pole | x 16.86–26.86, y 65.79–71.47, z 1.60–15.60. Bores at **y 68.329, 73.409, 78.489**. Pins to z −2.90. |
| J5 | TB007-508-04, 4-pole | x 16.86–26.86, y 82.64–87.72, z 1.60–15.67. Bores at **y 85.18, 90.26, 95.34, 100.42**. Pins to z −2.22. |
| J3 | TB007-508-03, 3-pole | x 16.86–26.86, y 104.58–110.26, z 1.60–15.60. Bores at **y 107.122, 112.202, 117.282**. Pins to z −2.90. |
| BZ1 | buzzer | **True part is TMB12A05 (Ø12 × 9.5), body centred on the pad midpoint (48.082, 116.356)**, z 1.6–11.1 above the board, leads ≈ z −3.9. **The 3D model on the board is a different part — see §2.6.** |
| SW1 | BOOT tact switch | x 56.51–61.51, y 47.83–50.83, z 1.60–3.60 |
| SW2 | RESET tact switch | x 30.89–33.89, y 61.77–66.77, z 1.60–3.60 |
| D6 | LED (STATUS) | x 46.34–47.59, y 60.66–62.66, z 1.60–2.70 |
| D12 | LED (LED2) | x 40.07–42.07, y 53.87–55.12, z 1.60–2.70 |
| RT1 | NTC 10k B3380 | **(68.5, 120.0)**, z 1.60–2.05 — **2.10 mm** from the board's right edge (x=70) |

> **Terminal-block screw axis — CORRECTED during build (see §9.2).** The datasheet puts the
> screw axis **2.60 mm back from the wire-entry face**. The terminal's *effective* wire-entry
> face is the upper setback face at **x = 19.047** (the lower skirt face at 16.858 is not the
> face the datasheet measures from), so `19.047 + 2.611 = 21.658`. Two independent
> confirmations. **The screw axis is therefore x = 21.658 — the pin line at 22.358 is 0.70 mm
> too far back.** (The Ø5 driver slots are placed at x = 21.658.) Screw heads take an M3 blade
> (PH1 or a 3–4 mm flat); torque 0.5 N·m.

### 2.4 Silkscreen available for labelling

All pin/title text is on **B.SilkS** (mirrored, correct when viewed from below). Ranges below
are computed as `justify-point ± (n_chars × 0.84 mm)` along the text's own axis — a ±0.9 mm
estimate per character, adequate for sizing windows. **These are the numbers the floor
windows were sized against.**

| Text | Angle | Computed board extent (x, y) | Window sized |
| --- | --- | --- | --- |
| `GND` / `+12V` (J1 pins) | 0° | x 17.7 ±1.7 / x 17.3 ±2.7, y 56.52 / 61.50 | — (windows at x 20.6–23.6, inside these) |
| `12V DC IN` (J1 title) | 90° | x 21.2–28.8, y 54.4–63.5 | y 53.9–63.9 |
| `GND` / `IN2` / `IN1` (J2) | 0° | x 18.2 ±1.7, y 68.5 / 73.6 / 78.7 | — |
| `SWC INPUT` (J2 title) | 90° | x 21.2–28.8, y 69.7–77.3 | y 68.5–78.5 |
| `GND` / `AUX3` / `AUX2` / `AUX1` (J5) | 0° | x 17.3 ±2.7, y 85.2 / 90.0 / 95.3 / 100.5 | — |
| `AUX INPUT` (J5 title) | 90° | x 21.2–28.8, y 88.7–96.3 | y 87.5–97.5 |
| `GND` / `OUT2` / `OUT1` (J3) | 0° | x 17.3 ±2.7, y 107.0 / 112.0 / 117.0 | — |
| `SWC OUTPUT` (J3 title) | 90° | x 20.8–29.2, y 107.7–115.3 | y 106.5–116.5 |

The **0° pin names** (`GND`, `IN1`, … ) sit at x 17.3–20.8, which falls **under the terminal
bodies** (x 16.86–26.86) and is therefore **not visible through the floor**. The windows are
sized on the **90° title texts**, which sit at x 21.2–29.2 — mostly clear of the body's
inner edge (26.86), so a window at **x 20.6–23.6** shows the first ~3 mm of each title.
Reading the full titles is what the §5.3 chamfer is for.

### 2.5 Manufacturing / printing constraints

| Item | Value | Source |
| --- | --- | --- |
| Printer | **Elegoo Centauri Carbon**, 0.4 mm nozzle | user profile |
| Build volume | **256 × 256 × 256 mm** | `system/Elegoo/machine/ECC/…0.4 nozzle.json` |
| Slicer | **OrcaSlicer 2.4.2**, CLI verified working | `--help` on the installed binary |
| Material | **ASA / ABS** (user's choice) | user |
| Shrinkage | ~0.8–1.2 % linear; Voron guidance = print ASA/ABS at **100 %** and put compensation in the profile | research |
| Hole print error | holes print **~0.25 mm undersize**; add 0.2–0.4 mm to CAD hole Ø | CNC Kitchen / Voron |
| Min wall | 0.8 mm absolute; **use ≥ 2.0 mm** (structural) | research |
| Min slot width | 0.8 mm; keep bridged spans < 10 mm | research |

### 2.6 Buzzer: the board's footprint is the wrong part (verified correction)

BZ1's BOM part is **`TMB12A05`** (`MANUFACTURING.md` and `SWC-bom.csv`: LCSC **C96093**,
Huaneng). But the footprint assigned on the board is
`Buzzer_Beeper:MagneticBuzzer_StarMicronics_HMB-06_HMB-12`, whose 3D model is a **Star
Micronics HMB — Ø16 body, 14.03 mm tall**. So the model is **~4 mm too wide and ~4.5 mm
too tall** for the part that will actually be fitted.

Consequences, applied:

- **Clearance is fine either way.** The wrong (larger) model is the *conservative* envelope:
  15.63 mm above the board vs the lid spigot at 22.0, and its −4.40 mm back can vs the floor
  at −6.0. Designing to the HMB envelope means the real TMB fits with room to spare, so
  nothing in the vertical stack changes.
- **The sound-port location — CORRECTED (see §9.2 item 2).** The hole sits over the **body
  centre**, which is the midpoint of the two pins: **(48.082, 116.356)**. This was verified
  against the footprint itself: its **F.Fab body circle and its F.CrtYd circle are both
  centred at local x = 3.81**, i.e. on the pad midpoint — the courtyard centre and the body
  centre coincide. The earlier figures were the errors: y = 112.551 is the footprint origin
  (pad 1), and 108.75 is pad 1 minus half the pitch. Because both the HMB model and the
  TMB12A05 share the 7.6 mm pin pitch, **the centre is identical for both parts**, so the
  hole position is safe whichever buzzer ends up fitted.
- The TMB port is tiny (2 × Ø0.6) and its datasheet warns against covering or baffling it,
  so the design uses a spread of holes rather than one hole over one vent.

> **Separate board issue, flagged not fixed:** the footprint/3D-model mismatch is a board
> defect, not an enclosure one — JLCPCB will place the TMB12A05 on HMB pads. Fixing it means
> changing `BZ1`'s footprint to `Buzzer_Beeper:Buzzer_12x9.5RM7.6` (also 7.6 mm pitch, so
> electrically drop-in) and re-running `tools/addmodels.py`. **Out of scope here and not
> done** — raising it because it is the kind of thing that is invisible until the part
> arrives.

## 3. Decisions taken (from the design review)

| Decision | Choice | Consequence |
| --- | --- | --- |
| Filament | **ASA / ABS** | tolerances compensated for 0.8–1.2 % shrink; needs the enclosed chamber the Centauri Carbon has |
| Lid fastening | **M5 screws through the board** — screws drop through the lid and the board's existing Ø5.3 mounting holes into threaded bosses in the base | needs ~10 mm of thread engagement **below the board** → a raised floor with 4 deep corner towers (the single biggest change to the internal layout) |
| Board orientation | **board bottom faces the case floor** | the etched pin names (`GND`, `+12V`, `IN1`… `OUT1`) stay readable through floor windows; nothing needed is hidden |
| Buttons | **plain recessed holes** in the lid (no plungers) | simplest, no extra parts |
| Antenna | **inside a closed plastic wall, ≥ 15 mm clearance** | front "nose" lengthened so the module's overhanging antenna end gets Espressif's full 15 mm (see §6) |

### 3.1 Why M5-through-board makes the case deep — and the trade

Thread-forming M5 in ABS/ASA wants **≈ 1.5–2 × D ≈ 8–10 mm of engagement**. The board
sits 1.52 mm below its own top face and there is nothing below it to thread into except
the base floor, which would only give ~3 mm. So the engagement must live in **bosses
reaching down below the floor**, which drives:

- floor top at **z = −6.0** (raised platform, 1.6 mm clear below the buzzer's −4.40),
- 4 **corner towers** from the floor down to **z = −14.4**, each housing a Ø4.2 pilot hole,
- overall height **42.4 mm** instead of ~35 mm.

> **Superseded — see §9.3 and §9.5.** Rev 2 brought the case to **36.40 mm**; rev 4 is
> **31.40 mm**. The engagement above is real, but §3.1's conclusion that it must live *below
> the floor* was wrong: the height is fixed by the screw *and* the lid height together (a
> standard M5 × 25 under a 23.0 mm lid reaches the pilot with 5.4838 mm of engagement), so the
> floor sits at −8.40 and the raised platform, ribs and feet are gone.

The M3 + heat-set-insert route would have been ~7 mm shallower, but the M5 route reuses the
board's existing holes and needs no inserts. **Noted as an accepted consequence, not a
defect.** The bosses are sized Ø11 with a Ø4.2 pilot so that M5 heat-set inserts
(Ø6.4 pocket) can be retrofitted later without changing the CAD.

## 4. Coordinate system & assembly stack

All dimensions are in **board coordinates**: x/y as in KiCad, **z = 0 at the board's top
face**, +z up toward the components. This avoids re-deriving the STEP transform.

| Plane | z | Feature |
| --- | --- | --- |
| 28.00 | lid top face |
| 24.00 | base rim / lid plate joint (the visible seam) |
| 22.00 | spigot bottom (lid); 2.0 mm engagement |
| 15.67 | tallest top-side part (terminal blocks) |
| 0.00 | **board top face** — lid sleeve bottom clamps here |
| −1.52 | **board bottom face** — board rests on the boss tops |
| −4.40 | deepest bottom-side part (buzzer) |
| −6.00 | floor top face (raised platform) |
| −8.40 | floor outer bottom |
| −14.40 | corner tower / foot bottom (lowest point) |

**X/Y envelope**

| | x | y |
| --- | --- | --- |
| board | 16.00 – 70.00 | 31.00 – 133.00 |
| cavity (inner) | 15.40 – 70.60 | **13.25** – 133.60 |
| case (outer) | 13.40 – 72.60 | **11.25** – 135.60 |
| lid flange (outer) | 11.90 – 74.10 | 9.75 – 137.10 |

**Outer size: 59.2 × 124.3 × 42.4 mm** (as built — the nose wall is 5.75 mm proud of the board's front edge to keep the 12 mm antenna gap; §9.2 item 6).
Both parts fit the 256 mm bed with room to spare.

## 5. Opening schedule

### 5.1 Base — left wall (x ≈ 13.4–15.4)

| Opening | Qty | Position | Size |
| --- | --- | --- | --- |
| Wire access, **vertically oval** | 12 | one per terminal pole: y = 56.566, 61.500, 68.329, 73.409, 78.489, 85.18, 90.26, 95.34, 100.42, 107.122, 112.202, 117.282 | **4.0 w × 5.6 h, 1.5 corner radius**, **z 1.4–7.0** (centred on the wire bore) |
| USB-C | 1 | y 42.44–53.98 | **11.54 w × 6.0 h**, z −0.35–5.65; 0.5 mm outer-wall chamfer |
| USB-C seat | — | wall inner face x 15.40, connector face kx 15.98 | 0.58 mm seat depth |

The wire window is **deliberately oval** (4.0 wide × 5.6 tall), **centred on the wire bore,
whose height was measured, not assumed**: probing the J5 3D solid at the wire-entry face
shows the bore is a rectangular tunnel spanning **kz 2.4–8.4, centred at kz ≈ 5.95** (not
the ~12.0 a "top-face exit" guess would give). The window is therefore placed at
**z 1.5–11.0**, which is centred 5.9 mm higher than a naive placement and comfortably
covers the bore with ±3 mm of vertical tolerance. 4.0 mm of width clears a 2.5 mm² wire and
a 3.5 mm blade.

> **Correction (found by re-probing the model):** an earlier draft placed these windows at
> z 3.0–12.0, i.e. **2.4 mm above the bore's actual centre**. The bore sits low, near the
> board, because the clamp screw is above it. The corrected z 1.5–11.0 range is what ships.

Adjacent wire windows are separated by **1.84–2.06 mm of solid wall** — a printable,
bridgeable rib (well under the 10 mm bridged-span limit).

### 5.2 Base — right & left walls, around RT1 (ventilation)

| Opening | Qty | Position | Size |
| --- | --- | --- | --- |
| Vent slots | **6** | 3 on the **+x** long wall beside RT1 at ky 112.0 / 119.0 / 126.0; 1 on the **−x** long wall at ky 126.0; 2 on the **y=133** end wall at kx 34.0 / 52.0 | **3.5 w × 3.2 h, 0.8 corner radius**, **z 0.80–4.00** |

- **Cross-flow:** the design requirement is vents on **both sides** around RT1, because the
  NTC measures *ambient* air and a sensor boxed behind a single-sided opening reads its own
  case temperature. The −x inlet at ky 126.0 and the +x slot at ky 126.0 are an **exactly
  opposed pair** (casegeom asserts this), so air enters the left wall at x 14.6 and crosses
  the board straight to RT1.
- **Right wall** is the sensing face: RT1 sits **2.10 mm** from the inner right wall, so a
  slot there lets the NTC actually see room air, with a short, low-resistance path.
- **Why the −x inlet goes where it does:** that wall is the terminal wall and carries all 12
  wire windows on a 5.08 mm pitch, which leaves gaps of only 1.1–2.8 mm — far too narrow for
  a 3.5 mm slot. The one free band starts behind the last window (which ends at y 119.28);
  at ky 126.0 the inlet clears it by 4.97 mm. The rear corner bosses sit at board y 127 but
  stop at the board's underside, so they do not obstruct the z 0.80–4.00 band.
- **All six slots share the low band (z 0.80–4.00)** so the air crosses the board at the
  height of the parts and of the terminal wire tunnels. Rev 1 put the band up at z 5–14,
  which both missed the sensors and printed straight through the lid's driver slots.
- **None of these openings is in the antenna's radiating path** — they are all in the
  y = 110–132 band, while the antenna's hemisphere is the front nose (y < 31). See §6.
- **Left wall** vents sit in the J3 zone (y 104.58–110.26) / the free strip below it, giving
  the chimney effect an inlet low and an outlet high.
- **None of these openings is in the antenna's radiating path** — they are all in the
  y = 104–132 band, while the antenna's hemisphere is the front nose (y < 31). See §6.

### 5.3 Base — floor (bottom)

| Opening | Qty | Position | Size |
| --- | --- | --- | --- |
| Silkscreen windows | **4** | one per terminal block, centred on that block's own **pole span** | the block's pole span **+ 1.10 mm** either side, same x span for all four, **1.5 corner radius**, **1.0 × 45° chamfer** on the underside so the text also reads at an oblique angle |

The board is mounted **component-side up**, so its `B.SilkS` text faces the case floor and is
read by looking **up** through these windows. Measured off the board's own `B.SilkS` SVG
export (§2.4), not estimated — the x extents are identical for all four blocks because they
are the same part silk-screened the same way:

| Column | x extent |
| --- | --- |
| Pin names (0°, right-aligned: `GND`, `+12V`, `AUX3` …) | **17.63–20.57** |
| Block titles (90°: `12V DC IN`, `SWC INPUT` …) | **24.56–25.37** |

The pin column itself sits between them at x 21.658, so the silkscreen is on **both sides** of
it and one window per block has to span x **16.93–26.07** to show both. (An earlier revision
sized the windows on the titles alone and left every pin name unreadable.)

**The window height is keyed to the block's pole span, not to its silkscreen.** Keying it to
the text is what produced the "different sizes even for the same size terminal blocks": J2
and J3 are the same 3-pole part, but `SWC INPUT` is 6.32 mm tall against `SWC OUTPUT` at
7.39 mm, and their pin-name strings differ too, so a text-derived window came out **0.16 mm
apart for identical hardware**. The clearance constant covers the worst silk overhang on any
block — a 3-pole block's pin name reaches 5.64 mm from centre against a 5.08 mm half-span, so
**1.10 mm** leaves ≥ 0.54 mm of reveal everywhere.

| Block | Poles | Pole span | Window (x, y) | Height |
| --- | --- | --- | --- | --- |
| J1 `12V DC IN` | 2 | 4.934 | 16.93–26.07, 55.47–62.60 | 7.13 |
| J2 `SWC INPUT` | 3 | 10.160 | 16.93–26.07, 67.23–79.59 | 12.36 |
| J5 `AUX INPUT` | 4 | 15.240 | 16.93–26.07, 84.08–101.52 | 17.44 |
| J3 `SWC OUTPUT` | 3 | 10.160 | 16.93–26.07, 106.02–118.38 | 12.36 |

Look up through these and read the board's own B.SilkS names: `GND/+12V` + `12V DC IN`,
`GND/IN2/IN1` + `SWC INPUT`, `GND/AUX3/AUX2/AUX1` + `AUX INPUT`, `GND/OUT2/OUT1` +
`SWC OUTPUT`. No printed text needed on the case.

**All four windows are the same width (9.14 mm) and leave the same ≥ 0.54 mm reveal (0.54–1.03 mm). Their
HEIGHTS differ (7.13, 12.36, 17.44, 12.36 mm) and must.** A 4-pole block's pin column is
10.16 mm longer than a 2-pole block's, so a common height covering J5 would overlap J3 —
those two blocks are only 15.70 mm apart centre to centre. Equal height is therefore
geometrically impossible; equal *reveal* is what the eye reads, and that is what is held.
J2 and J3 are 3-pole, and their heights now agree to the last digit by construction.
(The earlier per-title widths — 6.68, 6.68, 12.32, 9.88 mm for four identical blocks — were an
artifact of a bad estimate for J5's title, not a real difference.)

> **These windows are a printability NON-issue, and the earlier claim that they had to be
> deleted for print quality was wrong.** A window is a through-hole in a 2.4 mm slab with
> the cavity already void above it, so the slicer never has to bridge it at ANY width — it
> prints as an inner perimeter. Verified in the sliced gcode: the base's **layer 1 is
> 982 bottom-surface + 772 brim + 150 inner-wall moves and zero bridges**, and the whole
> print was sliced with `enable_support = 0`. The real constraints are structural, and they
> hold: the slab stays one connected piece — the ribs between consecutive windows are
> **4.63, 4.49 and 4.50 mm** (limit 2.0) and the narrowest strip out to the cavity wall is
> **1.53 mm** on the −x side, where that strip is the cavity wall's own footprint,
> continuous along y with the 2.0 mm standing wall directly above it.

### 5.4 Lid

| Opening | Qty | Position | Size |
| --- | --- | --- | --- |
| Terminal screwdriver access | **4 slots** | one per terminal block, at **x 21.658** (§9.2 item 3), spanning (block bore y range + 2.7 mm each end) | **5.0-wide rounded slot** through, with a 1.0 × 45° lead-in chamfer (§9.2 item 5) |
| LED windows | **2** | D6 (46.97, 61.66), D12 (41.07, 54.49) | **Ø4.0** through + **Ø7.0 × 1.0** recess, 0.8 × 45° chamfer |
| Button access | **2** | SW1 (59.01, 49.33), SW2 (32.39, 64.27) | **Ø5.0** through + **Ø9.0 × 1.5** recess, 0.8 × 45° chamfer. See the depth caveat below. |
| Buzzer sound port | 1 set | BZ1 **body** centre **(48.082, 116.356)** — the midpoint of the 7.6 mm pin pitch (see §2.6, §9.2 item 2) | **Ø4.0** centre + **6 × Ø2.4 on an R4.5 bolt circle**. Every hole stays inside the TMB12A05's **Ø12 body** (max radius 4.5 + 1.2 = 5.7 < 6.0), and the Ø4.0 centre covers the part's tiny 2 × Ø0.6 port. The cluster — not a single hole — exists because the datasheet warns against baffling the port. |
| Lid screws | 4 | on the M5 hole axes | **Ø5.8** through + **Ø9.5 × 5.2 counterbore** |

**No antenna opening.** The front wall over the antenna is solid — this is deliberate (§6).

### 5.5 Lid ↔ base joint

| Feature | Dimension |
| --- | --- |
| Base rim top | z 20.0 |
| Lid spigot | z 18.0–20.0, **2.0 mm** engagement, **0.2 mm** clearance all round (ASA sliding fit), wall a constant 2.0 mm **through the corners too** (the inner profile's corner radius is `CAV_R + 2.0`; an equal radius makes the wall pinch and leaves the inner corner scarred with sliver notches) |
| Lid flange | z 20.0–23.0, **3.0 mm** thick; **flush with the base** (no overhang) |
| Lid sleeve | Ø11.0 OD × Ø5.8 bore, z 0.0–23.0 — **guides each M5 screw and clamps the board onto the boss tops** |
| Base boss | Ø11.0 OD, z −1.52…−8.40, **Ø4.2 blind pilot**, floored at **z −7.60** with 0.8 mm of skin under it, 1.0 × 45° entry chamfer. **The boss top reaches the board's underside (−1.5162) so the board rests on it (§9.2).** The pilot is blind so no screw can poke out of the bottom and the first layer keeps its integrity |
| Fasteners | **4 × M5 × 25 socket-head cap screws**, flush in the Ø9.5 × 5.0 counterbores |
| Height | base **28.40 mm**, lid plate 3.0 mm, assembled **31.40 mm** |

The lid's four **sleeves are load-bearing**: they run from the lid down to the board's top
face, so tightening the screws clamps the board between the sleeve bottoms (z 0) and the
boss tops (z −1.52). Without them the board would merely be trapped, not clamped.

### 5.6 A caveat on the button holes (user's option, with its limit)

The TS-1088 series' actuator is only **Ø1.8 mm and sits 2.0 mm above the board** — i.e.
**22 mm below the lid face**. A Ø5 hole is mechanically generous for the stem but at that
depth it is really a **"poke it with something small"** hole, not a thumb button: with no
plunger there is nothing to bridge the 22 mm gap.

Three ways forward, cheapest first:

1. **Accept it** — a pencil tip or small screwdriver presses BOOT/RESET. These are
   setup-time controls (flash, reset), not everyday buttons, so this is often right.
2. **Let the lid sleeve do the work** — put one of the four M5 sleeves directly over each
   switch so the sleeve bottom reaches down near it. This is **geometrically impossible
   here**: SW1 is at x 59.01 and SW2 at x 32.39, while the M5 axes are at x 22 and 64, and
   all four sit at y 37/127 — none is within 20 mm of a switch.
3. **Add a plunger cap** — a small printed cap retained in the recess, ~20 mm long. Reliable
   and cheap, but it is the option the user declined.

The design as specified is **option 1**, with the recess sized (Ø9.0) so option 3 can be
dropped in later without re-cutting the lid.

## 6. Antenna treatment

**Requirement (user):** the antenna stays inside the box; dead space at that end is fine.

**Constraint (Espressif design guidelines):** "Ensure that the PCB antenna on the base
board also has a sufficiently large clearance area **inside the housing**… A clearance of
at least **15 mm is recommended in all directions**."

Design:

- The module's antenna end already hangs **5.75 mm off the board's front edge**; the module
  PCB itself becomes the ground plane under the antenna, so the off-board portion is the
  real radiator.
- The cavity's inner front wall is placed at **y = 13.25**, giving the antenna's front edge
  (y = 25.25) a **12.0 mm** clear air gap straight ahead, plus **8.0 mm** from the board
  edge (y = 31) and ≥ **7.9 mm** sideways (cavity x 15.40/70.60 vs antenna x 38.72–53.72).
- The front wall over the antenna is **solid** — no vents, no cutout — and **thinned to
  1.4 mm** locally over the antenna's footprint (x 37.7–54.8) instead of the nominal 2.0 mm,
  to hold dielectric loading and pattern ripple down (BYITL: 1.2–1.6 mm over an antenna).
- **No metal** of any kind in the nose: no screws, no inserts, no copper. The nearest
  fastener is 21 mm away at the corner bosses.
- Expected penalty: **≈ 1–2 dB** and a **50–100 MHz** downward resonance shift — inherent to
  any plastic enclosure. Espressif requires end-product range testing; that is the user's
  step, not a CAD step.
- The case nose is solid plastic from z −6.0 to 28 **at y < 31**, so the module's antenna
  has no line of sight to anything metallic.

**Honest limit:** the 15 mm figure is Espressif's "recommended", and the third-party
consensus range is 3–15 mm (5 mm preferred floor). 12.0 mm ahead plus ≥ 7.9 mm sideways
meets the practical consensus and most of Espressif's recommendation; going to a full 15 mm
ahead would add 3 mm to the case for a marginal gain. **Flagged for the user to overrule.**

## 7. Printability

| Check | Result |
| --- | --- |
| Both parts fit the bed | base 59.2 × 124.3 × 38.4, lid 62.2 × 127.4 × 28 — both ≪ 256³ |
| Base print orientation | as-modelled (feet down). First layer = 4 corner feet + the wall footprint; no island is disconnected — each tower Ø14 merges into the perimeter wall at its corner |
| Lid print orientation | as-modelled or flipped; sleeves print as vertical tubes, no overhang |
| Overhangs | all opening edges ≥ 1 mm chamfered; no overhang exceeds 45° |
| Bridges | the whole print is sliced with **`enable_support = 0`** and succeeds. The base's first layer carries **zero** bridges (bottom surface + brim + walls only), so the floor silkscreen windows need no support at any width. The only true bridges are the four M5 counterbore heads — Ø5.8 round spans inside a Ø9.5 recess, which is precisely what bridging is for. The other `Bridge`/`Overhang` moves are inner-wall spans beside the corner bosses and around the spigot corners |
| Min feature | smallest = 0.8 mm corner radii and Ø2.4 buzzer holes — above the 0.8 mm floor |
| Wall thickness | nominal 2.0 mm (5 × 0.4 mm lines); antenna window 1.4 mm (3–4 lines) |
| Elephant foot | 0.6 mm × 45° chamfer on every first-layer edge, incl. all 4 feet and the wall base |
| Supports | **none required** |
| Warping | ASA/ABS in an enclosed chamber; large flat floor is the risk — mitigated by the raised floor (thin) + perimeter walls + towers |

## 8. FreeCAD implementation plan

A parametric script is the source of truth so the whole case can be regenerated when the
board moves. Everything is driven from `casegeom.py`, whose only hard-coded literals are the
**verified** board constants of §2 (with provenance comments).

```
plastic_case/
  casegeom.py        parameters + derived dims + assertion checks
  build_case.py      build_base(), build_lid(), build_assembly()  → Case_Body / Case_Lid
  export.py          STL + STEP export for both parts
  build.sh           the one-command rebuild + verify loop
  SWC.step           the KiCad 3D export (step 1 of the request)
  *.png              the KiCad renders used to verify orientation
```

**FreeCAD modelling order** (per the `freecad-modeling-order` skill): sketch → pad/pocket →
booleans → **fillet/chamfer last**. The board STEP is imported only as a *reference* body
into a separate document for collision checking — it is never fused into the case.

**Verification** (a real gate, not a glance):

1. `casegeom.py` asserts every derived clearance (board-to-wall, antenna gap, headroom,
   boss-to-component, opening-to-peg) and fails loudly if one goes negative.
2. A FreeCAD **interference check** between the case solids and the imported board STEP
   (`spatial_query(operation="interference_check")`) must report zero overlaps.
3. Mesh validation: each exported STL must be watertight/manifold.
4. A bounding-box assertion per part against the §4 envelope.
5. OrcaSlicer CLI slice of both STLs must produce gcode with no error (this is the
   "make sure it is printable" check that actually exercises the toolchain).

## 9. Open items / assumptions to confirm before printing

1. ~~**Terminal screw axis (highest risk).**~~ **RESOLVED, then CORRECTED — see §9.2 item 3.**
   The datasheet's 2.60 mm is measured from the terminal's upper setback wire-entry face
   (x = 19.047), giving a screw axis at **x = 21.658**, not the pin line at 22.358 (which is
   0.70 mm too far back). The **Ø5.0 driver slots** are centred there. Because a single
   5.0-wide slot per block clears the whole screw-cap row, the design is robust
   to ±1 mm of residual error anyway.
2. **USB-C opening height.** 6.0 mm covers the connector's shell plus a plug's
   overmold. If the user's cable has a taller overmold, raise z to 6.5 mm.
3. **Antenna clearance** — 12 mm ahead vs Espressif's 15 mm (§6). One-line parameter change.
4. ~~**Buzzer port location.**~~ **RESOLVED.** The TMB12A05's port is on the **top face**,
   proven two ways: probing the board's model showed solid material from kz 2.6→13.6 with
   the centre open only above, and the TMB datasheet confirms a top-face port. The holes go
   over the **body centre (48.082, 116.356)** (= the pad midpoint; see §9.2 item 2);
   because the HMB model and the TMB share the
   7.6 mm pin pitch, that centre is correct for either part — see §2.6 and §5.4.
   **The board's BZ1 footprint is still the wrong part** (§2.6) — an enclosure-independent
   board defect, flagged but deliberately not fixed.
5. **Screw length.** As built in rev 4: **M5 × 25** SHCS, flush in a 5.0 mm counterbore, with
   **5.4838 mm of thread engagement** below the board (board bottom −1.52 to the tip at −7.00).
   The lid top is set at 23.0 mm precisely to buy that engagement; see §9.5 for why the lid and
   the screw have to be chosen together. Verify against the actual screws before printing.
6. **Button actuation depth** (§5.6) — without plungers the buttons are poke-only. Confirm
   that is acceptable or resurrect option 3.
7. **Terminal block is not J1's `2.4 mm` pad** — `get_footprint_pads` reports J1's pads as
   Ø2.4 with a 5.08 pitch, i.e. the through-hole lands, consistent with the datasheet's
   Ø2.4 pad / Ø1.6 drill. No action; recorded because a "Ø1.6 hole" reading would have been
   mistaken for the wire bore.

## 9.2 Build-time corrections (as-built, 2026-09-12)

Corrections made while building the model against the real STEP geometry. The CAD in
`plastic_case/` reflects these; where the two disagree, the CAD wins.

| # | Item | Spec said | As built | Why |
| --- | --- | --- | --- | --- |
| 1 | STEP z frame | z origin = board top | **z origin = board BOTTOM**, so `bf_z = step_z + 1.5162` | the substrate solid `SWC209` spans STEP Z 0.0000–1.5162 with all components above it |
| 2 | Buzzer body centre | (48.082, 108.75) | **(48.082, 116.356)** = the pad midpoint | the footprint's F.Fab **and** F.CrtYd circles are both centred at local x=3.81, so the courtyard and body centres coincide; §2.6's "108.75" was the wrong reading |
| 3 | Terminal screw axis | x = 22.358 (pin line) | **x = 21.658** | measured from the setback wire-entry face (19.047) + the datasheet's 2.60 mm |
| 4 | **Board support** | board rests on boss tops | **bosses added** — none were drawn | the towers stopped at the floor slab, leaving a **4.48 mm air gap** under every M5 hole: tightening the screw would have pressed the board into air instead of clamping it. Ø14/Ø11 bosses seated at −1.5162 interfere with nothing (verified: 0 mm³ common volume at all four holes; no bottom-side part within 7 mm of any hole) |
| 5 | Driver access | 12 × Ø6.5 holes | **4 × Ø5.0 rounded slots** | on the 5.08 mm pole pitch, driver-sized holes merge within a block and leave a 0.19 mm web between blocks — unprintable. Slots give 1.29–1.43 mm webs |
| 6 | Case length / floor | 120.4 mm; floor top −6.0, bottom −8.4 | **124.3 mm**; floor slab raised to −6.0…−8.4 with 6 rib walls under it | front wall kept at the spec's y=13.25 for the 12 mm antenna gap; the raised floor + ribs cut the warping risk and move the thread engagement below the floor |
| 7 | Lid STL print orientation | — | **flipped 180°** (flange down) | as modelled, the lid's 4 sleeves hang 24 mm below the flange, which would span ~90 mm on four unsupported corners. Flange-down makes the spigot ring + sleeves plain vertical walls — **zero supports, no cleanup** |

### 9.3 Rev 2 corrections (2026-09-12, from a printability review of §9.2)

| # | Item | Rev 1 said | As built (rev 2) | Why |
| --- | --- | --- | --- | --- |
| 8 | **Overall height** | 42.4 mm; foot at −14.4, or −10.0 as an interim | **36.40 mm** — foot at **−8.40** (rev 4: **31.40 mm**, see §9.5) | §3.1 assumed ~8–10 mm of engagement needed bosses *below* the floor. It does not: the height is set by the **screw**, not by engagement. `tip = lid_top − counterbore_depth − screw_len` and `engagement = Z_BOARD_BOT − tip`, so the engagement is fixed once the screw and the lid height are chosen together. A standard M5 × 30 with the lid at 28.0 therefore needs the floor at −8.40 and no lower. Trimming the buzzer pins — which the user allows — frees **nothing**: the screw tip (−7.00) is well below every board part, so the buzzer and terminal pins were never the constraint |
| 9 | **Floor** | raised platform at −6.0…−8.4 with 6 rib walls and 4 floor windows | **flat solid slab**, −8.40…−6.00, no ribs, no windows | the raised floor printed as a field of overhangs across the whole underside; a flat slab's underside is a single rectangle, i.e. the first layer, so nothing overhangs and one chamfer pass gives the elephant-foot relief. The silkscreen windows are gone with it |
| 10 | **Lid fit** | 1.5 mm flange overhang all round | **flush** — flange = case outline | the overhang left a lip that is awkward to print and that the user did not want |
| 11 | **M5 pilots** | drilled from `Z_FOOT − 0.5`, i.e. **through** the floor | **blind**, floored at −7.60 with 0.8 mm of skin under it | a through pilot put four holes in the first layer *and* let the screw poke out of the bottom; blind is what a printed boss wants |
| 12 | **Vents** | 4 tall slots (z 5–14) on both long walls | **3 low slots on the +x wall beside RT1** (z 0.8–4.0) **+ 2 on the y = 133 end wall**; none on the −x wall | the tall band missed the sensors and, over the terminal blocks, printed through the lid's driver slots (a slot hanging in mid-air that cannot print). Low slots put the air at RT1's height and at the wire tunnels; the −x wall is already opened by its 12 wire windows and has no 3.5 mm gap for a vent |
| 13 | **Screw counterbore** | recess depth 6.2 mm, sunk *proud* of the lid | **flush**: depth 5.0 mm = the head height, no raised rim | the raised rim would lift the flipped lid off the bed, so the slicer would bridge the whole 59 × 124 mm plate 2.6 mm up on four small rims. Flush keeps 100 % of the plate on the bed and puts the screw head level with the lid face |
| 14 | **Logo (third body)** | 40.3 × 6.4 mm, scale 2.5× | **`Logo_Inlay`**, a separate `Part::Feature` / STL / STEP, **7.60 × 60.50 × 1.2 mm**, dropped flush into a matching pocket in the lid top | the mark's 14 `fp_poly` outlines self-intersect, so the filled shape is their **even-odd** region (rebuilt with shapely's `make_valid`, verified against a raster of the footprint, i.e. the counter-holes of the O/R/D and the gear are real holes). Native size is 15.5 × 1.95 mm with 0.06–0.36 mm hairlines: a morphological **opening** at a 0.4 mm feature loses 44 % of it, so it is **widened 0.20 mm, applied AFTER the 3.898× scale** so the figure is real millimetres, then extruded as **13 separate islands** (the lighter widening no longer welds the letters into one blob, so every island is emitted, each with its own counters) |
| 15 | **Logo position** | front band of the lid plate, board y 19.25–26.75 | **+x side opposite every connector, board x 56.85–64.95, y 57.51–118.49**, **rotated +90°** so it runs the long way up the lid | every connector and every connector cut-out is on the −x wall, so the opposite side is the only place the mark reads as "away from the wiring". The mark is centred on its **bounding box** (the gear is far more massive than the final `s`, so a centroid placement sits ~1.6 mm off). It sits on bare plate with **3.67 mm** clear of the USB routing hole below it, **3.75 mm** of the rear counterbores above it and **3.06 mm** of the button/LED/buzzer cluster beside it; verified by intersecting the pocket outline with the lid's own top face built with the pocket suppressed — **0.0000 mm² off bare plate**. A real pocket cannot clear the four corner counterbores in x (it starts 1.3 mm inside them), so that pair is expressed as a y-gap instead |

### 9.4 Rev 3 — the mark was mirrored, and the rev-2 "fix" was the mirror (2026-09-12)

Reported from the model twice. First: *"the logo is mirrored and doesn't read correctly."* A
change was made. Then: *"The logo is still mirrored."* The second report was correct, and the
rev-2 change was the cause: it took a map that was already a proper rotation and turned it into
a reflection.

`rings_step()` places the mark with a *single* map from source `(x, y)` to **STEP** `(x, y)`:

```
board_x = cx - (y - my)
board_y = (x - mx) - cy          <- the trailing `- cy` IS the board -> STEP y negation
```

```
J = [[dX/dx, dX/dy], [dY/dx, dY/dy]] = [[0, -1], [+1, 0]]      det = 0*0 - (-1)(+1) = +1
```

**det = +1, a proper rotation.** That is the correct form, and it is what ships.

The rev-2 revision changed `cx -` to `cx +` on the stated grounds that the `-` form gave det = −1.
That derivation wrote the Jacobian's second row as `[-1, 0]`, but the map's second component is
`+(x - mx) - cy`, so `dY/dx = +1`. With the true second row the determinant is
`0*0 - (+1)(+1) = -1` for the `cx +` form — **the sign change introduced the mirror it claimed to
fix.** Because the product of the diagonal is zero, the determinant here is just minus the sign
on the `y - my` term, which makes it very easy to get backwards on paper.

Two independent checks confirm the restored form, both with a wide margin:

- **Algebraic.** The map sends `∂/∂x -> (0, +1)` and `∂/∂y -> (s, 0)` where `s = -1`. Reading
  direction × letter-up `= -s`, which must be `+1` for readable text, so `s = -1`.
- **Image fit.** Rasterise the placed mark and fit it against all eight transforms of the source
  (four proper rotations, four reflections). With `cx -`: **proper 1.000, mirror 0.309. With
  `cx +`: proper 0.309, mirror 0.992.** Rendered, the restored mark reads `ⓄETSolutions` with the
  gear leading — identical to the source, support IoU 0.888.

**Why the rev-2 guards did not catch it.** They rasterised both marks to a 64 × 64 grid and
scored overlap. At that resolution the logotype's strokes are one to two pixels wide, so the real
det = −1 difference showed up as a delta of roughly 0.01 that flips sign with the sampling grid —
the check passed on a mirrored model. Worse, the harness shared `rings_step`'s own y convention,
so a consistent frame error cancelled inside the test exactly as it appeared in the geometry. **A
within-frame checker cannot adjudicate a handedness question.**

**The guard now shipped is exact.** `casegeom._checks()` probes the placement map itself — three
source points forming a right angle, pushed through `logo._place()` and cross-producted — and
asserts the signed area is positive. It reads **+1.000** for the correct form and **−1.000** for
the mirrored one, so it cannot wash out. The placement map is now the single function
`logo._place`, called by `rings_step` and probed by the guard; a hand-copied map is how this check
silently became a no-op once before.

**Do not** "simplify" this into a bounding-box, centroid or signed-area test. The first two are
blind to chirality by construction, and the third is too: shapely normalises ring orientation on
construction, so the signed area of a valid polygon is always positive however the mark was
mirrored. An overlap raster is the same trap — see above.

No dimension changed at any point: the mark is still **7.60 × 60.50 × 1.2 mm** in the same pocket
at the same place, still 13 islands, still **0.0000 mm² off bare plate**. Only the handedness of
the letterforms changed. Note this is *not* "flip it so it reads the other way" — negating either
coefficient is a reflection and would re-introduce the bug; turning the finished part 180° is the
only legitimate way to change the reading direction.

**Also fixed in this pass:** `build.sh check` had been a **silent no-op**. `freecadcmd` never sets
`__name__ == "__main__"`, so the guard at the bottom of each module was dead code and the target
exited 0 having run nothing. `build.sh` now writes a tiny driver that imports the module and calls
its `main()` explicitly, and `all` runs the check first instead of skipping it.


### 9.5 Rev 4 — the headroom above the terminals, and the 30 mm screw that caused it (2026-09-12)

**What was wrong.** The box was 6.329 mm taller than it needed to be. Between the terminal
bodies (top at `Z_TALLEST` = 15.671) and the lid spigot underside (22.000) sat 6.329 mm of dead
air, while the terminals need ~2 mm. Worse, that dead air was **my own doing and I had then
presented it back to the user as their constraint**: the 30 mm screw length was my choice, and
`Z_LID_TOP` had been set from it rather than the reverse.

**Why the naive fix does not work.** Just shortening the screw does not shorten the box. The
screw tip is pinned by the blind pilot bore, so

```
tip        = Z_LID_TOP − CBORE_DEPTH − SCREW_LEN
engagement = Z_BOARD_BOT − tip
```

For a fixed lid height, dropping `SCREW_LEN` *reduces engagement* and the case keeps its
height. Both have to move together: the screw shortens **and** the lid top comes down with it.
Once `Z_LID_TOP` drops, `Z_RIM` and `Z_SPIGOT` follow, and the headroom finally shrinks. That
is why the change is a **5 mm** drop and not a 5 mm screw swap.

**The two real constraints.**

1. `FLANGE_T + SPIGOT_ENGAGE ≥ CBORE_DEPTH = 5.0`. The flush M5 head is 5 mm tall and has to
   be buried in the lid, so the plate plus the spigot lip cannot be thinner than the head is
   tall. This is what bottoms the headroom out at `7.329 − 5.0 = 2.329 mm`.
2. `engagement ≥ 5.0`. With a 25 mm screw that caps `Z_LID_TOP` at 23.48; **23.0** leaves
   **5.4838 mm** — bit-for-bit the same thread depth the 30 mm screw gave.

**The answer.** **M5 × 25**, with the lid split as a **3.0 mm plate + 2.0 mm spigot lip**
(pinning constraint 1). `Z_LID_TOP` 28.0 → **23.0**, `Z_RIM` 24.0 → **20.0**, `Z_SPIGOT`
22.0 → **18.0**. **M5 × 20 is the next standard size down and is not usable**: it would put
`Z_SPIGOT` at 13.0, which is **2.67 mm below the terminal bodies**. So 25 mm is the shortest
standard screw that works — the earlier "M5 × 30 is the goal" framing was simply wrong.

**What did not move.** The screw tip stays at **−7.00** (the lid came down exactly as much as
the screw shortened), so `Z_FOOT` stays −8.40, the pilot floor −7.60, the floor skin 0.8 mm and
the tip clearance 0.6 mm. The whole base is byte-identical; only the lid and the four M5 screws
changed. Verified `31.40 mm` assembled, `2.329 mm` headroom, 153/153 checks passing.

**A second, unrelated defect found in the same pass.** The OrcaSlicer target had been slicing
**ASA as PLA**. `--load-filaments` does not follow `inherits`, and the Elegoo ASA preset
inherits `Elegoo ASA @base` → `fdm_filament_asa`; with nothing to resolve against, Orca filled
every unset key from its **default (PLA)** definition and produced `; filament_type = PLA` with
a 35 °C bed and the PLA chamber-fan branch in the start gcode. `build.sh` now flattens the three
profile files itself before slicing and **fails loudly** if the header does not come back as
ASA. Resolved: `270 °C` nozzle, `90 °C` bed.


## 10. Accepted limitations

- Overall height **31.40 mm** — down from 36.40 mm; the lid top dropped 5 mm, the base is
  unchanged. The base is 16.4 mm of it and the lid 23.0 − 8.4 = 14.6 mm. **This is the minimum**
  for the M5-through-board fastening choice: the headroom over the terminals is now **2.329 mm**,
  and it cannot shrink further without the flush M5 head standing proud of the lid (see §9.4).
  The screw is a standard **M5 × 25** (DIN 912); M5 × 20 is the next size down and would put the
  spigot 2.67 mm *below* the terminal bodies.

  The height is set by the **screw**, not by the board's bottom-side parts. Getting to 31.40 mm
  needed both halves to move: shortening the screw alone is not enough, because a shorter screw
  under an unmoved lid just reduces thread engagement. `tip = lid_top − CBORE_DEPTH − SCREW_LEN`
  and `engagement = Z_BOARD_BOT − tip`, so for a given screw the engagement is fixed by where the
  lid top sits. Dropping the lid top 5 mm and the screw 5 mm keeps the tip at **−7.00** and the
  engagement at **5.4838 mm** — the base, pilot bore and floor skin are all unchanged.
- **The buzzer's two pins must be trimmed flush** (they extend to −5.915 mm, i.e. 5.9 mm below the
  board, while the buzzer body sits entirely on the top side). Trimming them to ≈ −5.0 mm leaves
  1.0 mm above the cavity floor. The terminal-block pins are **not** trimmed and reach −4.415 mm,
  which the floor clears by 1.585 mm.
- The antenna is dielectric-loaded and detuned by ~50–100 MHz; throughput should be verified
  in the finished enclosure.
- ASA/ABS tolerance compensation lives in the slicer profile, so the CAD numbers assume the
  profile applies ~0.25 mm of hole compensation. Printing in PLA without changing the
  profile will make every hole slightly tight.
