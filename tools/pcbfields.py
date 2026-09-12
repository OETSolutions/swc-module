#!/usr/bin/env python3
"""Place every PCB reference designator in a legal, readable spot.

DRC reports silkscreen collisions one pair at a time and says nothing about
which way the text reads; both matter here.  The placement directive is that
every silk item reads UP (0 deg) or ROTATED LEFT (90 deg), consistently, and
sits on no pad, under no component, and clear of the board edge.

The solver separates the two kinds of rule, which is what makes it fast and
predictable:

  * HARD constraints go into a binary occupancy raster -- pads, the courtyards
    of parts big enough to hide text under, vias, the board edge, and design-
    ators already placed.  Rasterised once into an integral image, so "does
    this box touch anything?" is four array lookups instead of a rectangle
    scan.  A candidate that is clean is taken immediately.
  * SOFT preferences rank the candidates that are clean, and are the only
    thing left to trade when nothing is: proximity to the part it names,
    proximity to where it already was, and not covering a small neighbour.
    Without them a clean-but-silly spot six millimetres away wins over the
    tidy one beside the part.

Everything is driven through the MCP server's CST parser.  --apply writes.

Usage: pcbfields.py PCB [--apply] [--move REF ...] [--only REF ...] [--verbose]

--move is the surgical form: only the named designators may be relocated,
every other one stays where it is and stays in the obstacle set, so a mover
still cannot land on it.  --only additionally drops the others from the
obstacle set, which frees a mover to overlap them -- useful for a from-
scratch pass, wrong for touching up the few that actually collide.
"""
import math
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
import silk                                               # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

PCB = silk.PCB

# --- clearances, in mm from the drawn ink ------------------------------
PAD_MARGIN = 0.30      # silk ink to pad copper
TEXT_MARGIN = 0.35     # silk ink to other silk ink
BODY_MARGIN = 0.25     # silk ink to a large part's courtyard
SILK_MARGIN = 0.15     # silk ink to another footprint's printed outline
EDGE_MARGIN = 0.40     # silk ink to the board edge
BIG_PART = 12.0        # mm^2 of courtyard above which a part hides text

# --- soft weights ------------------------------------------------------
W_NEIGHBOUR = 30.0     # per mm^2 of a small neighbour's courtyard covered
W_ANGLE = 0.8          # for turning a designator that was already upright
W_SILK = 25.0          # per mm^2 of another footprint's printed outline covered
W_TXT = 200.0          # per mm^2 of a designator already placed covered
W_BODY = 500.0         # per mm^2 of a large part's courtyard covered
W_OCC = 400.0          # per mm^2 still touching a pad, a via or the edge

# Silk ink belongs in the SOFT set, not the hard one.  DRC scores the three silk
# rule families very differently and this board cannot satisfy all of them at
# once: silk_over_copper (ink on a pad) and silk_edge_clearance (ink clipped by
# the outline) are real -- the first is an assembly/rework problem, the second
# is the board reading as unfinished -- while silk_overlap (two printed items
# touching) is cosmetic and no fab acts on it.  Marking every footprint outline
# hard made 102 of 108 designators unclean, so the solver had no clean spot to
# aim at and fell back on the least-bad one; it now keeps ink off pads and the
# edge absolutely and merely prefers not to print over a neighbour.


CELL = 0.2             # occupancy raster pitch, mm
PITCH = 0.25           # candidate lattice pitch, mm
RADIUS = 7.0           # how far a designator may stray from its part, mm

# Text metrics.  A glyph's advance is wider than its nominal size, and the
# box KiCad tests for clearance is taller still -- it carries ascender and
# descender allowance on top of the cap height.  Modelling the box as
# size+thickness (the naive reading) understates it enough that the solver
# calls a spot clean and DRC then reports the same pair as overlapping, so
# these two factors are deliberately generous: overestimating the ink only
# makes the solver keep its distance.
TEXT_ADVANCE = 1.20    # per glyph, as a multiple of the nominal size
TEXT_HEIGHT = 1.45     # box height, as a multiple of the nominal size


class Raster:
    """Occupancy over the board, with an integral image for O(1) tests.

    Two of these run side by side -- one for what a designator may not touch,
    one for the designators already placed -- so "is this box free?" is a test
    against each rather than a count that then has to be told apart from the
    candidate's own ink.
    """

    def __init__(self, box, cell=CELL):
        self.cell = cell
        self.x0 = math.floor((box[0] - 10.0) / cell)
        self.y0 = math.floor((box[1] - 10.0) / cell)
        self.w = int((box[2] + 10.0) / cell) - self.x0 + 2
        self.h = int((box[3] + 10.0) / cell) - self.y0 + 2
        self.buf = [0] * (self.w * self.h)
        self.sum = None

    def _i(self, x, y):
        return (int((x / self.cell)) - self.x0,
                int((y / self.cell)) - self.y0)

    def mark(self, x0, y0, x1, y1, v=1):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        i0, j0 = self._i(x0, y0)
        i1, j1 = self._i(x1, y1)
        i0, j0 = max(0, i0), max(0, j0)
        i1, j1 = min(self.w - 1, i1), min(self.h - 1, j1)
        buf = self.buf
        w = self.w
        for j in range(j0, j1 + 1):
            row = j * w
            for i in range(i0, i1 + 1):
                buf[row + i] += v
        self.sum = None

    def build(self):
        w, h = self.w, self.h
        s = [[0] * (w + 1) for _ in range(h + 1)]
        buf = self.buf
        for j in range(h):
            row = j * w
            sj = s[j]
            sj1 = s[j + 1]
            acc = 0
            for i in range(w):
                acc += buf[row + i]
                sj1[i + 1] = sj[i + 1] + acc
        self.sum = s

    def count(self, x0, y0, x1, y1):
        """Total occupancy weight inside the box, in O(1)."""
        if self.sum is None:
            self.build()
        i0, j0 = self._i(min(x0, x1), min(y0, y1))
        i1, j1 = self._i(max(x0, x1), max(y0, y1))
        i0, j0 = max(0, i0), max(0, j0)
        i1, j1 = min(self.w - 1, i1), min(self.h - 1, j1)
        if i1 < i0 or j1 < j0:
            return 0
        s = self.sum
        return (s[j1 + 1][i1 + 1] - s[j0][i1 + 1]
                - s[j1 + 1][i0] + s[j0][i0])


class Weighted:
    """Bucketed rectangles, for ranking candidates that are already clean."""

    def __init__(self, cell=2.0):
        self.cell = cell
        self.g = {}

    def add(self, x0, y0, x1, y1, w):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        r = (x0, y0, x1, y1, w)
        for i in range(int(x0 // self.cell), int(x1 // self.cell) + 1):
            for j in range(int(y0 // self.cell), int(y1 // self.cell) + 1):
                self.g.setdefault((i, j), []).append(r)

    def cost(self, x0, y0, x1, y1):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        seen = set()
        total = 0.0
        for i in range(int(x0 // self.cell), int(x1 // self.cell) + 1):
            for j in range(int(y0 // self.cell), int(y1 // self.cell) + 1):
                for r in self.g.get((i, j), ()):
                    if id(r) in seen:
                        continue
                    seen.add(id(r))
                    ox = min(x1, r[2]) - max(x0, r[0])
                    oy = min(y1, r[3]) - max(y0, r[1])
                    if ox > 0 and oy > 0:
                        total += ox * oy * r[4]
        return total


def inflate(b, m):
    return (b[0] - m, b[1] - m, b[2] + m, b[3] + m)


def footprint_at(fp):
    a = F.child(fp, 'at')
    return (F.fnum(a.atoms[1].text), F.fnum(a.atoms[2].text),
            F.fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0)


def graphic_extent(g):
    """Local (minx, miny, maxx, maxy) of one footprint graphic.

    A circle needs special handling that reading `center` and `end` as two
    ordinary points does not give you.  KiCad writes a circle as
    `(center X Y) (end X Y)` with NO radius node -- the radius is the
    centre-to-end distance -- so taking the bounding box of those two stored
    points collapses the circle onto the line between them.  BZ1's 17mm
    buzzer courtyard, a circle r=8.5 about local (3.8, 0), came out as
    (3.8, 0.0, 12.3, 0.0): zero height.  The solver then priced a designator
    parked inside the buzzer can at essentially zero and did exactly that,
    four times.  The same bug silenced the circular courtyard of every
    electrolytic on the board.

    The `radius` node is still honoured when present, since a hand-written
    or older footprint may carry one.
    """
    ce = F.child(g, 'center')
    en = F.child(g, 'end')
    if ce is not None and en is not None and F.child(g, 'start') is None:
        # (center ...) (end ...) and no (start ...) -- KiCad's circle, whose
        # radius is implied rather than stored.
        cx, cy = F.fnum(ce.atoms[1].text), F.fnum(ce.atoms[2].text)
        ex, ey = F.fnum(en.atoms[1].text), F.fnum(en.atoms[2].text)
        r = math.hypot(ex - cx, ey - cy)
        return (cx - r, cy - r, cx + r, cy + r)

    pts = []
    for key in ('start', 'end', 'mid', 'center'):
        n = F.child(g, key)
        if n is not None:
            pts.append((F.fnum(n.atoms[1].text), F.fnum(n.atoms[2].text)))
    rad = F.child(g, 'radius')
    if rad is not None and pts:
        r = F.fnum(rad.atoms[1].text)
        cx, cy = pts[-1]
        pts = [(cx - r, cy - r), (cx + r, cy + r)]
    p = F.child(g, 'pts')
    if p is not None:
        for xy in F.children(p, 'xy'):
            pts.append((F.fnum(xy.atoms[1].text), F.fnum(xy.atoms[2].text)))
    if not pts:
        return None
    xs = [q[0] for q in pts]
    ys = [q[1] for q in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def courtyard_of(fp):
    """Board-space bbox of a footprint's F.CrtYd graphics.

    A footprint whose courtyard is a circle otherwise parses as a degenerate
    line -- see graphic_extent.
    """
    xs, ys = [], []
    for head in ('fp_line', 'fp_rect', 'fp_arc', 'fp_circle', 'fp_poly'):
        for g in F.children(fp, head):
            lay = F.child(g, 'layer')
            if lay is None or lay.atoms[1].text.strip('"') != 'F.CrtYd':
                continue
            e = graphic_extent(g)
            if e is None:
                continue
            xs += [e[0], e[2]]
            ys += [e[1], e[3]]
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def silk_graphics(fp, fat, layer='F.SilkS'):
    """Board-space bboxes of a footprint's own printed silkscreen.

    Pads and courtyards are not the whole obstacle set.  A part's outline is
    routinely drawn a little outside its courtyard, so a designator can clear
    every courtyard on the board and still land squarely on someone's printed
    outline -- which is how R54 came to sit on U6.  Each graphic is returned
    with its stroke half-width so the caller can inflate by ink, not by centre
    line.

    A rectangle or polygon is marked as its whole bounding box rather than as
    its four edges.  That over-blocks the interior, but the interior of a
    component outline is the component, and a designator there would be
    hidden anyway.
    """
    out = []
    for head in ('fp_line', 'fp_rect', 'fp_arc', 'fp_circle', 'fp_poly'):
        for g in F.children(fp, head):
            lay = F.child(g, 'layer')
            if lay is None or lay.atoms[1].text.strip('"') != layer:
                continue
            half = 0.10
            stroke = F.child(g, 'stroke')
            if stroke is not None:
                wn = F.child(stroke, 'width')
                if wn is not None:
                    half = F.fnum(wn.atoms[1].text) / 2.0
            e = graphic_extent(g)
            if e is None:
                continue
            out.append((to_board(e, fat), half))
    return out


def to_board(box, fat):
    pts = [silk.xform(fat, cx, cy)
           for cx, cy in ((box[0], box[1]), (box[2], box[1]),
                          (box[2], box[3]), (box[0], box[3]))]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def ref_of(fp):
    for p in F.children(fp, 'property'):
        if p.atoms[1].text.strip('"') == 'Reference':
            return p.atoms[2].text.strip('"')
    return ''


def main():
    pcb = PCB
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if args:
        pcb = args[0]
    apply_ = '--apply' in sys.argv
    verbose = '--verbose' in sys.argv
    only = []
    if '--only' in sys.argv:
        only = sys.argv[sys.argv.index('--only') + 1:]
    move = set()
    if '--move' in sys.argv:
        move = set(sys.argv[sys.argv.index('--move') + 1:])
    global RADIUS
    if '--radius' in sys.argv:
        RADIUS = float(sys.argv[sys.argv.index('--radius') + 1])

    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]
    items = [i for i in silk.collect(root)
             if i['src'] == 'fp' and i['kind'] == 'Reference']
    if only:
        items = [i for i in items if i['ref'] in only]

    box = silk.outline(root)
    occ = Raster(box)
    soft = Weighted()

    # ---- hard: pads, all layers ---------------------------------------
    pads_by_ref = {}
    for fp in F.children(root, 'footprint'):
        ref = ref_of(fp)
        ps = []
        fat = footprint_at(fp)
        for pd in F.children(fp, 'pad'):
            at = F.child(pd, 'at')
            s = F.child(pd, 'size')
            if at is None or s is None:
                continue
            px, py = F.fnum(at.atoms[1].text), F.fnum(at.atoms[2].text)
            pr = F.fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
            sw, sh = F.fnum(s.atoms[1].text), F.fnum(s.atoms[2].text)
            cx, cy = silk.xform(fat, px, py)
            e = silk.rot_pt(sw / 2.0, sh / 2.0, fat[2] + pr)
            r = (cx - abs(e[0]), cy - abs(e[1]),
                 cx + abs(e[0]), cy + abs(e[1]))
            ps.append(r)
            occ.mark(*inflate(r, PAD_MARGIN))
        pads_by_ref[ref] = ps

    # ---- hard: big courtyards + all silk ink; soft: every courtyard ----
    for fp in F.children(root, 'footprint'):
        fat = footprint_at(fp)
        cy = courtyard_of(fp)
        if cy is not None:
            bb = to_board(cy, fat)
            a = (bb[2] - bb[0]) * (bb[3] - bb[1])
            # A big body is a preference, not a constraint: text under a module
            # is unreadable, not illegal, so it is heavily weighted in the soft
            # set rather than banned.  Banning it left the dense cluster round
            # U6 with no legal spot at all, and the score then had nothing to
            # say about *how* illegal a fallback was -- which is how six
            # designators ended up printed on other parts' pads.
            soft.add(*inflate(bb, BODY_MARGIN),
                     W_BODY if a >= BIG_PART else W_NEIGHBOUR)
        for b, half in silk_graphics(fp, fat):
            soft.add(*inflate(b, half + SILK_MARGIN), W_SILK)

    # ---- vias: NOT an obstacle ------------------------------------------
    # DRC's silk_over_copper tests the SOLDER-MASK APERTURE.  This board sets
    # `(tenting (front yes) (back yes))` board-wide and no via overrides it, so
    # none of the 129 vias has an aperture and none can clip silk.  Marking
    # them (as this did) excluded good spots for nothing -- vias are dense
    # exactly where the parts are.
    #
    # ---- hard: board edge ----------------------------------------------
    m = EDGE_MARGIN
    occ.mark(box[0] - 60, box[1] - 60, box[0] + m, box[3] + 60)
    occ.mark(box[2] - m, box[1] - 60, box[2] + 60, box[3] + 60)
    occ.mark(box[0] - 60, box[1] - 60, box[2] + 60, box[1] + m)
    occ.mark(box[0] - 60, box[3] - m, box[2] + 60, box[3] + 60)
    occ.build()

    # ---- freeze: whatever is already legal stays where the user put it ---
    # Only a fifth of the designators actually collide; a solver that
    # re-places all 96 churns the whole board -- and the board is hand-edited
    # and is the source of truth, so gratuitous movement is a cost, not a tie.
    def box_of(i, px, py, ang):
        w, h, th = i['size']
        n = len(F.render_text(i['text']))
        L = n * w * TEXT_ADVANCE + th
        H = h * TEXT_HEIGHT + th
        aw, ah = (L, H) if ang == 0 else (H, L)
        return (px - aw / 2.0, py - ah / 2.0,
                px + aw / 2.0, py + ah / 2.0)

    # ---- obstacles: every PRINTED silk text that is not a designator ------
    # `items` above holds footprint References only.  The board's own printed
    # labels -- 'RESET', 'STATUS', 'LED2', 'BOOT', every terminal pin name,
    # every GPIO name -- are gr_text nodes, and a footprint's user text (BZ1's
    # "(+)" ) is an fp_text.  Neither was in any obstacle set, which is how a
    # designator came to be printed straight across 'RESET' and 'STATUS'.
    # Kept per layer: front ink cannot collide with ink on the back.
    free_r = {'F.SilkS': Raster(box), 'B.SilkS': Raster(box)}
    for it in silk.collect(root):
        if it['kind'] == 'Reference' or it['layer'] not in free_r:
            continue
        free_r[it['layer']].mark(
            *inflate(box_of(it, it['anchor'][0], it['anchor'][1], it['ang']),
                     TEXT_MARGIN))
    for _r in free_r.values():
        _r.build()

    order = []
    for i in items:
        fp = next(fp for fp in F.children(root, 'footprint')
                  if ref_of(fp) == i['ref'])
        cy = courtyard_of(fp)
        bb = to_board(cy, footprint_at(fp)) if cy else None
        carea = ((bb[2] - bb[0]) * (bb[3] - bb[1])) if bb else 0.0
        order.append((carea, i['ref'], i,
                      box_of(i, i['anchor'][0], i['anchor'][1], i['ang'])))

    frozen = set()
    for _, _, i, b in order:
        if move and i['ref'] not in move:
            frozen.add(id(i))              # --move pinned it; leave it alone
            continue
        if occ.count(*b) > 0:
            continue                       # on a pad, a via, or the edge
        if soft.cost(*inflate(b, TEXT_MARGIN)) > 0.0:
            continue                       # printed on a courtyard or on ink
        if free_r[i['layer']].count(*b) > 0:
            continue                       # printed across a board label
        if any(silk.area(inflate(b, TEXT_MARGIN),
                         inflate(ob, TEXT_MARGIN)) > 0.0
               for _, _, j, ob in order if j is not i):
            continue                       # within margin of another designator
        frozen.add(id(i))
    print('=== %d of %d designators already clear; %d to place'
          % (len(frozen), len(items), len(items) - len(frozen)))

    # Marks for the designators that are not moving, so the movers keep off
    # them.  Movers are added as they are placed, biggest part first, which
    # is the order that matters: a U3 with nowhere to go is a real problem,
    # a 0402 designator with nowhere to go is a nudge.
    txt = Raster(box)
    for _, _, i, b in order:
        if id(i) in frozen:
            txt.mark(*inflate(b, TEXT_MARGIN))

    order.sort(key=lambda t: -t[0])
    placed = 0
    for _, ref, i, _b in order:
        if id(i) in frozen:
            continue
        ps = pads_by_ref.get(ref, [])
        cen = (sum((p[0] + p[2]) / 2.0 for p in ps) / len(ps),
               sum((p[1] + p[3]) / 2.0 for p in ps) / len(ps)) if ps \
            else i['anchor']
        cur = i['anchor']

        best = None          # (clean, score, px, py, ang, box)
        for ang in (i['ang'], 0, 90):
            lim = RADIUS
            k = int(lim / PITCH)
            for gi in range(-k, k + 1):
                for gj in range(-k, k + 1):
                    dx, dy = gi * PITCH, gj * PITCH
                    d = math.hypot(dx, dy)
                    if d > lim:
                        continue
                    px, py = cen[0] + dx, cen[1] + dy
                    b = box_of(i, px, py, ang)
                    clean = (occ.count(*b) == 0 and txt.count(*b) == 0
                             and free_r[i['layer']].count(*b) == 0)
                    if not clean and best is not None and best[0]:
                        continue          # a clean spot is already banked
                    score = (soft.cost(*b) + 0.30 * d
                             + 0.25 * math.hypot(px - cur[0], py - cur[1])
                             + W_TXT * txt.count(*b) * CELL * CELL
                             + W_TXT * free_r[i['layer']].count(*b) * CELL * CELL
                             + W_OCC * occ.count(*b) * CELL * CELL)
                    if ang != i['ang']:
                        score += W_ANGLE
                    key = (0 if clean else 1, score)
                    if best is None or key < (0 if best[0] else 1, best[1]):
                        best = (clean, score, px, py, ang, b)

        clean, score, px, py, ang, b = best
        txt.mark(*inflate(b, TEXT_MARGIN))
        placed += 1
        if verbose or not clean:
            print('%-6s %-3d->%-3d  (%7.2f,%7.2f)  %s  score %.1f'
                  % (ref, i['ang'], ang, px, py,
                     'clean' if clean else 'COLLIDES', score))
        if apply_:
            fp = next(fp for fp in F.children(root, 'footprint')
                      if ref_of(fp) == ref)
            fat = footprint_at(fp)
            lx, ly = silk.rot_pt(px - fat[0], py - fat[1], -fat[2])
            # The stored angle IS the drawn angle for a footprint field -- see
            # silk.drawn_angle.  Subtracting the footprint's rotation here, as
            # this wrote at first, turns a designator meant to lie flat into
            # one standing on end.
            F.set_at(i['node'], lx, ly, ang % 360)

    print('=== %d designators, %d frozen, %d placed, %s'
          % (len(items), len(frozen), placed,
             'written' if apply_ else 'dry run'))
    if apply_:
        open(pcb, 'wb').write(C.serialize(doc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
