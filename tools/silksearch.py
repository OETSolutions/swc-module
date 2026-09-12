#!/usr/bin/env python3
"""Find a DRC-legal spot for a footprint's Reference field.

pcbfields.py solves the same problem but with inflated margins (0.30 pad /
0.35 text / 0.15 silk) that DRC does not require, so on a dense board it
declares "no spot" and parks the designator 11 mm away.  DRC's silk rules are
zero-clearance: ink may not TOUCH copper, other ink, or the board edge.

This models every obstacle DRC's silk checks see -- pads (mask aperture),
footprint and board silk graphics (including a footprint's own user text),
other designators and board labels, and the board edge.  Vias are deliberately
absent: DRC tests the solder-mask APERTURE, and this board tents every via
front and back, so no via can clip silk.

Read-only.

Usage: silksearch.py PCB REF [RADIUS]
"""
import math
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                       # noqa: E402
import j4fan                                  # noqa: E402
import silk                                   # noqa: E402
from mcp_server_kicad import _cst as C        # noqa: E402

ADV, TH, LH = 1.20, 0.15, 1.45                # pcbfields.py text metrics
BOARD = (16.0, 31.0, 70.0, 133.0)
EDGE_MARGIN = 0.20
# DRC's silk_over_copper tests against the SOLDER-MASK APERTURE, not the bare
# pad, and its drawn text box is a little larger than ADV/LH model.  Measured
# on RT1: a modelled 0.175 mm gap still tripped DRC.  Require this much slack.
SAFETY = 0.22

# THE TRANSFORM IS NOT LOCAL TO THIS FILE.  This used to define its own
# rot_pt as (x cos t - y sin t, x sin t + y cos t), which is the opposite
# sense to the one KiCad uses, so every obstacle belonging to a footprint
# rotated +/-90 was placed at its mirror image about the footprint origin and
# this tool's "clean spots" were clean of nothing.  Only parts at 0/180 were
# ever right, which is why the error survived a full placement pass.
#
# silk.rot_pt is the correct one -- the same sense as boardgeo.rot_pt, whose
# header records two of KiCad's own DRC messages pinning it on rotated
# footprints.  Re-checked here against a third source: F2, footprint at
# (31.119, 50.07) rot 90 with its Reference at local (6.25, -2.50), plots in
# KiCad's own F.SilkS SVG at (28.619, 43.820), which is what silk.rot_pt
# gives and what the old sign did not (it gave 33.619, 56.320).  Delegating
# rather than copying so the two can never drift apart again.
rot_pt = silk.rot_pt


def local_to_board(fp, pt):
    return silk.xform(fp, pt[0], pt[1])


def drawn_angle(a):
    """The angle KiCad draws a footprint text at: the STORED angle alone.

    Not the footprint's plus it -- see silk.drawn_angle, established from the
    rotate() groups in KiCad's own SVG export.  Getting this wrong turns a
    designator's collision box through 90 degrees without moving its anchor.
    """
    return silk.drawn_angle(a)


def num(n):
    return float(n.text) if n is not None else None


def child(e, kw):
    for c in e.children:
        if c.atoms and c.atoms[0].text == kw:
            return c
    return None


def children(e, kw):
    return [c for c in e.children if c.atoms and c.atoms[0].text == kw]


def atom_text(e, i):
    a = e.atoms[i]
    return a.text.strip('"')


def xy(e, kw):
    """(x, y) of a child like (start 1 2)."""
    c = child(e, kw)
    return (num(c.atoms[1]), num(c.atoms[2]))


def layer_of(e):
    c = child(e, 'layer')
    return atom_text(c, 1) if c else ''


def stroke_w(e):
    s = child(e, 'stroke')
    if s:
        w = child(s, 'width')
        if w:
            return num(w.atoms[1])
    return 0.15


# --------------------------------------------------------------- obstacles
def load_obstacles(path, skip_ref):
    doc = C.parse(open(path, 'rb').read())
    root = doc.children[0]
    obs = []          # (kind, geom, label)

    def add_text_box(label, text, x, y, ang, s=1.0):
        # render_text, not len(): markup like ~{X} is not drawn, and counting
        # it over-states the width.  silk.collect measures the same way.
        L = len(F.render_text(text)) * s * ADV + TH
        H = s * LH + TH
        w, h = (H, L) if ang % 180 == 90 else (L, H)
        obs.append(('rect', (x - w / 2, y - h / 2, x + w / 2, y + h / 2),
                    label))

    for fp in children(root, 'footprint'):
        a = child(fp, 'at')
        fx, fy = num(a.atoms[1]), num(a.atoms[2])
        frot = num(a.atoms[3]) if len(a.atoms) > 3 else 0.0
        F0 = (fx, fy, frot)
        ref = '?'
        for p in children(fp, 'property'):
            if atom_text(p, 1) == 'Reference':
                ref = atom_text(p, 2)
        for p in children(fp, 'property'):
            if atom_text(p, 1) not in ('Reference', 'Value'):
                continue
            hide = any(c.atoms and c.atoms[0].text == 'hide' for c in p.children)
            if hide:
                continue
            # A field is only an obstacle if it is PRINTED.  All 100 of this
            # board's Value fields sit on F.Fab, and F.Fab is not silkscreen --
            # counting them made this tool avoid 108 phantom texts and report
            # "no clean spot within 8 mm" for parts that in fact have room.
            lay = child(p, 'layer')
            if lay is None or atom_text(lay, 1) != 'F.SilkS':
                continue
            at = child(p, 'at')
            lx, ly = num(at.atoms[1]), num(at.atoms[2])
            la = num(at.atoms[3]) if len(at.atoms) > 3 else 0.0
            sz = child(p, 'effects')
            s = 1.0
            if sz:
                fnt = child(sz, 'font')
                if fnt:
                    sc = child(fnt, 'size')
                    if sc:
                        s = num(sc.atoms[1])
            if atom_text(p, 1) == 'Reference' and atom_text(p, 2) == skip_ref:
                continue
            x, y = local_to_board(F0, (lx, ly))
            add_text_box('text:' + atom_text(p, 2), atom_text(p, 2), x, y,
                         drawn_angle(la), s)
        for pad in children(fp, 'pad'):
            at = child(pad, 'at')
            px, py = num(at.atoms[1]), num(at.atoms[2])
            prot = num(at.atoms[3]) if len(at.atoms) > 3 else 0.0
            sz = child(pad, 'size')
            w, h = num(sz.atoms[1]), num(sz.atoms[2])
            if (frot + prot) % 180 == 90:
                w, h = h, w
            x, y = local_to_board(F0, (px, py))
            obs.append(('rect', (x - w / 2, y - h / 2, x + w / 2, y + h / 2),
                        f'{ref}.{atom_text(pad, 1)}'))
        for g in fp.children:
            kw = g.atoms[0].text if g.atoms else ''
            if kw not in ('fp_line', 'fp_rect', 'fp_circle', 'fp_arc', 'fp_poly',
                          'fp_text'):
                continue
            if layer_of(g) != 'F.SilkS':
                continue
            if kw == 'fp_text':
                # User text on the footprint (BZ1's "(+)" is one) is printed
                # silkscreen and collides like any other ink.  The Reference and
                # Value fields are `property` nodes in KiCad 10, handled above.
                if any(c.atoms and c.atoms[0].text == 'hide' for c in g.children):
                    continue
                t = ''
                for a in g.atoms[1:]:
                    if a.text.startswith('"'):
                        t = a.text.strip('"')
                        break
                a = child(g, 'at')
                bx, by = local_to_board(F0, (num(a.atoms[1]), num(a.atoms[2])))
                s = 1.0
                f = child(g, 'effects')
                if f:
                    fnt = child(f, 'font')
                    if fnt:
                        sc = child(fnt, 'size')
                        if sc:
                            s = num(sc.atoms[1])
                fa = num(a.atoms[3]) if len(a.atoms) > 3 else 0.0
                add_text_box('text:' + ref, t, bx, by, drawn_angle(fa), s)
                continue
            hw = stroke_w(g) / 2
            if kw in ('fp_line', 'fp_rect'):
                p1 = local_to_board(F0, xy(g, 'start'))
                p2 = local_to_board(F0, xy(g, 'end'))
                pts = [p1, p2] if kw == 'fp_line' else [
                    p1, (p2[0], p1[1]), p2, (p1[0], p2[1]), p1]
                for i in range(len(pts) - 1):
                    obs.append(('seg', (*pts[i], *pts[i + 1], hw), f'silk:{ref}'))
            elif kw == 'fp_circle':
                c = local_to_board(F0, xy(g, 'center'))
                e = local_to_board(F0, xy(g, 'end'))
                obs.append(('circ', (c[0], c[1],
                                     math.hypot(e[0] - c[0], e[1] - c[1]), hw),
                            f'silk:{ref}'))
            elif kw == 'fp_poly':
                pts = [local_to_board(F0, (num(q.atoms[1]), num(q.atoms[2])))
                       for q in children(child(g, 'pts'), 'xy')]
                for i in range(len(pts)):
                    obs.append(('seg', (*pts[i], *pts[(i + 1) % len(pts)], hw),
                                f'silk:{ref}'))
            elif kw == 'fp_arc':
                p1 = local_to_board(F0, xy(g, 'start'))
                pm = local_to_board(F0, xy(g, 'mid'))
                p2 = local_to_board(F0, xy(g, 'end'))
                prev = p1
                for t in [i / 8 for i in range(1, 9)]:
                    x = ((1 - t) ** 2 * p1[0] + 2 * (1 - t) * t * pm[0]
                         + t * t * p2[0])
                    y = ((1 - t) ** 2 * p1[1] + 2 * (1 - t) * t * pm[1]
                         + t * t * p2[1])
                    obs.append(('seg', (*prev, x, y, hw), f'silk:{ref}'))
                    prev = (x, y)
    for g in children(root, 'gr_line') + children(root, 'gr_rect') \
            + children(root, 'gr_poly'):
        if layer_of(g) != 'F.SilkS':
            continue
        hw = stroke_w(g) / 2
        if g.atoms[0].text == 'gr_poly':
            pts = [(num(q.atoms[1]), num(q.atoms[2]))
                   for q in children(child(g, 'pts'), 'xy')]
        else:
            p1 = xy(g, 'start'); p2 = xy(g, 'end')
            pts = [p1, p2] if g.atoms[0].text == 'gr_line' else [
                p1, (p2[0], p1[1]), p2, (p1[0], p2[1]), p1]
        for i in range(len(pts) - 1):
            obs.append(('seg', (*pts[i], *pts[i + 1], hw), 'bsilk'))
    for g in children(root, 'gr_text'):
        if layer_of(g) != 'F.SilkS':
            continue
        t = atom_text(g, 1)
        a = child(g, 'at')
        x, y = num(a.atoms[1]), num(a.atoms[2])
        rot = num(a.atoms[3]) if len(a.atoms) > 3 else 0.0
        add_text_box('pcb:' + t, t, x, y, drawn_angle(rot))
    return obs


def rect_dist(b, o):
    """Signed distance from text box b to obstacle o (negative = overlap)."""
    x0, y0, x1, y1 = b
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2
    k, g, _l = o
    if k == 'rect':
        gx = max(x0, g[0]) - min(x1, g[2])
        gy = max(y0, g[1]) - min(y1, g[3])
        if gx > 0 or gy > 0:
            return math.hypot(max(gx, 0.0), max(gy, 0.0))
        return max(gx, gy)
    if k == 'seg':
        d = j4fan.seg_rect((g[0], g[1]), (g[2], g[3]), cx, cy, hw, hh)
        return d - g[4]
    if k == 'circ':
        return j4fan.pt_rect((g[0], g[1]), cx, cy, hw, hh) - g[2] - g[3]
    raise ValueError(k)


SIZE = 1.0


def score(obs, b):
    worst = 1e9
    for o in obs:
        d = rect_dist(b, o)
        if d < worst:
            worst = d
    e = min(b[0] - BOARD[0], BOARD[2] - b[2],
            b[1] - BOARD[1], BOARD[3] - b[3])
    return min(worst, e)


def main():
    pcb, ref = sys.argv[1], sys.argv[2]
    radius = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
    global SIZE
    if len(sys.argv) > 4:
        SIZE = float(sys.argv[4])
    obs = load_obstacles(pcb, ref)
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    fp = None
    for f in children(root, 'footprint'):
        for p in children(f, 'property'):
            if atom_text(p, 1) == 'Reference' and atom_text(p, 2) == ref:
                fp = f
    a = child(fp, 'at')
    fx, fy = num(a.atoms[1]), num(a.atoms[2])
    print(f'{ref} at ({fx},{fy})   obstacles {len(obs)}   radius {radius}')

    def box(ang, x, y):
        L = len(F.render_text(ref)) * SIZE * ADV + TH
        H = SIZE * LH + TH
        w, h = (H, L) if ang % 180 == 90 else (L, H)
        return (x - w / 2, y - h / 2, x + w / 2, y + h / 2)

    best = []
    step = 0.1
    n = int(radius / step)
    for ang in (0, 90):
        for i in range(-n, n + 1):
            for j in range(-n, n + 1):
                x, y = fx + i * step, fy + j * step
                s = score(obs, box(ang, x, y))
                if s > SAFETY:
                    best.append((math.hypot(x - fx, y - fy), s, ang, x, y))
    best.sort()
    if not best:
        print('   nothing clean within the radius')
        return
    seen = []
    for d, s, ang, x, y in best:
        if any(abs(x - u) < 0.5 and abs(y - v) < 0.5 for _, u, v in seen):
            continue
        seen.append((ang, x, y))
        print(f'   ({x:8.3f},{y:8.3f}) ang {ang:3d}   gap {s:6.3f}   '
              f'{d:5.2f} mm from part')
        if len(seen) >= 6:
            break


main()
