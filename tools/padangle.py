#!/usr/bin/env python3
"""Set the pad-level `at` angle of every pad in one footprint.

The board's pad-level rotation is invisible to `get_footprint_pads` -- that
report gives the pad's LOCAL centre and size but not its angle -- so a pad
rotated 90 deg inside its own footprint reads as correct through the API while
its copper is wrong on the board.  C2 is the live case: footprint rot -90 with
pad angle 270 gives a total of 0, so its 3.5 x 1.6 pads sit with the long axis
PERPENDICULAR to a 5.4 mm pitch instead of along it.

Rewriting the angle to the library's (0) makes the total -90, which puts the
long axis back along the pitch -- the standard land pattern.

Usage: padangle.py PCB REF ANGLE
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                        # noqa: E402
from mcp_server_kicad import _cst as C        # noqa: E402


def main():
    pcb, ref, ang = sys.argv[1], sys.argv[2], float(sys.argv[3])
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    fp = next(f for f in F.children(root, 'footprint')
              if any(p.atoms[1].text.strip('"') == 'Reference'
                     and p.atoms[2].text.strip('"') == ref
                     for p in F.children(f, 'property')))
    at = F.child(fp, 'at')
    frot = F.fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
    n = 0
    for pad in F.children(fp, 'pad'):
        a = F.child(pad, 'at')
        if a is None:
            continue
        x, y = F.fnum(a.atoms[1].text), F.fnum(a.atoms[2].text)
        old = F.fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
        F.set_at(pad, x, y, ang)
        n += 1
        print('  pad %-4s ang %-6g -> %-6g   total %g -> %g deg'
              % (pad.atoms[1].text.strip('"'), old, ang,
                 (frot + old) % 180, (frot + ang) % 180))
    open(pcb, 'wb').write(C.serialize(doc))
    print('%s (fp rot %g): %d pads set to ang %g' % (ref, frot, n, ang))


main()
