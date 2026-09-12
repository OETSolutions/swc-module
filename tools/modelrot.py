#!/usr/bin/env python3
"""Set a footprint's 3D-model rotation, to test a model-orientation question.

KiCad applies a model node's rotate/scale/offset in the footprint's LOCAL
frame, before the footprint's own placement angle, so a model that a vendor
authored in a different frame from the footprint is corrected here rather
than by turning the part.

Usage: modelrot.py IN.kicad_pcb OUT.kicad_pcb REF X Y Z
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402


def ref_of(fp):
    for p in F.children(fp, 'property'):
        if p.atoms[1].text.strip('"') == 'Reference':
            return p.atoms[2].text.strip('"')
    return ''


def main():
    src, dst, ref = sys.argv[1], sys.argv[2], sys.argv[3]
    xyz = [float(v) for v in sys.argv[4:7]]
    doc = C.parse(open(src, 'rb').read())
    root = doc.children[0]
    for fp in F.children(root, 'footprint'):
        if ref_of(fp) != ref:
            continue
        for m in F.children(fp, 'model'):
            rot = F.child(m, 'rotate')
            if rot is None:
                print('=== %s: model has no (rotate ...) node' % ref)
                return 1
            inner = F.child(rot, 'xyz')
            for k, v in enumerate(xyz):
                inner.atoms[k + 1].set_text('%.6g' % v)
            print('=== %s model rotate -> %s'
                  % (ref, ' '.join('%.6g' % v for v in xyz)))
        open(dst, 'wb').write(C.serialize(doc))
        return 0
    print('=== %s not found' % ref)
    return 1


if __name__ == '__main__':
    sys.exit(main())
