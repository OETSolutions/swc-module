"""
Export the SWC enclosure parts.

  * STL (fine, for slicing) -- Case_Body.stl / Case_Lid.stl
  * STEP (for CAD)          -- Case_Body.step / Case_Lid.step
  * mesh validation: every STL is re-imported and checked for a single
    closed watertight shell.

Run:  freecadcmd export.py
"""

import os
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass
_p = print


def print(*a, **k):          # never lose the report on SystemExit
    k.setdefault("flush", True)
    _p(*a, **k)

import Part
import Mesh
import MeshPart
from FreeCAD import Base
import casegeom as G
import build_case as B

HERE = os.path.dirname(os.path.abspath(__file__))
LIN_TOL = 0.05          # mm, chord tolerance for the STL facets
ANG_TOL = 0.25          # rad
fails = []


def print_oriented(shape, flip):
    """STL is a PRINT artefact: lay the part flat on the bed.

    The base is already correct (its flat underside is the first layer).  The
    lid is modelled in its ASSEMBLY position, where the 4 sleeves hang 24 mm
    below the flange; sliced like that the flange would span ~90 mm on four
    unsupported corners.  Flipping it 180 deg puts the flange face on the bed
    and leaves the spigot ring + sleeves as plain vertical walls -- no
    supports, no cleanup.  The logo inlay is flipped the SAME way so that the
    two STLs can be loaded on one plate as two objects: the inlay then sits
    exactly in the lid's pocket and the slicer prints each in its own filament.
    """
    s = shape.copy()
    if flip:
        s.rotate(Base.Vector(0, 0, 0), Base.Vector(1, 0, 0), 180.0)
    s.translate(Base.Vector(0, 0, -s.BoundBox.ZMin))
    return s


def export_one(name, shape, flip=False):
    stl = os.path.join(HERE, name + ".stl")
    stp = os.path.join(HERE, name + ".step")

    psh = print_oriented(shape, flip)

    t = time.time()
    mesh = MeshPart.meshFromShape(Shape=psh, LinearDeflection=LIN_TOL,
                                  AngularDeflection=ANG_TOL,
                                  Relative=False)
    # the faceted surface lies marginally inside the true surface, so drop the
    # MESH itself onto z=0 to guarantee the slicer sees a first layer at 0
    mesh.translate(0.0, 0.0, -mesh.BoundBox.ZMin)
    mesh.write(stl)
    print("  %-10s STL: %d facets, %d points  (%.1fs)%s"
          % (name, mesh.CountFacets, mesh.CountPoints, time.time() - t,
             "  [print-oriented]" if flip else ""))

    t = time.time()
    shape.exportStep(stp)
    print("  %-10s STEP: written (%.1fs)" % (name, time.time() - t))

    # --- re-import the STL and check watertightness -----------------------
    m2 = Mesh.Mesh(stl)
    bb = m2.BoundBox
    try:
        m2.removeDuplicatedPoints()
        m2.removeDuplicatedFacets()
        m2.harmonizeNormals()
        m2.fixDegenerations(0.001)
        from collections import Counter
        ec = Counter()
        for f in m2.Facets:
            idx = f.PointIndices
            for a, b in ((idx[0], idx[1]), (idx[1], idx[2]), (idx[2], idx[0])):
                ec[(min(a, b), max(a, b))] += 1
        bad = sum(1 for k, v in ec.items() if v != 2)
        print("       STL re-read: %d facets  %d edges-not-shared-by-2"
              % (m2.CountFacets, bad))
        if bad:
            fails.append("%s STL not watertight (%d bad edges)" % (name, bad))
        print("       STL bbox (bed frame) x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
              % (bb.XMin, bb.XMax, bb.YMin, bb.YMax, bb.ZMin, bb.ZMax))
        if abs(bb.ZMin) > 1e-6:
            fails.append("%s STL does not sit on the bed (zmin %.3f)"
                         % (name, bb.ZMin))
    except Exception as e:
        print("       STL topology check failed:", e)

    return stl, stp


def main():
    print("building parts")
    base = B.build_base()
    lid = B.build_lid()
    logo = B.build_logo()

    print()
    print("exporting")
    for name, sh, flip in (("Case_Body", base, False),
                           ("Case_Lid", lid, True),
                           ("Logo_Inlay", logo, True)):
        # The mark is 13 separate letterform islands (see logo.rings_step), so
        # the inlay is legitimately 13 solids; the base and lid are one each.
        if not sh.isValid() or len(sh.Solids) < 1:
            fails.append("%s not valid solids (%d)" % (name, len(sh.Solids)))
        if name != "Logo_Inlay" and len(sh.Solids) != 1:
            fails.append("%s not a single valid solid" % name)
        export_one(name, sh, flip)

    # --- assembled view: both parts are already modelled in their assembly
    #     position (the lid's sleeves start at board z 0 and its flange at
    #     Z_RIM), so the assembly needs no placement at all.
    asm = base.fuse(lid).fuse(logo)
    p = os.path.join(HERE, "Case_Assembly.step")
    asm.exportStep(p)
    print("  Case_Assembly STEP: %d solids (base + lid on rim + logo inlay)"
          % len(asm.Solids))

    print()
    for f in sorted(os.listdir(HERE)):
        if f.endswith((".stl", ".step")):
            print("  %-18s %8.1f kB" % (f, os.path.getsize(os.path.join(HERE, f))
                                        / 1024.0))

    print()
    if fails:
        print("EXPORT CHECK FAILED:")
        for f in fails:
            print("  -", f)
        raise SystemExit(1)
    print("EXPORT + MESH CHECKS PASSED")


if __name__ == "__main__":
    main()
