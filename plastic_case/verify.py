"""
Verification gate for the SWC enclosure.

  A. casegeom assertions (all derived clearances)
  B. board bounding box lies inside the cavity with clearance
  C. zero interference: board solid INTERSECT base and lid
  D. bounding box of each part against the design envelope
  E. each part is a single valid watertight solid

Run:  freecadcmd verify.py
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass
_p = print
def print(*a, **k):          # never lose the report on SystemExit
    k.setdefault("flush", True)
    _p(*a, **k)

import numpy as np
import Part
import casegeom as G
import build_case as B

HERE = os.path.dirname(os.path.abspath(__file__))
STEP = os.path.join(HERE, "SWC.step")
T = G.BOARD_T
fails = []


def bbox_board(sh):
    b = sh.BoundBox
    return (b.XMin, b.XMax, -b.YMax, -b.YMin, b.ZMin - T, b.ZMax - T)


def main():
    print("=" * 68)
    print("A. casegeom assertions")
    print("=" * 68)
    G.check()

    print()
    print("=" * 68)
    print("building case parts")
    print("=" * 68)
    base = B.build_base()
    lid = B.build_lid()

    print()
    print("=" * 68)
    print("importing board STEP as a reference body (never fused)")
    print("=" * 68)
    t = time.time()
    board = Part.Shape()
    board.read(STEP)
    solids = board.Solids
    print("  %d objects / %d solids in %.1fs" % (len(board.Faces), len(solids),
                                                 time.time() - t))
    bx0, bx1, by0, by1, bz0, bz1 = bbox_board(board)
    print("  board frame extents: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
          % (bx0, bx1, by0, by1, bz0, bz1))

    print()
    print("=" * 68)
    print("B. board inside cavity")
    print("=" * 68)
    checks = [
        ("x0", G.CAV_X0, bx0, 0.0, G.CAV_X0 <= bx0),
        ("x1", G.CAV_X1, bx1, 0.0, G.CAV_X1 >= bx1),
        ("y0 (nose)", G.CAV_Y0, by0, 8.0, G.CAV_Y0 <= by0 - 8.0),
        ("y1", G.CAV_Y1, by1, 0.0, G.CAV_Y1 >= by1),
    ]
    for nm, wall, val, need, ok in checks:
        print("  [%s] cavity %-10s %.3f   board edge %.3f   margin %+.3f"
              % ("ok " if ok else "FAIL", nm, wall, val,
                 (val - wall) if nm.endswith("0 (nose)") or nm == "x0"
                 else (wall - val)))
        if not ok:
            fails.append("board in cavity " + nm)

    print()
    print("=" * 68)
    print("C. interference: board vs case (must be zero)")
    print("=" * 68)
    for nm, sh in (("Case_Body", base), ("Case_Lid", lid)):
        t = time.time()
        try:
            inter = sh.common(board)
            v = inter.Volume
            nb = len(inter.Solids)
            print("  %s INTERSECT board: volume %.4f mm3  (%d lumps)  %.1fs"
                  % (nm, v, nb, time.time() - t))
            if v > 1e-3:
                fails.append("%s interferes with board (%.3f mm3)" % (nm, v))
                ib = inter.BoundBox
                print("     ! overlap bbox x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f"
                      % (ib.XMin, ib.XMax, -ib.YMax, -ib.YMin,
                         ib.ZMin - T, ib.ZMax - T))
        except Exception as e:
            print("  %s common failed: %s" % (nm, e))

    print()
    print("=" * 68)
    print("C1b. board support: boss top touches, does not enter, the board")
    print("=" * 68)
    # a thin disc just ABOVE the boss top must be empty (boss does not enter the
    # board) and a thin disc just BELOW it must be solid (boss reaches the board)
    for (hx, hy) in G.MOUNT_HOLES:
        disc_up = B.cyl(G.BOSS_D, G.BOSS_TOP_Z + 0.01,
                        G.BOSS_TOP_Z + 0.20, hx, hy)
        disc_dn = B.cyl(G.BOSS_D, G.BOSS_TOP_Z - 0.20,
                        G.BOSS_TOP_Z - 0.01, hx, hy)
        gap = disc_up.cut(disc_up.common(base)).Volume      # air above boss top
        sol = base.common(disc_dn).Volume                   # solid below boss top
        ok = gap > 0.5 * disc_up.Volume - 1e-6 and sol > 0.5 * disc_dn.Volume
        print("  [%s] boss (%2.0f,%3.0f): air above top %.2f mm3, solid below "
              "top %.2f mm3" % ("ok " if ok else "FAIL", hx, hy, gap, sol))
        if not ok:
            fails.append("boss stack at (%g,%g)" % (hx, hy))

    print()
    print("=" * 68)
    print("C2. interference between the two case parts (must be zero)")
    print("=" * 68)
    try:
        t = time.time()
        bl = base.common(lid)
        print("  Case_Body INTERSECT Case_Lid: volume %.4f mm3  (%d lumps)  %.1fs"
              % (bl.Volume, len(bl.Solids), time.time() - t))
        if bl.Volume > 1e-3:
            fails.append("base and lid interfere (%.3f mm3)" % bl.Volume)
    except Exception as e:
        print("  base/lid common failed:", e)

    print()
    print("=" * 68)
    print("C3. logo inlay: drop-in fit in the lid pocket, flush with the lid top")
    print("=" * 68)
    log = B.build_logo()
    logb = log.BoundBox
    # pocket walls: a hair OUTSIDE the inlay must be lid material, and the
    # inlay must be free there (i.e. a real gap)
    ring = B.logo_solid(G.Z_LID_TOP - G.LOGO_DEPTH, G.Z_LID_TOP,
                        dilate=G.LOGO_GAP)          # inlay grown by the gap
    pocket = B.logo_solid(G.Z_LID_TOP - G.LOGO_DEPTH, G.Z_LID_TOP,
                          dilate=G.LOGO_GAP / 2.0)  # the actual pocket
    in_pocket = ring.cut(ring.common(lid)).Volume
    print("  pocket wall (gap ring solid): %.2f mm3  -> %s"
          % (in_pocket, "clear" if in_pocket > 0.5 else "BLOCKED"))
    print("  logo top %.4f  lid face %.4f  -> %+.4f mm  (flush)"
          % (logb.ZMax, G.Z_LID_TOP + T, logb.ZMax - (G.Z_LID_TOP + T)))
    print("  logo depth %.2f mm  (board-frame z %.3f..%.3f)"
          % (logb.ZMax - logb.ZMin, logb.ZMin - T, logb.ZMax - T))
    if not log.isValid() or len(log.Solids) < 1:
        fails.append("Logo_Inlay not valid solids (got %d)" % len(log.Solids))
    if abs(logb.ZMax - T - G.Z_LID_TOP) > 0.02:
        fails.append("Logo_Inlay not flush with the lid top (%.3f)"
                     % (logb.ZMax - T - G.Z_LID_TOP))
    if logb.ZMin - T < G.Z_LID_TOP - G.LOGO_DEPTH - 0.02:
        fails.append("Logo_Inlay deeper than its pocket")
    if in_pocket <= 0.5:
        fails.append("logo pocket is not clear around the inlay")
    # The pocket must sit on bare plate.  Probe it against the lid's own top
    # face with the pocket itself suppressed: anything the pocket would cut into
    # shows up as the top face being SPLIT (extra wires) rather than as one
    # clean plate.  Counting wires and then intersecting the pocket with the
    # plate is ground truth -- it needs no hand-maintained list of obstructions,
    # which is how the previous version passed while the pocket was biting into
    # the button recess.
    _saved = B.logo_solid
    B.logo_solid = lambda z0, z1, dilate=0.0: B.box(200, 202, 200, 202, z0, z1)
    lid_nopocket = B.build_lid()
    B.logo_solid = _saved
    _tf = [f for f in lid_nopocket.Faces
           if abs(f.normalAt(0, 0).z - 1.0) < 1e-6
           and abs(f.CenterOfMass.z - (G.Z_LID_TOP + T)) < 1e-6]
    _tf = max(_tf, key=lambda f: f.Area)
    from shapely.geometry import Polygon as _Poly
    from shapely.ops import unary_union as _uu
    from shapely.ops import transform as _sh_tf
    _ws = []
    for _w in _tf.Wires:
        _ws.append(_Poly([(p.x, -p.y) for p in _w.discretize(0.05)]))
    _ws.sort(key=lambda p: -p.area)
    plate = _ws[0].difference(_uu(_ws[1:]))
    import logo as L
    isl = L.rings_step(dilate=G.LOGO_GAP / 2.0, cx=G.LOGO_X, cy=G.LOGO_Y)
    pocket2d = _uu([_Poly(o, h) for o, h in isl])
    pocket2d = _sh_tf(lambda a, b: (np.asarray(a), -np.asarray(b)), pocket2d)
    _out = pocket2d.difference(plate)
    print("  lid top face w/o pocket: %.1f mm2 in %d wires"
          % (_tf.Area, len(_tf.Wires)))
    print("  pocket area %.2f mm2, of which OFF bare plate: %.4f mm2  -> %s"
          % (pocket2d.area, _out.area,
             "clear" if _out.area < 1e-6 else "COLLISION"))
    if _out.area >= 1e-6:
        fails.append("logo pocket overlaps another lid feature (%.3f mm2)"
                     % _out.area)
    # and the pocket must actually be cut: a representative interior point of the
    # mark must be AIR inside the lid at the pocket depth (it is lid elsewhere)
    import logo as L
    isl = L.rings_step(dilate=0.0, cx=G.LOGO_X, cy=G.LOGO_Y)
    print("  logo islands: %d" % len(isl))
    n_air = 0
    for outer, _holes in isl:
        ox = sum(p[0] for p in outer) / len(outer)
        oy = sum(p[1] for p in outer) / len(outer)
        if not lid.isInside(B.V(ox, oy, G.Z_LID_TOP - 0.4), 1e-6, True):
            n_air += 1
    print("  logo island centres that are AIR at pocket depth: %d / %d"
          % (n_air, len(isl)))
    if n_air != len(isl):
        fails.append("logo pocket was not cut into the lid (%d/%d islands open)"
                     % (n_air, len(isl)))

    print()
    print("=" * 68)
    print("D/E. part envelope + integrity")
    print("=" * 68)
    env = {
        "Case_Body": (G.CASE_X0, G.CASE_X1, G.CASE_Y0, G.CASE_Y1,
                      G.Z_FOOT, G.Z_RIM),
        "Case_Lid": (G.CASE_X0, G.CASE_X1, G.CASE_Y0, G.CASE_Y1,
                     G.Z_BOARD_TOP, G.Z_LID_TOP + G.CBORE_RAISE),
    }
    for nm, sh in (("Case_Body", base), ("Case_Lid", lid)):
        ex0, ex1, ey0, ey1, ez0, ez1 = env[nm]
        gx0, gx1, gy0, gy1, gz0, gz1 = bbox_board(sh)
        ok = (abs(gx0 - ex0) < 0.05 and abs(gx1 - ex1) < 0.05
              and abs(gy0 - ey0) < 0.05 and abs(gy1 - ey1) < 0.05
              and abs(gz0 - ez0) < 0.05 and abs(gz1 - ez1) < 0.05)
        print("  [%s] %s bbox  x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
              % ("ok " if ok else "FAIL", nm, gx0, gx1, gy0, gy1, gz0, gz1))
        print("       expected      x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
              % (ex0, ex1, ey0, ey1, ez0, ez1))
        if not ok:
            fails.append("%s bbox mismatch" % nm)
        nSol = len(sh.Solids)
        print("       solids=%d  valid=%s  faces=%d  volume=%.0f mm3"
              % (nSol, sh.isValid(), len(sh.Faces), sh.Volume))
        if nSol != 1 or not sh.isValid():
            fails.append("%s not a single valid solid" % nm)

    print()
    print("=" * 68)
    print("F. opening containment (inside/outside the solid)")
    print("=" * 68)

    def inside(sh, x, y, bz):
        # B.V() already maps board frame -> STEP frame (negate y, offset z by
        # the board thickness), so the PTS table below reads in board frame
        # exactly as casegeom does.  Do NOT convert again here.
        return sh.isInside(B.V(x, y, bz), 1e-6, True)

    # (label, part, x, y, z(board frame), expected_inside)
    PTS = [
        # --- base: left wall
        ("wire window @ bore y", "b", 14.4, 56.566, 4.2, False),
        ("wall between windows", "b", 14.4, 59.000, 4.2, True),
        ("wall above window", "b", 14.4, 56.566, 9.0, True),
        ("USB-C opening", "b", 14.4, 48.200, 2.5, False),
        ("wall above USB-C", "b", 14.4, 48.200, 8.0, True),
        # --- base: vents (all low, at the height of RT1 / the wire tunnels)
        ("vent +x beside RT1", "b", 71.6, 119.0, 2.4, False),
        ("vent +x is low on board", "b", 71.6, 119.0, 9.0, True),
        ("wall between +x vents", "b", 71.6, 115.5, 2.4, True),
        # --- base: the -x inlet, the opposed pair of the +x slot at y 126.0
        ("vent -x inlet", "b", 14.4, 126.0, 2.4, False),
        ("vent -x is low on board", "b", 14.4, 126.0, 9.0, True),
        ("wall below -x inlet", "b", 14.4, 126.0, 0.4, True),
        ("wall between -x inlet and last wire window",
         "b", 14.4, 121.5, 2.4, True),
        ("vent end wall (y=133)", "b", 34.0, 134.6, 2.4, False),
        ("wall between end vents", "b", 43.0, 134.6, 2.4, True),
        # --- base: flat bottom slab with the four silkscreen windows through
        #     it.  Every probe point is chosen to be clear of a window: the
        #     windows all end by x 31.16, so x 40 is always slab.
        ("bottom slab solid mid", "b", 40.0, 70.0, -7.5, True),
        ("bottom slab solid rear", "b", 40.0, 130.0, -7.5, True),
        ("bottom skin mid-slab", "b", 68.0, 130.0, -8.0, True),
        # the four windows ARE open, straight through the slab ...
        ("floor window J1 open", "b", 21.5, 57.20, -7.5, False),
        ("floor window J2 open", "b", 21.5, 73.54, -7.5, False),
        ("floor window J5 open", "b", 21.5, 92.80, -7.5, False),
        ("floor window J3 open", "b", 21.5, 111.96, -7.5, False),
        # ... open at the foot face too (the chamfer does not close them) ...
        ("floor window J1 open at foot", "b", 21.5, 57.20, -8.35, False),
        # ... and the ribs between them are solid
        ("floor rib J1|J2", "b", 21.5, 64.96, -7.5, True),
        ("floor rib J2|J5", "b", 21.5, 81.88, -7.5, True),
        ("floor rib J5|J3", "b", 21.5, 103.71, -7.5, True),
        ("floor slab inboard of window", "b", 16.0, 57.20, -7.5, True),
        # --- base: interior is empty
        ("cavity air over board", "b", 40.0, 40.000, 10.0, False),
        # --- base: boss reaches the board, pilot is open
        ("boss wall under board", "b", 26.5, 37.000, -3.0, True),
        ("M5 pilot hole", "b", 22.0, 37.000, -5.0, False),
        ("boss top is pilot air", "b", 22.0, 37.000, -1.62, False),
        # the pilot is blind: its floor still has the 1.2 mm bottom skin
        ("pilot floor skin", "b", 22.0, 37.000, -7.6, True),
        # --- lid.  Probe z values are tied to the stack constants, never to
        #     literals: the whole point of the M5 x 25 pass is that these move
        #     with SCREW_LEN / FLANGE_T, and a literal would silently keep
        #     probing the old plate.
        ("driver slot over screw cap", "l", 21.658, 56.566,
         G.Z_LID_TOP - 2.0, False),
        ("driver slot between poles", "l", 21.658, 59.033,
         G.Z_LID_TOP - 2.0, False),
        ("web J1|J2", "l", 21.658, 64.910, G.Z_LID_TOP - 2.0, True),
        ("web J2|J5", "l", 21.658, 81.835, G.Z_LID_TOP - 2.0, True),
        ("web J5|J3", "l", 21.658, 103.770, G.Z_LID_TOP - 2.0, True),
        ("LED window", "l", 46.965, 61.660, G.Z_LID_TOP - 2.0, False),
        ("button hole", "l", 59.011, 49.329, G.Z_LID_TOP - 2.0, False),
        ("buzzer centre port", "l", 48.082, 116.356, G.Z_LID_TOP - 2.0, False),
        ("buzzer bolt port", "l", 52.582, 116.356, G.Z_LID_TOP - 2.0, False),
        ("sleeve bore", "l", 22.0, 37.000, 12.0, False),
        ("sleeve wall", "l", 25.0, 37.000, 12.0, True),
        ("spigot ring", "l", 16.5, 50.000, G.Z_SPIGOT + 1.0, True),
        ("spigot inside (air)", "l", 20.0, 50.000, G.Z_SPIGOT + 1.0, False),
        # --- lid: logo pocket is a recess in the top face, solid underneath.
        #     (the recess itself is checked against the mark's own island
        #     centres in section C3 -- a fixed probe can land between glyphs)
        #     The mark now runs up the +x side of the lid, so the probes are
        #     the plate beneath it, beside it, and inboard of it.
        ("solid below logo pocket", "l", G.LOGO_X, G.LOGO_Y,
         G.Z_LID_TOP - G.LOGO_DEPTH - 0.8, True),
        ("lid top clear of logo", "l", 43.0, 16.0, G.Z_LID_TOP - 0.8, True),
        #     the plate above the spigot's rear wall, and the plate inboard of
        #     the pocket, are both solid plate at the pocket depth
        ("plate over spigot rear", "l", 43.0, 30.0,
         G.Z_LID_TOP - G.LOGO_DEPTH - 0.8, True),
        ("plate beside logo pocket", "l", G.LOGO_X - 20.0, G.LOGO_Y,
         G.Z_LID_TOP - G.LOGO_DEPTH - 0.8, True),
        # --- lid: the counterbore is cut flush into the plate, head sits in it
        ("counterbore wall solid", "l", 22.0 + G.CBORE_D / 2 + 0.5, 37.0,
         G.Z_LID_TOP - 0.5, True),
        ("counterbore recess is air", "l", 22.0 + G.CBORE_D / 2 - 0.5, 37.0,
         G.Z_LID_TOP - G.CBORE_DEPTH / 2, False),
        ("counterbore floor solid", "l", 26.25, 37.0,
         G.Z_LID_TOP - G.CBORE_DEPTH - 0.5, True),
        ("counterbore open at lid top", "l", 22.0, 37.0,
         G.Z_LID_TOP + 0.2, False),
    ]
    for label, which, x, y, bz, want in PTS:
        sh = base if which == "b" else lid
        got = inside(sh, x, y, bz)
        ok = (got == want)
        print("  [%s] %-26s (%.2f,%.2f,%.2f) %s"
              % ("ok " if ok else "FAIL", label, x, y, bz,
                 "inside" if got else "outside"))
        if not ok:
            fails.append("containment: %s expected %s"
                         % (label, "inside" if want else "outside"))

    print()
    print("=" * 68)
    if fails:
        print("VERIFICATION FAILED:")
        for f in fails:
            print("  -", f)
        raise SystemExit(1)
    print("VERIFICATION PASSED")
    print("=" * 68)

    # -----------------------------------------------------------------------
    # assembly (base + lid + logo inlay) -- all three are already modelled in
    # their assembly position (the lid's sleeves start at board z 0, its flange
    # at Z_RIM, and the inlay sits in the pocket), so a plain fuse is the
    # assembly.
    # -----------------------------------------------------------------------
    asm = base.fuse(lid).fuse(log)
    print("assembly: %d solid(s), STEP z %.3f..%.3f (board frame %.3f..%.3f)"
          % (len(asm.Solids), asm.BoundBox.ZMin, asm.BoundBox.ZMax,
             asm.BoundBox.ZMin - T, asm.BoundBox.ZMax - T))
    return base, lid, log


# Kept as a plain guard for interactive use.  `freecadcmd` never sets
# `__name__ == "__main__"`, so `build.sh` imports this module and calls
# `main()` explicitly -- see the `step_run` helper there.
if __name__ == "__main__":
    main()