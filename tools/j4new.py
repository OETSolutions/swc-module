#!/usr/bin/env python3
"""J4 fan-out: design + DRC-true clearance checker (read-only).

Checks a proposed J4 fan-out against every OTHER item on the board, using the
project's real design rules, and against the new items themselves.

Layer semantics that matter here (and that made this fan-out hard):
  - an F.Cu SMD pad only constrains F.Cu copper; a B.Cu trace may run straight
    under it.  Only vias and thru-hole pads span every layer.
  - so the corridor x 24.42..25.884 is 1.46 mm for F.Cu but ~2.6 mm for B.Cu.

Usage: j4new.py PCB
"""
import math
import re
import sys

CLR = 0.150          # copper -> copper
CLR_HOLE = 0.200     # copper -> hole edge
CLR_EDGE = 0.500     # copper -> board edge
VIA_D, VIA_DRILL = 0.55, 0.25
TRK = 0.200

# ---- design (board frame, mm) -------------------------------------------
# each entry: (net, layer, [(x,y), ...]) polyline;  via: (net, x, y)
#
# Only NEW items appear below.  Everything else the fan-out needs is already
# on the board and is checked as "kept" by main().
#
# Topology (v9):
#   The corridor x 24.42..25.884 has only 1.46 mm, minus CC1's via (edge
#   25.175) and U8's pad column (25.884), which leaves 0.709 mm between them -
#   enough for ONE 0.2 trace with 0.15 either side, not two.  So D+ and D- must
#   ride different layers through it.
#
#   D+ : B6 goes east at y=49.030 and drops to B.Cu at (25.450,49.030).  B.Cu
#     at x=25.450 is clear from y 49.030 up to 43.600 (nearest B.Cu: CC1's
#     trunk edge 25.025, the /VBUS trunk edge 26.360, the /VBUS horizontal at
#     y 43.10).  It surfaces at (25.450,43.600) - 0.250 above that /VBUS
#     horizontal - and runs east along y=43.440 to the kept north return, which
#     reaches R12.2 -> U8.1.  (The old F.Cu lane here also T'd into the D+
#     trunk at (25.450,47.994), which is what keeps U8.6's side tied to R12.2's;
#     the B.Cu lane crosses it the same way.)
#   D- : B7 (47.456) and A7 (48.456) straddle the D+ trunk (47.880..48.119),
#     which cannot be crossed in the corridor.  So B7 crosses to A7 WEST of the
#     pad column, at x=22.400 - the only free F.Cu in the connector bay.  B7
#     then runs east at y=47.500 - not 47.456, which would leave only 0.161 to
#     CC1's via - in the channel between that via (edge 47.195) and A6's pad
#     (edge 47.806), and turns north at x=25.450 straight into U8.4.
#   CC1, CC2 and /VBUS are already correct on the board except for the
#   severed /VBUS feed, restored below.
SEGS = [
    # ---- D+ : B6 east, T into the trunk, then down to B.Cu -----------------
    # The T-stub matters: the trunk is the only copper joining A6/U8.6 to
    # B6/R12.2/U8.1, and on B.Cu the lane no longer crosses it.
    ('DP','F.Cu',[(23.695,48.956),(25.450,48.956),(25.450,48.700)]),
    ('DP','F.Cu',[(24.700,48.956),(24.700,47.978)]),
    ('DP','B.Cu',[(25.450,48.700),(25.450,43.600)]),
    ('DP','F.Cu',[(25.450,43.600),(25.450,43.440),(29.574,43.440)]),

    # ---- D- : A7 -> B7, west of the pad column -----------------------------
    ('DM','F.Cu',[(23.695,48.456),(22.400,48.456),(22.400,47.500),(23.695,47.500)]),
    # ---- D- : B7 east through the corridor and north into U8.4 -------------
    ('DM','F.Cu',[(23.695,47.500),(25.500,47.500),(25.500,46.119),(26.546,46.119)]),

    # ---- /VBUS : restore the feed the earlier rip severed ----------------
    ('VBUS','B.Cu',[(24.200,45.800),(24.200,43.000)]),
]
VIAS = [
    ('DP', 25.450, 48.700, 0.50),
    ('DP', 25.450, 43.600, 0.50),
]

RIP = (0.0, 0.0, 0.0, 0.0)
# items the board still carries that this revision deletes
RIP_EXTRA = [
    ((23.695, 47.456), (25.900, 47.456)),      # D- B7 east lane (old)
    ((25.900, 47.456), (25.900, 46.119)),      # D- B7 north lane (old)
    ((25.900, 46.119), (26.546, 46.119)),      # D- B7 return (old)
    ((25.500, 46.300), (26.546, 46.119)),      # D- A7 return (old)
    ((24.300, 46.300), (25.500, 46.300)),      # D- A7 B.Cu leg 2 (old)
    ((24.300, 48.500), (24.300, 46.300)),      # D- A7 B.Cu leg 1 (old)
    ((23.695, 48.500), (24.300, 48.500)),      # D- A7 stub (old)
    ((23.695, 49.020), (25.400, 49.020)),      # D+ B6 lane (old)
    ((25.400, 49.020), (25.400, 47.994)),      # D+ B6 drop (old)
    ((24.300, 48.500), (24.300, 48.500)),      # D- A7 via 1 (old)
    ((25.500, 46.300), (25.500, 46.300)),      # D- A7 via 2 (old)
]
KEEP = []
NET = {'DP': 'Net-(J4-D+-PadA6)', 'DM': 'Net-(J4-D--PadA7)',
       'CC1': 'Net-(J4-CC1)', 'CC2': 'Net-(J4-CC2)', 'VBUS': '/VBUS'}
J4NETS = set(NET.values()) | {'GND',
                              'unconnected-(J4-SBU1-PadA8)',
                              'unconnected-(J4-SBU2-PadB8)'}


def rot(px, py, deg):
    """footprint-local -> board offset.

    KiCad's +y is down and a footprint angle turns clockwise on screen, so in
    file coordinates this is the usual rotation with the angle negated.  For
    J4 (at 19.65 48.206 270) that is (lx,ly) -> (-ly, lx), which reproduces
    every observed pad: A6 local (-0.25,-4.045) -> board (23.695, 47.956).

    The pad's own angle is deliberately ignored: this footprint stores 90 on
    pads whose geometry is plainly unrotated (A6 is 1.45 across x and 0.30
    across y on the board, from a local size of 0.3 x 1.45), so applying it
    would double-count.
    """
    t = math.radians(-deg)
    return (px * math.cos(t) - py * math.sin(t), px * math.sin(t) + py * math.cos(t))


def seg_rect(p, q, cx, cy, hw, hh):
    """min distance from segment p-q to axis-aligned rect centred (cx,cy).

    The minimum between two convex sets is attained at a vertex of one and a
    point on the other, so testing both endpoints against the rect, the four
    corners against the segment, and the four edges against the segment is
    exact.  (Measuring to the rect's centre instead silently lets pads be
    approached far closer than the rule allows.)
    """
    r = (cx - hw, cy - hh, cx + hw, cy + hh)
    c = [(r[0], r[1]), (r[2], r[1]), (r[2], r[3]), (r[0], r[3])]
    best = min(pt_rect(p[0], p[1], r), pt_rect(q[0], q[1], r))
    for k in c:
        best = min(best, seg_pt(p, q, k))
    for i in range(4):
        best = min(best, seg_seg(p, q, c[i], c[(i + 1) % 4]))
    return best


def seg_seg(a, b, c, d):
    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2 = cross(c, d, a), cross(c, d, b)
    d3, d4 = cross(a, b, c), cross(a, b, d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(seg_pt(a, b, c), seg_pt(a, b, d), seg_pt(c, d, a), seg_pt(c, d, b))


def seg_pt(a, b, p):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L2))
    return math.hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy))


def pt_rect(x, y, r):
    dx = max(r[0] - x, 0.0, x - r[2])
    dy = max(r[1] - y, 0.0, y - r[3])
    return math.hypot(dx, dy)


def overlap(a, b):
    """True when two copper items can share a layer.

    A via or a thru-hole pad spans every layer, so it must be tested against
    everything; two SMD items only ever meet on a common side.  Comparing with
    plain `!=` silently skips every via, which is exactly the class of miss
    that let a via sit 0.110 mm from the /VBUS trunk on B.Cu.
    """
    return 'all' in (a, b) or a == b


def load(path):
    s = open(path).read()
    pads, segs, vias, edge = [], [], [], []
    pos = 0
    while True:
        k = s.find('(footprint ', pos)
        if k < 0:
            break
        d, j = 0, k
        while True:
            if s[j] == '(':
                d += 1
            elif s[j] == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        blk = s[k:j + 1]
        pos = j + 1
        ref = re.search(r'\(property "Reference" "([^"]*)"', blk)
        ref = ref.group(1) if ref else '?'
        fa = re.search(r'\(at ([\d.-]+) ([\d.-]+)(?: ([\d.-]+))?\)', blk)
        fx, fy, fr = float(fa.group(1)), float(fa.group(2)), float(fa.group(3) or 0)
        for pm in re.finditer(
                r'\(pad ("[^"]*"|\S+) (\S+) (\S+)\s*\(at ([\d.-]+) ([\d.-]+)'
                r'(?: ([\d.-]+))?\)\s*\(size ([\d.-]+) ([\d.-]+)\)(.*?)\n\s*\)\n',
                blk, re.S):
            name, ptype = pm.group(1).strip('"'), pm.group(2)
            lx, ly = float(pm.group(4)), float(pm.group(5))
            pa = float(pm.group(6) or 0)
            pw, ph = float(pm.group(7)), float(pm.group(8))
            bx, by = rot(lx, ly, fr)
            bx, by = fx + bx, fy + by
            if fr % 180 == 90:
                pw, ph = ph, pw
            dl = re.search(r'\(drill ([^)]*)\)', pm.group(9))
            drill = 0.0
            dhw = dhh = 0.0
            if dl:
                nums = [float(v) for v in re.findall(r'[\d.]+', dl.group(1))]
                drill = max(nums)
                if len(nums) >= 2:
                    # slotted (oval) drill: the two numbers are the slot's own
                    # axes in pad-local frame, so they swap with the pad.
                    dhw, dhh = nums[0] / 2, nums[1] / 2
                    if fr % 180 == 90:
                        dhw, dhh = dhh, dhw
                else:
                    dhw = dhh = nums[0] / 2
            nn = re.search(r'\(net "([^"]*)"\)', pm.group(9))
            net = nn.group(1) if nn else '-'
            r = (bx - pw / 2, by - ph / 2, bx + pw / 2, by + ph / 2)
            through = ptype in ('thru_hole', 'np_thru_hole')
            # read the pad's own layer set rather than assuming F.Cu; a B.Cu SMD
            # pad is invisible to F.Cu copper but very much in the way of B.Cu.
            lay = re.search(r'\(layers "([^"]*)"\)', pm.group(9))
            if through or (lay and '*.Cu' in lay.group(1)):
                pl = 'all'
            elif lay:
                pl = 'B.Cu' if 'B.Cu' in lay.group(1) else 'F.Cu'
            else:
                pl = 'F.Cu'
            if ptype == 'np_thru_hole':
                pads.append(dict(kind='hole', ref=ref, name=name, r=r,
                                 drill=drill, dhw=dhw, dhh=dhh,
                                 net=None, layers=None, thru=True))
            else:
                pads.append(dict(kind='pad', ref=ref, name=name, r=r,
                                 drill=drill, dhw=dhw, dhh=dhh,
                                 net=net, layers=pl, thru=through))
    for m in re.finditer(r'\(segment\s*\(start ([\d.-]+) ([\d.-]+)\)\s*'
                         r'\(end ([\d.-]+) ([\d.-]+)\)\s*\(width ([\d.-]+)\)\s*'
                         r'\(layer "([^"]+)"\)\s*\(net "([^"]*)"\)', s):
        segs.append(dict(p=(float(m.group(1)), float(m.group(2))),
                         q=(float(m.group(3)), float(m.group(4))),
                         w=float(m.group(5)), layer=m.group(6), net=m.group(7)))
    for m in re.finditer(r'\(via\s*\(at ([\d.-]+) ([\d.-]+)\)\s*\(size ([\d.-]+)\)\s*'
                         r'\(drill ([\d.-]+)\)\s*\(layers ([^)]*)\)\s*\(net "([^"]*)"\)', s):
        vias.append(dict(p=(float(m.group(1)), float(m.group(2))),
                         w=float(m.group(3)), drill=float(m.group(4)),
                         net=m.group(6)))
    for m in re.finditer(r'\(gr_(?:line|rect|poly)[^\n]*\n(?:.*?\n)*?', s):
        pass
    for m in re.finditer(r'\(gr_line\s*\(start ([\d.-]+) ([\d.-]+)\)\s*'
                         r'\(end ([\d.-]+) ([\d.-]+)\)[^)]*\)[^)]*\)\s*'
                         r'\(layer "Edge.Cuts"\)', s):
        edge.append(((float(m.group(1)), float(m.group(2))),
                     (float(m.group(3)), float(m.group(4)))))
    return pads, segs, vias, edge


def main():
    path = sys.argv[1]
    pads, segs, vias, edge = load(path)
    x0, x1, y0, y1 = RIP

    def same(a, b):
        for k, v in NET.items():
            if {a, b} <= {k, v}:
                return True
        return a == b

    def ripped(o):
        if o['net'] not in J4NETS:
            return False
        a = o['p']
        b = o.get('q', o['p'])
        for (c, d) in KEEP:
            if {tuple(a), tuple(b)} == {c, d}:
                return False
        for (c, d) in RIP_EXTRA:
            if {tuple(a), tuple(b)} == {c, d}:
                return True
        return all(x0 <= v[0] <= x1 and y0 <= v[1] <= y1 for v in (a, b))

    keep_segs = [o for o in segs if not ripped(o)]
    keep_vias = [o for o in vias if not ripped(o)]
    print(f'board: {len(pads)} pad/hole items, {len(keep_segs)} kept segments '
          f'(ripped {len(segs)-len(keep_segs)}), {len(keep_vias)} kept vias '
          f'(ripped {len(vias)-len(keep_vias)})')

    # ---- flat lists of new copper -------------------------------------
    new = []
    for i, (net, layer, pts) in enumerate(SEGS):
        for a, b in zip(pts, pts[1:]):
            new.append(dict(id=f'S{i}:{net}', kind='seg', layer=layer, net=net,
                            p=a, q=b, w=TRK))
    for i, (net, x, y, dia) in enumerate(VIAS):
        new.append(dict(id=f'V{i}:{net}', kind='via', layer='all', net=net,
                        p=(x, y), q=(x, y), w=dia, drill=dia - 0.30))

    bad = 0
    for e in new:
        worst, need, who = 1e9, CLR, ''
        for o in keep_segs:
            if not overlap(o['layer'], e['layer']) or same(o['net'], e['net']):
                continue
            d = seg_seg(e['p'], e['q'], o['p'], o['q']) - e['w'] / 2 - o['w'] / 2
            if d < worst:
                worst, need, who = d, CLR, f"seg {o['net']} {o['layer']} {o['p']}->{o['q']}"
        for o in keep_vias:
            if same(o['net'], e['net']):
                continue
            d = seg_pt(e['p'], e['q'], o['p']) - e['w'] / 2 - o['w'] / 2
            n = CLR
            for dd in (e['w'] / 2 + o['drill'] / 2,
                       (e.get('drill') or 0) / 2 + o['w'] / 2):
                if seg_pt(e['p'], e['q'], o['p']) - dd < d:
                    d, n = seg_pt(e['p'], e['q'], o['p']) - dd, CLR_HOLE
            if d < worst:
                worst, need, who = d, n, f"via {o['net']} {o['p']}"
        for o in pads:
            if o['kind'] != 'pad' or same(o['net'], e['net']):
                continue
            if not overlap(o['layers'], e['layer']):
                continue
            cx = (o['r'][0] + o['r'][2]) / 2
            cy = (o['r'][1] + o['r'][3]) / 2
            hw = (o['r'][2] - o['r'][0]) / 2
            hh = (o['r'][3] - o['r'][1]) / 2
            d = seg_rect(e['p'], e['q'], cx, cy, hw, hh) - e['w'] / 2
            n = CLR
            # Hole clearance, in both directions: this pad's drill against the
            # new copper, and -- when the new item is itself a via -- the new
            # via's drill against this pad's copper.
            if o['drill']:
                dh = seg_rect(e['p'], e['q'], cx, cy, o['dhw'], o['dhh']) - e['w'] / 2
                if dh < d:
                    d, n = dh, CLR_HOLE
            if e.get('drill'):
                dv = seg_rect(e['p'], e['q'], cx, cy, hw, hh) - e['drill'] / 2
                if dv < d:
                    d, n = dv, CLR_HOLE
            if d < worst:
                worst, need, who = d, n, (f"{o['ref']}.{o['name']} "
                                          f"{'TH ' if o['thru'] else ''}pad net={o['net']}")
        for o in pads:
            if o['kind'] != 'hole':
                continue
            hr = (o['r'][2] - o['r'][0]) / 2
            d = seg_rect(e['p'], e['q'], (o['r'][0] + o['r'][2]) / 2,
                         (o['r'][1] + o['r'][3]) / 2, hr, hr) - e['w'] / 2 - hr
            if d < worst:
                worst, need, who = d, CLR_HOLE, f"{o['ref']}.{o['name']} NPTH"
        for a, b in edge:
            d = min(seg_pt(e['p'], e['q'], a), seg_pt(e['p'], e['q'], b),
                    seg_pt(a, b, e['p']), seg_pt(a, b, e['q'])) - e['w'] / 2
            if d < worst:
                worst, need, who = d, CLR_EDGE, 'Edge.Cuts'
        flag = ''
        if worst < need:
            flag = '  <<< VIOLATION'
            bad += 1
        print(f'{e["id"]:<12} worst {worst:7.3f} (need {need:.3f})  {who}{flag}')

    # ---- new vs new ---------------------------------------------------
    print('\n--- new vs new ---')
    for i in range(len(new)):
        for j in range(i + 1, len(new)):
            a, b = new[i], new[j]
            if a['net'] == b['net']:
                continue
            if a['layer'] != b['layer'] and 'all' not in (a['layer'], b['layer']):
                continue
            d = seg_seg(a['p'], a['q'], b['p'], b['q']) - a['w'] / 2 - b['w'] / 2
            if d < CLR:
                print(f'  {a["id"]} vs {b["id"]}: {d:.3f}  <<< VIOLATION')
                bad += 1
    print(f'\n{bad} violations')


main()
