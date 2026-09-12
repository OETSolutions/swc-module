#!/usr/bin/env python3
"""Pin a PCB footprint's Reference field to an explicit board position.

pcbfields.py solves for a spot; this is the escape hatch for the handful of
designators where the solver's model is stricter than DRC's and it therefore
parks the text ten millimetres away.  Give it a board X/Y and a drawn angle and
it writes the equivalent footprint-local (at ...).

Usage: setfield.py PCB REF,X,Y,ANG [REF,X,Y,ANG ...]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                       # noqa: E402
import silk                                  # noqa: E402
from mcp_server_kicad import _cst as C       # noqa: E402


def main():
    pcb = sys.argv[1]
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    for spec in sys.argv[2:]:
        ref, sx, sy, sa = spec.split(',')
        x, y, ang = float(sx), float(sy), float(sa)
        fp = next(f for f in F.children(root, 'footprint')
                  if any(p.atoms[1].text.strip('"') == 'Reference'
                         and p.atoms[2].text.strip('"') == ref
                         for p in F.children(f, 'property')))
        a = F.child(fp, 'at')
        fx, fy = F.fnum(a.atoms[1].text), F.fnum(a.atoms[2].text)
        frot = F.fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
        # inverse of silk.xform: local delta = R(+frot) . board delta, and
        # silk.rot_pt already rotates by -t, so pass -frot to get +frot.
        lx, ly = silk.rot_pt(x - fx, y - fy, -frot)
        node = next(p for p in F.children(fp, 'property')
                    if p.atoms[1].text.strip('"') == 'Reference')
        # `ang` is the angle the text is DRAWN at, and for a footprint field
        # that IS the stored angle -- see silk.drawn_angle.  This used to store
        # `ang - frot`, which is the rule for a footprint's own graphics, not
        # for its fields; on a part rotated 180 it flipped horizontal text to
        # vertical.  pcbfields.py, which does the other 105 designators, has
        # always written `ang` unchanged.
        F.set_at(node, lx, ly, ang % 360)
        print('%-6s board(%8.3f,%8.3f) ang %g  ->  local(%8.3f,%8.3f)'
              % (ref, x, y, ang, lx, ly))
    open(pcb, 'wb').write(C.serialize(doc))
    print('written')


main()
