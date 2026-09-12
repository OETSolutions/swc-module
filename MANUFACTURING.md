# Manufacturing — SWC Steering Wheel Controls Adapter

Sourcing, BOM generation, assembly cost and fabrication output. For the circuit
itself see [DESIGN.md](DESIGN.md); for the front page see [README.md](README.md).

Target: **JLCPCB, 4-layer, 5-off, Economic PCBA.**

- [1. BOM generation](#1-bom-generation)
- [2. Part list](#2-part-list)
- [3. Assembly cost](#3-assembly-cost)
- [4. Fabrication output](#4-fabrication-output)
- [5. Ordering decisions](#5-ordering-decisions)

## 1. BOM generation

The BOM is built by **parsing the schematic's own symbol properties**, because
`export_bom` emits only `Refs, Value, Footprint, Qty, DNP` — no LCSC or MPN.
Every one of the 101 placed symbols with a purchased part carries `LCSC`, `MPN`,
`Manufacturer` and `Datasheet`; the 8 test points carry none by design.

That yields **34 unique LCSC types across 96 packages**. Plus 4 mounting holes
and the copper logo gives the board's 104 footprints, which is why DRC reports
no parity error.

Two wrinkles worth knowing before regenerating it:

- `U6` is one quad TSSOP-14 package driven by a five-unit symbol, so its
  designator list must be deduped to `U6`, not `U6,U6,U6,U6,U6`.
- `J2`/`J3` and `SW1`/`SW2` share an LCSC code, so their BOM `Comment` falls back
  to the MPN — their schematic values are net names, not part names.

## 2. Part list

Extended-tier parts, most expensive first:

| Ref | Value | LCSC | Mfr | $/1 |
| --- | --- | --- | --- | --- |
| U3 | ESP32-S3-WROOM-1-N4 | C2913197 | Espressif | 4.1136 |
| U4 | MCP4728T-E/UN | C478093 | Microchip | 2.6141 |
| J5 | AUX_IN, 4P | C42377749 | Kangnex | 0.3250 |
| U6 | TLV9004IPWR (quad, 1 pkg) | C2058050 | TI | 0.2413 |
| J2, J3 | SWC_IN / SWC_OUT, 3P | C72334 | Kangnex | 0.2143 |
| BZ1 | TMB12A05 | C96093 | Huaneng | 0.1919 |
| L1 | 47 µH | C408471 | Sunlord | 0.1939 |
| J4 | USB-C 2.0 16P | C165948 | Korean Hroparts | 0.1856 |
| U8 | USBLC6-2SC6 | C7519 | ST | 0.1825 |
| J1 | 12V_DC_IN, 2P | C8465 | Kangnex | 0.1336 |
| F1 | 1.5 A, 1812 | C883154 | BHFUSE | 0.0792 |
| C1, C2 | 100 µF/50 V | C3151829 | ROQANG | 0.0631 |
| F2 | 1.0 A, 0805 | C46640991 | hongjiacheng | 0.0429 |
| RT1 | 10 k B3380 NTC | C316397 | Sunlord | 0.0179 |

Three *other* BOM parts are Preferred Extended and therefore already exempt on
Economic: C17840 (82 k), C19077573 (SMBJ18A) and C7420333 (BAT54S, five
placements). Seventeen more codes are Basic.

## 3. Assembly cost

The charge from JLCPCB is a **feeders-loading fee per unique Extended part
type** on Economic PCBA, **$3.07 each** as of its 2026-09-09 pricing table.
Basic parts are free, and Preferred Extended parts are exempt — but **only on
Economic**. Standard PCBA charges $1.53 on *every* part type including Basic, so
Economic is both cheaper per type and cheaper in setup ($8.18 vs $25.56) and
stencil ($1.53 vs $8.21).

This board has **14 Extended part types**, so the loading line is
**14 × $3.07 ≈ $43**.

The brief asked for Extended parts only when there is no alternative, so this was
tested rather than assumed. **It cannot be designed away by substitution**, and
the earlier assessment that it could was wrong on both count and premise.
Thirteen of the fourteen sit in subcategories that contain **no no-fee part at
all**:

| Subcategory | Basic | Preferred | Extended |
| --- | --- | --- | --- |
| Screw Terminal Blocks | 0 | 0 | 1604 |
| USB Connectors | 0 | 0 | 6075 |
| NTC Thermistors | 0 | 0 | 1647 |
| Aluminum Electrolytic SMD | 0 | 0 | 5977 |
| Power Inductors | 0 | 0 | 863 |
| Resettable Fuses | 0 | 0 | 665 |
| Buzzers | 0 | 0 | 36 |
| WiFi Modules | 0 | 0 | 232 |
| DACs | 0 | 0 | 842 |
| TVS/ESD, SOT-23-6 | 0 | 0 | 24 |
| **Operational Amplifier** | **3** | **3** | 5153 |

The op-amp is the only category with no-fee stock, and the substitution still
fails: of the six no-fee op-amps, LM358 / LM2904 / NE5532 / MCP6002 are **dual**
(`U6` is one quad TSSOP-14, so each would become two packages and a board
change), LMV321 is a **single**, and LM324 is a quad but **not rail-to-rail** and
needs 3 V minimum, which breaks a 3.3 V single-supply rail-to-rail signal chain.
The nearest on paper is the MCP6002 — viable only as part of a deliberate
two-package redesign, not a substitution.

The ~$43 is therefore a **purchasing decision, not a design one**.

## 4. Fabrication output

Gerbers, drill, position files and the assembly BOM are produced by the kicad MCP
server's export tools with the project as the default export directory — see
[AGENTS.md](AGENTS.md) for the tool names. Export artifacts belong in `attic/` or
`/tmp`, not the repository root.

Board data: 54.00 × 102.00 mm outline, 4-layer (F.Cu / In1.Cu / In2.Cu / B.Cu),
1.6 mm, rounded corners r ≈ 2.83 mm, five copper zones. Fabrication and
solder-mask minimums are in [DESIGN.md §5](DESIGN.md#5-design-rules-and-stackup).

## 5. Ordering decisions

### 4-layer vs 2-layer

The board is already 4-layer. Whether the delta is worth it is an ordering
decision, and it is not obvious from the ordering page: a 54 × 102 mm outline is
outside JLCPCB's ≤100 × 100 mm promotional tier and is area-priced, so the
difference shown **may not hold**.

Reaching 100.000 mm in y means finding 2.000 mm, and the copper is not there:

- the nearest copper above is `U3`'s pad copper edge at y 32.290 — 1.290 mm of
  margin;
- below is a trace at y 132.002 — 0.998 mm.

Taking it off the bottom alone breaks the clearance rule before the trim is half
done. Measure pad **edges**, not centres, or the margin is overstated by
0.450 mm. This is reported as a priced option only; **nothing was changed to
chase it**, and the trim should not be attempted unprompted.
