#!/usr/bin/env python3
"""Corridor router verifier for SWC.

RETIRED (SWC2 era).  Kept for provenance.  Do NOT repoint ``PCB`` at
SWC.kicad_pcb to make it "work".  Its clearance model does not agree with
KiCad's own DRC on the reworked board: run against SWC.kicad_pcb it reports
~48 clearance violations the DRC does not produce (DRC: 0 clearance errors at
min_clearance 0.15).  This is the same class of false "un-routable" proof that
the ``swc2-usb-corridor-proven-blocked`` memory records -- the model treats
SMD pads as blocking on both layers.  The stale path is deliberate: it fails
loudly instead of printing confident, wrong clearances.  The project's
tie-breaker is DRC's own message text, not this tool's arithmetic.

Read-only. Checks the USB / VBUS corridor against the board's own design rules
BEFORE any write, so a bad route is never committed.

  python3 tools/corridor_check.py                 # full board, all layers
  python3 tools/corridor_check.py --box 20 42 42 60   # limit to a window

Checks
  1. clearance  — every copper pair on a layer, different nets, min gap
  2. widths     — every track's width vs the requirement for its net
  3. nets       — union-find: is each net's copper one connected piece?

Obstacles are read from the live board with the same transform the MCP server
uses: board = origin + R(rot) . local   (see tools/README.md).
"""
import argparse, json, math, os, re, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(ROOT, "SWC2.kicad_pcb")

# ---- board design rules (read from SWC2.kicad_pcb setup block) -------------
MIN_CLEAR = 0.15
MIN_WIDTH = 0.15

# Required width per net.  IPC-2221 external-layer, 0.035 mm (1 oz) copper,
# dT = 10 C:  I = 0.048 * dT^0.44 * A^0.725   (A in mil^2)
#   0.2 mm -> 0.745 A   0.3 -> 1.00 A   0.5 -> 1.51 A   0.6 -> 1.73 A
# F1 = 1.5 A (+12V -> buck), F2 = 1.0 A (VBUS -> fused VBUS)
WIDTH_REQ = {
    "/VBUS":        0.50,   # whole-board input, F2 = 1.0 A, headroom to F1's 1.5 A
    "/VBUS_FUSED":  0.50,
    "+5V":          0.50,
    "/+5V_BUCK":    0.50,
    "/+12V_SW":     0.60,
    "+12V_DC_IN":   0.60,
    "Net-(J1-Pin_2)": 0.60,
}
# Nets the rework owns; everything else is only checked for clearance.
USB_NETS = {"Net-(J4-D+-PadA6)", "Net-(J4-D--PadA7)",
            "Net-(U3-USB_D+)", "Net-(U3-USB_D-)"}


# ---- geometry --------------------------------------------------------------
def seg_seg(p, q, r, s):
    """Min distance between segments pq and rs (0 if they cross)."""
    def pt_seg(a, b, c):
        bx, by = b[0] - a[0], b[1] - a[1]
        cx, cy = c[0] - a[0], c[1] - a[1]
        d = bx * bx + by * by
        t = 0.0 if d == 0 else max(0.0, min(1.0, (cx * bx + cy * by) / d))
        return math.hypot(cx - t * bx, cy - t * by)
    def orient(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    o1, o2 = orient(p, q, r), orient(p, q, s)
    o3, o4 = orient(r, s, p), orient(r, s, q)
    if o1 * o2 < 0 and o3 * o4 < 0:
        return 0.0
    return min(pt_seg(p, q, r), pt_seg(p, q, s), pt_seg(r, s, p), pt_seg(r, s, q))


def rect_pts(cx, cy, w, h, ang):
    t = math.radians(ang); c, s = math.cos(t), math.sin(t)
    return [(cx + a * c - b * s, cy + a * s + b * c)
            for a, b in ((-w/2, -h/2), (w/2, -h/2), (w/2, h/2), (-w/2, h/2))]


def seg_rect(a, b, poly):
    """Distance from segment ab to a convex polygon; 0 if it touches/enters."""
    def inside(p, poly):
        sgn = 0
        for i in range(len(poly)):
            x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % len(poly)]
            cr = (x2 - x1) * (p[1] - y1) - (y2 - y1) * (p[0] - x1)
            if abs(cr) < 1e-12:
                continue
            g = 1 if cr > 0 else -1
            if sgn and g != sgn:
                return False
            sgn = g
        return True
    if inside(a, poly) or inside(b, poly):
        return 0.0
    d = min(seg_seg(a, b, poly[i], poly[(i + 1) % len(poly)]) for i in range(len(poly)))
    return d


def rect_rect(p1, p2):
    d = min(min(seg_seg(p1[i], p1[(i+1) % 4], p2[j], p2[(j+1) % 4]) for j in range(4))
            for i in range(4))
    return d


# ---- load ------------------------------------------------------------------
def load(pcb=PCB, propose=None):
    txt = open(pcb).read()
    segs, vias, fps = [], [], []
    for ch in txt.split("\n\t(segment\n")[1:]:
        a = re.search(r"\(start ([-\d.]+) ([-\d.]+)\)", ch)
        b = re.search(r"\(end ([-\d.]+) ([-\d.]+)\)", ch)
        w = re.search(r"\(width ([-\d.]+)\)", ch)
        l = re.search(r"\(layer \"([^\"]*)\"\)", ch)
        n = re.search(r"\(net \"([^\"]*)\"\)", ch)
        if a and b:
            segs.append(dict(x1=float(a.group(1)), y1=float(a.group(2)),
                             x2=float(b.group(1)), y2=float(b.group(2)),
                             w=float(w.group(1)), L=l.group(1),
                             N=n.group(1) if n else "", new=False))
    for ch in txt.split("\n\t(via\n")[1:]:
        a = re.search(r"\(at ([-\d.]+) ([-\d.]+)\)", ch)
        s = re.search(r"\(size ([-\d.]+)\)", ch)
        n = re.search(r"\(net \"([^\"]*)\"\)", ch)
        if a:
            vias.append(dict(x=float(a.group(1)), y=float(a.group(2)),
                             size=float(s.group(1)), N=n.group(1) if n else "",
                             new=False))
    # ---- proposed geometry (validated in memory; never written by this tool)
    if propose:
        prop = json.load(open(propose))
        for s in prop.get("segs", []):
            segs.append(dict(x1=s["x1"], y1=s["y1"], x2=s["x2"], y2=s["y2"],
                             w=s.get("w", 0.2), L=s["layer"], N=s["net"], new=True))
        for v in prop.get("vias", []):
            vias.append(dict(x=v["x"], y=v["y"], size=v.get("size", 0.5),
                             N=v["net"], new=True))
        # a proposed net may also *replace* existing copper
        drop = set(prop.get("drop_nets", []))
        if drop:
            segs = [s for s in segs if s["new"] or s["N"] not in drop]
            vias = [v for v in vias if v["new"] or v["N"] not in drop]
        for p in prop.get("drop", []):
            x0, y0, x1, y1 = p
            segs = [s for s in segs if s["new"] or not (
                x0 <= min(s["x1"], s["x2"]) and max(s["x1"], s["x2"]) <= x1 and
                y0 <= min(s["y1"], s["y2"]) and max(s["y1"], s["y2"]) <= y1)]
            vias = [v for v in vias if v["new"] or not (x0 <= v["x"] <= x1 and y0 <= v["y"] <= y1)]
    for ch in txt.split("\n\t(footprint ")[1:]:
        r = re.search(r'\(property "Reference" "([^"]*)"', ch)
        a = re.search(r"\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)", ch)
        if not (r and a):
            continue
        ox, oy, rot = float(a.group(1)), float(a.group(2)), float(a.group(3) or 0)
        t = math.radians(rot); c, s = math.cos(t), math.sin(t)
        for pch in ch.split("\n\t\t(pad ")[1:]:
            num = re.match(r'"?([^"]*)"?', pch).group(1)
            pat = re.search(r"\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)", pch)
            psz = re.search(r"\(size ([-\d.]+) ([-\d.]+)\)", pch)
            pnet = re.search(r'\(net "([^"]*)"\)', pch)
            pthr = re.search(r"\(drill", pch)
            pls = re.findall(r'\(layers "?([^")\s]+)"?', pch)
            if not (pat and psz):
                continue
            lx, ly = float(pat.group(1)), float(pat.group(2))
            ang = float(pat.group(3) or 0)
            w, h = float(psz.group(1)), float(psz.group(2))
            bx = ox + lx * c + ly * s
            by = oy - lx * s + ly * c
            if pls and pls[0].startswith("*"):
                lays = ("F.Cu", "B.Cu")
            else:
                lays = tuple(l for l in pls if l.endswith(".Cu")) or ("F.Cu",)
            fps.append(dict(ref=r.group(1), num=num, x=bx, y=by, w=w, h=h, ang=ang,
                            N=pnet.group(1) if pnet else "", thru=bool(pthr),
                            lays=lays))
    return segs, vias, fps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--box", nargs=4, type=float, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--clear", type=float, default=MIN_CLEAR)
    ap.add_argument("--quiet-clear", action="store_true")
    ap.add_argument("--propose", metavar="JSON",
                    help="validate a proposed route in memory (never written)")
    args = ap.parse_args()

    segs, vias, fps = load(propose=args.propose)
    box = args.box

    def bb(o, kind):
        if kind == "seg":
            return (min(o["x1"], o["x2"]), min(o["y1"], o["y2"]),
                    max(o["x1"], o["x2"]), max(o["y1"], o["y2"]))
        if kind == "via":
            r = o["size"] / 2
        else:
            r = max(o["w"], o["h"]) / 2
        return (o["x"] - r, o["y"] - r, o["x"] + r, o["y"] + r)

    def inbox(o, kind):
        if not box:
            return True
        x0, y0, x1, y1 = box
        a, b, c, d = bb(o, kind)
        return x0 <= a and c <= x1 and y0 <= b and d <= y1

    print(f"loaded {len(segs)} segs, {len(vias)} vias, {len(fps)} pads")
    bad = 0

    # ---- 1. clearance ------------------------------------------------------
    items = []   # (layer, net, kind, geom)
    for s in segs:
        items.append((s["L"], s["N"], "seg", s))
    for v in vias:
        for L in ("F.Cu", "B.Cu"):
            items.append((L, v["N"], "via", v))
    for p in fps:
        for L in p["lays"]:
            items.append((L, p["N"], "pad", p))

    viol = []
    n = len(items)
    for i in range(n):
        Li, Ni, ki, gi = items[i]
        if box and not inbox(gi, ki):
            continue
        gi_box = bb(gi, ki)
        for j in range(i + 1, n):
            Lj, Nj, kj, gj = items[j]
            if Li != Lj or Ni == Nj or not Ni or not Nj:
                continue
            if box and not inbox(gj, kj):
                continue
            gj_box = bb(gj, kj)
            gap_ub = max(gi_box[0] - gj_box[2], gj_box[0] - gi_box[2],
                         gi_box[1] - gj_box[3], gj_box[1] - gi_box[3])
            if gap_ub > 3.0:
                continue
            # exact
            def geo(k, g):
                """-> (kind, a, b, halfwidth) where kind in {s,v,p}"""
                if k == "seg":
                    return ("s", (g["x1"], g["y1"]), (g["x2"], g["y2"]), g["w"] / 2)
                if k == "via":
                    return ("v", (g["x"], g["y"]), None, g["size"] / 2)
                return ("p", rect_pts(g["x"], g["y"], g["w"], g["h"], g["ang"]), None, 0.0)
            ka, ga, gb, ra = geo(ki, gi)
            kb, ha, hb, rb = geo(kj, gj)
            if ka == "s" and kb == "s":
                d = seg_seg(ga, gb, ha, hb)
            elif ka == "s" and kb == "p":
                d = seg_rect(ga, gb, ha)
            elif ka == "p" and kb == "s":
                d = seg_rect(ha, hb, ga)
            elif ka == "p" and kb == "p":
                d = rect_rect(ga, ha)
            elif ka == "s":                      # s vs v
                d = _pt_seg(ga, gb, ha)
            elif kb == "s":                      # v vs s
                d = _pt_seg(ha, hb, ga)
            elif ka == "p":                      # p vs v
                d = seg_rect(ha, ha, ga)
            elif kb == "p":                      # v vs p
                d = seg_rect(ga, ga, ha)
            else:                                # v vs v
                d = math.dist(ga, ha)
            gap = d - ra - rb
            if gap < args.clear - 1e-6:
                viol.append((gap, Li, Ni, ki, gi, Nj, kj, gj))
    viol.sort(key=lambda r: (r[0], r[1], str(r[2]), str(r[5])))
    if viol and not args.quiet_clear:
        print(f"\nCLEARANCE: {len(viol)} violations (< {args.clear} mm)")
        for gap, L, Ni, ki, gi, Nj, kj, gj in viol[:25]:
            loc = gi.get("x", gi.get("x1"))
            locy = gi.get("y", gi.get("y1"))
            print(f"  {gap:7.4f}  {L:5} {Ni:<22} vs {Nj:<22} near ({loc:.2f},{locy:.2f})")
        bad += len(viol)
    else:
        print(f"\nCLEARANCE: OK (0 < {args.clear} mm)")

    # ---- 2. widths ---------------------------------------------------------
    print("\nWIDTHS")
    for net, req in sorted(WIDTH_REQ.items()):
        ws = [s["w"] for s in segs if s["N"] == net]
        if not ws:
            continue
        thin = [w for w in ws if w < req - 1e-9]
        flag = "FAIL" if thin else "ok  "
        print(f"  {flag} {net:<20} req {req:.2f}  have min {min(ws):.2f} max {max(ws):.2f} "
              f"({len(ws)} segs, {len(thin)} thin)")
        bad += len(thin)
    usb = [s for s in segs if s["N"] in USB_NETS]
    if usb:
        print(f"  usb  {'/'.join(sorted(USB_NETS))}: widths "
              f"{sorted(set(s['w'] for s in usb))}")

    # ---- 3. connectivity ---------------------------------------------------
    print("\nCONNECTIVITY")
    # Nets poured as planes: their copper is the fill, not the tracks, so a
    # union-find over tracks/pads legitimately shows many "pieces".
    PLANE_NETS = {"GND", "+3V3"}
    TOUCH = 0.05        # copper must actually meet, not merely come close

    bynet = defaultdict(list)
    for s in segs:
        if s["N"]:
            bynet[s["N"]].append(dict(k="s", a=(s["x1"], s["y1"]), b=(s["x2"], s["y2"]),
                                      L=s["L"], r=s["w"] / 2))
    for v in vias:
        if v["N"]:
            bynet[v["N"]].append(dict(k="v", a=(v["x"], v["y"]), b=None, L=None,
                                      r=v["size"] / 2))
    for p in fps:
        if p["N"]:
            bynet[p["N"]].append(dict(k="p", a=(p["x"], p["y"]), b=None, L=None,
                                      r=0.0,
                                      rect=rect_pts(p["x"], p["y"], p["w"], p["h"], p["ang"])))

    def touch(u, v):
        if u["k"] == "p" and v["k"] == "p":
            return rect_rect(u["rect"], v["rect"]) <= TOUCH
        if u["k"] == "p" or v["k"] == "p":
            seg, pad = (u, v) if v["k"] == "p" else (v, u)
            if seg["k"] == "s":
                return seg_rect(seg["a"], seg["b"], pad["rect"]) <= seg["r"] + TOUCH
            # via (a point) vs pad
            return seg_rect(seg["a"], seg["a"], pad["rect"]) <= seg["r"] + TOUCH
        if u["k"] == "s" and v["k"] == "s":
            if u["L"] != v["L"]:
                return False
            return seg_seg(u["a"], u["b"], v["a"], v["b"]) <= u["r"] + v["r"] + TOUCH
        if u["k"] == "s":                                  # s vs v
            return _pt_seg(u["a"], u["b"], v["a"]) <= u["r"] + v["r"] + TOUCH
        if v["k"] == "s":                                  # v vs s
            return _pt_seg(v["a"], v["b"], u["a"]) <= u["r"] + v["r"] + TOUCH
        return math.dist(u["a"], v["a"]) <= u["r"] + v["r"] + TOUCH   # v vs v

    splits = []
    parent = {}
    def find(a):
        while parent.get(a, a) != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for net, ns in sorted(bynet.items()):
        for i, x in enumerate(ns):
            parent[(net, i)] = (net, i)
        for i in range(len(ns)):
            for j in range(i + 1, len(ns)):
                if touch(ns[i], ns[j]):
                    union((net, i), (net, j))
        groups = defaultdict(list)
        for i in range(len(ns)):
            groups[find((net, i))].append(ns[i])
        if len(groups) > 1:
            nseg = sum(1 for x in ns if x["k"] == "s")
            nvia = sum(1 for x in ns if x["k"] == "v")
            npad = sum(1 for x in ns if x["k"] == "p")
            splits.append((net, len(groups), nseg, nvia, npad,
                           [(x["k"], round(x["a"][0], 2), round(x["a"][1], 2))
                            for g in list(groups.values())[1:]][:4]))
    for net, ng, nseg, nvia, npad, sample in splits:
        if net in PLANE_NETS:
            print(f"  ok   {net:<24} {ng} pieces (plane-poured; expected)")
            continue
        tag = "FAIL" if (net in USB_NETS or net in WIDTH_REQ) else "warn"
        print(f"  {tag} {net:<24} {ng} pieces ({nseg} seg, {nvia} via, {npad} pad)"
              f"   stray: {sample}")
        if tag == "FAIL":
            bad += ng - 1
    if not splits:
        print("  every net is a single connected piece")
    print(f"\nTOTAL ISSUES: {bad}")
    return 1 if bad else 0


def _pt_seg(a, b, c):
    bx, by = b[0] - a[0], b[1] - a[1]
    cx, cy = c[0] - a[0], c[1] - a[1]
    d = bx * bx + by * by
    t = 0.0 if d == 0 else max(0.0, min(1.0, (cx * bx + cy * by) / d))
    return math.hypot(cx - t * bx, cy - t * by)


if __name__ == "__main__":
    sys.exit(main())
