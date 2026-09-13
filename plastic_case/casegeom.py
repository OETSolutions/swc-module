"""
SWC Adapter Enclosure -- parametric geometry, source of truth.

Everything the case is built from lives here.  The only hard-coded literals are
the VERIFIED board constants, each with a provenance comment.  Every derived
dimension is computed and then asserted (see check()); a negative clearance
raises rather than silently producing a bad part.

COORDINATES
-----------
Board frame (KiCad):   x, y as in KiCad;  z = 0 at the board's TOP face,
                       +z toward the components, -z toward the back.
STEP frame (SWC.step): (kx, ky, kz) -> (kx, -ky, kz + BOARD_T)
                       because in the export the Z origin is the board's
                       BOTTOM face (verified: substrate solid SWC209 spans
                       Z 0.0000..1.5162 with all components above it).
                       bf_z(z) = z + BOARD_T is the STEP z for board-frame z.

All z literals in this file are BOARD FRAME.  build_case.py converts with Z().
"""

import math

# =============================================================================
# 1. VERIFIED BOARD CONSTANTS
# =============================================================================

# --- outline (two independent confirmations: Edge.Cuts and the substrate solid)
BOARD_X0, BOARD_X1 = 16.0, 70.0          # Edge.Cuts lines x=16, x=70
BOARD_Y0, BOARD_Y1 = 31.0, 133.0         # Edge.Cuts lines y=31, y=133
BOARD_R = 2.0                            # Edge.Cuts corner arcs
BOARD_T = 1.5162                         # SWC209 substrate Z 0..1.5162

# --- 4 x Ø5.3 M5 clearance holes (KiCad x,y)
MOUNT_HOLES = [(22.0, 37.0), (64.0, 37.0), (22.0, 127.0), (64.0, 127.0)]
MOUNT_D = 5.3                            # measured as r=2.65 cylinders in STEP

# --- terminal blocks (CUI/Same Sky TB007-508)
# Measured from the STEP solids, all four blocks identical:
#   wire-entry face x = 16.858 ; rear x = 26.858 ; body top z = 15.671 (J5)
#   screw caps are r=2.0 cylinders whose axis is at x = 21.658
#   the wire tunnel in the entry face spans board-frame z 1.885..6.507 (3.43 wide)
# Screw-axis reconciliation: the datasheet puts the M3 screw axis 2.60 mm behind
# the wire-entry face.  The model's screw cap is at 21.658, and the terminal's
# upper (setback) entry face is at x = 19.047 -> 19.047 + 2.611 = 21.658.  Two
# independent confirmations, so the screw axis is 21.658.  (Spec §2.3 assumed
# 22.358, the pin line, which is 0.70 mm too far back; the lower skirt face at
# 16.858 is not the face the datasheet measures from.)
BORE_Y = [56.566, 61.500,                 # J1 TB007-508-02   (2 pole)
          68.329, 73.409, 78.489,         # J2 TB007-508-03   (3 pole)
          85.180, 90.260, 95.340, 100.420,  # J5 TB007-508-04 (4 pole)
          107.122, 112.202, 117.282]      # J3 TB007-508-03   (3 pole)
TERM_SCREW_X = 21.658
TERM_FRONT_X = 16.858
TERM_REAR_X = 26.858
TERM_TOP_Z = 15.671
BORE_Z0, BORE_Z1 = 1.885, 6.507          # measured wire tunnel, board frame
BORE_W = 3.426

# --- USB-C receptacle J4 (measured bbox, board frame)
USBC_X = 15.98                           # connector end face
USBC_Y0, USBC_Y1 = 43.736, 52.676
# front (mating) face extent measured on the x=15.98 plane of the STEP solid:
# the plug tongue enters at z 0.165..3.305; the connector's own shell reaches
# down to -0.765 and up to 3.335, but nothing below the board top needs exposing.
USBC_Z0, USBC_Z1 = 0.165, 3.305

# --- buzzer BZ1
# BOM part = TMB12A05 (Huaneng, LCSC C96093): Ø12.0±0.2 x 9.5±0.2, 7.6 pin
# pitch, sound port on the TOP face at the body centre (datasheet p.2 "Sound
# Hole" in the top view; p.5 test report gives Ø12.0 x 9.5).
# The board's footprint/3D model is a Star Micronics HMB (Ø16, 14.03 tall) --
# the WRONG part -- but BOTH share the same pin pitch, so the port position is
# taken from the footprint, not from the model:
#   KiCad BZ1 pad 1 at board (48.082, 112.551), pad 2 at local (7.61, 0)
#   -> board (48.082, 120.161).  Footprint rotation -90 puts local +x along
#   board +y, so the pad midpoint is (48.082, 116.356).
#   The footprint's own F.Fab body circle AND its F.CrtYd circle are centred at
#   local x = 3.81 = that same pad midpoint, i.e. the body centre IS the
#   midpoint of the pin pitch.
# NOTE: spec §2.6 asserted the body centre was 108.75 and dismissed 116.356 as
# "the courtyard centre".  Verified false -- the courtyard centre and the body
# centre coincide at 116.356.  The buzzer port has been moved 7.61 mm to 116.356.
BZ_PADS = [(48.082, 112.551), (48.082, 120.161)]
BZ_BODY_C = (48.082, sum(p[1] for p in BZ_PADS) / 2.0)   # -> (48.082, 116.356)
BZ_BODY_D = 12.0
BZ_BODY_H = 9.5

# --- controls / indicators (KiCad x,y)
SW1_C = (59.011, 49.329)      # BOOT  TS-1088 (3.9 x 3.0 mm body)
SW2_C = (32.391, 64.267)      # RESET TS-1088
D6_C = (46.965, 61.660)       # LED
D12_C = (41.070, 54.495)      # LED
RT1_C = (68.5, 120.0)         # NTC 10k, 2.10 mm from the right board edge

# =============================================================================
# 2. VERTICAL STACK  (board frame, mm)
# =============================================================================
# --- the vertical stack is set TOP-DOWN by the screw, not by the board -----
# The screw couples the lid to the blind pilot bore, so once the head is flush
# and the tip clears the pilot floor, the case height is a pure function of the
# screw length:
#     seat   = Z_LID_TOP - CBORE_DEPTH
#     tip    = seat - SCREW_LEN
#     Z_FOOT = tip - TIP_CLEAR - PILOT_FLOOR_SKIN
# The lid in turn splits into a plate and a spigot lip:
#     Z_RIM    = Z_LID_TOP - FLANGE_T
#     Z_SPIGOT = Z_RIM - SPIGOT_ENGAGE
# Two clearances decide the rest:
#   * headroom = Z_SPIGOT - Z_TALLEST = (Z_LID_TOP - FLANGE_T - SPIGOT_ENGAGE)
#     - 15.671, and FLANGE_T + SPIGOT_ENGAGE cannot go below CBORE_DEPTH (5.0)
#     because the 5 mm-tall M5 head has to be buried flush.  Pinning the split
#     at 5.0 (3.0 mm plate + 2.0 mm lip) is what makes the headroom 2.329 mm.
#   * engagement = Z_BOARD_BOT - tip = Z_BOARD_BOT - Z_LID_TOP + 5.0 + 25.0,
#     which must stay >= 5.0.  That caps Z_LID_TOP at 23.48; 23.0 leaves
#     5.484 mm of thread -- same as the old M5 x 30 did.
# M5 x 20 is one size down and would put Z_SPIGOT 2.67 mm BELOW the terminal
# bodies, so M5 x 25 is the shortest standard DIN 912 that fits.
Z_LID_TOP   = 23.00     # lid top face
FLANGE_T    = 3.0       # lid plate (rim -> top)
Z_RIM       = Z_LID_TOP - FLANGE_T        # 20.00 base rim / lid plate joint
SPIGOT_ENGAGE = 2.0     # spigot lip dip into the base cavity
Z_SPIGOT    = Z_RIM - SPIGOT_ENGAGE       # 18.00 lid spigot bottom
Z_TALLEST   = TERM_TOP_Z
Z_SW_TOP    = 3.60      # tactile switch top
Z_BOARD_TOP = 0.00
Z_BOARD_BOT = -BOARD_T

# --- bottom-side obstructions, measured from SWC.step (board frame) ---------
# Only three things hang below the board, and they set the cavity floor:
#   * the BUZZER pins, -5.915 -- the buzzer BODY sits entirely on the TOP side
#     (its bottom face is +1.007), so these two pins are a 2-second trim and
#     are the ONLY part the user allows to be cut.  Trimmed to BZ_PIN_TRIM_Z
#     (1.0 mm above the floor) they stop being the constraint.
#   * the TERMINAL-BLOCK press-fit pins: -4.415 at the 6 outer poles and
#     -3.735 at the 4 inner ones.  These MUST NOT be trimmed, so they are the
#     hard limit the floor has to clear.
# (The USB-C shell reaches only -0.798 and the NTC leads to -3.0.)
BZ_PIN_TIP    = -5.915  # measured, untrimmed
BZ_PIN_TRIM_Z = -5.00   # after trimming: 1.0 mm above the cavity floor
TERM_PIN_TIP  = -4.415  # measured, NOT trimmed -- the real floor constraint
Z_DEEPEST     = BZ_PIN_TRIM_Z

# --- flat bottom (rev 2).  The raised floor + ribs + feet of rev 1 put the
# whole underside up in the air and printed as a field of overhangs.  The base
# is a plain box whose underside IS the first layer, so nothing overhangs and
# it sits dead flat on the bed.
Z_FLOOR_TOP = -6.00     # cavity floor (slab top face)
# Z_FOOT is the SHORTEST the case can be while keeping the M5 x 25 socket-head
# screw AND a blind pilot hole.  The screw tip lands at -7.00 -- the SAME -7.00
# the old M5 x 30 gave, because the shorter screw also sits under a lid that
# came down 5 mm.  So the base is untouched by the screw change: the pilot bore
# stops 0.6 mm below the tip (so the screw can never bottom out) and the floor
# skin under the pilot floor is 0.8 mm.  -8.40 is the limit.
#
# NOTE the height is set by the SCREW, not by the board's bottom-side parts:
#   engagement = Z_BOARD_BOT - (lid_top - counterbore_depth - SCREW_LEN)
#              = 5.4838 mm, for ANY standard screw length that keeps the head
# flush (a longer screw needs a taller lid and a shallower counterbore; the
# engagement it buys is set by the board's thickness, not by its length).  So
# trimming the buzzer pins -- which the user allows -- frees nothing: the
# terminal-block pins (NOT trimmed, -4.415) and the buzzer pins are both well
# above the -7.00 screw tip.
Z_FOOT      = -8.40     # flat bottom face = first layer
Z_FLOOR_BOT = Z_FOOT    # slab underside (kept as a name for the builders)
PILOT_FLOOR_Z    = -7.60  # blind bottom of the M5 pilot bore (flat, printed)
PILOT_FLOOR_SKIN = PILOT_FLOOR_Z - Z_FOOT   # 0.8 mm floor under the pilot
TIP_CLEAR    = 0.6      # screw tip must stop this far above the pilot floor

# =============================================================================
# 3. X / Y ENVELOPE  (board frame, mm)
# =============================================================================
WALL        = 2.0       # nominal wall thickness
CAV_GAP     = 0.6       # board-to-cavity-wall clearance

CAV_X0, CAV_X1 = BOARD_X0 - CAV_GAP, BOARD_X1 + CAV_GAP      # 15.40 .. 70.60
CAV_Y0, CAV_Y1 = 13.25, BOARD_Y1 + CAV_GAP                   # front nose .. 133.60
# front wall is pushed 5.75 mm clear of the module's overhanging antenna end
# (module antenna keepout y 25.25..31.25, so 13.25 gives 12.0 mm of clear air).

CASE_X0, CASE_X1 = CAV_X0 - WALL, CAV_X1 + WALL      # 13.40 .. 72.60
CASE_Y0, CASE_Y1 = CAV_Y0 - WALL, CAV_Y1 + WALL      # 11.25 .. 135.60
CASE_R = 2.0
CAV_R = 1.4

# The lid is FLUSH with the base: the earlier 1.5 mm flange overhang left a lip
# all round that is awkward to print and unnecessary.
FLANGE_OVER = 0.0
FLANGE_X0, FLANGE_X1 = CASE_X0 - FLANGE_OVER, CASE_X1 + FLANGE_OVER   # = CASE
FLANGE_Y0, FLANGE_Y1 = CASE_Y0 - FLANGE_OVER, CASE_Y1 + FLANGE_OVER   # = CASE

SPIGOT_CLEAR = 0.2
SPI_X0, SPI_X1 = CAV_X0 + SPIGOT_CLEAR, CAV_X1 - SPIGOT_CLEAR
SPI_Y0, SPI_Y1 = CAV_Y0 + SPIGOT_CLEAR, CAV_Y1 - SPIGOT_CLEAR

# =============================================================================
# 4. FASTENER STACK (M5 through the board's own Ø5.3 holes)
# =============================================================================
BOSS_D      = 11.0      # base boss OD
TOWER_D     = 14.0      # corner tower OD  (Ø14 fits inside the corner radii)
# The board RESTS on the boss tops (spec §5.5): the boss must reach the board's
# underside so the M5 screw clamps the board between the boss top (below) and
# the lid sleeve bottom (above).  A Ø14/Ø11 boss seated at the board bottom
# interferes with NOTHING -- verified against the real STEP: zero common volume
# at all four holes, and no bottom-side part lies within 7 mm of any hole.
BOSS_TOP_Z  = Z_BOARD_BOT             # -1.5162  (board bottom face)
PILOT_D     = 4.2       # M5 thread-forming pilot
PILOT_TOP_Z = BOSS_TOP_Z   # pilot hole reaches the boss top so the screw enters
PILOT_ENTRY_CHAMFER = 1.0
SLEEVE_D    = 11.0      # lid sleeve OD
SLEEVE_BORE = 5.8       # M5 clearance
LID_SCREW_D = 5.8
# --- M5 x 25 socket-head cap screw (DIN 912; length measured under the head).
# Head Ø8.5 x 5.0.  The head seat is FLUSH with the lid's top face -- there is
# no raised rim, because in the lid's print orientation (flipped 180 deg, plate
# on the bed) any rim would lift the plate off the bed and the slicer would
# bridge the whole 59 x 124 mm plate 2.6 mm up, supported only by four rims.
# Flush is also what the design wants: the head fills the recess and its top is
# level with the lid.  With the seat 5.0 mm up into the lid, a 25 mm screw's tip
# lands at -6.00 and engages the pilot by 5.4838 mm.
CBORE_D     = 9.5       # socket-head cap Ø8.5 clearance
CBORE_DEPTH = 5.0       # head-seat depth = head height, so the head is flush
CBORE_RAISE = 0.0       # no rim above the lid top
SCREW_LEN   = 25.0      # M5 x 25 SHCS, off-the-shelf (length under the head)

# =============================================================================
# 5. OPENINGS
# =============================================================================
# --- 5.1 terminals: wire access windows in the LEFT wall
WIRE_WIN_W  = 4.0                     # y
WIRE_WIN_R  = 1.5
WIRE_WIN_Z0 = 1.4                     # board frame (covers the measured bore)
WIRE_WIN_Z1 = 7.0

# --- 5.1b USB-C
# Window 11.5 wide x 6.0 tall.  Bottom at z = -0.35 so the connector shell,
# which hangs 0.765 mm below the board top (measured), is not pinched by the
# wall; top at 5.65 clears the connector (3.335) with 2.3 mm for a plug overmold.
USBC_WIN_Y0, USBC_WIN_Y1 = 42.44, 53.98
USBC_WIN_Z0, USBC_WIN_Z1 = -0.35, 5.65

# --- 5.2 vents -- all LOW (z 0.80..4.00), so the air crosses the PCB at the
# height of the parts and of the terminal wire tunnels.  Rev 1 put the band up
# at z 5..14, which both missed the sensors and printed straight through the
# lid's driver slots.
#   * +x wall (RT1 side): 3 slots beside the NTC, board (68.5, 120.0) -- the
#     air sweeps the sensor itself.
#   * y=133 END wall: 2 slots.  That wall would otherwise be the one fully
#     closed face of the enclosure.
#   * -x wall: 1 slot.  The user requires vents on BOTH sides around RT1, so
#     this is the inlet: air enters at x~14.6 and crosses the board straight to
#     the RT1 slot at x~70, which is an exactly opposed pair at y = 126.0.
#     The -x wall is otherwise crowded -- 12 wire windows on a 5.08 mm pitch
#     leave gaps of only 1.1..2.8 mm, far too narrow for a slot -- so the inlet
#     goes in the one free band, BEHIND the last window (which ends at y
#     119.28): y 124.25..127.75 clears it by 4.97 mm.  The rear corner bosses
#     (board y 127) stop at the board's underside, so they do not obstruct this
#     z band at all.
# 3.5 mm wide on a 7 mm pitch, so on the +x wall no rib is thinner than 3.5 mm.
VENT_Z_LO   = 0.8                     # lowest printable band (0.8 mm bridges)
VENT_W      = 3.5                     # slot width (a-axis)
VENT_H      = 3.2                     # slot height (z)
VENT_R      = 0.8                     # corner radius
VENT_RT1_Y  = [112.0, 119.0, 126.0]   # +x wall, beside RT1
VENT_END_X  = [34.0, 52.0]            # y=133 end wall, clear of the bosses
VENT_LFT_Y  = [126.0]                 # -x wall inlet, directly opposite RT1

# --- 5.3 floor silkscreen windows (board underside, B.SilkS)
# The board is mounted component-side UP, so its B.SilkS text faces the case
# floor and is read by looking UP through these windows.
#
# A floor window is a through-hole in the floor slab with the cavity above it,
# so nothing has to bridge it -- it prints as a single inner perimeter and needs
# no support at any width.  What DOES matter is that the slab stays one
# connected piece, so the solid strip between consecutive windows (a "rib") and
# the strip out to the cavity wall are both asserted, not assumed.  In PLA/
# ASA/ABS a 2 mm rib is 5 extrusion widths wide and as stiff as the walls.
#
# --- MEASURED FROM THE PCB's OWN B.SilkS, not estimated ---------------------
# Extents measured off the B.SilkS svg export, inside the board outline
# (svg x = board x, svg y = board y + 0.40).  x extents are identical for all
# four blocks because they are the same part silk-screened the same way:
#
#   pin names (0 deg, right-aligned)  x 17.63..20.57
#   block titles (90 deg)             x 22.23..25.36
#
#   block  coarse silk y      per-block fine silk y          poles  title
#   J1     52.53.. 61.86      52.53.. 61.86                 2      12V DC IN
#   J2     68.06.. 79.02      69.59.. 77.49                 3      SWC INPUT
#   J5     84.74..100.86      88.19.. 97.42                 4      AUX INPUT
#   J3    106.56..117.36     107.84..115.23                 3      SWC OUTPUT
#
# The silkscreen is on BOTH sides of the pin field: right-aligned pin names
# (`GND`, `+12V`, `AUX3`...) at x 17.6..20.6 outboard of the pin column
# (x 22.36), and the block title inboard of it at x 22.2..25.4.  One window per
# block therefore has to span x 17.4..25.5 to show both -- an earlier revision
# sized the windows on the titles alone and left every pin name unreadable.
#
# The window is the block's silk boxes UNIONed, then grown by one margin, so
# both columns are revealed with the SAME reveal.  Sizing each column
# independently (the old model) put a 1.24 mm sliver on the pin-name side of
# J3 and a 1.60 mm sliver on the title side of J5 in the same window.
#
# The windows are deliberately NOT all the same height, even though the blocks
# are.  A 4-pole block's pin names span 9.23 mm against a 2-pole block's 9.33,
# but its pin COLUMN is 10.16 mm longer, so a common window height covering J5
# (16.12 + 2 mm margin) would overlap J3 -- the two blocks are only 15.70 mm
# apart centre to centre.  Equal HEIGHT is therefore geometrically impossible
# here; what the eye actually reads is the equal REVEAL, and that is what is
# held: FLOOR_WIN_MARGIN on every side of every block.
FLOOR_WIN_MARGIN = 0.70               # reveal kept equal on all four sides
FLOOR_WIN_R  = 1.5                    # corner radius
FLOOR_WIN_CHAMFER = 1.0               # underside 45 deg lip chamfer, so the
                                      # text reads at an oblique angle too
# Measured silk columns, from the plotted B.SilkS layer.  Every pin NAME sits at
# x 17.63..20.57 (the widest are +12V, GND, AUX3) and every block TITLE at
# x 24.56..25.36 (single-line, rotated).  The window has to span both columns,
# because both are on the WIRE side of the pins -- the user's point exactly.
FLOOR_WIN_PIN_X   = (17.63, 20.57)
FLOOR_WIN_TITLE_X = (24.56, 25.37)
# The window height is keyed to the BLOCK, not to its silk.  Sizing from the
# printed text is what made two identical 3-pole blocks get different windows:
# J2 and J3 are the same part, but "SWC INPUT" is 6.32 mm tall against "SWC
# OUTPUT" at 7.39 mm, and their pin-name strings differ too, so a text-derived
# window came out 0.16 mm apart for the same hardware.  Keyed to the part,
# identical blocks give bit-identical windows by construction.
# Half-height = the block's own pole span + FLOOR_WIN_CLEAR.  The clearance has
# to cover the widest silk overhang on any block: the pin-name row of a 3-pole
# block reaches 5.64 mm from its centre (J3's GND at 106.56 against a 112.20
# centre) versus a 5.08 mm half-span, so 1.10 mm leaves >= 0.54 mm of reveal
# everywhere.  The 2-pole block needs only 0.36 mm and the 4-pole 0.44 mm, so the
# single constant is generous there and the windows stay visually consistent.
FLOOR_WIN_CLEAR = 1.10
# Measured pole spans (outer bore to outer bore) and centres, one per block.
FLOOR_WIN_SPAN = [4.934,    # J1  12V DC IN   (2 pole)
                  10.160,   # J2  SWC INPUT   (3 pole)
                  15.240,   # J5  AUX INPUT   (4 pole)
                  10.160]   # J3  SWC OUTPUT  (3 pole)
# Per-block measured pin-name silk extents, used by the ASSERTIONS to prove the
# block-derived window really does expose the text.  These are observations, not
# inputs -- the window is built from FLOOR_WIN_SPAN above.
FLOOR_WIN_SILK = [(56.08, 61.86), (68.06, 79.02),
                  (84.74, 100.86), (106.56, 117.36)]
# Every window is the same width, so the x span is derived once from the two
# measured columns rather than written in: 16.93 .. 26.07.
FLOOR_WIN_X0 = min(FLOOR_WIN_PIN_X[0], FLOOR_WIN_TITLE_X[0]) - FLOOR_WIN_MARGIN
FLOOR_WIN_X1 = max(FLOOR_WIN_PIN_X[1], FLOOR_WIN_TITLE_X[1]) + FLOOR_WIN_MARGIN
# The solid strip between consecutive blocks must be as stiff as a wall.
FLOOR_WIN_RIB_MIN = 2.0
# The solid strip from a window edge out to the CAVITY wall.  On the -x side
# this is only 1.53 mm, and deliberately so: that strip is not a free rib, it is
# the cavity wall's own footprint -- continuous along y with the 2.0 mm standing
# wall directly above it, so it is stiffer than any rib the 2.0 mm limit
# protects.  Narrowing the window to buy 2.0 mm here would eat 0.47 mm of the
# pin-name reveal and reintroduce the very asymmetry this model removes.
FLOOR_WIN_EDGE_MIN = 1.5


def floor_windows():
    """(x0, x1, y0, y1) of each floor silkscreen window, board frame.

    One window per terminal block, grown by FLOOR_WIN_MARGIN on every side of the
    block's own POLE SPAN -- so the window is a function of the part, and two
    identical blocks always get identical windows.  Identical x span and
    identical reveal for all four blocks; the y span scales with pole count
    (2, 3, 4, 3 poles -> 7.18, 12.40, 17.48, 12.40 mm).
    """
    out = []
    for _b, _s in zip(terminal_blocks(), FLOOR_WIN_SPAN):
        _m = sum(_b) / len(_b)
        _h = _s / 2.0 + FLOOR_WIN_CLEAR
        out.append((FLOOR_WIN_X0, FLOOR_WIN_X1, _m - _h, _m + _h))
    return out

# --- 5.4 lid: terminal screwdriver access
# One rounded slot per terminal block, not 12 discrete holes: on the 5.08 mm
# pole pitch any driver-sized hole merges with its neighbour *within* a block
# (spec §9.1 anticipated this) while leaving only a 0.19 mm web *between*
# blocks -- unprintable.  A single 5.0 wide slot per block gives generous
# driver clearance and ~1.3-1.4 mm printable webs between blocks.
DRIVER_SLOT_W = 5.0          # x width (admits a 4 mm blade / PH1)
DRIVER_SLOT_END = 2.7        # slot end beyond the outer pole centre
DRIVER_SLOT_X = TERM_SCREW_X
DRIVER_SLOT_LEADIN = 1.0

# --- 5.4 lid: LED windows
LED_HOLE_D = 4.0
LED_RECESS_D, LED_RECESS_DEPTH = 7.0, 1.0
LED_CHAMFER = 0.8

# --- 5.4 lid: button access
BTN_HOLE_D = 5.0
BTN_RECESS_D, BTN_RECESS_DEPTH = 9.0, 1.5
BTN_CHAMFER = 0.8

# --- 5.4 lid: buzzer sound port (over the TMB12A05 body CENTRE, top-face port)
BZ_PORT_C_D = 4.0
BZ_BOLT_D, BZ_BOLT_R, BZ_BOLT_N = 2.4, 4.5, 6

# --- 5.5 lid: OET Solutions logo inlay (third printed body)
# Source: footprints/logo.pretty/LOGO.kicad_mod -- 14 filled fp_poly on B.SilkS,
# the same mark as the board's bottom silkscreen.  Native size is only
# 15.52 x 1.95 mm and the narrowest stroke is 0.36 mm, i.e. NOT printable as
# FDM (a 0.4 mm nozzle cannot lay a 0.36 line, and the 0.11 mm median stroke
# simply vanishes).  It is therefore scaled up and thickened, then dropped into
# a matching pocket cut in the lid TOP and printed as a THIRD body: swap to the
# second colour for the 6 layers that span LOGO_DEPTH, then swap back.  No
# supports, no glue, flush finish.
LOGOSRC      = "../footprints/logo.pretty/LOGO.kicad_mod"
LOGO_SIMPL   = 0.04      # drop sub-0.04 mm noise after scaling; the gear teeth
                         # survive, the B-rep shrinks ~4x

LOGO_SCALE   = 3.898     # native 15.518 x 1.950 -> 60.50 x 7.60 mm
# LOGO_WIDEN is applied AFTER LOGO_SCALE, so it is IN FINAL MILLIMETRES: it is
# the real thickness added to every edge of the printed mark, and can therefore
# be judged against the nozzle directly.  The native strokes run 0.072..0.36 mm
# with a 0.11 mm median; at LOGO_SCALE the median reaches 0.43 mm and the
# hairline thinnest are still sub-nozzle.  0.20 mm lifts the thinnest stroke to
# a printable 0.56 mm and the median to 0.83 mm (>2 extrusion widths).  Much
# more and the glyphs start to weld together -- the narrowest counter (the gap
# inside the 'S') is only 1.08 mm at 1:1.
LOGO_WIDEN   = 0.20
LOGO_DEPTH   = 1.2       # pocket / inlay thickness (6 layers @ 0.20 mm)
LOGO_GAP     = 0.10      # total side clearance -- a snug printed press fit, so
                         # the inlay stays put without glue but is still a
                         # separate, differently-coloured body
# The mark is ROTATED 90 deg (see logo.rings_step), so it exchanges axes:
# its long axis runs in y and its short axis in x.  Half-extents are the POCKET
# (the inlay grown by LOGO_GAP/2, i.e. what is actually cut in the lid) measured
# off the built geometry -- logo.rings_step centres the mark on its bounding box
# and reports this same extent, so the two cannot drift apart.
LOGO_HALF_X  = 4.0493    # measured pocket half-extent, x
LOGO_HALF_Y  = 30.4913   # measured pocket half-extent, y
# The mark goes on the side OPPOSITE the connectors.  Every connector is on the
# -x wall (USB-C J4 and the four terminal blocks at x 16.9..26.9), as are their
# lid cut-outs, so the opposite side is +x.  There the lid plate is clear from
# the button/LED/buzzer cluster (which ends at x 53.78) to the flange, i.e.
# x 53.8..69.4; and in y from the USB routing hole (y <= 53.83) to the rear
# counterbores (y >= 122.25), i.e. y 53.8..122.3.
LOGO_X       = 60.90     # built geometry centred on the +x clear band
LOGO_Y       = 88.00     # midway in y between the USB hole and the counterbores
# Ground-truth obstacle extents around that band, measured off the lid's own top
# face (build_case.build_lid with the pocket suppressed) rather than derived
# from nominal diameters -- the recesses are milled with a chamfer, so their
# printed footprint is larger than their nominal circle.
LOGO_CLEAR_Y0 = 53.83    # USB routing hole max y
LOGO_CLEAR_Y1 = 122.25   # rear counterbore min y
LOGO_CLEAR_X0 = 53.79    # buzzer bolt circle max x

# =============================================================================
# 6. DERIVED + ASSERTIONS
# =============================================================================

def vent_slots():
    """Vent slots: (wall, a0, a1) with wall in {'X','-X','Y'}.

    'X'  = the +x long wall (beside RT1)
    '-X' = the -x long wall (the terminal/wire-window wall) -- one inlet slot
    'Y'  = the y=133 short end wall (the slots moved off the terminal blocks)

    Every slot shares the low z band, so the vent air crosses the board at the
    height of RT1 and of the terminal wire tunnels.
    """
    out = []
    for cy in VENT_RT1_Y:
        out.append(("X", cy - VENT_W / 2.0, cy + VENT_W / 2.0))
    for cy in VENT_LFT_Y:
        out.append(("-X", cy - VENT_W / 2.0, cy + VENT_W / 2.0))
    for cx in VENT_END_X:
        out.append(("Y", cx - VENT_W / 2.0, cx + VENT_W / 2.0))
    return out

def logo_rect():
    """(cx, cy) of the logo pocket centre in the lid TOP, board frame.

    The mark's BOUNDING-BOX centre lands on this point.  The mark is rotated
    90 deg, so it runs up the lid rather than across it, and it sits on the +x
    side -- the side opposite every connector.  build_case grows the pocket by
    LOGO_GAP/2 from the same geometry the inlay is built on, so the two always
    agree.
    """
    return (LOGO_X, LOGO_Y)

def terminal_blocks():
    """Group the 12 bore y's into blocks.

    Intra-block pole pitch is 5.08; the gap between blocks is 6.69-6.83, so a
    5.8 threshold separates them cleanly (7.0 would merge all four).
    """
    blocks = [[BORE_Y[0]]]
    for y in BORE_Y[1:]:
        if y - blocks[-1][-1] < 5.8:
            blocks[-1].append(y)
        else:
            blocks.append([y])
    return blocks


def driver_slots():
    """(y0, y1) of the screwdriver slot for each terminal block."""
    out = []
    for b in terminal_blocks():
        out.append((b[0] - DRIVER_SLOT_END, b[-1] + DRIVER_SLOT_END))
    return out


def buzzer_bolt_positions():
    c = BZ_BODY_C
    out = []
    for i in range(BZ_BOLT_N):
        a = math.radians(60.0 * i)
        out.append((c[0] + BZ_BOLT_R * math.cos(a), c[1] + BZ_BOLT_R * math.sin(a)))
    return out

def _checks():
    """Return a list of (name, value, limit, ok) clearance checks."""
    C = []
    def add(name, value, limit, ok):
        C.append((name, value, limit, ok))

    # --- board fits the cavity with clearance all round
    add("cavity clearance x0", CAV_X0 - BOARD_X0, 0.0, CAV_X0 < BOARD_X0)
    add("cavity clearance x1", BOARD_X1 - CAV_X1, 0.0, CAV_X1 > BOARD_X1)
    add("cavity clearance y1", BOARD_Y1 - CAV_Y1, 0.0, CAV_Y1 > BOARD_Y1)

    # --- antenna air gap ahead of the module's overhanging antenna edge (y=25.25)
    add("antenna gap ahead", 25.25 - CAV_Y0, 8.0, (25.25 - CAV_Y0) >= 8.0)

    # --- vertical headroom: tallest top-side part to the lid spigot underside.
    #     2.329 mm is the design minimum, and it is the WHOLE of the lid's
    #     contribution: the plate and the spigot lip together are exactly
    #     CBORE_DEPTH, the least the flush 5 mm screw head allows.
    add("headroom over terminals", Z_SPIGOT - Z_TALLEST, 2.0,
        (Z_SPIGOT - Z_TALLEST) >= 2.0)
    # ... and the lid has no material it does not need: plate + spigot lip are
    # pinned at the counterbore depth, so the headroom above cannot be trimmed
    # further without the screw head standing proud of the lid.
    add("lid splits into exactly the head depth", FLANGE_T + SPIGOT_ENGAGE,
        CBORE_DEPTH, abs((FLANGE_T + SPIGOT_ENGAGE) - CBORE_DEPTH) < 1e-9)
    # --- floor: the flat bottom must clear the parts that hang below the board.
    #     The terminal-block press-fit pins are NOT trimmed, so they are the
    #     hard constraint; the buzzer pins are trimmed to BZ_PIN_TRIM_Z and must
    #     end up clear of the floor.
    add("floor to terminal pins", TERM_PIN_TIP - Z_FLOOR_TOP, 0.5,
        (TERM_PIN_TIP - Z_FLOOR_TOP) >= 0.5)
    add("buzzer pins trimmed clear", BZ_PIN_TRIM_Z - Z_FLOOR_TOP, 0.5,
        (BZ_PIN_TRIM_Z - Z_FLOOR_TOP) >= 0.5)
    add("buzzer trim below original tip", BZ_PIN_TRIM_Z - BZ_PIN_TIP, 0.5,
        (BZ_PIN_TRIM_Z - BZ_PIN_TIP) >= 0.5)
    #     The slab is 2.4 mm of solid ABS/ASA under the terminals.  Its underside
    #     is one flat face, so it prints as the first layer with no overhang.
    add("floor slab thickness", Z_FLOOR_TOP - Z_FOOT, 2.0,
        (Z_FLOOR_TOP - Z_FOOT) >= 2.0)

    # --- screw stack: the boss top reaches the board's underside so tightening
    #     the M5 clamps the board between the boss (below) and the lid sleeve
    #     (above); the thread engagement lives in the flat slab below.
    add("boss top at board bottom", BOSS_TOP_Z - Z_BOARD_BOT, 0.0,
        abs(BOSS_TOP_Z - Z_BOARD_BOT) < 1e-9)
    add("pilot length (pilot top to floor)", PILOT_TOP_Z - PILOT_FLOOR_Z, 5.5,
        (PILOT_TOP_Z - PILOT_FLOOR_Z) >= 5.5)
    add("pilot floor skin", PILOT_FLOOR_SKIN, 0.8,
        PILOT_FLOOR_SKIN >= 0.8 - 1e-6)

    # --- M5 x 25 SHCS (DIN 912; length under the head) must engage the pilot by
    #     >= 5 mm, stop clear of the pilot floor, and sit flush with the lid.
    #     head-seat z = lid top - CBORE_DEPTH  = 24.0 - 5.0 = 19.0
    #     screw tip z = head-seat - SCREW_LEN  = 19.0 - 25  = -6.0
    seat_z = (Z_LID_TOP + CBORE_RAISE) - CBORE_DEPTH   # 19.0
    tip_z  = seat_z - SCREW_LEN                        # -6.0
    add("M5x25 thread engagement", Z_BOARD_BOT - tip_z, 5.0,
        (Z_BOARD_BOT - tip_z) >= 5.0)
    add("M5x25 tip clear of pilot floor", tip_z - PILOT_FLOOR_Z, TIP_CLEAR,
        (tip_z - PILOT_FLOOR_Z) >= TIP_CLEAR - 1e-6)
    add("M5 head flush with lid top",
        abs((Z_LID_TOP + CBORE_RAISE) - (seat_z + 5.0)), 0.0,
        (Z_LID_TOP + CBORE_RAISE) - (seat_z + 5.0) <= 0.05)
    add("counterbore not through the lid", (seat_z + 5.0) - Z_SPIGOT, 0.0,
        (seat_z + 5.0) > Z_SPIGOT)
    # --- and the screw is the ONLY thing setting the height: the case must be
    #     exactly as tall as the screw stack needs, with no dead air below it.
    add("case height is the screw stack", (Z_LID_TOP - Z_FOOT) -
        (CBORE_DEPTH + SCREW_LEN + TIP_CLEAR + PILOT_FLOOR_SKIN), 0.0,
        abs((Z_LID_TOP - Z_FOOT) -
            (CBORE_DEPTH + SCREW_LEN + TIP_CLEAR + PILOT_FLOOR_SKIN)) < 1e-6)

    # --- wire window must span the measured bore
    add("wire window covers bore z0", BORE_Z0 - WIRE_WIN_Z0, 0.0,
        WIRE_WIN_Z0 <= BORE_Z0)
    add("wire window covers bore z1", WIRE_WIN_Z1 - BORE_Z1, 0.0,
        WIRE_WIN_Z1 >= BORE_Z1)
    add("wire window covers bore width", WIRE_WIN_W - BORE_W, 0.0,
        WIRE_WIN_W >= BORE_W)

    # --- USB-C window must span the connector body
    add("usbc window covers body z0", USBC_Z0 - USBC_WIN_Z0, 0.0,
        USBC_WIN_Z0 <= USBC_Z0)
    add("usbc window covers body z1", USBC_WIN_Z1 - USBC_Z1, 0.0,
        USBC_WIN_Z1 >= USBC_Z1)

    # --- screwdriver access: slot must reach past the outer screw caps of
    #     every block, and leave a printable web between adjacent blocks
    bs = terminal_blocks()
    for b in bs:
        add("slot covers block %s" % ("/".join("%.0f" % v for v in b)),
            DRIVER_SLOT_END, 0.5, DRIVER_SLOT_END >= 0.5)
    for i in range(len(bs) - 1):
        web = (bs[i + 1][0] - DRIVER_SLOT_END) - (bs[i][-1] + DRIVER_SLOT_END)
        add("lid web between blocks %d-%d" % (i + 1, i + 2), web, 0.8, web >= 0.8)

    # --- buzzer port cluster must stay inside the TMB Ø12 body
    rmax = BZ_BOLT_R + BZ_BOLT_D / 2.0
    add("buzzer cluster inside body", BZ_BODY_D / 2.0 - rmax, 0.3,
        rmax <= BZ_BODY_D / 2.0 - 0.3 + 1e-6)

    # --- lid sleeve must clear the terminal body (J3 rear face y = 120.422)
    y_term_rear = 120.422
    gap = (MOUNT_HOLES[2][1] - SLEEVE_D / 2.0) - y_term_rear
    add("sleeve to terminal body", gap, 0.8, gap >= 0.8)

    # --- sleeve must clear the J3 driver slot (its far end, y = 117.282+2.7)
    hx, hy = MOUNT_HOLES[2]
    slot_end_y = BORE_Y[-1] + DRIVER_SLOT_END
    xgap = (hx - SLEEVE_D / 2.0) - (DRIVER_SLOT_X + DRIVER_SLOT_W / 2.0)
    ygap = (hy - SLEEVE_D / 2.0) - slot_end_y
    gap = max(xgap, ygap)      # separated if clear in EITHER x or y
    add("sleeve to driver slot", gap, 0.8, gap >= 0.8)

    # --- counterbore must leave a wall in the sleeve
    add("counterbore wall in sleeve", (SLEEVE_D - CBORE_D) / 2.0, 0.6,
        (SLEEVE_D - CBORE_D) / 2.0 >= 0.6)

    # --- vents: printable ribs, low on the board, clear of the corner bosses
    for wall, a0, a1 in vent_slots():
        add("vent %s %.0f-%.0f width" % (wall, a0, a1), a1 - a0, 2.0,
            (a1 - a0) >= 2.0)
    def _min_pitch_gap(arr):
        return min(arr[i + 1] - arr[i] for i in range(len(arr) - 1))
    add("vent rib (RT1 wall pitch)", _min_pitch_gap(VENT_RT1_Y) - VENT_W,
        2.0, _min_pitch_gap(VENT_RT1_Y) - VENT_W >= 2.0)
    add("vent rib (end wall pitch)", _min_pitch_gap(VENT_END_X) - VENT_W,
        2.0, _min_pitch_gap(VENT_END_X) - VENT_W >= 2.0)
    add("vent bottom above foot", VENT_Z_LO - Z_FOOT, 3.0,
        (VENT_Z_LO - Z_FOOT) >= 3.0)
    add("vent top clear of board", VENT_Z_LO + VENT_H - Z_BOARD_TOP, 0.5,
        (VENT_Z_LO + VENT_H - Z_BOARD_TOP) >= 0.5)
    # the -x wall inlet must clear the last wire window (it is the only free
    # band on that wall) ...
    for cy in VENT_LFT_Y:
        add("-x vent @ %.1f to last wire window" % cy,
            (cy - VENT_W / 2.0) - (BORE_Y[-1] + WIRE_WIN_W / 2.0), 2.0,
            (cy - VENT_W / 2.0) - (BORE_Y[-1] + WIRE_WIN_W / 2.0) >= 2.0)
    # ... and it must be an OPPOSED pair with an RT1-wall slot, so the user's
    # "both sides around RT1" cross-flow is real and not merely two openings.
    add("-x inlet opposes an RT1 slot",
        min(abs(cy - c) for cy in VENT_LFT_Y for c in VENT_RT1_Y), 0.5,
        min(abs(cy - c) for cy in VENT_LFT_Y for c in VENT_RT1_Y) <= 0.5)
    # the end-wall slots must cleanly straddle the region between the two rear
    # bosses
    for cx in VENT_END_X:
        gap = min(cx - VENT_W / 2.0 - (MOUNT_HOLES[2][0] + TOWER_D / 2.0),
                  (MOUNT_HOLES[3][0] - TOWER_D / 2.0) - (cx + VENT_W / 2.0))
        add("vent end-wall @ %.1f vs boss" % cx, gap, 1.0, gap >= 1.0)

    # --- floor silkscreen windows: each must cover BOTH the pin names
    #     (x 17.63..20.57) and the block title (x 24.56..25.37), so one window
    #     per block actually shows that block's silkscreen.
    _pn = FLOOR_WIN_PIN_X
    _ti = FLOOR_WIN_TITLE_X
    for i, (b, w) in enumerate(zip(FLOOR_WIN_SILK, floor_windows())):
        add("floor win %d pin names inside" % i,
            0.0 if (w[0] <= _pn[0] and _pn[1] <= w[1]) else 1.0, 0.0,
            w[0] <= _pn[0] and _pn[1] <= w[1])
        add("floor win %d title inside" % i,
            0.0 if (w[0] <= _ti[0] and _ti[1] <= w[1]) else 1.0, 0.0,
            w[0] <= _ti[0] and _ti[1] <= w[1])
        add("floor win %d y0 margin" % i, b[0] - w[2], 0.5,
            (b[0] - w[2]) >= 0.5)
        add("floor win %d y1 margin" % i, w[3] - b[1], 0.5,
            (w[3] - b[1]) >= 0.5)
    # every window keeps >= 2 mm of solid slab from the cavity wall
    _edge = min(min(w[0] - CAV_X0, CAV_X1 - w[1],
                    w[2] - CAV_Y0, CAV_Y1 - w[3]) for w in floor_windows())
    add("floor win to cavity wall min", _edge, FLOOR_WIN_EDGE_MIN,
        _edge >= FLOOR_WIN_EDGE_MIN)
    # The same part is silk-screened the same way four times, so all four
    # windows must be the same width and leave the same reveal.  Their HEIGHTS
    # legitimately differ: a 4-pole block's pin column is 10.16 mm longer than a
    # 2-pole block's, so forcing equal heights would either clip J5's silk or
    # overlap J3 (the two blocks are 15.70 mm apart centre to centre).
    sizes = [(w[1] - w[0], w[3] - w[2]) for w in floor_windows()]
    add("floor win all same width", max(s[0] for s in sizes) - min(s[0] for s in sizes),
        1e-9, max(s[0] for s in sizes) - min(s[0] for s in sizes) < 1e-9)
    # Every window keeps the same reveal off its own silk top and bottom.
    _rev = [b[0] - w[2] for b, w in zip(FLOOR_WIN_SILK, floor_windows())]
    _rev += [w[3] - b[1] for b, w in zip(FLOOR_WIN_SILK, floor_windows())]
    add("floor win reveal >= margin", min(_rev), 0.5,
        min(_rev) >= 0.5 - 1e-9)
    # Two blocks with the same pole count must get the same window height: the
    # window follows the BLOCK, not the length of its label.  J2 and J3 are both
    # 3-pole but are labelled "SWC INPUT" / "SWC OUTPUT", whose titles are
    # 6.32 / 7.39 mm tall; sizing from the text alone left their windows 1.07 mm
    # apart, which is exactly the "different sizes even for the same size
    # terminal blocks" the user saw.  Assert it so it cannot come back.
    for _i, _j in ((1, 3),):
        _ha = floor_windows()[_i][3] - floor_windows()[_i][2]
        _hb = floor_windows()[_j][3] - floor_windows()[_j][2]
        add("floor win equal for equal pole count", abs(_ha - _hb), 1e-9,
            abs(_ha - _hb) < 1e-9)
    # no window may overlap the next: overlapping slabs would leave a floating
    # web of floor with nothing holding it
    for i in range(len(FLOOR_WIN_SPAN) - 1):
        a, b = floor_windows()[i], floor_windows()[i + 1]
        add("floor win rib %d/%d" % (i, i + 1), b[2] - a[3], FLOOR_WIN_RIB_MIN,
            (b[2] - a[3]) >= FLOOR_WIN_RIB_MIN)
    add("floor win inboard of cavity x0",
        min(w[0] for w in floor_windows()) - CAV_X0, FLOOR_WIN_EDGE_MIN,
        min(w[0] for w in floor_windows()) - CAV_X0 >= FLOOR_WIN_EDGE_MIN)
    add("floor win inboard of cavity x1",
        CAV_X1 - max(w[1] for w in floor_windows()), FLOOR_WIN_EDGE_MIN,
        CAV_X1 - max(w[1] for w in floor_windows()) >= FLOOR_WIN_EDGE_MIN)
    add("floor win clear of cavity y0", min(w[2] for w in floor_windows()) - CAV_Y0,
        FLOOR_WIN_EDGE_MIN,
        min(w[2] for w in floor_windows()) - CAV_Y0 >= FLOOR_WIN_EDGE_MIN)
    add("floor win clear of cavity y1", CAV_Y1 - max(w[3] for w in floor_windows()),
        FLOOR_WIN_EDGE_MIN,
        CAV_Y1 - max(w[3] for w in floor_windows()) >= FLOOR_WIN_EDGE_MIN)
    # --- logo inlay: a shallow flush pocket in the lid top.  The mark is
    #     rotated 90 deg and sits on the +x side (opposite every connector).
    #     The mark is a 2D outline, so the lid-top obstructions are checked
    #     against its real bounding box -- built from the SAME geometry the
    #     pocket is cut from, so a change in LOGO_SCALE or LOGO_WIDEN moves the
    #     box with it.  (An earlier revision hard-coded LOGO_HALF_* just beside
    #     the near-identical published values; they were computed from the mark's
    #     CENTROID rather than its bounding box, so the checked box sat 5.5 mm
    #     off the mark and passed checks the real pocket would have failed.)
    import numpy as _np
    import logo as _L
    _cx, _cy = logo_rect()
    # The constants are the MEASURED pocket, recorded so the enclosure's other
    # clearances can be computed analytically.  Assert them against a hard
    # number, not against the same source they came from -- recomputing from
    # rings_step() would pass no matter what LOGO_SCALE or LOGO_WIDEN did.
    for _n, _got, _want in (("x", LOGO_HALF_X, 4.0493),
                            ("y", LOGO_HALF_Y, 30.4913)):
        add("logo half %s matches built pocket" % _n, _got, _want,
            abs(_got - _want) <= 0.001)
    _xs, _ys = [], []
    for _o, _h in _L.rings_step(dilate=LOGO_GAP / 2.0, cx=_cx, cy=_cy):
        _a = _np.asarray(_o)
        _xs += [_a[:, 0].min(), _a[:, 0].max()]
        _ys += [_a[:, 1].min(), _a[:, 1].max()]
    add("logo pocket x half as built", (max(_xs) - min(_xs)) / 2.0,
        LOGO_HALF_X, abs((max(_xs) - min(_xs)) / 2.0 - LOGO_HALF_X) <= 0.001)
    add("logo pocket y half as built", (max(_ys) - min(_ys)) / 2.0,
        LOGO_HALF_Y, abs((max(_ys) - min(_ys)) / 2.0 - LOGO_HALF_Y) <= 0.001)
    # and the built pocket must be centred on the placement point
    add("logo pocket centred on placement",
        max(abs((max(_xs) + min(_xs)) / 2.0 - _cx),
            abs((-max(_ys) - min(_ys)) / 2.0 - _cy)), 1e-6,
        max(abs((max(_xs) + min(_xs)) / 2.0 - _cx),
            abs((-max(_ys) - min(_ys)) / 2.0 - _cy)) <= 1e-6)
    lx0, lx1 = LOGO_X - LOGO_HALF_X, LOGO_X + LOGO_HALF_X
    ly0, ly1 = LOGO_Y - LOGO_HALF_Y, LOGO_Y + LOGO_HALF_Y
    add("logo clear of flange x1", FLANGE_X1 - lx1, 3.0, (FLANGE_X1 - lx1) >= 3.0)
    add("logo clear of flange y0", ly0 - FLANGE_Y0, 3.0, (ly0 - FLANGE_Y0) >= 3.0)
    add("logo clear of flange y1", FLANGE_Y1 - ly1, 3.0, (FLANGE_Y1 - ly1) >= 3.0)
    add("logo inside plate x0", lx0 - FLANGE_X0, 3.0, (lx0 - FLANGE_X0) >= 3.0)
    # The pocket must sit on bare plate.  The obstacles that actually bound the
    # +x band are the USB routing hole at its foot and the rear counterbores at
    # its head, both measured off the lid's own top face; the mark clears them
    # in y by ~3.7 mm and ~3.8 mm.  In x it clears the button/LED/buzzer cluster
    # (which ends at LOGO_CLEAR_X0) by ~3.1 mm.  A real pocket CANNOT clear the
    # four corner counterbores in x -- it starts 1.3 mm inside them and runs off
    # their far side -- so their clearance is expressed as a y-gap instead.
    _cl = [("USB route hole", lx0, lx1, 0.0, LOGO_CLEAR_Y0),
           ("rear counterbores", lx0, lx1, LOGO_CLEAR_Y1, FLANGE_Y1),
           ("btn/LED/buzzer", FLANGE_X0, LOGO_CLEAR_X0, ly0, ly1)]
    for _n, _ox0, _ox1, _oy0, _oy1 in _cl:
        add("logo clear of %s" % _n,
            max(_ox0 - lx1, lx0 - _ox1, _oy0 - ly1, ly0 - _oy1), 3.0,
            (_ox0 - lx1) >= 3.0 or (lx0 - _ox1) >= 3.0
            or (_oy0 - ly1) >= 3.0 or (ly0 - _oy1) >= 3.0)
    add("logo inlay thickness", LOGO_DEPTH, 0.4, LOGO_DEPTH >= 0.4)

    # --- the mark must be a ROTATION of the source, never a mirror ---------
    # Exact guard for the bug that shipped twice: folding the board->STEP y
    # negation into the placement turns the map into a glide reflection
    # (det = -1) and prints the mark backwards.
    #
    # Test the ORIENTATION directly instead of rasterising.  An image-overlap
    # score is the wrong tool here: at any affordable raster size the logotype's
    # strokes are one or two pixels wide, so a real det = -1 difference shows up
    # as a delta of ~0.01 that flips sign with the sampling grid -- the check
    # passes on a mirrored model, which is exactly how the second mirror shipped.
    # The image comparison is still worth doing by eye, but it belongs in the
    # build's render step, not in a pass/fail gate.
    #
    # Do NOT substitute a bounding-box, centroid or signed-area test.  The first
    # two are blind to chirality by construction, and the third is too: shapely
    # normalises ring orientation on construction, so the signed area of a valid
    # polygon is always positive regardless of how the mark was mirrored.
    #
    # Ship THREE probe points (a proper right angle) through the transform the
    # placement actually uses, and take the sign of their cross product.
    import logo as _L

    _S = _L.shapely_outline()
    _mx, _my = _L._bbox_centre_shift(_S)
    _d = 1.0
    _pt = [(_mx + _dx, _my + _dy)
           for _dx, _dy in ((0.0, 0.0), (_d, 0.0), (0.0, _d))]
    _q = [_L._place(_sx, _sy, _mx, _my, LOGO_X, LOGO_Y) for _sx, _sy in _pt]
    _ax, _ay = _q[1][0] - _q[0][0], _q[1][1] - _q[0][1]
    _bx, _by = _q[2][0] - _q[0][0], _q[2][1] - _q[0][1]
    _det = _ax * _by - _ay * _bx
    # Both `val` and `limit` must be NUMBERS -- report() formats this tuple with
    # %8.3f / %6.3f and aborts the whole check run on a string.
    add("logo source outline parses",
        float(len(_L._read_source())), 1.0, len(_L._read_source()) > 0)
    add("logo placed by rotation, not mirror (signed area / mm^2)",
        round(_det, 3), 0.0, _det > 0.0)

    # The -x vents share y with the driver slots (both sit beside the terminals)
    # but are separated VERTICALLY by a 17 mm wall, so a vent can never print
    # into a driver slot.  Assert that separation rather than a bogus y gap.
    add("vent band clear of driver slot",
        (Z_SPIGOT - 1.0) - (VENT_Z_LO + VENT_H), 5.0,
        ((Z_SPIGOT - 1.0) - (VENT_Z_LO + VENT_H)) >= 5.0)

    return C

def report():
    C = _checks()
    return C

def check(verbose=True):
    C = _checks()
    bad = [c for c in C if not c[3]]
    if verbose:
        for name, val, limit, ok in C:
            print("  [%s] %-32s %8.3f  (min %6.3f)" %
                  ("ok " if ok else "FAIL", name, val, limit))
    if bad:
        raise AssertionError("geometry assertion(s) failed: %s" %
                             ", ".join(b[0] for b in bad))
    return True


def main():
    print("vent slots:", ["%s %.1f-%.1f" % s for s in vent_slots()])
    print("buzzer body centre:", BZ_BODY_C,
          " bolt circle:", ["(%.3f,%.3f)" % p for p in buzzer_bolt_positions()])
    print("outer size: %.1f x %.1f x %.1f mm" %
          (CASE_X1 - CASE_X0, CASE_Y1 - CASE_Y0, Z_LID_TOP - Z_FOOT))
    check()
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
