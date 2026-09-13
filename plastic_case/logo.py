"""
OET Solutions logo -- the source mark, turned into printable geometry.

The mark is the same one that is screened on the BOTTOM of the PCB
(footprints/logo.pretty/LOGO.kicad_mod).  That footprint draws it with 14
filled `fp_poly` outlines on B.SilkS; several of them self-intersect, so the
rendered shape is the EVEN-ODD fill of each outline (verified against a
rasterisation of the footprint).

At its native size the mark is 15.5 x 1.95 mm with 0.06..0.36 mm strokes --
far below what a 0.4 mm nozzle can lay down (a raster opening test loses 44%
of it at a 0.4 mm feature).  So it is:

    1. scaled by LOGO_SCALE, then
    2. widened by a uniform 2D buffer (LOGO_WIDEN, in FINAL millimetres),

which turns the hairline lettering into a chunky, high-contrast mark that
prints as a solid colour patch.  The order matters: applying the buffer BEFORE
the scale multiplies it by `scale`, so a nominal 0.20 mm became 0.78 mm at
3.898x and welded every glyph to its neighbour -- which is why the mark "just
ran together".  Everything else about the mark is unchanged.

Needs numpy + shapely, both present in FreeCAD's bundled Python.
"""

import math
import os
import re

import numpy as np
from shapely.geometry import Polygon
from shapely.ops import unary_union
from shapely.validation import make_valid
from shapely import affinity

import casegeom as G

_HERE = os.path.dirname(os.path.abspath(__file__))
_MOD = os.path.normpath(os.path.join(_HERE, G.LOGOSRC))


def _read_source():
    """[(x,y), ...] per fp_poly, in the footprint's local frame."""
    txt = open(_MOD).read()
    out = []
    for block in re.findall(r'\(fp_poly\s*\(pts(.*?)\)\s*\(stroke', txt, re.S):
        pts = re.findall(r'\(xy\s+([-\d.]+)\s+([-\d.]+)\)', block)
        out.append([(float(x), float(y)) for x, y in pts])
    if not out:
        raise ValueError("no fp_poly found in %s" % _MOD)
    return out


def shapely_outline(scale=None, widen=None):
    """The scaled + thickened mark as a shapely (Multi)Polygon, in logo mm.

    make_valid() resolves each self-intersecting outline with the even-odd rule
    to machine precision (agrees with an independent polygonize+parity build and
    with the rasterised footprint), so this reproduces the screened mark.

    `widen` is applied AFTER scaling and is therefore in FINAL mm -- it is the
    real millimetres added to every edge of the printed mark, so it can be
    chosen against the nozzle size.  (Applying it before scaling, as an earlier
    revision did, multiplies it by `scale`: 0.30 mm at 1:1 became 1.2 mm at
    4x, which welded every letter to its neighbour and is what made the mark
    run together.)
    """
    scale = G.LOGO_SCALE if scale is None else scale
    widen = G.LOGO_WIDEN if widen is None else widen
    parts = [make_valid(Polygon(r)) for r in _read_source()]
    merged = unary_union(parts)
    g = affinity.scale(merged, xfact=scale, yfact=scale, origin=(0, 0))
    if widen:
        g = g.buffer(widen)
    if G.LOGO_SIMPL > 0:
        g = g.simplify(G.LOGO_SIMPL, preserve_topology=True)
    return g


def outline():
    """[(x, y), ...] of the mark's outer ring, centred on the origin."""
    g = shapely_outline()
    geoms = [g] if g.geom_type == "Polygon" else list(g.geoms)
    ring = max(geoms, key=lambda p: p.area).exterior
    cx, cy = g.centroid.x, g.centroid.y
    return [(x - cx, y - cy) for x, y in ring.coords]


def _bbox_centre_shift(g):
    """(mx, my) for `_place`: the mark's bounding-box centre, not its centroid.

    The mark is bottom-heavy -- the gear is far more massive than the final 's'
    -- so its centroid sits ~1.6 mm toward the gear.  Placing the centroid on the
    target therefore pushed the finished mark 1.6 mm long-ways off its own centre
    and tipped the gear's rim past the target band (0.51 mm in x, 5.5 mm in y),
    which is what bit the button recess.  The bbox centre is what "put the mark
    here" means, and it makes LOGO_HALF_* / logo_rect() describe the real pocket
    instead of a box offset from it.
    """
    b = g.bounds
    return (b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0


def _place(xs, ys, mx, my, cx, cy):
    """THE placement map: source (x, y) -> STEP (x, y).  Single source of truth.

    board_x = cx - (y - my)     board_y = (x - mx) - cy
    (the trailing `- cy` is the board -> STEP y negation.)

    `casegeom._checks` probes this function for orientation rather than
    re-deriving it -- a hand-copied version of this map is how the mirror guard
    silently became a no-op once already.
    """
    return (cx - (ys - my), (xs - mx) - cy)


def rings_step(dilate=0.0, cx=0.0, cy=0.0):
    """[(exterior, [hole, ...]), ...] per island, in STEP XY coordinates.

    The mark is NOT one connected part: the native footprint is 14 separate
    `fp_poly` and only a heavy widening welds them into a single blob.  At the
    widening actually used here the letterforms stay distinct islands, so every
    island has to be returned -- returning only the largest (as an earlier
    revision did, when the over-wide buffer happened to merge them all) silently
    prints the first letter and nothing else.

    `dilate` grows every ring outwards (used for the lid pocket, which must
    clear the inlay).

    The placement is ONE map from source (x, y) to board (x, y):

        board_x = cx - (y - my)
        board_y = (x - mx) - cy

    (the trailing `- cy` is the board -> STEP y negation, since the return
    value is in STEP frame.)

    Its Jacobian is [[0, -1], [+1, 0]], det = +1: a proper rotation, which is
    what keeps the text readable.  The mark's long axis runs along board y, and
    source +x (the reading direction) maps to board -y, so the text reads
    BOTTOM-TO-TOP in board coordinates.  An image rotation to the other sense
    would be equally valid, but negating either coefficient instead is a
    reflection and prints the mark backwards.

    det = -1 here is the entire mirroring bug.  It is easy to mis-derive
    because the map is written as explicit lambdas and the product of the
    diagonal is zero, so the determinant is just -1 x (the sign on the `y -
    my` term) -- and that sign is easy to get backwards when two independent
    negations have to cancel: the 90 deg turn from the mark's long axis onto
    board y, and the board -> STEP y negation at the end.  Verified
    numerically: with `cx + (y - my)` (det = -1, a reflection) the placed mark
    matched the source's mirror+270 deg transform at IoU 0.992 and its best
    proper rotation at only 0.309; with `cx - (y - my)` (det = +1) the proper
    rotation matches at 1.000 and the mirror at 0.309.  Do not let a
    within-frame checker adjudicate this -- one that shares this function's
    own y convention cannot see the difference.

    The mark is deliberately NOT y-flipped on its own: the source is B.SilkS,
    seen from the board's BOTTOM, so its coordinates already read correctly
    from the lid's TOP.  Negating the mark itself, rather than folding the
    negation into the placement as above, is the other way to get a mirror.

    Centre on the mark's BOUNDING BOX, not its centroid (see below).  The
    return value is in STEP frame: board (x, y) -> STEP (x, -y).
    """
    from shapely.ops import transform as sh_transform
    g = shapely_outline()
    if dilate:
        g = g.buffer(dilate)
    mx, my = _bbox_centre_shift(g)
    g = sh_transform(lambda xs, ys: _place(xs, ys, mx, my, cx, cy), g)
    geoms = [g] if g.geom_type == "Polygon" else list(g.geoms)
    geoms = sorted(geoms, key=lambda p: p.area, reverse=True)
    return [(list(p.exterior.coords), [list(h.coords) for h in p.interiors])
            for p in geoms]


def size():
    g = shapely_outline()
    b = g.bounds
    return (b[2] - b[0], b[3] - b[1])


def report():
    g = shapely_outline()
    return {
        "polys": 1 if g.geom_type == "Polygon" else len(g.geoms),
        "holes": sum(len(p.interiors) for p in
                     ([g] if g.geom_type == "Polygon" else g.geoms)),
        "area": g.area,
        "w": g.bounds[2] - g.bounds[0],
        "h": g.bounds[3] - g.bounds[1],
        "source": _MOD,
        "src_polys": len(_read_source()),
    }


if __name__ == "__main__":
    r = report()
    print("logo source: %s (%d fp_poly)" % (r["source"], r["src_polys"]))
    print("widened+scaled: %.1f x %.1f mm, %d parts, %d holes, %.1f mm2"
          % (r["w"], r["h"], r["polys"], r["holes"], r["area"]))
