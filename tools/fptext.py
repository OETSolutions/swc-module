#!/usr/bin/env python3
"""Move one footprint's own printed silkscreen text (an fp_text node).

The kicad MCP server can move a footprint, a pad's angle and a field, but it
has no tool that reaches an `fp_text` -- the free text a footprint library
draws on its own silkscreen.  That leaves the handful of stock-library marks
that are drawn ON TOP OF a pad, where the solder-mask opening clips them.
BZ1's "(+)" polarity mark is the one on this board: the HMB-12 footprint puts
it at the footprint origin, which is exactly PTH pad 1.

Give it a board X/Y and the angle the text should be drawn at, and it writes
the equivalent footprint-local `(at ...)`.

Two consequences worth knowing before you use it:
  * the footprint stops matching its library copy, so DRC adds a
    `lib_footprint_mismatch` for it -- the same trade the J4/U3/R60 edits made;
  * the new spot still has to clear the pads and any other ink.  Check it
    with silksearch.py first, or re-run DRC after.

IDX is the 0-based index among the footprint's fp_text nodes, in file order
(most footprints have exactly one, so it is 0).

Usage: fptext.py PCB REF,IDX,X,Y[,ANG] [REF,IDX,X,Y[,ANG] ...]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                       # noqa: E402
import silk                                  # noqa: E402
from mcp_server_kicad import _cst as C       # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pcb = sys.argv[1]
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    for spec in sys.argv[2:]:
        parts = spec.split(',')
        if len(parts) not in (4, 5):
            sys.exit('bad spec %r -- want REF,IDX,X,Y[,ANG]' % spec)
        ref, idx = parts[0], int(parts[1])
        x, y = float(parts[2]), float(parts[3])
        ang = float(parts[4]) if len(parts) == 5 else 0.0

        fp = next((f for f in F.children(root, 'footprint')
                   if any(p.atoms[1].text.strip('"') == 'Reference'
                          and p.atoms[2].text.strip('"') == ref
                          for p in F.children(f, 'property'))), None)
        if fp is None:
            sys.exit('no footprint with Reference %r' % ref)
        texts = F.children(fp, 'fp_text')
        if not texts:
            sys.exit('%s has no fp_text node' % ref)
        if not 0 <= idx < len(texts):
            sys.exit('%s has %d fp_text node(s); index %d is out of range'
                     % (ref, len(texts), idx))
        node = texts[idx]

        a = F.child(fp, 'at')
        fx, fy = F.fnum(a.atoms[1].text), F.fnum(a.atoms[2].text)
        frot = F.fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
        # inverse of silk.xform: board = f + R(rot) . local
        lx, ly = silk.rot_pt(x - fx, y - fy, -frot)
        old = F.child(node, 'at')
        was = (F.fnum(old.atoms[1].text), F.fnum(old.atoms[2].text))
        # DRAWN angle, stored unchanged -- see silk.drawn_angle.  Storing
        # `ang - frot`, as this did, is the rule for a footprint's own graphics;
        # applied to text it stands a mark meant to lie flat on its end on any
        # part whose rotation is not 0 or 180.
        F.set_at(node, lx, ly, ang % 360)
        print('%-6s fp_text[%d] board(%8.3f,%8.3f) ang %g  local(%7.3f,%7.3f)'
              '  was local(%7.3f,%7.3f)'
              % (ref, idx, x, y, ang, lx, ly, was[0], was[1]))
    open(pcb, 'wb').write(C.serialize(doc))
    print('written')


main()
