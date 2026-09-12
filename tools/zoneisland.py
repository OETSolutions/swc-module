#!/usr/bin/env python3
"""Show, and optionally set, a zone's island-removal mode.

A pour over a board full of other-net pads fragments: a pocket of copper can
end up ringed by clearance with no pad of its own net inside it.  Such an
island connects nothing, so KiCad flags it as isolated copper -- and on a real
board it is a loose flake waiting to shift.  island_removal_mode 0 tells the
filler to drop exactly those, which is what a plane wants; 1 keeps them.

Usage: zoneisland.py PCB [--remove] [--apply]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402


def netname(z):
    n = F.child(z, 'net_name')
    return n.atoms[1].text.strip('"') if n is not None else '?'


def layername(z):
    n = F.child(z, 'layer')
    if n is None:
        return '?'
    return ' '.join(a.text.strip('"') for a in n.atoms[1:])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    pcb = args[0] if args else '<repo-root>/SWC.kicad_pcb'
    apply_ = '--apply' in sys.argv
    remove = '--remove' in sys.argv

    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    touched = 0
    for z in F.children(root, 'zone'):
        fill = F.child(z, 'fill')
        if fill is None:
            print('  %-8s %-10s  (no fill node)' % (layername(z), netname(z)))
            continue
        keys = [getattr(ch, 'head', None) for ch in fill.children]
        n = F.child(fill, 'island_removal_mode')
        cur = n.atoms[1].text if n is not None else '(absent)'
        print('  %-8s %-10s  island_removal_mode=%s   fill: %s'
              % (layername(z), netname(z), cur,
                 ', '.join(k for k in keys if k)))
        if remove and apply_:
            if n is not None:
                n.atoms[1].set_text('0')
            else:
                fill.children.append(
                    C.parse(b'(island_removal_mode 0)').children[0])
            touched += 1
    print('=== %d zones%s'
          % (len(F.children(root, 'zone')),
             ', %d set to 0 (always remove)' % touched if apply_ else ' -- dry run'))
    if apply_ and remove:
        open(pcb, 'wb').write(C.serialize(doc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
