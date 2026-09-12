#!/usr/bin/env python3
"""Read-only: list every segment/via/pad intersecting a box, any net."""
import re, sys
p = sys.argv[1]
x0,x1,y0,y1 = [float(v) for v in sys.argv[2].split(',')]
lay_f = sys.argv[3] if len(sys.argv) > 3 else None
s = open(p).read()
out = []
for m in re.finditer(r'\(segment\s*\(start ([\d.eE+-]+) ([\d.eE+-]+)\)\s*'
                     r'\(end ([\d.eE+-]+) ([\d.eE+-]+)\)\s*\(width ([\d.eE+-]+)\)\s*'
                     r'\(layer "([^"]+)"\)\s*(?:\(net "([^"]*)"\))?', s):
    a = (float(m.group(1)), float(m.group(2))); b = (float(m.group(3)), float(m.group(4)))
    if lay_f and m.group(6) != lay_f: continue
    lo = (min(a[0],b[0]), min(a[1],b[1])); hi = (max(a[0],b[0]), max(a[1],b[1]))
    if hi[0] < x0 or lo[0] > x1 or hi[1] < y0 or lo[1] > y1: continue
    out.append(('seg', m.group(7) or '', m.group(6), a, b, float(m.group(5))))
for m in re.finditer(r'\(via\s*\(at ([\d.eE+-]+) ([\d.eE+-]+)\)\s*\(size ([\d.eE+-]+)\)\s*'
                     r'\(drill ([\d.eE+-]+)\)\s*\(layers ([^)]*)\)\s*\(net "([^"]*)"\)', s):
    a = (float(m.group(1)), float(m.group(2)))
    if not (x0 <= a[0] <= x1 and y0 <= a[1] <= y1): continue
    out.append(('via', m.group(6) or '', 'all', a, a, float(m.group(3))))
out.sort(key=lambda t: (t[2], t[3][0], t[3][1]))
print(f'{len(out)} items in {x0},{y0} .. {x1},{y1}' + (f' on {lay_f}' if lay_f else ''))
for k, net, lay, a, b, w in out:
    print(f'  {k:<4} {net:<26} {lay:<6} ({a[0]:8.3f},{a[1]:8.3f}) -> ({b[0]:8.3f},{b[1]:8.3f}) w{w}')
