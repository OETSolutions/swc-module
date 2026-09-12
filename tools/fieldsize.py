#!/usr/bin/env python3
"""Set a footprint field's text size.

In a dense cluster two adjacent parts can have reference designators that each
want the same patch of free silk, and no amount of moving finds a spot that is
both clean and beside the part.  Shrinking the glyph is the standard way out:
the designator stays where it is useful -- next to the part it names -- at the
cost of a slightly smaller letter, which beats a designator parked 15 mm away
or one printed across a neighbouring pad.

Usage: fieldsize.py PCB REF H [W] [--apply]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    pcb, ref, h = args[0], args[1], args[2]
    w = args[3] if len(args) > 3 else h
    apply_ = '--apply' in sys.argv

    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    for fp in F.children(root, 'footprint'):
        for p in F.children(fp, 'property'):
            if p.atoms[1].text.strip('"') != 'Reference':
                continue
            if p.atoms[2].text.strip('"') != ref:
                continue
            size = F.child(F.child(p, 'effects'), 'font')
            size = F.child(size, 'size') if size is not None else None
            if size is None:
                print('=== %s: Reference has no (size ...) node' % ref)
                return 1
            old = ' '.join(a.text for a in size.atoms[1:3])
            size.atoms[1].set_text(h)
            size.atoms[2].set_text(w)
            print('=== %s Reference size %s -> %s %s%s'
                  % (ref, old, h, w, '' if apply_ else '  (dry run)'))
            if apply_:
                open(pcb, 'wb').write(C.serialize(doc))
            return 0
    print('=== %s not found' % ref)
    return 1


if __name__ == '__main__':
    sys.exit(main())
