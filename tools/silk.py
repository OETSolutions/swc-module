#!/usr/bin/env python3
"""Silkscreen text audit for SWC.

The placement directive is explicit: every silkscreen text item must read UP
(0 deg) or ROTATED LEFT (90 deg), consistently, and must not overlap a pad,
sit under a component body, or be clipped by the board edge.  DRC reports
those as silk_over_copper / silk_overlap / silk_edge_clearance, but it reports
them one collision at a time and never tells you the *orientation*, which is
the part a human actually notices when reading the board.

This tool answers both questions in one pass:

  * the drawn angle of every silk text, which for a footprint field is the
    stored angle alone (see drawn_angle below) -- text is never drawn upside
    down, so 180/270 come back as 0/90;
  * the board-space box of that text, so it can be tested against pads,
    vias, courtyards and the board outline.

Rotation convention (proven against two of KiCad's own DRC messages):

    bx = x + lx*cos(t) + ly*sin(t)
    by = y - lx*sin(t) + ly*cos(t)

Read-only unless --fix is given.  Goes through the MCP server's CST parser.

Usage: silk.py PCB [--fields REF ...] [--pads] [--json OUT]
"""
import math
import os
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

# Repo root is two levels up from this file (tools/ lives at the root).
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(ROOT, 'SWC.kicad_pcb')

# Stroke-font advance, as a fraction of the requested text width.  KiCad's
# Hershey-derived font averages a little under one width per glyph; 1.0
# over-estimates, which over-flags collisions rather than missing them.
ADVANCE = 1.0


def rot_pt(lx, ly, t):
    """Footprint-local -> board delta, for a footprint at angle t degrees."""
    th = math.radians(t)
    ct, st = math.cos(th), math.sin(th)
    return (lx * ct + ly * st, -lx * st + ly * ct)


def xform(fp_at, lx, ly):
    return (fp_at[0] + rot_pt(lx, ly, fp_at[2])[0],
            fp_at[1] + rot_pt(lx, ly, fp_at[2])[1])


def at_of(n):
    """(x, y, rot) from any node's (at ...).  KiCad omits a zero angle."""
    a = F.child(n, 'at')
    x, y = F.fnum(a.atoms[1].text), F.fnum(a.atoms[2].text)
    r = F.fnum(a.atoms[3].text) if len(a.atoms) > 3 else 0.0
    return (x, y, r)


def font_of(n):
    """(w, h, thickness, hjust, mirrored, vjust) of a text node.

    KiCad's defaults are centre/centre.  The vertical side matters as soon as
    a string is more than one line: `(justify left bottom)` puts the anchor on
    the BOTTOM line's baseline, so the block grows upward from it.  Ignoring
    that dropped the back-side title block 1.4 mm down the board, onto the
    pads it was being tested against.
    """
    w = h = 1.0
    th = 0.15
    just = 'center'
    mirr = False
    vjust = 'center'
    eff = F.child(n, 'effects')
    if eff is None:
        return (w, h, th, just, mirr, vjust)
    f = F.child(eff, 'font')
    if f is not None:
        s = F.child(f, 'size')
        if s is not None:
            w, h = F.fnum(s.atoms[1].text), F.fnum(s.atoms[2].text)
        t = F.child(f, 'thickness')
        if t is not None:
            th = F.fnum(t.atoms[1].text)
    j = F.child(eff, 'justify')
    if j is not None:
        for a in j.atoms[1:]:
            if a.text in ('left', 'right', 'center'):
                just = a.text
            elif a.text == 'mirror':
                mirr = True
            elif a.text in ('top', 'bottom'):
                vjust = a.text
    return (w, h, th, just, mirr, vjust)


def drawn_angle(t_rot):
    """The angle KiCad actually draws a footprint field at.

    The stored field angle, NOT the footprint's plus it.  That is the opposite
    of the rule this tool first shipped with, and the difference is not a
    rounding detail: it moves a designator from standing on end to lying flat,
    which is a 3.15mm-long box instead of a 3.15mm-tall one.  The solver
    believed R27 was horizontal, parked it in a slot that was only clear for
    horizontal text, and DRC found it printed across U3's pad row.

    Established from the export itself rather than from reasoning: rendering
    F.SilkS emits one `<g transform="rotate(-90 A B)">` per rotated field, and
    those groups appear for exactly the 36 designators whose stored angle is
    odd in 90s -- 108 of 108 agree with `stored % 180`, and 16 of them
    disagree with the `(footprint + stored) % 180` this used to return.  The
    same elements carry `text-anchor="middle"`, which is what makes the box
    symmetric about the anchor.

    KiCad still refuses to draw upside down, so the reading orientation lives
    in [0, 180) and 0/90 are the only two the placement directive permits.
    """
    return int(round(t_rot)) % 180


def text_box(ax, ay, ang, text, w, h, th, just='center', mirr=False,
             vjust='center'):
    """Board-space box of a silk string anchored at (ax, ay).

    Two things this got wrong at first, both of which produced a clean-looking
    report about a board that was not there:

      * a `\\n` in the string is a LINE BREAK, not two characters.  The
        back-side title block ("Steering Wheel\\nControls Adapter\\nREV
        ${REVISION} ASSY") was measured as one 44 mm line and reported as
        hanging 15 mm off the board edge.  Lines stack, so the box is the
        widest line wide and one line-height tall per line.
      * `(justify left bottom)` puts the anchor at the text's left edge, not
        its centre.  All of this board is centred EXCEPT that same title
        block, so treating everything as centred is right 139 times out of
        140 and wrong exactly where it is most visible.

    The justification offset is applied along the reading direction, so it
    holds for both permitted angles (0 and 90) rather than only for 0.
    """
    lines = text.split('\n')
    n = max(len(F.render_text(ln)) for ln in lines)
    L = n * w * ADVANCE + th
    H = (h + th) * len(lines)
    if mirr:
        # Back-side text is mirrored so it reads correctly from the back, so
        # `left` extends in -x on the board, not +x.
        just = {'left': 'right', 'right': 'left'}.get(just, just)
    # The block's own axes: `u` runs along the reading direction, `v` across
    # the lines.  Deriving the box in this frame rather than special-casing
    # ang 0 keeps the rotated labels honest for the same reason.
    th_r = math.radians(ang)
    ux, uy = math.cos(th_r), -math.sin(th_r)
    vx, vy = math.sin(th_r), math.cos(th_r)
    du = {'left': L / 2.0, 'right': -L / 2.0}.get(just, 0.0)
    dv = {'top': H / 2.0, 'bottom': -H / 2.0}.get(vjust, 0.0)
    cx = ax + du * ux + dv * vx
    cy = ay + du * uy + dv * vy
    w_ = abs(L * ux) + abs(H * vx)
    h_ = abs(L * uy) + abs(H * vy)
    return (cx - w_ / 2.0, cy - h_ / 2.0, cx + w_ / 2.0, cy + h_ / 2.0)


def is_hidden(n):
    for a in n.atoms[1:]:
        if a.text == 'hide':
            return True
    for parent in (n, F.child(n, 'effects')):
        if parent is None:
            continue
        hid = F.child(parent, 'hide')
        if hid and any(a.text in ('yes', 'true') for a in hid.atoms[1:]):
            return True
    return False


def collect(root):
    """Every visible silk text item: fp_text on each footprint, plus gr_text.

    KiCad 10 moved a footprint's Reference and Value out of fp_text and into
    (property "Reference" "R1" (at ...) (layer ...) (effects ...)) nodes, so
    reading only fp_text finds the user text and misses every designator --
    the first run of this tool reported exactly one item for that reason.
    Both spellings are collected here.
    """
    out = []
    for fp in F.children(root, 'footprint'):
        ref = ''
        for p in F.children(fp, 'property'):
            if p.atoms[1].text.strip('"') == 'Reference':
                ref = p.atoms[2].text.strip('"')
        fat = at_of(fp)
        nodes = ([(t, t.atoms[1].text.strip('"'), t.atoms[2].text.strip('"'))
                  for t in F.children(fp, 'fp_text')]
                 + [(p, p.atoms[1].text.strip('"'), p.atoms[2].text.strip('"'))
                    for p in F.children(fp, 'property')])
        for t, kind, txt in nodes:
            lay = F.child(t, 'layer')
            if lay is None:
                continue
            layer = lay.atoms[1].text.strip('"')
            if layer not in ('F.SilkS', 'B.SilkS') or is_hidden(t):
                continue
            lat = at_of(t)
            w, h, th, just, mirr, vjust = font_of(t)
            ax, ay = xform(fat, lat[0], lat[1])
            out.append(dict(src='fp', ref=ref, kind=kind, text=txt,
                            layer=layer, node=t, anchor=(ax, ay),
                            ang=drawn_angle(lat[2]),
                            raw=(fat[2], lat[2]), size=(w, h, th), just=just,
                            mirr=mirr, vjust=vjust))
    for g in F.children(root, 'gr_text'):
        lay = F.child(g, 'layer')
        if lay is None:
            continue
        layer = lay.atoms[1].text.strip('"')
        if layer not in ('F.SilkS', 'B.SilkS') or is_hidden(g):
            continue
        txt = g.atoms[1].text.strip('"')
        at = at_of(g)
        w, h, th, just, mirr, vjust = font_of(g)
        out.append(dict(src='free', ref='-', kind='user', text=txt,
                        layer=layer, node=g, anchor=(at[0], at[1]),
                        ang=drawn_angle(at[2]), raw=(0, at[2]),
                        size=(w, h, th), just=just, mirr=mirr, vjust=vjust))
    return out


def pad_rings(root):
    """Every pad's board-space rectangle.

    (x0, y0, x1, y1, ref, padname, side) where side is 'F', 'B', '*' (a
    through-hole pad, open on both) or '-' (no mask aperture at all).
    """
    side = '*' 
    out = []
    for fp in F.children(root, 'footprint'):
        ref = ''
        for p in F.children(fp, 'property'):
            if p.atoms[1].text.strip('"') == 'Reference':
                ref = p.atoms[2].text.strip('"')
        fat = at_of(fp)
        for pd in F.children(fp, 'pad'):
            num = pd.atoms[1].text.strip('"')
            at = F.child(pd, 'at')
            px, py = F.fnum(at.atoms[1].text), F.fnum(at.atoms[2].text)
            pr = F.fnum(at.atoms[3].text) if len(at.atoms) > 3 else 0.0
            s = F.child(pd, 'size')
            if s is None:
                continue
            sw, sh = F.fnum(s.atoms[1].text), F.fnum(s.atoms[2].text)
            # The pad's stored angle is its BOARD angle, not its angle relative
            # to the footprint.  Adding the footprint rotation double-counts it.
            #
            # This was measured, not reasoned: C2 is a CP_Elec_6.3x7.7 at rot
            # 270 whose pads store 270.  Adding gives 180, which draws the
            # 3.5x1.6 pad 3.5mm along X; the F.Mask export shows it 1.59mm wide
            # by 3.51mm tall, i.e. drawn at 270.  R6 (rot 90, pad 90) and R12
            # (rot 180, pad 180) agree.  It matters because the terminal pin
            # labels live in a 4mm channel whose far wall IS C2's pad: the
            # transposed rectangle reported that wall 1mm closer than it is,
            # and the solver, finding no legal slot at all, parked OUT1 and
            # OUT2 half under the connector body with a 1e6 penalty.
            lay = F.child(pd, 'layers')
            names = [a.text.strip('"') for a in lay.atoms[1:]] if lay else []
            # A pad with no mask layer cannot clip silk: DRC tests the APERTURE.
            # An F.Paste-only pad (U3's thermal pads have three) opens nothing,
            # and counting it made those two labels look boxed in.
            if '*.Mask' in names:
                side = '*'
            elif 'F.Mask' in names:
                side = 'F'
            elif 'B.Mask' in names:
                side = 'B'
            else:
                side = '-'
            cx, cy = xform(fat, px, py)
            ang = pr
            ex = rot_pt(sw / 2.0, sh / 2.0, ang)
            hw = abs(ex[0])
            hh = abs(ex[1])
            out.append((cx - hw, cy - hh, cx + hw, cy + hh, ref, num, side))
    return out


def outline(root):
    """Board outline bbox, from Edge.Cuts only -- unfiltered gr_rect picks up
    a mounting-hole or courtyard rectangle and reports the wrong board."""
    xs, ys = [], []
    for head in ('gr_rect', 'gr_line', 'gr_arc', 'gr_poly'):
        for g in F.children(root, head):
            lay = F.child(g, 'layer')
            if lay is None or lay.atoms[1].text.strip('"') != 'Edge.Cuts':
                continue
            for n in (F.child(g, 'start'), F.child(g, 'end'),
                      F.child(g, 'center')):
                if n is not None:
                    xs.append(F.fnum(n.atoms[1].text))
                    ys.append(F.fnum(n.atoms[2].text))
            pts = F.child(g, 'pts')
            if pts is not None:
                for xy in F.children(pts, 'xy'):
                    xs.append(F.fnum(xy.atoms[1].text))
                    ys.append(F.fnum(xy.atoms[2].text))
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


def area(a, b):
    return (max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
            * max(0.0, min(a[3], b[3]) - max(a[1], b[1])))


def main():
    pcb = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') \
        else PCB
    root = C.parse(open(pcb, 'rb').read()).children[0]

    items = collect(root)
    pads = pad_rings(root)
    box = outline(root)

    print('=== %d visible silk text items' % len(items))
    bad_ang = [i for i in items if i['ang'] not in (0, 90)]
    print('=== %d at neither 0 nor 90 deg' % len(bad_ang))
    print()
    print('%-6s %-10s %-4s %-4s %-22s %-9s %s'
          % ('ref', 'kind', 'ang', 'just', 'text', 'anchor', 'level'))
    for i in sorted(items, key=lambda i: (i['ang'], i['ref'])):
        lvl = 'SILK' if i['layer'] == 'F.SilkS' else 'BACK'
        print('%-6s %-10s %-4d %-4s %-22s (%7.2f,%7.2f) %s'
              % (i['ref'], i['kind'][:10], i['ang'], i['just'][:4],
                 i['text'][:22], i['anchor'][0], i['anchor'][1], lvl))

    print()
    print('=== text over a pad (box overlap > 0.02 mm^2)')
    hits = 0
    for i in items:
        b = text_box(i['anchor'][0], i['anchor'][1], i['ang'], i['text'],
                     *i['size'], just=i['just'], mirr=i['mirr'], vjust=i['vjust'])
        for p in pads:
            # Front ink cannot reach a back-side aperture, or vice versa.  A
            # through-hole pad ('*') is open on both and is tested either way.
            want = 'F' if i['layer'] == 'F.SilkS' else 'B'
            if p[6] not in (want, '*'):
                continue
            a = area(b, p[:4])
            if a > 0.02:
                hits += 1
                print('  %-6s %-9s %-18s vs pad %s.%s  (%.2f mm^2)'
                      % (i['ref'], i['kind'][:9], i['text'][:18],
                         p[4], p[5], a))
    print('=== %d pad hits' % hits)

    if box:
        print()
        print('=== outside the board outline x %.2f..%.2f y %.2f..%.2f'
              % (box[0], box[2], box[1], box[3]))
        off = 0
        for i in items:
            b = text_box(i['anchor'][0], i['anchor'][1], i['ang'], i['text'],
                         *i['size'], just=i['just'], mirr=i['mirr'], vjust=i['vjust'])
            if (b[0] < box[0] or b[1] < box[1]
                    or b[2] > box[2] or b[3] > box[3]):
                off += 1
                print('  %-6s %-18s box x %.2f..%.2f y %.2f..%.2f'
                      % (i['ref'], i['text'][:18], b[0], b[2], b[1], b[3]))
        print('=== %d outside' % off)

    print()
    print('=== text-box pairs that overlap (same layer, > 0.02 mm^2)')
    boxes = [(i, text_box(i['anchor'][0], i['anchor'][1], i['ang'], i['text'],
                          *i['size'], just=i['just'], mirr=i['mirr'], vjust=i['vjust'])) for i in items]
    n = 0
    for a in range(len(boxes)):
        for b in range(a + 1, len(boxes)):
            ia, ba = boxes[a]
            ib, bb = boxes[b]
            if ia['layer'] != ib['layer']:
                continue
            ov = area(ba, bb)
            if ov > 0.02:
                n += 1
                print('  %-6s %-16s  <>  %-6s %-16s (%.2f mm^2)'
                      % (ia['ref'], ia['text'][:16], ib['ref'],
                         ib['text'][:16], ov))
    print('=== %d text-vs-text overlaps' % n)


if __name__ == '__main__':
    main()
