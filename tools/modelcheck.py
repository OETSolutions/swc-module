#!/usr/bin/env python3
"""Does each retrieved 3D model actually cover the footprint it is placed on?

A STEP file is plain text, so the model's bounding box can be read straight off
its CARTESIAN_POINTs and compared with the footprint's own F.Fab body.  Two
things make the naive version of that lie, and both are handled here:

  * **Stray points.**  A vendor STEP carries construction junk.  This vendor's
    USB-C model has clusters at Z 12.5, Y -13.5 and Y +9.25, so a plain min/max
    calls the part 23 mm deep.  The box is therefore percentile-trimmed, and the
    raw box is printed beside it so the strays stay visible.
  * **The frame.**  KiCad applies the STEP's *internal* placement, which is not
    the frame the CARTESIAN_POINTs are written in -- for the USB-C model that is
    worth ~2.1 mm in Y.  So this script's output is a **screen, not proof**: it
    catches a model that is wildly off or is the wrong part, and the render is
    what confirms a placement.

**What it cannot catch: a 180-degree error on a near-symmetric body.**  The
USB-C shell is symmetric enough that both ends score the same here, and the
wrong end scored fine for two revisions.  The decisive test for a connector is
whether the *contact tails* land on the *pads* -- see addmodels.py's J4 note.

Usage: modelcheck.py PCB
"""
import os
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

PT = re.compile(rb'CARTESIAN_POINT\s*\(\s*[^,]*,\s*\(\s*'
                rb'(-?[\d.E+-]+)\s*,\s*(-?[\d.E+-]+)\s*,\s*(-?[\d.E+-]+)')
TRIM = 0.015                     # fraction dropped from each tail of each axis
TOL = 1.5                        # mm.  Deliberately loose: a footprint's F.Fab
                                 # often draws terminals, leads and wire tabs
                                 # that the 3D model omits, so a ~1 mm shortfall
                                 # is normal.  This catches a model that is
                                 # wildly off, or the wrong part.


def step_bbox(path):
    """(raw, trimmed) bounding boxes of a STEP's CARTESIAN_POINTs."""
    pts = PT.findall(open(path, 'rb').read())
    if not pts:
        return None
    out = []
    for axis in range(3):
        v = sorted(float(p[axis]) for p in pts)
        n = len(v)
        out.append((v[0], v[-1], v[int(n * TRIM)], v[int(n * (1 - TRIM)) - 1]))
    raw = tuple((o[0], o[1]) for o in out)
    lo = tuple(o[2] for o in out)
    hi = tuple(o[3] for o in out)
    return ((raw[0][0], raw[1][0], raw[2][0]),
            (raw[0][1], raw[1][1], raw[2][1])), (lo, hi)


def rotz(b, deg):
    """Rotate a bbox about Z.  Only 0 and 180 matter for these parts."""
    d = int(deg) % 360
    (x0, y0, z0), (x1, y1, z1) = b
    if d == 0:
        return b
    if d == 180:
        return (-x1, -y1, z0), (-x0, -y0, z1)
    raise SystemExit('unhandled Z rotation %d' % d)


FAB_LAYER = ('F.Fab',)


def fab_bbox(fp):
    """Local-frame bbox of a footprint's own F.Fab body graphics.

    This is the thing the model is supposed to sit on -- it is drawn by the
    footprint author, so it is the right reference and it needs no rotation
    handling (both boxes are compared in the footprint's local frame).
    """
    xs, ys = [], []
    for kind in ('fp_rect', 'fp_line', 'fp_circle', 'fp_poly'):
        for g in F.children(fp, kind):
            if g.find('layer') is None:
                continue
            if g.find('layer').atoms[1].text.strip('"') not in FAB_LAYER:
                continue
            for a in g.find_all('start') + g.find_all('end') + \
                    g.find_all('center'):
                xs.append(F.fnum(a.atoms[1].text))
                ys.append(F.fnum(a.atoms[2].text))
    if not xs:
        return None
    return (min(xs), min(ys)), (max(xs), max(ys))


def main():
    pcb = sys.argv[1]
    prj = os.path.dirname(os.path.abspath(pcb))
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]

    print('%-4s %-18s %-22s %-22s %s'
          % ('ref', 'model, placed', 'model box (trimmed)', 'F.Fab body',
             'delta per side'))
    flagged = 0
    for fp in F.children(root, 'footprint'):
        ms = F.children(fp, 'model')
        if not ms:
            continue
        path = ms[0].atoms[1].text.strip('"')
        if '${KIPRJMOD}' not in path:
            continue
        ref = ''
        for p in F.children(fp, 'property'):
            if p.atoms[1].text.strip('"') == 'Reference':
                ref = p.atoms[2].text.strip('"')

        real = path.replace('${KIPRJMOD}', prj, 1)
        got = step_bbox(real)
        if got is None:
            print('%-4s  no CARTESIAN_POINT in %s' % (ref, real))
            flagged += 1
            continue
        raw, trim = got
        off = F.child(ms[0], 'offset')
        ox, oy, oz = (F.fnum(a.text) for a in F.child(off, 'xyz').atoms[1:4])
        rz = F.fnum(F.child(F.child(ms[0], 'rotate'), 'xyz').atoms[3].text)

        (x0, y0, _), (x1, y1, _) = rotz(trim, rz)
        fab = fab_bbox(fp)
        placed = '%+.2f,%+.2f' % (ox, oy)
        mbox = 'x[%7.2f %7.2f] y[%7.2f %7.2f]' % (x0 + ox, x1 + ox,
                                                 y0 - oy, y1 - oy)
        if fab is None:
            print('%-4s %-18s %-22s %-22s no F.Fab graphic'
                  % (ref, placed, mbox, '-'))
            continue
        (fx0, fy0), (fx1, fy1) = fab
        dl = (x0 + ox) - fx0
        dr = (x1 + ox) - fx1
        dt = (y0 - oy) - fy0
        db = (y1 - oy) - fy1
        worst = max(abs(dl), abs(dr), abs(dt), abs(db))
        verdict = 'OK' if worst <= TOL else 'CHECK'
        if verdict == 'CHECK':
            flagged += 1
        print('%-4s %-18s %-22s %-22s L%+.2f R%+.2f T%+.2f B%+.2f  %s'
              % (ref, placed, mbox, 'x[%7.2f %7.2f] y[%7.2f %7.2f]'
                 % (fx0, fx1, fy0, fy1),
                 dl, dr, dt, db, verdict))
        if verdict == 'CHECK':
            print('       raw STEP box (untrimmed -- strays, and any frame '
                  'offset, included):')
            print('         x[%7.2f %7.2f] y[%7.2f %7.2f] z[%7.2f %7.2f]'
                  % (raw[0][0], raw[1][0], raw[0][1], raw[1][1],
                     raw[0][2], raw[1][2]))

    print()
    print('%d footprint(s) flagged.  A pass here is a screen, not proof -- for a '
          'connector, confirm the contact tails land on the pads.'
          % flagged)
    return 1 if flagged else 0


if __name__ == '__main__':
    sys.exit(main())
