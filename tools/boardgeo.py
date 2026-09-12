#!/usr/bin/env python3
"""Board-space geometry: outline, courtyards as polygons, and real collisions.

Local footprint coordinates are useless for layout -- a courtyard is written in
the footprint's own frame and has to be rotated by the placement angle before
anything can be compared.

THE TRANSFORM.  For a footprint at (x, y) with stored angle t and a local point
(lx, ly):

    bx = x + lx*cos(t) + ly*sin(t)
    by = y - lx*sin(t) + ly*cos(t)

i.e. an ordinary rotation (determinant +1) that turns the footprint
counter-clockwise as seen on screen.  This was NOT guessed.  Two of KiCad's own
DRC messages pin it, to the micron, on rotated footprints:

  * J1 at (22.3575, 56.92) rot -90, pad 2 at local (5.08, 0).  DRC reports that
    pad at board (22.3575, 62.00).  56.92 + 5.08 = 62.00: local +x becomes
    board +y.  The opposite sign gives 51.84, which DRC does not report.
  * J4 at (19.40, 48.11) rot -90, shield pad at local (4.32, -3.13).  DRC
    reports it at board (22.530, 52.430) -- exactly what this formula gives.
    The opposite sign gives x = 16.27, off by 6.26 mm.

An earlier version of this file had sin's sign inverted.  Everything it said
about rotated footprints was therefore wrong, including a reported collision
between the locked connector blocks that does not exist.  Believe DRC, not
arithmetic, and re-derive from the two anchors above if this is ever doubted.

COURTYARDS ARE POLYGONS.  U3's courtyard is a T: a closed ring of eight
fp_line segments that is the 19.5 x 20.2 mm module body unioned with the 48 x 21
mm antenna stub.  Its bounding box is roughly 48 x 41 mm, so comparing bboxes
invents collisions that are not there and hides the shape that matters.  So
rings are reconstructed by chaining segments, and overlaps are decided by a
real polygon intersection test (vertex containment plus edge crossing).

Reports:
  * the Edge.Cuts box and each Edge.Cuts graphic
  * every footprint: board-space courtyard polygon(s), rotation, locked flag
  * every pair of footprints whose courtyards actually intersect, with area
  * every pad and footprint origin landing inside a keepout polygon
  * every courtyard extending past the board edge
  * a coarse occupancy grid, if asked

Usage: boardgeo.py PCB [--grid N] [--pads] [--json OUT]
"""
import json
import math
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

TOL = 1e-3


# ---------------------------------------------------------------- transforms

def rot_pt(pt, deg):
    """Rotate a point about the origin, same sense as xform()."""
    t = math.radians(deg)
    ct, st = math.cos(t), math.sin(t)
    return (pt[0] * ct + pt[1] * st, -pt[0] * st + pt[1] * ct)


def xform(pt, deg, ox, oy):
    """Footprint-local point -> board point.  See the header for the proof."""
    rx, ry = rot_pt(pt, deg)
    return (ox + rx, oy + ry)


def pad_ring(pad):
    """A pad's outline as a polygon, in the footprint's local frame.

    Pads must be tested as shapes, not centres: DRC flags the 5.3 mm mounting
    hole at (73.5, 37.5) against U3's keepout, yet that point is 0.75 mm
    outside the keepout's x limit.  It is the pad's *body* that reaches in.
    """
    at = F.child(pad, 'at')
    sz = F.child(pad, 'size')
    if at is None or sz is None:
        return None
    lx, ly = _num(at, 1), _num(at, 2)
    lrot = F.fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
    w, h = _num(sz, 1), _num(sz, 2)
    shape = pad.atoms[3].text.strip('"') if len(pad.atoms) > 3 else 'rect'
    if shape in ('circle', 'oval'):
        n = 16
        pts = [(lx + (w / 2) * math.cos(2 * math.pi * k / n),
                ly + (h / 2) * math.sin(2 * math.pi * k / n))
               for k in range(n)]
    else:
        hw, hh = w / 2, h / 2
        offs = [rot_pt(d, lrot)
                for d in ((-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh))]
        pts = [(lx + o[0], ly + o[1]) for o in offs]
    return pts


def xform_ring(ring, deg, ox, oy):
    return [xform(p, deg, ox, oy) for p in ring]


# ------------------------------------------------------------ polygon helpers

def poly_area(poly):
    a = 0.0
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2.0


def poly_bbox(poly):
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    return (min(xs), min(ys), max(xs), max(ys))


def point_in_poly(pt, poly):
    """Strictly inside: a point exactly on the boundary is not inside, which
    matches KiCad's courtyard rule that touching is legal."""
    x, y = pt
    inside = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xi = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < xi:
                inside = not inside
    return inside


def _cross(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def _seg_cross(a, b, c, d):
    d1, d2 = _cross(c, d, a), _cross(c, d, b)
    d3, d4 = _cross(a, b, c), _cross(a, b, d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return True
    return False


def polys_overlap(A, B):
    """True when two simple polygons' interiors intersect."""
    for p in A:
        if point_in_poly(p, B):
            return True
    for p in B:
        if point_in_poly(p, A):
            return True
    na, nb = len(A), len(B)
    for i in range(na):
        a, b = A[i], A[(i + 1) % na]
        for j in range(nb):
            c, d = B[j], B[(j + 1) % nb]
            if _seg_cross(a, b, c, d):
                return True
    return False


def ring_intersection_area(A, B):
    """Area of A intersect B, for ranking.  Clips A against B with Sutherland-
    Hodgman, which is exact for the convex clip window and a good ranking
    estimate otherwise (U3's T is handled by clipping against its 8 edges)."""
    out = A
    n = len(B)
    for i in range(n):
        if not out:
            return 0.0
        c, d = B[i], B[(i + 1) % n]
        inp, out = out, []
        for k in range(len(inp)):
            cur, nxt = inp[k], inp[(k + 1) % len(inp)]
            cin, nin = _cross(c, d, cur) > 0, _cross(c, d, nxt) > 0
            if cin:
                out.append(cur)
            if cin != nin:
                den = _cross(c, d, nxt) - _cross(c, d, cur)
                if abs(den) > 1e-12:
                    t = _cross(c, d, cur) / den
                    out.append((cur[0] + t * (nxt[0] - cur[0]),
                                cur[1] + t * (nxt[1] - cur[1])))
    return poly_area(out) if len(out) >= 3 else 0.0


# -------------------------------------------------------- shape reconstruction

def _num(node, i):
    return F.fnum(node.atoms[i].text)


def _on_layer(g, layer):
    l = F.child(g, 'layer')
    return l is not None and l.atoms[1].text.strip('"') == layer


def _chain(segs):
    """Chain 2-point segments into closed rings.  Open chains are returned as
    they stand rather than dropped -- a courtyard that fails to close is a
    finding, not something to hide."""
    rings, segs = [], list(segs)
    while segs:
        ring = list(segs.pop(0))
        changed = True
        while changed:
            changed = False
            for i, s in enumerate(segs):
                if abs(ring[-1][0] - s[0][0]) < TOL and \
                   abs(ring[-1][1] - s[0][1]) < TOL:
                    ring.append(s[1])
                elif abs(ring[-1][0] - s[1][0]) < TOL and \
                        abs(ring[-1][1] - s[1][1]) < TOL:
                    ring.append(s[0])
                elif abs(ring[0][0] - s[1][0]) < TOL and \
                        abs(ring[0][1] - s[1][1]) < TOL:
                    ring.insert(0, s[0])
                elif abs(ring[0][0] - s[0][0]) < TOL and \
                        abs(ring[0][1] - s[0][1]) < TOL:
                    ring.insert(0, s[1])
                else:
                    continue
                segs.pop(i)
                changed = True
                break
        if len(ring) > 1 and abs(ring[0][0] - ring[-1][0]) < TOL and \
                abs(ring[0][1] - ring[-1][1]) < TOL:
            ring.pop()
        if len(ring) >= 3:
            rings.append(ring)
    return rings


def rings_on(fp, layer):
    """Every closed polygon a footprint draws on one layer, in local mm."""
    segs, rings = [], []
    for g in F.children(fp, 'fp_line'):
        if not _on_layer(g, layer):
            continue
        s, e = F.child(g, 'start'), F.child(g, 'end')
        if s is None or e is None:
            continue
        segs.append(((_num(s, 1), _num(s, 2)), (_num(e, 1), _num(e, 2))))
    rings += _chain(segs)

    for g in F.children(fp, 'fp_rect'):
        if not _on_layer(g, layer):
            continue
        s, e = F.child(g, 'start'), F.child(g, 'end')
        if s is None or e is None:
            continue
        x1, y1, x2, y2 = _num(s, 1), _num(s, 2), _num(e, 1), _num(e, 2)
        rings.append([(x1, y1), (x2, y1), (x2, y2), (x1, y2)])

    for g in F.children(fp, 'fp_poly'):
        if not _on_layer(g, layer):
            continue
        pts = F.child(g, 'pts')
        if pts is not None:
            r = [(_num(x, 1), _num(x, 2)) for x in F.children(pts, 'xy')]
            if len(r) >= 3:
                rings.append(r)

    for g in F.children(fp, 'fp_circle'):
        if not _on_layer(g, layer):
            continue
        c, rad = F.child(g, 'center'), F.child(g, 'radius')
        if c is None or rad is None:
            continue
        cx, cy, r = _num(c, 1), _num(c, 2), F.fnum(rad.atoms[1].text)
        rings.append([(cx + r * math.cos(a * math.pi / 12),
                       cy + r * math.sin(a * math.pi / 12))
                      for a in range(24)])
    return rings


def keepout_rings(fp):
    """Keepout polygons of a footprint, ALREADY IN BOARD COORDINATES.

    Unlike every other graphic in a footprint, KiCad stores a footprint's zone
    polygons in the board frame -- they do not rotate or translate with the
    placement.  U3 sits at (48.75, 43.91) and its keepout is written as
    (24.75, 16.16)-(72.75, 37.16), which is 48.75 +/- 24 by 43.91 - 27.75 ..
    43.91 - 6.75.  Transforming these again moves them 48 mm off the board and
    invents intrusions.  So: no xform here.
    """
    out = []
    for z in F.children(fp, 'zone'):
        poly = F.child(z, 'polygon')
        pts = F.child(poly, 'pts') if poly is not None else None
        if pts is None:
            continue
        r = [(_num(x, 1), _num(x, 2)) for x in F.children(pts, 'xy')]
        if len(r) >= 3:
            out.append(r)
    return out


def outline(root):
    box, items, xs, ys = None, [], [], []
    for head in ('gr_line', 'gr_rect', 'gr_arc', 'gr_circle', 'gr_poly'):
        for g in F.children(root, head):
            if not _on_layer(g, 'Edge.Cuts'):
                continue
            pts = []
            for key in ('start', 'end', 'mid', 'center'):
                n = F.child(g, key)
                if n is not None:
                    pts.append((_num(n, 1), _num(n, 2)))
            pl = F.child(g, 'pts')
            if pl is not None:
                pts += [(_num(x, 1), _num(x, 2))
                        for x in F.children(pl, 'xy')]
            for p in pts:
                xs.append(p[0])
                ys.append(p[1])
            items.append((head, pts))
    if xs:
        box = (min(xs), min(ys), max(xs), max(ys))
    return box, items


# ------------------------------------------------------------------- assembly

def collect(root):
    parts = []
    for fp in F.children(root, 'footprint'):
        at = F.child(fp, 'at')
        x, y = _num(at, 1), _num(at, 2)
        rot = F.fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
        ref = val = ''
        for p in F.children(fp, 'property'):
            k = p.atoms[1].text.strip('"')
            if k == 'Reference':
                ref = p.atoms[2].text.strip('"')
            elif k == 'Value':
                val = p.atoms[2].text.strip('"')

        cy_local = rings_on(fp, 'F.CrtYd')
        if not cy_local:
            cy_local = rings_on(fp, 'B.CrtYd')
        cy = [xform_ring(r, rot, x, y) for r in cy_local]
        ko = keepout_rings(fp)                 # already board coordinates

        pads = []
        for pad in F.children(fp, 'pad'):
            pat = F.child(pad, 'at')
            if pat is None:
                continue
            lx, ly = _num(pat, 1), _num(pat, 2)
            bx, by = xform((lx, ly), rot, x, y)
            lr = pad_ring(pad)
            pads.append(dict(num=pad.atoms[1].text.strip('"'),
                             board=(bx, by), local=(lx, ly),
                             ring=[xform(p, rot, x, y) for p in lr]
                             if lr else None))

        parts.append(dict(
            ref=ref, value=val, lib=fp.atoms[1].text.strip('"'),
            x=x, y=y, rot=rot, locked=F.child(fp, 'locked') is not None,
            side=F.child(fp, 'layer').atoms[1].text,
            courtyard=cy, keepouts=ko, pads=pads,
            bbox=poly_bbox([p for r in cy for p in r]) if cy else None,
            holes=F.children(fp, 'pad') and any(
                F.child(p, 'drill') is not None for p in F.children(fp, 'pad'))))
    return parts


def main():
    argv = sys.argv[1:]
    pcb = argv[0]
    root = C.parse(open(pcb, 'rb').read()).children[0]

    box, items = outline(root)
    if box:
        print('=== Edge.Cuts: %d graphic(s), x %.2f..%.2f  y %.2f..%.2f  '
              '(%.2f x %.2f mm = %.0f mm^2)'
              % (len(items), box[0], box[2], box[1], box[3],
                 box[2] - box[0], box[3] - box[1],
                 (box[2] - box[0]) * (box[3] - box[1])))
        for head, pts in items:
            print('    %-8s %s' % (head, ' '.join('(%.2f, %.2f)' % p
                                                  for p in pts)))
    else:
        print('=== Edge.Cuts: NONE')

    parts = collect(root)
    nlock = sum(1 for p in parts if p['locked'])
    print('=== %d footprints, %d locked' % (len(parts), nlock))

    if '--grid' in argv:
        n = float(argv[argv.index('--grid') + 1])
        if box:
            cols = int((box[2] - box[0]) / n) + 1
            rows = int((box[3] - box[1]) / n) + 1
            grid = [[' '] * cols for _ in range(rows)]
            for p in parts:
                if not p['bbox']:
                    continue
                b = p['bbox']
                ch = '#' if p['locked'] else '.'
                for r in range(rows):
                    for c in range(cols):
                        gx = box[0] + (c + 0.5) * n
                        gy = box[1] + (r + 0.5) * n
                        if b[0] <= gx <= b[2] and b[1] <= gy <= b[3]:
                            grid[r][c] = ch
            for r in range(rows):
                print('%7.1f |%s|' % (box[1] + (r + 0.5) * n,
                                      ''.join(grid[r])))
            print('        +' + '-' * cols + '+')
            print('         ' + ''.join(
                str(int(box[0] + (c + 0.5) * n) // 10 % 10)
                for c in range(cols)))

    # ---- courtyard collisions, decided by real polygon intersection
    hits = []
    for i in range(len(parts)):
        for j in range(i + 1, len(parts)):
            best = 0.0
            for A in parts[i]['courtyard']:
                for B in parts[j]['courtyard']:
                    if polys_overlap(A, B):
                        best = max(best, ring_intersection_area(A, B))
            if best > 0:
                hits.append((i, j, best))
    print('=== %d courtyard collisions (polygon test)'
          % len(hits))
    for i, j, a in sorted(hits, key=lambda h: -h[2]):
        A, B = parts[i], parts[j]
        print('   %-6s x %-6s  ~%.1f mm^2%s%s'
              % (A['ref'], B['ref'], a,
                 ' [LOCKED]' if A['locked'] or B['locked'] else '',
                 '  <- both locked, USER MUST FIX'
                 if A['locked'] and B['locked'] else ''))

    # ---- keepout intrusions, pad bodies not pad centres
    kos = [(p['ref'], k) for p in parts for k in p['keepouts']]
    if kos:
        print('=== %d keepout polygon(s); intrusions:' % len(kos))
        n_int = 0
        for ref, ko in kos:
            for p in parts:
                if p['ref'] == ref:
                    continue
                for pad in p['pads']:
                    if pad['ring'] and polys_overlap(pad['ring'], ko):
                        print('   PAD       %-6s pad %-4s @ (%.2f, %.2f) '
                              'overlaps keepout of %s'
                              % (p['ref'], pad['num'], pad['board'][0],
                                 pad['board'][1], ref))
                        n_int += 1
                for r in p['courtyard']:
                    if polys_overlap(r, ko):
                        print('   COURTYARD %-6s overlaps keepout of %s'
                              % (p['ref'], ref))
                        n_int += 1
                        break
        if not n_int:
            print('   none')

    # ---- over the board edge
    if box:
        print('=== footprints crossing the board edge:')
        n_edge = 0
        for p in parts:
            r = p['bbox']
            if not r:
                continue
            if r[0] < box[0] - TOL or r[2] > box[2] + TOL or \
               r[1] < box[1] - TOL or r[3] > box[3] + TOL:
                over = []
                if r[0] < box[0] - TOL:
                    over.append('left %.2f' % (box[0] - r[0]))
                if r[2] > box[2] + TOL:
                    over.append('right %.2f' % (r[2] - box[2]))
                if r[1] < box[1] - TOL:
                    over.append('top %.2f' % (box[1] - r[1]))
                if r[3] > box[3] + TOL:
                    over.append('bottom %.2f' % (r[3] - box[3]))
                print('   %-6s %s' % (p['ref'], ', '.join(over)))
                n_edge += 1
        if not n_edge:
            print('   none')

    if '--pads' in argv:
        print('=== pads in board space')
        for p in sorted(parts, key=lambda q: q['ref']):
            for pad in p['pads']:
                print('   %-6s %-4s (%.2f, %.2f)'
                      % (p['ref'], pad['num'], pad['board'][0],
                         pad['board'][1]))

    print('=== placement table (sorted by board y)')
    for p in sorted(parts, key=lambda q: (q['bbox'] or (0, 0, 0, 0))[1]):
        if not p['bbox']:
            print('%-6s %-9s rot %-6g %-5s  NO COURTYARD'
                  % (p['ref'], p['value'][:9], p['rot'], p['side']))
            continue
        b = p['bbox']
        print('%-6s %-9s rot %-6g %-5s x %7.2f..%7.2f  y %7.2f..%7.2f  %s'
              % (p['ref'], p['value'][:9], p['rot'], p['side'],
                 b[0], b[2], b[1], b[3], 'LOCKED' if p['locked'] else ''))

    if '--json' in argv:
        out = argv[argv.index('--json') + 1]
        json.dump(dict(outline=box, parts=parts), open(out, 'w'), indent=1)
        print('wrote %s' % out)


if __name__ == '__main__':
    main()
