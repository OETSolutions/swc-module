#!/usr/bin/env python3
"""Collision-aware field autoplacer for a KiCad schematic.

Why this exists: the kicad MCP server has no tool that sets a field's
*position* (set_component_property only rewrites text), and place_component
puts Reference/Value at a fixed +-3.81mm from the symbol centre no matter how
big the body is.  That is what puts "ESP32-S3-WROOM-1-N4" across the module
body and "100uF/50V" across the +12V rail.  This is the sanctioned
generator-side exception: it parses and writes the file with the MCP server's
OWN CST parser/serializer (never a hand-rolled regex), and every run is
validated afterwards by export_netlist + run_erc.

Geometry rules, all verified against real renders (see docs in the repo):
  * a field's `at` is the text-box CENTRE vertically (default vertical
    justification), so a box is [y - h/2, y + h/2], not a baseline.
  * a field's angle is relative to the symbol: upright horizontal text needs
    field_angle(rot) below.  At rot 180 the field angle must be 0 (180 renders
    the glyphs upside down) and KiCad mirrors the justification, so a stored
    'left' anchors the text's right edge.  Both are special-cased here.
  * a schematic label anchors at its wire point: rot 0 -> box right of the
    anchor, 180 -> left, 90 -> up-and-left, 270 -> down-and-right.

Usage: fieldplace.py SCH [--dry]
"""
import sys
import math

sys.path.insert(0, '<uv-cache>/mcp_server_kicad')
from mcp_server_kicad import _cst                                   # noqa: E402

CHAR_W = 1.30          # mm advance per char at 1.27mm text (PDF-measured 1.08-1.33)
NOTE_ADV = 0.95        # per-char advance, as a fraction of font size, for free
                       # text nodes.  KiCad's stroke font advances ~0.91x the
                       # size for bold text; boxes.py measures its 2.0mm bold
                       # block titles at 1.86mm/char (0.93) while the render
                       # shows 1.82.  0.95 covers the widest case, so a note's
                       # modelled ink is never narrower than the ink on paper.
GLYPH_H = 1.35         # cap height above the baseline for 1.27mm text
LINE_H = 2.00
PAD = 0.45             # keep-out margin around every occupied rect
GAP = 1.50             # field-to-body clearance
LATERAL = 3.81         # side-slot offsets tried along the body edge

# Rendered ink of one line, as the eye and clearance.py measure it.  The
# reserved LINE_H box is 2.00 tall and CHAR_W per char; the glyphs actually
# occupy 1.40 and ~1.15/char.  Scoring candidates on the reserved box is what
# let a wire sit 0.37mm off the top of "GND": the wire's padded rect (0.25) plus
# PAD (0.45) ends exactly 0.70mm out, so every slot inside that band scored
# zero and the optimiser could not tell 0.37mm from 10mm.  Score the ink box
# against the real segment geometry instead.
INK_H = 1.40
INK_PAD_X = 0.15
MIN_GAP = 0.55         # clear space a field's ink must keep from a wire/stroke
INK_MARGIN = 0.30      # model-vs-rendered box error, measured on the sheet
NEAR_W = 1.5           # cost per mm a field sits clear of its drawn body
EM_PDF = 4.09          # pdftotext box height of 1.27mm text on this sheet


# ------------------------------------------------------------------ helpers
def atoms(n):
    return [a.text for a in n.atoms if hasattr(a, 'text')]


def fnum(s):
    # Accepts either a raw string or a CST Atom.  Passing an Atom straight in
    # used to return 0.0 with no complaint, so a caller that forgot `.text`
    # silently read every coordinate as zero instead of failing loudly.
    if hasattr(s, 'text'):
        s = s.text
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


def child(n, kind):
    for c in n.children:
        if getattr(c, 'head', None) == kind:
            return c
    return None


def children(n, kind):
    return [c for c in n.children if getattr(c, 'head', None) == kind]


def prop(n, name):
    for p in children(n, 'property'):
        if p.atoms[1].text == name:
            return p
    return None


def prop_val(n, name):
    p = prop(n, name)
    return p.atoms[2].text if p else None


import re

_MARKUP = re.compile(r'[_~^]\{([^}]*)\}')


def render_text(s):
    """Rendered glyph count of a KiCad string: markup ~{X} _{X} ^{X} is not
    drawn, so measuring the raw string over-states the width by 3 chars."""
    return _MARKUP.sub(r'\1', s)


def is_hidden(n):
    """True for KiCad's (hide yes) child list or a legacy bare `hide` atom.

    KiCad 10 writes (pin_numbers (hide yes)) / (pin_names (offset ..) (hide yes))
    / (pin ... (hide yes)); older files write a bare `hide` token.  Checking
    only the bare form silently treats hidden text as visible.

    There are THREE homes for the flag, and all three mean hidden:
      * a direct child of the node        -- KiCad 10's own writer
      * inside (effects ...)              -- KiCad 9, and the MCP
                                             place_component /
                                             set_component_property writers
      * a bare `hide` token               -- pre-6 files
    Missing the middle one is not hypothetical: it is the shape every
    MCP-written property has, and it is what made the first pass of these
    gates report F2's six sourcing fields as visible text on the sheet.
    """
    if any(a.text == 'hide' for a in n.atoms[1:]):
        return True
    for parent in (n, child(n, 'effects')):
        if parent is None:
            continue
        h = child(parent, 'hide')
        if h and any(a.text in ('yes', 'true') for a in h.atoms[1:]):
            return True
    return False


def set_at(p, x, y, rot=0):
    at = child(p, 'at')
    at.atoms[1].set_text('%.2f' % x)
    at.atoms[2].set_text('%.2f' % y)
    if len(at.atoms) > 3:
        at.atoms[3].set_text(str(rot))
    else:
        from mcp_server_kicad import _cst as C
        at.children.append(C.Atom(str(rot).encode(), b' '))


def set_effects(p, hide, justify):
    """KiCad 10 writes these as child lists: (justify left) and (hide yes)."""
    eff = child(p, 'effects')
    if eff is None:
        return
    for c in list(eff.children):
        if getattr(c, 'head', None) in ('justify', 'hide'):
            eff.children.remove(c)
    from mcp_server_kicad import _cst as C

    def sub(head, arg):
        return C.List(b'\n\t\t\t\t',
                      [C.Atom(head.encode(), b'\n\t\t\t\t'),
                       C.Atom(arg.encode(), b' ')], b'\n\t\t\t')

    if justify and justify != 'center':
        eff.children.append(sub('justify', justify))
    if hide:
        eff.children.append(sub('hide', 'yes'))


def field_angle(srot):
    """Stored field angle that renders upright horizontal text on this symbol."""
    return 0 if srot in (0, 180) else (360 - srot) % 360


def stored_justify(srot, mir, want):
    """Stored justification that RENDERS as `want`.

    KiCad flips a field's horizontal justification when the symbol is rotated
    180 or mirrored about Y, so the stored side is the opposite of the intended
    one.  J4 (mirror y) proved this: stored `left` rendered right-justified and
    the value string ran back across the USB-C body -- the "text inside the
    symbols" punch item.
    """
    flip = (int(srot) == 180) != (mir == 'y')
    if flip:
        return {'left': 'right', 'right': 'left'}.get(want, want)
    return want


def box_xy(x, y, w, h, justify):
    """Text box for an anchor (x, y) and rendered justification."""
    if justify == 'left':
        return (x, y - h / 2.0, x + w, y + h / 2.0)
    if justify == 'right':
        return (x - w, y - h / 2.0, x, y + h / 2.0)
    return (x - w / 2.0, y - h / 2.0, x + w / 2.0, y + h / 2.0)


def label_box(x, y, rot, text):
    """Rendered glyph box of a KiCad label, measured from the PDF text layer.

    labeljustify.py gives every label (justify left|right bottom), so the text
    sits BESIDE the anchor with its baseline on it -- glyphs occupy the 1.35mm
    above the anchor, never straddling the wire.  Measured width is 1.08-1.33
    mm/char depending on glyph mix; CHAR_W over-estimates, which over-flags
    rather than misses.
    """
    w, h = len(text) * CHAR_W, GLYPH_H
    r = int(rot) % 360
    if r == 0:
        return (x, y - h, x + w, y)
    if r == 180:
        return (x - w, y - h, x, y)
    if r == 90:
        return (x - h, y - w, x, y)
    return (x - h, y, x, y + w)


# ------------------------------------------------------------- transforms
def xform(lx, ly, r, m):
    if m == 'y':
        lx = -lx
    elif m == 'x':
        ly = -ly
    if r == 0:
        rx, ry = lx, ly
    elif r == 90:
        rx, ry = -ly, lx
    elif r == 180:
        rx, ry = -lx, -ly
    else:
        rx, ry = ly, -lx
    return rx, -ry


class Occ:
    """Occupancy of sheet-space rectangles, bucketed into a 5mm grid."""

    CELL = 5.0
    WEIGHT = {'wire': 3.0, 'junction': 2.0, 'label': 1.5, 'pin': 1.2,
              'body': 1.0, 'noconnect': 1.0, 'note': 1.0, 'field': 1.0,
              'pinnum': 1.4}

    def __init__(self):
        self.grid = {}
        self.n = 0

    def add(self, x0, y0, x1, y1, tag=''):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        r = (x0 - PAD, y0 - PAD, x1 + PAD, y1 + PAD, tag)
        self.n += 1
        c = self.CELL
        for i in range(int(math.floor(r[0] / c)), int(math.floor(r[2] / c)) + 1):
            for j in range(int(math.floor(r[1] / c)), int(math.floor(r[3] / c)) + 1):
                self.grid.setdefault((i, j), []).append(r)

    def cost(self, x0, y0, x1, y1):
        """Weighted overlap area plus a per-intersection penalty."""
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        c = self.CELL
        seen = set()
        area = 0.0
        hits = 0
        for i in range(int(math.floor(x0 / c)), int(math.floor(x1 / c)) + 1):
            for j in range(int(math.floor(y0 / c)), int(math.floor(y1 / c)) + 1):
                for r in self.grid.get((i, j), ()):
                    k = id(r)
                    if k in seen:
                        continue
                    seen.add(k)
                    ox = min(x1, r[2]) - max(x0, r[0])
                    oy = min(y1, r[3]) - max(y0, r[1])
                    if ox > 0 and oy > 0:
                        area += ox * oy * self.WEIGHT.get(r[4], 1.0)
                        hits += 1
        return area + 1.5 * hits

    def field_gap(self, x0, y0, x1, y1):
        """Smallest gap to an already-placed field or sheet note box.

        Overlap *area* alone is too weak a signal for text-on-text: two
        1.27mm-high fields clipping by a fraction of a millimetre cost ~2,
        less than the 6.0 "keep both fields on one side" preference, so the
        optimiser stacks a Value onto a Reference and the sheet renders two
        labels over each other (audit FIELD-FIELD).  Text has to keep the
        same MIN_GAP from other text that it keeps from a wire.

        Free text nodes count too ('note'): the block titles boxes.py draws
        are text like any other, and a field may not sit in one.
        """
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        c = self.CELL
        seen = set()
        best = None
        for i in range(int(math.floor(x0 / c)) - 1,
                       int(math.floor(x1 / c)) + 2):
            for j in range(int(math.floor(y0 / c)) - 1,
                           int(math.floor(y1 / c)) + 2):
                for r in self.grid.get((i, j), ()):
                    if r[4] not in ('field', 'note'):
                        continue
                    k = id(r)
                    if k in seen:
                        continue
                    seen.add(k)
                    # add() stores every rect inflated by PAD; compare the ink
                    # the eye sees on both sides, exactly as segs.min_gap does
                    # for a wire, so MIN_GAP means the same 0.55mm here.
                    gx0, gy0, gx1, gy1 = glyph_box(
                        (r[0] + PAD, r[1] + PAD, r[2] - PAD, r[3] - PAD))
                    dx = max(gx0 - x1, x0 - gx1, 0.0)
                    dy = max(gy0 - y1, y0 - gy1, 0.0)
                    d = math.hypot(dx, dy)
                    if best is None or d < best:
                        best = d
        return 1e9 if best is None else best   # no body nearby at all

    def body_gap(self, x0, y0, x1, y1):
        """Smallest gap from a glyph box to any symbol body's outline.

        The area model charges a body overlap area times WEIGHT['body'] = 1.0
        -- two orders of magnitude below the 500-class MIN_GAP penalty text
        and wires now carry -- so when the two rules conflict the optimiser
        trades the cheap one away and parks a field across a neighbouring
        symbol (overlapcheck: TEXT-ON-BODY R8/'R9').  Strokes get the same
        MIN_GAP as everything else, measured against the *unpadded* graphics
        bbox, so a field's ink can never touch a body it does not belong to.

        Only 'body', not 'pin': a body's own fields sit at GAP=1.5mm from its
        bbox by construction, but a resistor's pin stub runs up the middle of
        the slot its Reference occupies, so a 0.55mm rule on pins would forbid
        the ordinary "label above the resistor" placement outright.
        """
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        c = self.CELL
        seen = set()
        best = None
        for i in range(int(math.floor(x0 / c)) - 1,
                       int(math.floor(x1 / c)) + 2):
            for j in range(int(math.floor(y0 / c)) - 1,
                           int(math.floor(y1 / c)) + 2):
                for r in self.grid.get((i, j), ()):
                    if r[4] != 'body':
                        continue
                    k = id(r)
                    if k in seen:
                        continue
                    seen.add(k)
                    bx0, by0, bx1, by1 = (r[0] + PAD, r[1] + PAD,
                                          r[2] - PAD, r[3] - PAD)
                    dx = max(bx0 - x1, x0 - bx1, 0.0)
                    dy = max(by0 - y1, y0 - by1, 0.0)
                    d = math.hypot(dx, dy)
                    if best is None or d < best:
                        best = d
        # 1e9, not 0.0, when there is no neighbour at all.  Returning 0.0
        # made "nothing nearby" read as a total overlap, so every candidate
        # in open space took the full 500-class penalty and the optimiser
        # preferred a real 0.27mm wire graze (528) to an empty slot (555) --
        # that is what pushed R52's '1k' 7.6mm off its body onto the ch1
        # sense-return wire while the sheet had clear room directly above it.
        return 1e9 if best is None else best


def _pt_seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    if L <= 1e-12:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def seg_seg_dist(p1, p2, p3, p4):
    """Min distance between two segments."""
    def orient(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    d1, d2 = orient(p3, p4, p1), orient(p3, p4, p2)
    d3, d4 = orient(p1, p2, p3), orient(p1, p2, p4)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(_pt_seg_dist(p1, p3, p4), _pt_seg_dist(p2, p3, p4),
               _pt_seg_dist(p3, p1, p2), _pt_seg_dist(p4, p1, p2))


def rect_seg_dist(r, a, b):
    """Min distance between an axis-aligned rect and a segment."""
    x0, y0, x1, y1 = r
    if (x0 <= a[0] <= x1 and y0 <= a[1] <= y1) or \
       (x0 <= b[0] <= x1 and y0 <= b[1] <= y1):
        return 0.0
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    d = 1e9
    for c, e in zip(corners, corners[1:] + corners[:1]):
        d = min(d, seg_seg_dist(c, e, a, b))
    return d


class SegIndex:
    """Wire and symbol-graphic segments, bucketed for 'how close is that text?'"""

    CELL = 5.0

    def __init__(self):
        self.grid = {}
        self.n = 0

    def add(self, a, b):
        self.n += 1
        c = self.CELL
        x0 = min(a[0], b[0]) - MIN_GAP
        x1 = max(a[0], b[0]) + MIN_GAP
        y0 = min(a[1], b[1]) - MIN_GAP
        y1 = max(a[1], b[1]) + MIN_GAP
        seg = (a, b)
        for i in range(int(math.floor(x0 / c)), int(math.floor(x1 / c)) + 1):
            for j in range(int(math.floor(y0 / c)), int(math.floor(y1 / c)) + 1):
                self.grid.setdefault((i, j), []).append(seg)

    def min_gap(self, box):
        """Distance from an ink box to the nearest stroke (9e9 if none near)."""
        if box is None:
            return 9e9
        x0, y0, x1, y1 = box
        c = self.CELL
        seen = set()
        best = 9e9
        for i in range(int(math.floor((x0 - MIN_GAP) / c)),
                       int(math.floor((x1 + MIN_GAP) / c)) + 1):
            for j in range(int(math.floor((y0 - MIN_GAP) / c)),
                           int(math.floor((y1 + MIN_GAP) / c)) + 1):
                for seg in self.grid.get((i, j), ()):
                    k = id(seg)
                    if k in seen:
                        continue
                    seen.add(k)
                    d = rect_seg_dist(box, seg[0], seg[1])
                    if d < best:
                        best = d
        return best


def glyph_box(r):
    """The ink the eye sees inside a reserved text box (None passes through).

    The reserved box is CHAR_W*len by LINE_H; the glyphs inside it are shorter
    (INK_H) and inset (INK_PAD_X), which is what pdftotext's rendered box
    shrinks to in overlapcheck.py.  ink_box() below is this plus a margin.
    """
    if r is None:
        return None
    return (r[0] + INK_PAD_X, r[1] + (LINE_H - INK_H) / 2.0,
            r[2] - INK_PAD_X, r[3] - (LINE_H - INK_H) / 2.0)


def ink_box(r):
    """The rendered ink box of a reserved text box (None passes through).

    Inflated by INK_MARGIN on every side.  The model places ink symmetrically
    about the anchor; the rendered glyph box does not sit exactly there -- on
    the real sheet its centre lands up to 0.20mm below and up to 0.24mm beside
    the modelled centre (KiCad centres on cap height, pdftotext reports the
    ascent..descent box).  clearance.py gates on the RENDERED box, so the
    model has to be a superset of it or the two disagree at the margin and the
    gate fails on a field the placer thought was clear.
    """
    g = glyph_box(r)
    if g is None:
        return None
    return (g[0] - INK_MARGIN, g[1] - INK_MARGIN,
            g[2] + INK_MARGIN, g[3] + INK_MARGIN)


def box_gap(a, b):
    """Smallest distance between two axis-aligned boxes (0.0 if they overlap)."""
    dx = max(a[0] - b[2], b[0] - a[2], 0.0)
    dy = max(a[1] - b[3], b[1] - a[3], 0.0)
    return math.hypot(dx, dy)


# ------------------------------------------------------------- lib geometry
class Lib:
    def __init__(self, libsyms):
        self.syms = {s.atoms[1].text: s for s in children(libsyms, 'symbol')}

    def _sym(self, lib_id):
        """Cache lookup tolerant of a missing library nickname.

        add_lib_symbol caches a project symbol under its bare name while the
        instance references 'SWC2:<name>'; without this fallback every SWC2
        part looks like a zero-geometry symbol and its fields land on pins.
        """
        s = self.syms.get(lib_id)
        if s is None and ':' in lib_id:
            s = self.syms.get(lib_id.split(':', 1)[1])
        return s

    def numbers_hidden(self, lib_id):
        """True when the symbol suppresses pin numbers (Device:R, Device:C...)."""
        s = self._sym(lib_id)
        if s is None:
            return False
        n = child(s, 'pin_numbers')
        return bool(n and is_hidden(n))

    def units(self, lib_id, unit):
        """graphic + pin nodes for the given unit (common unit 0 included)."""
        s = self._sym(lib_id)
        if s is None:
            return [], []
        gfx, pins = [], []
        for sub in children(s, 'symbol'):
            name = sub.atoms[1].text
            parts = name.rsplit('_', 2)
            if len(parts) != 3 or not parts[1].isdigit():
                continue
            u = int(parts[1])
            if u not in (0, unit):
                continue
            for g in sub.children:
                h = getattr(g, 'head', None)
                if h in ('rectangle', 'polyline', 'circle', 'arc',
                         'bezier', 'text'):
                    gfx.append(g)
                elif h == 'pin':
                    pins.append(g)
        return gfx, pins


def gfx_pts(g):
    k = getattr(g, 'head', None)
    out = []
    if k == 'rectangle':
        s, e = child(g, 'start'), child(g, 'end')
        out = [(fnum(s.atoms[1].text), fnum(s.atoms[2].text)),
               (fnum(e.atoms[1].text), fnum(e.atoms[2].text))]
    elif k == 'circle':
        c, r = child(g, 'center'), child(g, 'radius')
        cx, cy, rr = fnum(c.atoms[1].text), fnum(c.atoms[2].text), fnum(r.atoms[1].text)
        out = [(cx - rr, cy - rr), (cx + rr, cy + rr)]
    elif k == 'arc':
        for tag in ('start', 'mid', 'end'):
            n = child(g, tag)
            if n:
                out.append((fnum(n.atoms[1].text), fnum(n.atoms[2].text)))
    elif k in ('polyline', 'bezier'):
        pts = child(g, 'pts')
        if pts:
            for xy in children(pts, 'xy'):
                out.append((fnum(xy.atoms[1].text), fnum(xy.atoms[2].text)))
    elif k == 'text':
        at = child(g, 'at')
        if at:
            out = [(fnum(at.atoms[1].text), fnum(at.atoms[2].text))]
    return out


def pin_number_box(ax, ay, ex, ey, num, size=1.27):
    """Rendered box of a pin's number text, in sheet mm.

    KiCad draws the number alongside the pin stub: above it for a horizontal
    pin, to its left for a vertical one.  Ignoring these is why a Reference or
    Value could be dropped straight onto a pin number.
    """
    w = max(1, len(num)) * 1.10
    mx, my = (ax + ex) / 2.0, (ay + ey) / 2.0
    if abs(ex - ax) >= abs(ey - ay):
        return (mx - w / 2.0, my - size * 0.5 - 0.70,
                mx + w / 2.0, my + size * 0.5 - 0.70)
    return (mx - size * 0.5 - 0.70, my - w / 2.0,
            mx + size * 0.5 - 0.70, my + w / 2.0)


def _text_box(x, y, w, h, hjust, vjust):
    """Box of a text anchor with rendered justification (default centre)."""
    if hjust == 'left':
        x0, x1 = x, x + w
    elif hjust == 'right':
        x0, x1 = x - w, x
    else:
        x0, x1 = x - w / 2.0, x + w / 2.0
    if vjust == 'top':
        y0, y1 = y - h, y
    elif vjust == 'bottom':
        y0, y1 = y, y + h
    else:
        y0, y1 = y - h / 2.0, y + h / 2.0
    return (x0, y0, x1, y1)


def sym_text_box(g, cx, cy, rot, mir):
    """Sheet-space box of a symbol's graphic (text ...) node, or None.

    Symbols carry hand-placed annotation text (BAT54S's COM/K2/A1).  That text
    is invisible to a body/pin model and can land on the pin numbers.
    """
    if len(g.atoms) < 2:
        return None
    txt = g.atoms[1].text
    at = child(g, 'at')
    if at is None:
        return None
    lx, ly = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
    ang = fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
    if abs(ang) > 360:                 # symbol text stores tenths of a degree
        ang /= 10.0
    sx = sy = 1.27
    hjust = vjust = 'center'
    eff = child(g, 'effects')
    if eff:
        f = child(eff, 'font')
        if f:
            sz = child(f, 'size')
            if sz:
                sx, sy = fnum(sz.atoms[1].text), fnum(sz.atoms[2].text)
        j = child(eff, 'justify')
        if j:
            args = [a.text for a in j.atoms[1:]]
            for a in args:
                if a in ('left', 'right'):
                    hjust = a
                elif a in ('top', 'bottom'):
                    vjust = a
    dx, dy = xform(lx, ly, rot, mir)
    x, y = cx + dx, cy + dy
    w, h = len(render_text(txt)) * 1.05 * sx, 1.35 * sy
    if int(ang + rot) % 180 == 90:
        w, h = h, len(render_text(txt)) * 1.05 * sx
    return _text_box(x, y, w, h, hjust, vjust)


def pin_name_box(libs, lib_id, p, cx, cy, rot, mir):
    """Sheet-space box of a pin's NAME text when names are rendered."""
    s = libs._sym(lib_id)
    if s is None:
        return None
    pn = child(s, 'pin_names')
    if pn and is_hidden(pn):
        return None
    nm = child(p, 'name')
    if nm is None or not nm.atoms[1].text or nm.atoms[1].text == '~':
        return None
    if is_hidden(nm):
        return None
    off = 0.508
    if pn:
        o = child(pn, 'offset')
        if o:
            off = fnum(o.atoms[1].text)
    (ax, ay), (ex, ey) = pin_pts(p)
    A = xform(ax, ay, rot, mir)
    E = xform(ex, ey, rot, mir)
    ax, ay, ex, ey = cx + A[0], cy + A[1], cx + E[0], cy + E[1]
    name = nm.atoms[1].text
    w, h = len(render_text(name)) * 1.05 * 1.27, 1.35 * 1.27
    if abs(ex - ax) >= abs(ey - ay):                       # horizontal pin
        x0 = ex + off if ex >= ax else ex - off - w
        return (x0, (ay + ey) / 2.0 - h / 2.0, x0 + w, (ay + ey) / 2.0 + h / 2.0)
    y0 = ey + off if ey >= ay else ey - off - w
    return ((ax + ex) / 2.0 - h / 2.0, y0, (ax + ex) / 2.0 + h / 2.0, y0 + w)


def pin_pts(p):
    at = child(p, 'at')
    ln = child(p, 'length')
    x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
    ang = fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
    L = fnum(ln.atoms[1].text) if ln else 0.0
    ex = x + L * math.cos(math.radians(ang))
    ey = y + L * math.sin(math.radians(ang))
    return (x, y), (ex, ey)


def lib_unit_max(libs, lib_id):
    """Highest unit number a lib symbol defines (1 = single-unit part)."""
    s = libs._sym(lib_id)
    top = 1
    if s is not None:
        for sub in children(s, 'symbol'):
            parts = sub.atoms[1].text.rsplit('_', 2)
            if len(parts) == 3 and parts[1].isdigit():
                top = max(top, int(parts[1]))
    return top


def font_scale(n):
    """(x, y) glyph scale of an item's effects relative to the 1.27mm default."""
    eff = child(n, 'effects')
    f = child(eff, 'font') if eff else None
    sz = child(f, 'size') if f else None
    if sz is None:
        return 1.0, 1.0
    return (fnum(sz.atoms[1].text) / 1.27, fnum(sz.atoms[2].text) / 1.27)


def just_of(p):
    """Stored horizontal justification of a property ('center' if unstated)."""
    eff = child(p, 'effects')
    j = child(eff, 'justify') if eff else None
    return j.atoms[1].text if j and len(j.atoms) > 1 else 'center'


def field_ink_box(p, srot, mir=''):
    """Rendered ink box of a property node, justification included.

    One implementation, shared with clearance.py: when the two drifted (this
    one centred every field, ignoring stored left/right justification) the
    orientation matcher failed to find six-character values and fell back to
    guessing.
    """
    at = child(p, 'at')
    x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
    ang = fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0
    text = p.atoms[2].text
    eff = child(p, 'effects')
    stored = 'center'
    if eff:
        j = child(eff, 'justify')
        if j and len(j.atoms) > 1:
            stored = j.atoms[1].text
    just = stored_justify(srot, mir, stored)
    inkw = len(render_text(text)) * CHAR_W - 2.0 * INK_PAD_X
    if (ang + srot) % 180 == 90:
        w, h = INK_H, inkw
        if just == 'left':                 # reads downward from the anchor
            return (x - w / 2.0, y, x + w / 2.0, y + h)
        if just == 'right':
            return (x - w / 2.0, y - h, x + w / 2.0, y)
        return (x - w / 2.0, y - h / 2.0, x + w / 2.0, y + h / 2.0)
    return box_xy(x, y, inkw, INK_H, just)


def _split_add(out, text, box, vert, reverse=False):
    """Model a possibly multi-word string as one WORD or one GROUP.

    pdftotext emits words, not strings: "10k NTC B=3380" arrives as three
    words.  Splitting the box proportionally by character count does not work
    -- KiCad's stroke font is proportional, so a 46-character note is 30%
    narrower than the character count predicts and the tail words miss by
    millimetres.  Instead the whole string becomes a GROUP: every word of the
    string is in it, and any rendered word of that text whose centre falls in
    the group's box takes the group's orientation.  The box is the string's
    ink box; all its words share one direction, which is all the caller needs.
    """
    if box is None or not text:
        return
    words = text.split()
    if not words:
        return
    if len(words) == 1:
        out.append((text, (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0, vert))
        return
    out.groups.append((box, vert, set(words)))


def text_models(sch):
    """[(text, cx, cy, vert)] for every rendered text item, read from the FILE.

    The PDF text layer gives a word's box but not its reading direction, and a
    three-character box is nearly square, so geometry cannot tell a horizontal
    "GND" from a vertical one -- the heuristic that compared against a fixed em
    height silently called 175 horizontal words vertical, which is how a +5V
    value "overlapped" its own arrow.  The schematic knows: every field, pin
    annotation, label and note carries its own angle.  Match a rendered word to
    the nearest same-text item and take the direction from there.
    """
    doc = _cst.parse(open(sch, 'rb').read())
    root = doc.children[0]
    libs = Lib(child(root, 'lib_symbols'))

    class Models(list):
        """Per-word entries, plus whole-string groups (see _split_add)."""

        def __init__(self):
            list.__init__(self)
            self.groups = []

    out = Models()

    def add(text, box, vert):
        if box is None or not text:
            return
        out.append((text, (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0, vert))

    for s in children(root, 'symbol'):
        at = child(s, 'at')
        cx, cy = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        rot = int(fnum(at.atoms[3].text)) if len(at.atoms) > 3 else 0
        mn = child(s, 'mirror')
        mir = mn.atoms[1].text if mn else ''
        lib_id = child(s, 'lib_id').atoms[1].text
        unit = int(fnum(child(s, 'unit').atoms[1].text)) if child(s, 'unit') else 1
        gfx, pins = libs.units(lib_id, unit)
        for g in gfx:
            if getattr(g, 'head', None) == 'text':
                a = child(g, 'at')
                ang = fnum(a.atoms[3].text) if a is not None and len(a.atoms) > 3 else 0.0
                if abs(ang) > 360:
                    ang /= 10.0
                add(render_text(g.atoms[1].text), sym_text_box(g, cx, cy, rot, mir),
                    int(ang + rot) % 180 == 90)
        for p in pins:
            if is_hidden(p):
                continue
            (ax, ay), (ex, ey) = pin_pts(p)
            vert = abs(ex - ax) < abs(ey - ay)
            nm = child(p, 'name')
            if nm is not None:
                add(render_text(nm.atoms[1].text),
                    pin_name_box(libs, lib_id, p, cx, cy, rot, mir), vert)
            num = child(p, 'number')
            if num is not None and not libs.numbers_hidden(lib_id):
                A, E = xform(ax, ay, rot, mir), xform(ex, ey, rot, mir)
                add(num.atoms[1].text,
                    pin_number_box(cx + A[0], cy + A[1], cx + E[0], cy + E[1],
                                   num.atoms[1].text), vert)
        for name in ('Reference', 'Value'):
            p = prop(s, name)
            if p is None or child(p, 'at') is None:
                continue
            eff = child(p, 'effects')
            if eff and child(eff, 'hide'):
                continue
            a = child(p, 'at')
            ang = fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
            vert = int(ang + rot) % 180 == 90
            txt = p.atoms[2].text
            if name == 'Reference' and lib_unit_max(libs, lib_id) > 1:
                txt += chr(ord('A') + unit - 1)      # renders U6A..U6E
            _split_add(out, txt, field_ink_box(p, rot, mir), vert,
                       reverse=vert and just_of(p) == 'right')

    for n in children(root, 'label') + children(root, 'global_label') + \
            children(root, 'hierarchical_label'):
        a = child(n, 'at')
        ang = fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
        _split_add(out, n.atoms[1].text,
                   label_box(fnum(a.atoms[1].text), fnum(a.atoms[2].text), ang,
                             n.atoms[1].text), int(ang) % 180 == 90)

    for n in children(root, 'text'):
        a = child(n, 'at')
        ang = int(fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0) % 360
        x, y = fnum(a.atoms[1].text), fnum(a.atoms[2].text)
        t = n.atoms[1].text
        eff = child(n, 'effects')
        j = child(eff, 'justify') if eff else None
        just = j.atoms[1].text if j and len(j.atoms) > 1 else 'left'
        sx, sy = font_scale(n)
        w, h = len(t) * CHAR_W * sx, GLYPH_H * sy
        if ang == 0:
            box = {'left': (x, y - h, x + w, y), 'right': (x - w, y - h, x, y),
                   'center': (x - w / 2, y - h, x + w / 2, y)}[just]
        elif ang == 180:
            box = {'left': (x - w, y - h, x, y), 'right': (x, y - h, x + w, y),
                   'center': (x - w / 2, y - h, x + w / 2, y)}[just]
        elif ang == 90:
            box = {'left': (x - h, y - w, x, y), 'right': (x - h, y, x, y + w),
                   'center': (x - h, y - w / 2, x, y + w / 2)}[just]
        else:
            box = {'left': (x - h, y, x, y + w), 'right': (x - h, y - w, x, y),
                   'center': (x - h, y - w / 2, x, y + w / 2)}[just]
        _split_add(out, t, box, ang % 180 == 90, reverse=ang in (90, 180))
    return out


def word_vert(models, text, box, tol=2.0):
    """Is this rendered word vertical?  From the file, else from the box."""
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    best = None
    for t, mx, my, vert in models:
        if t != text:
            continue
        d = (mx - cx) ** 2 + (my - cy) ** 2
        if d < tol * tol and (best is None or d < best[0]):
            best = (d, vert)
    if best is not None:
        return best[1]
    for gb, vert, words in getattr(models, 'groups', ()):
        if text in words and (gb[0] - 2.0 <= cx <= gb[2] + 2.0 and
                             gb[1] - 2.0 <= cy <= gb[3] + 2.0):
            return vert
    w, h = box[2] - box[0], box[3] - box[1]
    return abs(h - EM_PDF) > abs(w - EM_PDF)


# ------------------------------------------------------------------- main
def main():
    path = sys.argv[1]
    dry = '--dry' in sys.argv
    doc = _cst.parse(open(path, 'rb').read())
    root = doc.children[0]
    libs = Lib(child(root, 'lib_symbols'))
    occ = Occ()
    segs = SegIndex()                            # wires + graphics, for real gaps

    syms = children(root, 'symbol')

    # Subcircuit boxes: contain.py requires every field to stay inside the box
    # its symbol was declared in, and the boxes are drawn on the sheet, so the
    # placer can see exactly what the gate will measure.  Without this the
    # widened slot search trades a collision for an excursion -- a Value parks
    # 9mm clear of every wire but lands outside the block (contain: "field
    # J1.Value 9.1mm out of A").  A symbol whose centre is in no box (or a
    # sheet with no boxes at all) is simply unconstrained.
    blocks = []
    for n in children(root, 'rectangle'):
        st, en = child(n, 'start'), child(n, 'end')
        if st is None or en is None:
            continue
        x0, y0 = fnum(st.atoms[1].text), fnum(st.atoms[2].text)
        x1, y1 = fnum(en.atoms[1].text), fnum(en.atoms[2].text)
        blocks.append((min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)))

    def owner_block(cx, cy):
        """Smallest drawn box containing (cx, cy), or None."""
        best = None
        for b in blocks:
            if b[0] <= cx <= b[2] and b[1] <= cy <= b[3]:
                if best is None or (b[2] - b[0]) * (b[3] - b[1]) < \
                        (best[2] - best[0]) * (best[3] - best[1]):
                    best = b
        return best

    # 1. seed occupancy with everything that is not a field
    for n in children(root, 'wire'):
        p = child(n, 'pts')
        xy = [(fnum(a.atoms[1].text), fnum(a.atoms[2].text))
              for a in children(p, 'xy')]
        for (x0, y0), (x1, y1) in zip(xy, xy[1:]):
            occ.add(min(x0, x1) - 0.25, min(y0, y1) - 0.25,
                    max(x0, x1) + 0.25, max(y0, y1) + 0.25, 'wire')
            segs.add((x0, y0), (x1, y1))
    for n in children(root, 'junction'):
        at = child(n, 'at')
        x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        occ.add(x - 0.6, y - 0.6, x + 0.6, y + 0.6, 'junction')
    for n in children(root, 'label') + children(root, 'global_label'):
        at = child(n, 'at')
        x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        r = fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0
        occ.add(*label_box(x, y, r, n.atoms[1].text), tag='label')
    for n in children(root, 'no_connect'):
        at = child(n, 'at')
        x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        occ.add(x - 0.8, y - 0.8, x + 0.8, y + 0.8, 'noconnect')
    for n in children(root, 'text'):
        at = child(n, 'at')
        x, y = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        t = n.atoms[1].text
        # Honour the horizontal justification.  A free text node's anchor is
        # its justification point, not its left edge, and the block titles
        # boxes.py writes are RIGHT-justified (top-right corner of the box).
        # Seeding every note as left-justified put block D's title ink at
        # x 440.7..504.4 when it actually renders at 351.5..440.7, so the
        # placer saw empty space and dropped D12's Reference onto the title
        # (overlapcheck: WORD-WORD 'D.' vs 'D12').
        #
        # The width must come from the note's OWN font size: the titles are
        # 2.0mm bold, whose advance is 1.82mm/char, not the 1.30mm CHAR_W of a
        # 1.27mm field label.  Modelling them at CHAR_W left the title's left
        # 26mm unoccupied -- exactly the strip D12's Reference landed in.
        eff2 = child(n, 'effects')
        fnt = child(eff2, 'font') if eff2 is not None else None
        szn = child(fnt, 'size') if fnt is not None else None
        size = fnum(szn.atoms[1].text) if szn is not None else 1.27
        w = len(t) * NOTE_ADV * size
        j = child(eff2, 'justify')
        hj = j.atoms[1].text if (j is not None and len(j.atoms) > 1) else 'left'
        x0 = x - w if hj == 'right' else (x - w / 2.0 if hj == 'center' else x)
        occ.add(x0, y - LINE_H, x0 + w, y, 'note')

    # 2. per-symbol body/pin geometry
    info = []
    for s in syms:
        at = child(s, 'at')
        cx, cy = fnum(at.atoms[1].text), fnum(at.atoms[2].text)
        rot = int(fnum(at.atoms[3].text)) if len(at.atoms) > 3 else 0
        mnode = child(s, 'mirror')
        mir = mnode.atoms[1].text if mnode else ''
        lib_id = child(s, 'lib_id').atoms[1].text
        unit = int(fnum(child(s, 'unit').atoms[1].text)) if child(s, 'unit') else 1
        gfx, pins = libs.units(lib_id, unit)
        pts = []
        gpts = []                 # graphics only -- what the eye reads as "the body"
        for g in gfx:
            gp = [xform(lx, ly, rot, mir) for lx, ly in gfx_pts(g)]
            pts.extend(gp)
            gpts.extend(gp)
            gp = [(cx + p[0], cy + p[1]) for p in gp]
            for a, b in zip(gp, gp[1:]):
                segs.add(a, b)
            if len(gp) > 2 and getattr(g, 'head', None) == 'rectangle':
                segs.add(gp[-1], gp[0])
            if getattr(g, 'head', None) == 'text':
                tb = sym_text_box(g, cx, cy, rot, mir)
                if tb:
                    occ.add(*tb, tag='symtext')
        pinsegs = []
        pinnums = []
        for p in pins:
            if is_hidden(p):
                continue
            (ax, ay), (ex, ey) = pin_pts(p)
            pinsegs.append((xform(ax, ay, rot, mir), xform(ex, ey, rot, mir)))
            pts.append(xform(ax, ay, rot, mir))
            pts.append(xform(ex, ey, rot, mir))
            num = child(p, 'number')
            if num is not None and (abs(ex - ax) > 1e-9 or abs(ey - ay) > 1e-9):
                A = xform(ax, ay, rot, mir)
                E = xform(ex, ey, rot, mir)
                pinnums.append((cx + A[0], cy + A[1], cx + E[0], cy + E[1],
                                num.atoms[1].text))
        if pts:
            xs = [cx + p[0] for p in pts]
            ys = [cy + p[1] for p in pts]
            bbox = (min(xs), min(ys), max(xs), max(ys))
        else:
            bbox = (cx - 2, cy - 2, cx + 2, cy + 2)
        # A second box over the *drawn* graphics alone.  For a Device:R/C the
        # pins run 2.54-3.81mm past the plates, so anchoring slots to `bbox`
        # parks the refdes and value ~4mm clear of the part and the sheet reads
        # as scattered labels -- the user's complaint.  `gbox` is what the eye
        # calls the body, so that is what the field should hug.  It is used
        # ONLY for anchoring slots; occupancy keeps the pin-inclusive `bbox`.
        if gpts:
            gxs = [cx + p[0] for p in gpts]
            gys = [cy + p[1] for p in gpts]
            gbox = (min(gxs), min(gys), max(gxs), max(gys))
        else:
            gbox = bbox
        info.append(dict(sym=s, cx=cx, cy=cy, rot=rot, mir=mir, lib=lib_id,
                         unit=unit, bbox=bbox, gbox=gbox, pinsegs=pinsegs,
                         pinnums=pinnums, pins=pins, blk=owner_block(cx, cy),
                         ref=prop_val(s, 'Reference') or '?',
                         val=prop_val(s, 'Value') or ''))
        occ.add(*bbox, 'body')

    # pins as thin rects (avoid running text across a pin)
    for d in info:
        for (ax, ay), (ex, ey) in d['pinsegs']:
            occ.add(min(ax, ex) + d['cx'] - 0.25, min(ay, ey) + d['cy'] - 0.25,
                    max(ax, ex) + d['cx'] + 0.25, max(ay, ey) + d['cy'] + 0.25,
                    'pin')
        if not libs.numbers_hidden(d['lib']):
            for ax, ay, ex, ey, num in d['pinnums']:
                occ.add(*pin_number_box(ax, ay, ex, ey, num), tag='pinnum')
        for p in d['pins']:
            nb = pin_name_box(libs, d['lib'], p, d['cx'], d['cy'], d['rot'], d['mir'])
            if nb:
                occ.add(*nb, tag='pinname')

    # 3. place fields, biggest bodies first so they claim space
    info.sort(key=lambda d: -(d['bbox'][2] - d['bbox'][0]) *
              (d['bbox'][3] - d['bbox'][1]))

    def slots(d, text):
        """Candidate (side, anchor_x, anchor_y, justify, lat) for a text box.

        The lateral offsets run to +-2 grid steps, not +-1: in a dense pocket
        every one of the 12 near candidates can violate a MIN_GAP rule, and
        the optimiser then has to spend a 500-class penalty however it
        chooses (audit FIELD-FIELD R49.Reference / #PWR973.Value).  The cost
        loop charges 0.8 per step, so a near slot that is legal still beats a
        far one, but a far *legal* slot beats a near illegal one.

        Anchored on `bbox` (graphics + pin tips), NOT on the drawn body alone.
        Anchoring on the graphics looks tighter on a two-terminal passive, but
        on a multi-pin part the body rectangle sits *inside* the pin corridors:
        every candidate then crosses a pin stub or a pin name, the 500-class
        rules fire on all 40 slots, and the optimiser is left spending the
        least-bad penalty (4 x audit FIELD-ON on U3/U6/U8).  Tightness is
        bought with NEAR_W below instead, which re-ranks sides without ever
        moving a slot into a corridor.
        """
        x0, y0, x1, y1 = d['bbox']
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        h = LINE_H
        out = []
        for k in (-2, -1, 0, 1, 2):
            cxk = cx + k * LATERAL
            out.append(('above', cxk, y0 - GAP - h / 2.0, 'center', abs(k)))
            out.append(('below', cxk, y1 + GAP + h / 2.0, 'center', abs(k)))
            out.append(('right', x1 + GAP, cy + k * LATERAL, 'left', abs(k)))
            out.append(('left', x0 - GAP, cy + k * LATERAL, 'right', abs(k)))
        return out

    tight = []
    for d in info:
        is_pwr = d['ref'].startswith('#')
        srot = d['rot']
        refp, valp = prop(d['sym'], 'Reference'), prop(d['sym'], 'Value')

        if refp is not None and is_pwr:
            set_effects(refp, True, None)
        if valp is not None and d['lib'].endswith('PWR_FLAG'):
            set_effects(valp, True, None)

        rs = slots(d, d['ref']) if (refp is not None and not is_pwr) else None
        vs = slots(d, d['val']) if (valp is not None and d['val'] and
                                    not d['lib'].endswith('PWR_FLAG')) else None
        rw = len(d['ref']) * CHAR_W
        vw = len(d['val']) * CHAR_W
        best = None
        for i in (range(len(rs)) if rs else [-1]):
            rb = box_xy(rs[i][1], rs[i][2], rw, LINE_H, rs[i][3]) if i >= 0 else None
            for j in (range(len(vs)) if vs else [-1]):
                vb = box_xy(vs[j][1], vs[j][2], vw, LINE_H, vs[j][3]) if j >= 0 else None
                c = 0.0
                if rb:
                    c += occ.cost(*rb)
                if vb:
                    c += occ.cost(*vb)
                # real clearance, not the padded-rect proxy: a slot whose ink
                # grazes a wire must lose to one with room, whatever the area
                # model says.
                gap = min(segs.min_gap(ink_box(rb)), segs.min_gap(ink_box(vb)))
                if gap < MIN_GAP:
                    c += 500.0 + 100.0 * (MIN_GAP - gap)
                # same rule against text already placed: the occupancy area
                # model is too coarse to stop two fields touching.
                fg = min(occ.field_gap(*glyph_box(rb)) if rb else 1e9,
                         occ.field_gap(*glyph_box(vb)) if vb else 1e9)
                if fg < MIN_GAP:
                    c += 500.0 + 100.0 * (MIN_GAP - fg)
                # ... and against other symbols' bodies, for the same reason:
                # an overlap *area* of ~2 costs less than the 6.0 half-side
                # preference, so without this the optimiser parks a field on
                # the neighbour's body to escape the two rules above.
                bg = min(occ.body_gap(*glyph_box(rb)) if rb else 1e9,
                         occ.body_gap(*glyph_box(vb)) if vb else 1e9)
                if bg < MIN_GAP:
                    c += 500.0 + 100.0 * (MIN_GAP - bg)
                # stay near the body when nothing forces us away
                c += 0.8 * rs[i][4] if i >= 0 else 0.0
                c += 0.8 * vs[j][4] if j >= 0 else 0.0
                # ... and hug the DRAWN body, not the pin tips.  On a Device:R
                # or :C the pins run 2.54-3.81mm past the plates, so a slot
                # offset from `bbox` sits ~4.5mm clear of the ink on the pin
                # axis and ~1.5mm on the other; both are equally safe, and
                # without this term they cost the same, so which side wins is
                # arbitrary -- which is how the sheet ended up with refdes and
                # values scattered away from their parts (99 of 338 fields
                # further than 3.0mm from the drawn body, worst 7.3mm).  This
                # is a preference, an order below the 6.0 one-side rule and two
                # orders below the 500-class hard rules, so it only decides
                # cases the gates already found legal.  Both fields are
                # charged: scoring the pair by its *nearer* half lets a tight
                # Reference pay for a Value parked 6mm up (C9, "100nF" over
                # empty sheet), which is the scatter in the first place.
                if rb:
                    c += NEAR_W * box_gap(glyph_box(rb), d['gbox'])
                if vb:
                    c += NEAR_W * box_gap(glyph_box(vb), d['gbox'])
                if rb and vb:
                    # Reference and Value are two pieces of text, so they get
                    # the same MIN_GAP rule as any other text -- on the same
                    # 500-class scale.  Charging only overlap *area* let the
                    # optimiser buy its way out of a 0.4mm wire graze by
                    # stacking the two labels (audit FIELD-FIELD C5).
                    gg = box_gap(glyph_box(rb), glyph_box(vb))
                    if gg < MIN_GAP:
                        c += 500.0 + 100.0 * (MIN_GAP - gg)
                # keep the ink inside the symbol's own subcircuit box: this is
                # the same rectangle contain.py will measure against, so the
                # placer and the gate cannot disagree.  Deliberately an order
                # of magnitude above the MIN_GAP penalties (which top out
                # around 555 for a total graze): contain is a hard gate, and a
                # slot just outside the box must never beat a cramped one
                # inside it -- that trade is how J1.Value ended up 9.1mm out.
                if d['blk']:
                    for cand in (rb, vb):
                        if cand is None:
                            continue
                        gb = glyph_box(cand)
                        out = max(0.0, d['blk'][0] - gb[0], gb[2] - d['blk'][2],
                                  d['blk'][1] - gb[1], gb[3] - d['blk'][3])
                        if out > 0:
                            c += 5000.0 + 1000.0 * out
                if rb and vb:
                    if rs[i][0] != vs[j][0]:
                        c += 6.0                     # prefer one side of the body
                if best is None or c < best[0]:
                    best = (c, i, j, rb, vb)
        if best is None:
            continue
        _, i, j, rb, vb = best
        g = min(segs.min_gap(ink_box(rb)), segs.min_gap(ink_box(vb)))
        if g < MIN_GAP:
            tight.append((d['ref'], round(g, 2)))
        if rb:
            occ.add(*rb, 'field')
        if vb:
            occ.add(*vb, 'field')
        if rs and i >= 0:
            _, x, y, just, _lat = rs[i]
            set_at(refp, x, y, field_angle(srot))
            set_effects(refp, False, stored_justify(srot, d['mir'], just))
        if vs and j >= 0:
            _, x, y, just, _lat = vs[j]
            set_at(valp, x, y, field_angle(srot))
            set_effects(valp, False, stored_justify(srot, d['mir'], just))

    if dry:
        print('dry run: %d symbols, %d occupancy rects' % (len(info), occ.n))
        return
    open(path, 'wb').write(_cst.serialize(doc))
    print('fieldplace: %d symbols rewritten in %s' % (len(info), path))
    if tight:
        print('fieldplace: %d fields still within %.2fmm of a stroke: %s'
              % (len(tight), MIN_GAP, tight[:8]))


if __name__ == '__main__':
    main()
