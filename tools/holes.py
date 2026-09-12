#!/usr/bin/env python3
"""Name the mounting holes and square them up against the board edge.

The four MountingHole footprints exist only on the board -- the schematic has
92 symbols and not one of them is a hole -- so nothing upstream will ever
annotate them, and `update_pcb_from_schematic` with delete_stale would take
them away.  They arrive as REF** and stay that way unless something names them
here.  H1..H4 by reading order (top-left, top-right, bottom-left, bottom-right)
gives the fabricator the same mental map a reader of the board has.

The second job is the edge inset.  Three of the four holes already sit 6.0 mm
in from their two nearest edges; the fourth is half a millimetre out on y.  It
is invisible on a render and obvious when a standoff will not seat, so the
holes are snapped to a uniform inset computed from the outline rather than from
the number someone typed.

Usage: holes.py PCB [--apply]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as FP                                   # noqa: E402
import pcbfields as PB                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

EDGE_INSET = 6.0
# Edge.Cuts from mcp__kicad__list_pcb_graphic_items, 2026-09-10: four lines and
# four R2.0 corner arcs, i.e. a rounded rectangle.
OUTLINE = (16.0, 31.0, 79.5, 135.5)          # min x, min y, max x, max y


def prop(fp, name):
    for p in FP.children(fp, 'property'):
        if p.atoms[1].text.strip('"') == name:
            return p
    return None


def text_of(p):
    return p.atoms[2].text.strip('"') if p is not None else '?'


def main():
    pcb = sys.argv[1]
    apply_ = '--apply' in sys.argv
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]

    holes = []
    for fp in FP.children(root, 'footprint'):
        ref = text_of(prop(fp, 'Reference'))
        if not ref.startswith('REF'):
            continue
        at = FP.child(fp, 'at')
        holes.append((FP.fnum(at.atoms[1].text), FP.fnum(at.atoms[2].text), fp))
    holes.sort(key=lambda h: (h[1], h[0]))

    if len(holes) != 4:
        print('=== expected 4 unnamed holes, found %d -- refusing' % len(holes))
        return 1

    x0, y0, x1, y1 = OUTLINE
    n = 0
    for i, (cx, cy, fp) in enumerate(holes, start=1):
        want_x = x0 + EDGE_INSET if abs(cx - (x0 + EDGE_INSET)) < \
            abs(cx - (x1 - EDGE_INSET)) else x1 - EDGE_INSET
        want_y = y0 + EDGE_INSET if abs(cy - (y0 + EDGE_INSET)) < \
            abs(cy - (y1 - EDGE_INSET)) else y1 - EDGE_INSET
        ref = 'H%d' % i
        print('   ( %7.3f, %7.3f)  REF** -> %s' % (cx, cy, ref))
        rp = prop(fp, 'Reference')
        rp.atoms[2].set_text(ref)
        if abs(cx - want_x) > 1e-6 or abs(cy - want_y) > 1e-6:
            at = FP.child(fp, 'at')
            print('         move (%.3f, %.3f) -> (%.3f, %.3f)'
                  % (cx, cy, want_x, want_y))
            at.atoms[1].set_text('%.4f' % want_x)
            at.atoms[2].set_text('%.4f' % want_y)
            n += 1
        print('         Reference = %s  layer=%s  Value = %s  layer=%s'
              % (ref, layer_of(rp), text_of(prop(fp, 'Value')),
                 layer_of(prop(fp, 'Value'))))
    print('=== 4 holes named, %d moved to a %.1f mm inset%s'
          % (n, EDGE_INSET, '' if apply_ else '  (dry run)'))
    if apply_:
        open(pcb, 'wb').write(C.serialize(doc))
    return 0


def layer_of(p):
    if p is None:
        return '?'
    lay = FP.child(p, 'layer')
    return lay.atoms[1].text.strip('"') if lay is not None else '?'


if __name__ == '__main__':
    sys.exit(main())
