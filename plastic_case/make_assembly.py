"""Rebuild SWC_Enclosure.FCStd -- the viewable assembly of the finished parts.

Four objects, each independently show/hide, all already modelled in their
assembled position so nothing needs a Placement:

  Case_Body    base shell
  Case_Lid     lid, seated on the base's rim
  Logo_Inlay   the two-colour mark
  PCB          the KiCad board + components, for inspecting cut-out fit
               (the floor windows under the terminal-block silkscreen)

Do NOT build the PCB object with `Import.insert`: the KiCad STEP is an
assembly of ~101 footprints, and the importer expands each one into an
`App::Part` containing its own Origin/planes/axes -- ~1000 objects.  Read
SWC.step into a single `Part::Shape` instead; the bboxes are identical and
the document stays four objects.  (The 210 solids are not fused: some of
them overlap, and fusing forces an expensive boolean that can fail.)

This script always writes a FRESH document.  Opening the existing file and
editing it in place is what let a polluted document get saved over the good
one -- regenerating is cheap because everything here is derived from the
STEP exports.

Run:  freecadcmd make_assembly.py
"""
import os
import Part
import FreeCAD as app

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "SWC_Enclosure.FCStd")

PARTS = [
    ("Case_Body",  "Case_Body.step",  (0.75, 0.75, 0.78), 0),
    ("Case_Lid",   "Case_Lid.step",   (0.80, 0.80, 0.83), 0),
    ("Logo_Inlay", "Logo_Inlay.step", (0.90, 0.55, 0.10), 0),
    ("PCB",        "SWC.step",        (0.15, 0.40, 0.20), 25),
]


def main():
    # Only replace our own document -- never sweep up whatever else the user
    # has open.
    if "SWC_Enclosure" in app.listDocuments():
        app.closeDocument("SWC_Enclosure")
    doc = app.newDocument("SWC_Enclosure")

    for name, step, colour, transparency in PARTS:
        path = os.path.join(HERE, step)
        if not os.path.exists(path):
            raise SystemExit("missing %s -- run ./build.sh export first" % step)
        sh = Part.Shape()
        sh.read(path)
        obj = doc.addObject("Part::Feature", name)
        obj.Shape = sh
        # ViewObject is None under `freecadcmd` (no GUI), so colouring only
        # happens when this runs inside the GUI session.
        if obj.ViewObject is not None:
            obj.ViewObject.ShapeColor = colour
            obj.ViewObject.Transparency = transparency
        bb = sh.BoundBox
        print("  %-11s %3d solids  x %7.3f..%7.3f  y %8.3f..%8.3f  z %7.3f..%7.3f"
              % (name, len(sh.Solids), bb.XMin, bb.XMax,
                 bb.YMin, bb.YMax, bb.ZMin, bb.ZMax))

    doc.recompute()
    doc.saveAs(OUT)
    print("wrote %s (%.1f MB, %d objects)"
          % (OUT, os.path.getsize(OUT) / 1e6, len(doc.Objects)))


if __name__ == "__main__":
    main()
