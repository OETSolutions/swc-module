#!/usr/bin/env python3
"""Loosen a keepout's item flags, leaving its copper restrictions alone.

KiCad's ESP32-S3-WROOM-1 footprint ships with the antenna rule area already
drawn -- 48 x 21 mm over the top of the module, banning tracks, vias, pads,
footprints and copper pour on every copper layer.  Banning copper there is the
whole point of the thing and must stay.  Banning *footprints* is a side effect
nobody asked for: a non-plated mounting hole has no copper to couple into the
antenna, so a hole inside the rule area is harmless while the DRC error it
raises is not.

Usage: keepoutflags.py PCB [--allow-footprints] [--allow-pads] [--apply]
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as FP                                   # noqa: E402
import pcbfields as PB                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402


def main():
    pcb = sys.argv[1]
    apply_ = '--apply' in sys.argv
    want = {'footprints': '--allow-footprints' in sys.argv,
            'pads': '--allow-pads' in sys.argv}
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    n = 0
    for fp in FP.children(root, 'footprint'):
        for z in FP.children(fp, 'zone'):
            ko = FP.child(z, 'keepout')
            if ko is None:
                continue
            for key, allow in want.items():
                if not allow:
                    continue
                node = FP.child(ko, key)
                if node is None:
                    continue
                old = node.atoms[1].text
                if old == 'allowed':
                    continue
                node.atoms[1].set_text('allowed')
                print('   %s: keepout %s %s -> allowed' % (PB.ref_of(fp), key, old))
                n += 1
    print('=== %d flags loosened%s' % (n, '' if apply_ else '  (dry run)'))
    if apply_ and n:
        open(pcb, 'wb').write(C.serialize(doc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
