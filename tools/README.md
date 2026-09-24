# `tools/` — the sanctioned direct writers

The kicad MCP server covers almost everything, but a few writes have no MCP tool
at all. These scripts are the one sanctioned exception.

**Still in force:** never hand-edit `.kicad_sch` / `.kicad_pcb` / `.kicad_sym` /
`.kicad_mod` / `.kicad_pro`, and avoid running `kicad-cli` directly. Prefer an MCP
tool whenever one exists. A script here is only for the gaps below.

Every script is narrow by construction: it acts on what is named on the command
line (one footprint, one model, one via, one field) and refuses ambiguous input
rather than guessing. The read-only ones are marked; run those freely.

## Scripts

| Script | Purpose | Writes? |
| --- | --- | --- |
| `addmodels.py` | Give the six model-less footprints (J1–J5, F1) their 3D model, with measured offsets. **Re-run after any footprint-library resync.** | yes |
| `pcbfields.py` | Place every PCB reference designator in a legal, readable spot. **Re-run after any resync.** | yes |
| `fieldplace.py` | Shared CST + geometry helper — imported by 15 of the other scripts here. Run standalone it also autoplaces schematic fields (the MCP server cannot set a field's *position*). | yes |
| `setfield.py` | Pin one footprint's Reference field to an explicit board position. | yes |
| `fptext.py` | Move one footprint's own printed silk text (an `fp_text` node — BZ1's "(+)" is the one on this board). No MCP tool reaches an `fp_text`. Costs a `lib_footprint_mismatch`. | yes |
| `silksearch.py` | Find a DRC-legal spot for one footprint's Reference field. | no |
| `silk.py` | Silkscreen text audit: the drawn angle of all 140 labels, plus text-vs-pad, text-vs-text and off-board tests. | no |
| `fieldsize.py` | Set one footprint field's text size. | yes |
| `padangle.py` | Set the pad-level `at` angle of every pad in one footprint. | yes |
| `modelrot.py` | Set one footprint's 3D-model rotation (for testing a model-orientation question). | yes |
| `modelcheck.py` | Does each retrieved 3D model actually cover the footprint it sits on? | no |
| `delvia.py` | Delete vias at named coordinates. No MCP tool can remove a via. Refuses a coordinate that does not match exactly one via. | yes |
| `keepoutflags.py` | Loosen a keepout's item flags, leaving its copper restrictions alone. | yes |
| `zoneisland.py` | Show, and optionally set, a zone's island-removal mode. | yes |
| `holes.py` | Name the mounting holes and square them up against the board edge. | yes |
| `boardgeo.py` | Board-space geometry: outline, courtyards as polygons, real collisions. | no |
| `box.py` | List every segment/via/pad intersecting a box, any net. | no |
| `j4rip.py` | List J4-net copper intersecting a region, with connectivity, so the rip set can be decided precisely. | no |
| `j4new.py` | J4 fan-out design + DRC-true clearance checker. | no |

Two more scripts are **retired — SWC2-era, written against the pre-rework
board**, and both **keep** the stale `SWC2.kicad_pcb` path on purpose: it fails
loudly rather than printing confident, wrong numbers. Neither's output
describes what is on `SWC.kicad_pcb`. Kept for provenance; read the header
before trusting either.

| Script | Purpose | Writes? |
| --- | --- | --- |
| `corridor_check.py` | USB / VBUS corridor verifier: clearance, track width vs net requirement, net connectivity. Its clearance model disagrees with KiCad's DRC on the reworked board (~48 phantom violations) — see its header. | no |
| `j4fan.py` | J4 (USB-C) fan-out designer/checker for the *rejected* placement (rot 90 @ 21.35; live is rot 270 @ 19.65, 48.206). | no |

## Two conventions that keep costing rounds

Both have been re-derived from scratch at least twice, each time after a wrong
answer had already been acted on. Neither is guessable — take the constants
from `silk.py` and `boardgeo.py` rather than writing them again.

**The footprint-local → board transform.** For a footprint at `(x, y)` at stored
angle `t` and a local point `(lx, ly)`:

```
bx = x + lx*cos t + ly*sin t
by = y - lx*sin t + ly*cos t
```

Note the sign on `sin`. `silk.rot_pt`, `silksearch.rot_pt` and
`boardgeo.rot_pt` are all this function — if you need it, import it, do not
write a fourth copy. A version with the sign the other way was live in
`silksearch.py` until 2026-09-10 and silently mis-placed every obstacle
belonging to a footprint at ±90°; only parts at 0/180 came out right, which is
why it survived a whole placement pass. Three independent checks pin it:
`boardgeo`'s docstring (two DRC messages on J1 and J4), KiCad's own F.SilkS SVG
(F2's field plots at 28.619, 43.820, which the wrong sign puts at 33.619,
56.320), and `get_footprint_bounds` on F2 (30.169..32.069 × 48.390..51.750).

**The drawn angle of a footprint field is the STORED angle alone** — not
`footprint + stored`. `silk.drawn_angle` is the definition; `pcbfields.py`
writes it, and `setfield.py` and `fptext.py` used to write `ang - frot` and
turn a designator meant to lie flat into one standing on end.

When a claim about geometry is in doubt, the tie-breakers, in order, are: DRC's
own message text, `mcp__kicad__export_pcb` to SVG (text elements carry an
explicit `rotate(...)` and `textLength`), then `get_footprint_bounds`. Not
arithmetic, and not a tool's own summary of itself.

## Running them

The board path is usually the argument (some take further flags; run with no
arguments or read the docstring for usage):

```
python3 tools/addmodels.py SWC.kicad_pcb
```

The writers use the MCP server's own s-expression parser
(`mcp_server_kicad._cst`) so their output matches KiCad's byte for byte.
`fieldplace` (imported by all of them) puts that package on `sys.path` itself,
resolving it from `SWC_MCP_SERVER_KICAD` if set or otherwise from the `uvx`
cache (`~/.cache/uv/archive-v0/*/mcp_server_kicad`) — set `SWC_MCP_SERVER_KICAD`
to the directory that contains `mcp_server_kicad/` to pin it explicitly, e.g. in
CI or a checkout where the module is vendored elsewhere.
`addmodels.py`, `pcbfields.py` and friends are idempotent — running one twice
changes nothing the second time.

They self-locate their imports, so they work from any working directory.

## `addmodels.py` in particular

It carries the measured derivation of every offset, including J4's, in its
docstring. Read it before changing an offset: both previous versions of the J4
note were wrong, and each wrong version cost a full round of rework. The short
version is `rotate (0 0 180)`, `offset (0 −1.07 0)`, and the reason the offset
cannot be derived from the STEP's `CARTESIAN_POINT`s is that KiCad applies the
STEP's internal placement (~2.1 mm in Y for this vendor file).
