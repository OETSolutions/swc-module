"""
SWC Adapter Enclosure -- FreeCAD solid builders.

    base = build_base()
    lid  = build_lid()
    logo = build_logo()      # third body: inlaid in the lid top

Everything is driven from casegeom.py.  Modelling order follows the
freecad-modeling-order skill: primitives -> placement -> booleans -> shell ->
patterns -> **fillet/chamfer last** -> verify -> export.

Can be run headless:
    /Applications/FreeCAD.app/Contents/Resources/bin/freecadcmd build_case.py
or exec'd inside the running GUI instance (which is what build.sh does).
"""

import math
import Part
from FreeCAD import Base

import casegeom as G
import logo as L

T = G.BOARD_T


# ----------------------------------------------------------------------------
# frame helpers
# ----------------------------------------------------------------------------
def Z(bz):
    """board-frame z -> STEP z (STEP z origin is the board's BOTTOM face)."""
    return bz + T


def V(x, y, bz):
    return Base.Vector(x, -y, bz + T)


def fuse_all(shapes):
    shapes = [s for s in shapes if s is not None]
    if not shapes:
        return None
    if len(shapes) == 1:
        return shapes[0]
    try:
        return Part.MultiFuse(shapes)
    except Exception:
        s = shapes[0]
        for o in shapes[1:]:
            s = s.fuse(o)
        return s


# ----------------------------------------------------------------------------
# 2D profile helpers
# ----------------------------------------------------------------------------
def _wire(edges):
    try:
        return Part.Wire(edges)
    except Exception:
        return Part.Wire(Part.__sortEdges__(edges))


def _rrect_edges(map2d, a0, a1, b0, b1, r):
    """CCW rounded rectangle in the (a,b) plane, mapped through map2d(a,b)."""
    e = []

    def line(ax, ay, bx, by):
        e.append(Part.LineSegment(map2d(ax, ay), map2d(bx, by)).toShape())

    def arc(ca, cb, s0, s1, n=8):
        pts = [map2d(ca + r * math.cos(math.radians(s0 + (s1 - s0) * t / n)),
                     cb + r * math.sin(math.radians(s0 + (s1 - s0) * t / n)))
               for t in range(n + 1)]
        e.append(Part.BSplineCurve(pts).toShape())

    line(a1, b0 + r, a1, b1 - r);      arc(a1 - r, b1 - r, 0, 90)
    line(a1 - r, b1, a0 + r, b1);      arc(a0 + r, b1 - r, 90, 180)
    line(a0, b1 - r, a0, b0 + r);      arc(a0 + r, b0 + r, 180, 270)
    line(a0 + r, b0, a1 - r, b0);      arc(a1 - r, b0 + r, 270, 360)
    return e


def _rrect_face(plane, p, a0, a1, b0, b1, r):
    """plane in {'XY','YZ','XZ'}; p = the constant ordinate (STEP units)."""
    if plane == 'XY':
        m = lambda a, b: Base.Vector(a, b, p)
    elif plane == 'YZ':
        m = lambda a, b: Base.Vector(p, a, b)
    else:  # XZ
        m = lambda a, b: Base.Vector(a, p, b)
    return Part.Face(_wire(_rrect_edges(m, a0, a1, b0, b1, r)))


# ----------------------------------------------------------------------------
# 3D primitive helpers  (all board-frame inputs)
# ----------------------------------------------------------------------------
def prism(x0, x1, y0, y1, r, z0, z1):
    """Rounded-rect prism, board-frame x/y/z.  STEP y is negated."""
    f = _rrect_face('XY', Z(z0), x0, x1, -y1, -y0, r)
    return f.extrude(Base.Vector(0, 0, z1 - z0))


def rrect_yz(y0, y1, z0, z1, r, x0, x1):
    """Opening cut through an x-normal wall (board-frame y/z, extruded in x)."""
    f = _rrect_face('YZ', x0, -y1, -y0, Z(z0), Z(z1), r)
    return f.extrude(Base.Vector(x1 - x0, 0, 0))


def rrect_xz(x0, x1, z0, z1, r, y0, y1):
    """Opening cut through a y-normal wall (board-frame x/z, extruded in y)."""
    f = _rrect_face('XZ', -y0, x0, x1, Z(z0), Z(z1), r)
    return f.extrude(Base.Vector(0, y0 - y1, 0))


def rrect_xy(x0, x1, y0, y1, r, z0, z1):
    """Opening cut through a z-normal face / slab (board-frame x/y)."""
    f = _rrect_face('XY', Z(z0), x0, x1, -y1, -y0, r)
    return f.extrude(Base.Vector(0, 0, z1 - z0))


def rrect_cone(x0, x1, y0, y1, r, c, z0, z1):
    """Rounded-rect lead-in: the profile at z0 is (w + 2c) and at z1 it is w.

    Used for the 45 deg underside chamfer on the floor silkscreen windows --
    a plain linear loft between two rounded rectangles of the same corner
    radius, which is exactly a constant-width chamfer all the way round.
    """
    lo = _rrect_face('XY', Z(z0), x0 - c, x1 + c, -y1 - c, -y0 + c, r + c)
    hi = _rrect_face('XY', Z(z1), x0, x1, -y1, -y0, r)
    # args are (wires, solid, ruled) -- solid MUST be True or the loft comes
    # back as an open shell and every later boolean on it is a Null shape.
    # RULED, because the two profiles are a rounded rect and its offset, which
    # are not similar figures: letting OCCT smooth between them makes the corner
    # arcs bulge outward past the prism (four ~2.2 mm2 stabs per window, seen as
    # the base reaching z -8.90 instead of -8.40).  Straight ruling gives the
    # constant-width 45 deg chamfer that is actually wanted.
    return Part.makeLoft([lo.Wires[0], hi.Wires[0]], True, True)


def cyl(d, z0, z1, x, y):
    return Part.makeCylinder(d / 2.0, z1 - z0,
                             Base.Vector(x, -y, Z(z0)), Base.Vector(0, 0, 1))


def cone(d0, d1, z0, z1, x, y):
    return Part.makeCone(d0 / 2.0, d1 / 2.0, z1 - z0,
                         Base.Vector(x, -y, Z(z0)), Base.Vector(0, 0, 1))


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0,
                        Base.Vector(x0, -y1, Z(z0)))


# ----------------------------------------------------------------------------
# logo geometry (shared by the lid pocket cut and the separate inlay body)
# ----------------------------------------------------------------------------
def logo_place():
    """(cx, cy) the logo is centred on, in board frame."""
    return G.logo_rect()


def logo_faces(dilate=0.0):
    """One planar Face per letterform island of the (thickened, scaled, rotated)
    mark, in the STEP XY plane at z=0.

    The mark is a set of separate islands (see logo.rings_step), and each island
    carries its own counter-holes.  The footprint's outlines self-intersect and
    are rebuilt by the even-odd rule, exactly how KiCad renders them, so the
    counters (the O, the E, the R, the D, the 'o' bowls and the gear's eye) come
    back as real holes -- without them the mark would print as a blob.
    """
    cx, cy = logo_place()
    islands = L.rings_step(dilate=dilate, cx=cx, cy=cy)
    def mk(coords):
        return Part.Wire(Part.__sortEdges__(Part.makePolygon(
            [Base.Vector(x, y, 0.0) for x, y in coords]).Edges))
    return [Part.Face([mk(outer)] + [mk(h) for h in holes])
            for outer, holes in islands]


def logo_solid(z0, z1, dilate=0.0):
    """The extruded logo body between board-frame z0 and z1, as ONE solid.

    Each island is extruded on its own and the results fused: extruding them
    together as a compound yields SHELLS, not solids, and a boolean cut against
    a shell silently mangles the lid instead of pocketing it.
    """
    dz = Z(z1) - Z(z0)
    solid = None
    for f in logo_faces(dilate):
        v = f.extrude(Base.Vector(0, 0, dz)) \
             .translate(Base.Vector(0, 0, Z(z0)))
        solid = v if solid is None else solid.fuse(v)
    return solid.removeSplitter()


# ----------------------------------------------------------------------------
# finishing helpers (always applied LAST)
# ----------------------------------------------------------------------------
def edge_ids(shape, pred):
    """Return the Edge objects of `shape` matching pred (makeFillet wants Edges)."""
    out = []
    for e in shape.Edges:
        try:
            if pred(e):
                out.append(e)
        except Exception:
            pass
    return out


def chamfer_bottom_perimeter(shape, width):
    """0.6 x 45deg elephant-foot chamfer on every first-layer (lowest) edge."""
    zlo = Z(G.Z_FOOT)
    ids = edge_ids(shape, lambda e: abs(e.BoundBox.ZMin - zlo) < 1e-6
                   and abs(e.BoundBox.ZMax - zlo) < 1e-6
                   and e.BoundBox.XMax - e.BoundBox.XMin
                   + e.BoundBox.YMax - e.BoundBox.YMin > 0.5)
    if not ids:
        return shape, 0
    try:
        return shape.makeChamfer(width, ids), len(ids)
    except Exception as ex:
        print("   ! elephant-foot chamfer skipped:", ex)
        return shape, 0


def fillet_outer_top(shape, zface, radius):
    """Round only the OUTER top perimeter (not every hole rim)."""
    zf = Z(zface)
    x0, x1 = G.FLANGE_X0, G.FLANGE_X1
    y0, y1 = -G.FLANGE_Y1, -G.FLANGE_Y0        # STEP y
    ids = edge_ids(shape, lambda e: abs(e.BoundBox.ZMin - zf) < 1e-6
                   and abs(e.BoundBox.ZMax - zf) < 1e-6
                   and (abs(e.BoundBox.XMin - x0) < 0.01
                        or abs(e.BoundBox.XMax - x1) < 0.01
                        or abs(e.BoundBox.YMin - y0) < 0.01
                        or abs(e.BoundBox.YMax - y1) < 0.01))
    if not ids:
        return shape, 0
    try:
        return shape.makeFillet(radius, ids), len(ids)
    except Exception as ex:
        print("   ! top fillet skipped:", ex)
        return shape, 0


# ----------------------------------------------------------------------------
# BASE
# ----------------------------------------------------------------------------
ELEPHANT = 0.4          # 0.4 x 45 deg first-layer chamfer on the bottom face
                        # (0.6 was too greedy for a 0.4 nozzle: it wants 3 layers)
SPIGOT_T = 2.0          # lid spigot ring wall thickness (constant, corners too)


def build_base():
    # --- outer shell: a plain box with a pocket milled into it ---------------
    #     Slab = Z_FOOT .. Z_FLOOR_TOP (2.4 mm); its underside is a single flat
    #     rectangle, so the case sits dead flat on the bed and takes one chamfer
    #     pass for elephant-foot relief.  The floor also has to clear the
    #     terminal-block pins (NOT trimmed, -4.415) -- the buzzer pins are
    #     trimmed to -5.00, so they are not the constraint on slab thickness.
    outer = prism(G.CASE_X0, G.CASE_X1, G.CASE_Y0, G.CASE_Y1, G.CASE_R,
                  G.Z_FOOT, G.Z_RIM)
    # Elephant-foot relief goes on the OUTER SHELL, here, not last: the floor
    # silkscreen windows also break the foot plane, and chamfering the perimeter
    # of a face that already carries four internal holes makes OCCT give up
    # (StdFail_NotDone).  On the plain prism it is a trivial 4-line + 4-arc
    # chamfer, and every later cut is >= 7 mm inboard of it, so nothing can
    # interact with it.
    outer, n_el = chamfer_bottom_perimeter(outer, ELEPHANT)
    cav = prism(G.CAV_X0, G.CAV_X1, G.CAV_Y0, G.CAV_Y1, G.CAV_R,
                G.Z_FLOOR_TOP, G.Z_RIM)
    s = outer.cut(cav)

    # --- corner bosses: the board rests on these; the screw thread forms here
    towers = [cyl(G.TOWER_D, G.Z_FLOOR_TOP, G.BOSS_TOP_Z, hx, hy)
              for (hx, hy) in G.MOUNT_HOLES]
    s = s.fuse(fuse_all(towers))

    cuts = []

    # --- antenna: thin the solid front wall to 1.4 mm over the antenna
    cuts.append(box(37.7, 54.8, G.CAV_Y0, G.CAV_Y0 + 0.6,
                    G.Z_FLOOR_TOP, G.Z_RIM))

    # --- 12 wire windows through the left wall
    for y in G.BORE_Y:
        cuts.append(rrect_yz(y - G.WIRE_WIN_W / 2, y + G.WIRE_WIN_W / 2,
                             G.WIRE_WIN_Z0, G.WIRE_WIN_Z1, G.WIRE_WIN_R,
                             G.CASE_X0 - 0.5, G.CAV_X0 + 0.5))

    # --- USB-C opening through the left wall
    cuts.append(rrect_yz(G.USBC_WIN_Y0, G.USBC_WIN_Y1,
                         G.USBC_WIN_Z0, G.USBC_WIN_Z1, 0.5,
                         G.CASE_X0 - 0.5, G.CAV_X0 + 0.5))

    # --- vents, all low so the air crosses the board at the height of the
    #     parts: 3 on +x beside RT1, 1 on -x as the opposed inlet (the user
    #     asked for both sides around RT1), 2 around the corner on the y=133
    #     end wall.
    for wall, a0, a1 in G.vent_slots():
        if wall == "X":                                  # +x long wall
            cuts.append(rrect_yz(a0, a1, G.VENT_Z_LO, G.VENT_Z_LO + G.VENT_H,
                                 G.VENT_R, G.CAV_X1 - 0.5, G.CASE_X1 + 0.5))
        elif wall == "-X":                               # -x long wall (inlet)
            cuts.append(rrect_yz(a0, a1, G.VENT_Z_LO, G.VENT_Z_LO + G.VENT_H,
                                 G.VENT_R, G.CASE_X0 - 0.5, G.CAV_X0 + 0.5))
        else:                                            # y=133 end wall
            cuts.append(rrect_xz(a0, a1, G.VENT_Z_LO, G.VENT_Z_LO + G.VENT_H,
                                 G.VENT_R, G.CAV_Y1 - 0.5, G.CASE_Y1 + 0.5))

    # --- floor silkscreen windows: the board's B.SilkS terminal titles are read
    #     by looking UP through the bottom slab.  Each window is chamfered on the
    #     UNDERSIDE (the foot face) so the text reads at an oblique angle as well
    #     as straight-on.  The window is cut before the pilot bores and before
    #     the elephant-foot chamfer, so both later passes see the finished edge.
    for (wx0, wx1, wy0, wy1) in G.floor_windows():
        cuts.append(rrect_xy(wx0, wx1, wy0, wy1, G.FLOOR_WIN_R,
                             G.Z_FOOT - 0.5, G.Z_FLOOR_TOP + 0.5))
        # underside lead-in chamfer: a rounded-rect cone from (w + 2c) at the
        # FOOT FACE down to w at (Z_FOOT + c), i.e. 45 deg all the way round.
        # The wide profile must sit exactly at Z_FOOT: put it any lower and the
        # taper is wider than the window all the way up through the slab, which
        # scours a hidden void out of the floor above the chamfer.
        cuts.append(rrect_cone(wx0, wx1, wy0, wy1, G.FLOOR_WIN_R,
                               G.FLOOR_WIN_CHAMFER,
                               G.Z_FOOT, G.Z_FOOT + G.FLOOR_WIN_CHAMFER))

    s = s.cut(fuse_all(cuts))

    # --- 4 M5 pilot holes, drilled AFTER the boss join so the bore wall
    #     belongs to one solid instead of being split by the join face.
    #     The pilot is BLIND: it stops at PILOT_FLOOR_Z, leaving 1.0 mm of floor
    #     under it, so the floor has no hole through it and the screw cannot
    #     poke out of the bottom of the case.
    pilots = []
    for (hx, hy) in G.MOUNT_HOLES:
        pilots.append(cyl(G.PILOT_D, G.PILOT_FLOOR_Z, G.PILOT_TOP_Z, hx, hy))
        pilots.append(cone(G.PILOT_D + 2 * G.PILOT_ENTRY_CHAMFER, G.PILOT_D,
                           G.PILOT_TOP_Z - G.PILOT_ENTRY_CHAMFER,
                           G.PILOT_TOP_Z + 0.01, hx, hy))
    s = s.cut(fuse_all(pilots))
    s = s.removeSplitter()

    # ---- FINISHING --------------------------------------------------------
    s = s.removeSplitter()
    print("   base: first-layer chamfer 0.4 x 45 on %d outer-shell edges" % n_el)
    return s


# ----------------------------------------------------------------------------
# LID
# ----------------------------------------------------------------------------
def build_lid():
    plate = prism(G.FLANGE_X0, G.FLANGE_X1, G.FLANGE_Y0, G.FLANGE_Y1,
                  G.CASE_R, G.Z_RIM, G.Z_LID_TOP)

    # spigot ring: outer = cavity inset 0.2, 2.0 mm thick, z 22..24
    # The INNER prism takes corner radius CAV_R + SPIGOT_T, not CAV_R.  Both
    # prisms are concentric, but the inner one is inset SPIGOT_T and its corner
    # arc centre sits SPIGOT_T inside the outer arc centre, so an equal radius
    # makes the inner arc BULGE outward by SPIGOT_T*(sqrt(2) - 1) = 0.083 mm at
    # each of the four corners.  The outer prism then cuts that bulge into four
    # loose slivers, which survive as ~0.65 mm notches scarred into the ring's
    # inner corner.  Growing the inner radius by SPIGOT_T keeps the wall a
    # constant 2.0 mm right through the corners and the cut comes out clean.
    spi_out = prism(G.SPI_X0, G.SPI_X1, G.SPI_Y0, G.SPI_Y1, G.CAV_R,
                    G.Z_SPIGOT, G.Z_RIM)
    spi_in = prism(G.SPI_X0 + SPIGOT_T, G.SPI_X1 - SPIGOT_T,
                   G.SPI_Y0 + SPIGOT_T, G.SPI_Y1 - SPIGOT_T,
                   G.CAV_R + SPIGOT_T,
                   G.Z_SPIGOT - 0.5, G.Z_RIM + 0.5)
    spigot = spi_out.cut(spi_in)

    sleeves = [cyl(G.SLEEVE_D, G.Z_BOARD_TOP, G.Z_LID_TOP, hx, hy)
               for (hx, hy) in G.MOUNT_HOLES]

    s = fuse_all([plate, spigot] + sleeves)

    cuts = []

    # --- terminal screwdriver access: one rounded slot per block
    for (y0, y1) in G.driver_slots():
        cuts.append(rrect_yz(y0, y1, G.Z_SPIGOT - 1.0, G.Z_LID_TOP + 0.5, 1.0,
                             G.DRIVER_SLOT_X - G.DRIVER_SLOT_W / 2,
                             G.DRIVER_SLOT_X + G.DRIVER_SLOT_W / 2))

    # --- 2 LED windows
    for (cx, cy) in (G.D6_C, G.D12_C):
        cuts.append(cyl(G.LED_HOLE_D, G.Z_RIM - 2.0, G.Z_LID_TOP + 0.5, cx, cy))
        cuts.append(cyl(G.LED_RECESS_D, G.Z_LID_TOP - G.LED_RECESS_DEPTH,
                        G.Z_LID_TOP + 0.5, cx, cy))

    # --- 2 button access holes
    for (cx, cy) in (G.SW1_C, G.SW2_C):
        cuts.append(cyl(G.BTN_HOLE_D, G.Z_RIM - 2.0, G.Z_LID_TOP + 0.5, cx, cy))
        cuts.append(cyl(G.BTN_RECESS_D, G.Z_LID_TOP - G.BTN_RECESS_DEPTH,
                        G.Z_LID_TOP + 0.5, cx, cy))

    # --- buzzer sound port: centre + 6 on a bolt circle, over the body centre
    bx, by = G.BZ_BODY_C
    cuts.append(cyl(G.BZ_PORT_C_D, G.Z_SPIGOT - 1.0, G.Z_LID_TOP + 0.5, bx, by))
    for (px, py) in G.buzzer_bolt_positions():
        cuts.append(cyl(G.BZ_BOLT_D, G.Z_SPIGOT - 1.0, G.Z_LID_TOP + 0.5, px, py))

    # --- 4 M5 clearance holes + counterbores.
    #     The counterbore is cut straight into the lid plate; its floor lands
    #     9.0 mm above the plate's underside, inside the 11 mm sleeve, so it
    #     does not break through the 4 mm plate.  Depth = the 5 mm head height,
    #     so an off-the-shelf M5 x 30 SHCS sits FLUSH with the lid top (no rim,
    #     which also keeps the flipped lid 100% on the bed when printing).
    cb_top = G.Z_LID_TOP + G.CBORE_RAISE
    for (hx, hy) in G.MOUNT_HOLES:
        cuts.append(cyl(G.LID_SCREW_D, G.Z_BOARD_TOP - 0.5, cb_top + 0.5,
                        hx, hy))
        cuts.append(cyl(G.CBORE_D, cb_top - G.CBORE_DEPTH, cb_top + 0.5,
                        hx, hy))

    s = s.cut(fuse_all(cuts))
    s = s.removeSplitter()

    # --- logo pocket: a matching recess in the lid TOP for the third body.
    #     0.025 mm clearance per side, so the two weld into one part and the
    #     logo's top face is flush with the lid's.
    s = s.cut(logo_solid(G.Z_LID_TOP - G.LOGO_DEPTH, G.Z_LID_TOP + 0.5,
                         dilate=G.LOGO_GAP / 2.0))
    s = s.removeSplitter()

    # ---- FINISHING (last) ------------------------------------------------
    s, n = fillet_outer_top(s, G.Z_LID_TOP, 0.8)
    print("   lid: top-edge fillet on %d edges" % n)
    return s


# ----------------------------------------------------------------------------
# LOGO (third body: printed in a contrasting colour, swapped in at its layers)
# ----------------------------------------------------------------------------
def build_logo():
    """The OET Solutions mark, sized to drop into the lid's pocket flush with
    the lid top.  Extruded from Z_LID_TOP - LOGO_DEPTH to Z_LID_TOP."""
    return logo_solid(G.Z_LID_TOP - G.LOGO_DEPTH, G.Z_LID_TOP)


# ----------------------------------------------------------------------------
def build_all():
    return build_base(), build_lid(), build_logo()


if __name__ == "__main__":
    import FreeCAD as App
    G.check()
    b, l, g = build_all()
    for nm, sh in (("Case_Body", b), ("Case_Lid", l), ("Logo_Inlay", g)):
        bb = sh.BoundBox
        print("%s: valid=%s solids=%d faces=%d vol=%.0f mm3  "
              "X %.2f..%.2f Y %.2f..%.2f Z %.2f..%.2f"
              % (nm, sh.isValid(), len(sh.Solids), len(sh.Faces), sh.Volume,
                 bb.XMin, bb.XMax, bb.YMin, bb.YMax, bb.ZMin, bb.ZMax))
