#!/usr/bin/env python3
"""J4 (USB-C) fan-out designer/checker for SWC.

RETIRED (SWC2 era).  Kept for provenance.  This models a J4 placement that was
REJECTED -- ``J4_NEW`` below is rot 90 @ 21.35, whereas the live board has J4 at
rot 270 (-90) @ 19.65, 48.206.  Do NOT repoint ``PCB`` at SWC.kicad_pcb to make
it "work": it would then read the live board while modelling the wrong J4 and
print confident, wrong clearances.  The stale path is deliberate -- it fails
loudly instead.  See ``tools/README.md`` and the ``swc2-j4-usb-orientation-bug``
memory before reviving it.

Read-only. Models the board *after* the planned J4 move (rot 270 @ 19.65, 48.206)
and the planned rip-up of the old fan-out, then tests a proposed fan-out
(segments + vias) for clearance violations against everything else.

Rules (from the board's own .kicad_pro):
    copper->copper        0.150 mm
    copper->hole edge     0.200 mm
    copper->board edge    0.500 mm
Geometry: pads are axis-aligned rectangles (all footprint rotations are 90 deg
multiples), vias/holes are circles, tracks are segments with width.  Distances
are exact for segment/segment, point/segment, point/rect, circle/rect, and are
treated as capsule-vs-primitive, i.e. track half-width subtracted at the end.
"""
import re, math, sys
from dataclasses import dataclass

PCB   = '<repo-root>/SWC2.kicad_pcb'
CLR      = 0.150
HOLE_CLR = 0.200
EDGE_CLR = 0.500
BOARD    = (16.0, 31.0, 70.0, 133.0)
J4_NEW   = (21.35, 48.206, 90.0)

# ------------------------------------------------------------------ parsing
def blocks(s, kw):
    out = []
    for m in re.finditer(r'\(' + kw + r'[ \n]', s):
        i = m.start(); d = 0; j = i
        while j < len(s):
            c = s[j]
            if c == '(':
                d += 1
            elif c == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        out.append(s[i:j + 1])
    return out


@dataclass
class Obj:
    kind: str      # pad | via | seg | hole
    net: str
    label: str
    g: tuple
    layers: frozenset = frozenset()   # empty = spans every copper layer


ALL_CU = frozenset({'F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu'})


def _pad_layers(b):
    m = re.search(r'\(layers ([^)]*)\)', b)
    if not m:
        return ALL_CU
    toks = set(re.findall(r'"([^"]+)"', m.group(1)))
    if '*.Cu' in toks or not toks:
        return ALL_CU
    return frozenset(t for t in toks if t.endswith('.Cu'))


def load():
    s = open(PCB).read()
    objs = []
    for fp in blocks(s, 'footprint'):
        ref = re.search(r'\(property "Reference" "([^"]+)"', fp).group(1)
        at = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', fp)
        fx, fy, rot = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
        if ref == 'J4':
            fx, fy, rot = J4_NEW
        th = math.radians(rot)
        for b in blocks(fp, 'pad'):
            nm = re.search(r'\(net "([^"]*)"\)', b)
            net = nm.group(1) if nm else ''
            pa = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', b)
            px, py = float(pa.group(1)), float(pa.group(2))
            prot = float(pa.group(3) or 0)
            bx = fx + px * math.cos(th) + py * math.sin(th)
            by = fy - px * math.sin(th) + py * math.cos(th)
            sz = re.search(r'\(size ([-\d.]+) ([-\d.]+)\)', b)
            w, h = float(sz.group(1)), float(sz.group(2))
            hd = re.search(r'\(pad "([^"]*)" ([a-z_]+)', b)
            typ = hd.group(2) if hd else 'smd'
            num = hd.group(1) if hd else '?'
            # 2026-09-10: J4's pads carry (at ... 90) but the (size w h) in the file
            # is already expressed in the FOOTPRINT frame -- the pad rotation is NOT
            # applied to it.  Proof: pads sit on a 0.5 mm row pitch, and only
            # w=0.3 (signal) / 0.6 (VBUS,GND) along that pitch direction gives the
            # 0.2 mm gaps that the board's own DRC reports as clean.  Using the pad
            # rotation there would make neighbours overlap by 0.95 mm.
            # So: rotate by the FOOTPRINT rotation only.
            if rot % 180 == 90:
                w, h = h, w
            objs.append(Obj('pad', net, f'{ref}.{num}', (bx, by, w / 2, h / 2),
                            _pad_layers(b)))
            if typ != 'smd':
                d1 = re.search(r'\(drill oval ([-\d.]+) ([-\d.]+)\)', b)
                if d1:
                    r = max(float(d1.group(1)), float(d1.group(2))) / 2
                else:
                    d2 = re.search(r'\(drill ([-\d.]+)\)', b)
                    r = float(d2.group(1)) / 2 if d2 else 0.3
                objs.append(Obj('hole', net, f'{ref}.{num}*drill', (bx, by, r),
                                ALL_CU))
    for b in blocks(s, 'via'):
        at = re.search(r'\(at ([-\d.]+) ([-\d.]+)\)', b)
        x, y = float(at.group(1)), float(at.group(2))
        nm = re.search(r'\(net "([^"]*)"\)', b)
        sz = re.search(r'\(size ([-\d.]+)\)', b)
        dr = re.search(r'\(drill ([-\d.]+)\)', b)
        objs.append(Obj('via', nm.group(1) if nm else '', 'via',
                        (x, y, float(sz.group(1)) / 2 if sz else 0.3), ALL_CU))
        objs.append(Obj('hole', nm.group(1) if nm else '', 'via*drill',
                        (x, y, float(dr.group(1)) / 2 if dr else 0.15), ALL_CU))
    for b in blocks(s, 'segment'):
        nm = re.search(r'\(net "([^"]*)"\)', b)
        st = re.search(r'\(start ([-\d.]+) ([-\d.]+)\)', b)
        en = re.search(r'\(end ([-\d.]+) ([-\d.]+)\)', b)
        wq = re.search(r'\(width ([-\d.]+)\)', b)
        ly = re.search(r'\(layer "([^"]+)"\)', b)
        objs.append(Obj('seg', nm.group(1) if nm else '', 'seg',
                        (float(st.group(1)), float(st.group(2)),
                         float(en.group(1)), float(en.group(2)),
                         float(wq.group(1)) if wq else 0.2),
                        frozenset({ly.group(1)}) if ly else ALL_CU))
    return objs


# ---------------------------------------------------------------- geometry
def pt_seg(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    if L == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def seg_seg(p1, p2, p3, p4):
    if _x(p1, p2, p3, p4):
        return 0.0
    return min(pt_seg(p1, p3, p4), pt_seg(p2, p3, p4),
               pt_seg(p3, p1, p2), pt_seg(p4, p1, p2))


def _ccw(a, b, c):
    return (c[1] - a[1]) * (b[0] - a[0]) > (b[1] - a[1]) * (c[0] - a[0])


def _x(a, b, c, d):
    return _ccw(a, c, d) != _ccw(b, c, d) and _ccw(a, b, c) != _ccw(a, b, d)


def pt_rect(p, cx, cy, hw, hh):
    return math.hypot(max(abs(p[0] - cx) - hw, 0.0),
                      max(abs(p[1] - cy) - hh, 0.0))


def seg_rect(a, b, cx, cy, hw, hh):
    x0, x1 = cx - hw, cx + hw
    y0, y1 = cy - hh, cy + hh
    # inside?
    if min(a[0], b[0]) >= x0 and max(a[0], b[0]) <= x1 and \
       min(a[1], b[1]) >= y0 and max(a[1], b[1]) <= y1:
        return -min(min(a[0] - x0, x1 - a[0], a[1] - y0, y1 - a[1]),
                    min(b[0] - x0, x1 - b[0], b[1] - y0, y1 - b[1]))
    c = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    d = min(seg_seg(a, b, c[i], c[(i + 1) % 4]) for i in range(4))
    return d


def rect_rect(a, b):
    ax, ay, ahw, ahh = a; bx, by, bhw, bhh = b
    dx = max(abs(ax - bx) - ahw - bhw, 0.0)
    dy = max(abs(ay - by) - ahh - bhh, 0.0)
    if dx == 0 and dy == 0:
        return -min(ahw + bhw - abs(ax - bx), ahh + bhh - abs(ay - by))
    return math.hypot(dx, dy)


def dist(n, o):
    """Raw geometric distance between proposed item `n` and obstacle `o`,
    in the sense of capsule surfaces (track half-width already subtracted)."""
    nk, ng = n.kind, n.g
    ok, og = o.kind, o.g
    if nk == 'seg':
        x1, y1, x2, y2, w = ng
        a, b, half = (x1, y1), (x2, y2), w / 2
        if ok == 'seg':
            return seg_seg(a, b, (og[0], og[1]), (og[2], og[3])) - half - og[4] / 2
        if ok == 'via':
            return pt_seg((og[0], og[1]), a, b) - og[2] - half
        if ok == 'hole':
            return pt_seg((og[0], og[1]), a, b) - og[2] - HOLE_CLR - half
        if ok == 'pad':
            return seg_rect(a, b, og[0], og[1], og[2], og[3]) - half
    if nk == 'via':
        cx, cy, r = ng
        if ok == 'seg':
            return pt_seg((cx, cy), (og[0], og[1]), (og[2], og[3])) - r - og[4] / 2
        if ok == 'via':
            return math.hypot(cx - og[0], cy - og[1]) - r - og[2]
        if ok == 'hole':
            return math.hypot(cx - og[0], cy - og[1]) - r - og[2] - HOLE_CLR
        if ok == 'pad':
            return pt_rect((cx, cy), og[0], og[1], og[2], og[3]) - r
    raise ValueError(nk)


# --------------------------------------------------- old fan-out to rip up
RIP = [
    ('F.Cu', (23.695, 46.956), (19.500, 46.956)),
    ('F.Cu', (19.500, 46.956), (19.500, 49.420)),
    ('F.Cu', (19.500, 49.420), (18.891, 49.420)),
    ('B.Cu', (18.891, 49.420), (19.500, 50.400)),
    ('B.Cu', (19.500, 50.400), (26.039, 50.400)),
    ('F.Cu', (23.695, 47.456), (20.347, 47.456)),
    ('F.Cu', (20.347, 47.456), (20.347, 48.820)),
    ('F.Cu', (23.695, 48.456), (20.347, 48.456)),
    ('B.Cu', (20.347, 48.820), (20.347, 46.400)),
    ('B.Cu', (20.347, 46.400), (25.220, 46.400)),
    ('F.Cu', (23.695, 47.956), (25.269, 47.956)),
    ('F.Cu', (23.695, 48.956), (25.500, 48.956)),
    ('F.Cu', (23.695, 49.956), (25.900, 49.956)),
    ('F.Cu', (23.695, 50.656), (27.513, 50.656)),
    ('B.Cu', (24.200, 45.800), (24.200, 43.000)),
]

def key(x1, y1, x2, y2):
    a = (round(x1, 3), round(y1, 3)); b = (round(x2, 3), round(y2, 3))
    return tuple(sorted([a, b]))

RIPSET = {key(*p, *q) for _, p, q in RIP}


def obstacles(objs):
    out = []
    for o in objs:
        if o.kind == 'seg':
            if key(o.g[0], o.g[1], o.g[2], o.g[3]) in RIPSET:
                continue
        out.append(o)
    return out


# ---------------------------------------------------------------- analysis
def check(items, obs, verbose=False):
    """items: list of Obj (kind 'seg' or 'via').  Returns list of
    (gap, item_label, obstacle_label, net_i, net_o)."""
    bad = []
    for i, n in enumerate(items):
        for o in obs:
            if n.net and o.net and n.net == o.net:
                continue          # same net: no clearance rule
            if n.layers and o.layers and not (n.layers & o.layers):
                continue          # different copper layers: no interaction
            d = dist(n, o)
            # dist() already subtracts HOLE_CLR for hole obstacles, so a hole
            # pair is clean at d >= 0; copper-to-copper needs CLR.
            lim = 0.0 if 'hole' in (n.kind, o.kind) else CLR
            if d < lim - 1e-9:
                bad.append((d - lim, n.label, o.label + f" {o.net}", n.net, o.net))
        # board edge
        if n.kind == 'via':
            e = min(n.g[0] - BOARD[0], BOARD[2] - n.g[0],
                    n.g[1] - BOARD[1], BOARD[3] - n.g[1]) - n.g[2]
        else:
            x1, y1, x2, y2, w = n.g
            e = min(min(x1, x2) - BOARD[0], BOARD[2] - max(x1, x2),
                    min(y1, y2) - BOARD[1], BOARD[3] - max(y1, y2)) - w / 2
        if e < EDGE_CLR:
            bad.append((e - EDGE_CLR, n.label, 'BOARD-EDGE', n.net, ''))
        for m in items[i + 1:]:
            if n.net == m.net:
                continue
            if n.layers and m.layers and not (n.layers & m.layers):
                continue
            d = dist(n, m)
            lim = 0.0 if 'hole' in (n.kind, m.kind) else CLR
            if d < lim - 1e-9:
                bad.append((d - lim, n.label, m.label + ' (new)', n.net, m.net))
    bad.sort()
    return bad


if __name__ == '__main__':
    objs = obstacles(load())
    print(f"obstacles (old J4 fan-out removed): {len(objs)}")
    j4 = [o for o in objs if o.label.startswith('J4.') and o.kind == 'pad']
    print(f"J4 pads at the new rot-90 location: {len(j4)}")
    for o in sorted(j4, key=lambda o: o.g[1]):
        print(f"   {o.label:<8s} {o.net:<28s} "
              f"({o.g[0]:7.3f},{o.g[1]:7.3f}) hw={o.g[2]:.3f} hh={o.g[3]:.3f}")
    # what does a signal pad's own row look like?  list obstacles within 2 mm
    print("\n--- obstacles within 2.2 mm of the pad column corner (18.0, 45.0..52) ---")
    for o in objs:
        if o.kind == 'pad':
            cx, cy, hw, hh = o.g
            if 16.0 < cx < 24.5 and 43.0 < cy < 53.5:
                print(f"   pad {o.label:<10s} {o.net:<30s} "
                      f"({cx:7.3f},{cy:7.3f}) hw={hw:.3f} hh={hh:.3f}")
    print("\n--- vias within 2 mm of the corridor ---")
    for o in objs:
        if o.kind == 'via' and 16.0 < o.g[0] < 26.0 and 43.0 < o.g[1] < 53.0:
            print(f"   via ({o.g[0]:7.3f},{o.g[1]:7.3f}) r={o.g[2]:.2f}  {o.net}")
