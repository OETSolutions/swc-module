#!/usr/bin/env python3
"""List J4-net copper intersecting a region, with connectivity, so the rip set
can be decided precisely.  Read-only."""
import re
import sys

BOX = (16.0, 27.5, 40.0, 58.0)     # x0,x1,y0,y1  (items intersecting this)
NETS = ('/VBUS', 'Net-(J4-CC1)', 'Net-(J4-CC2)',
        'Net-(J4-D+-PadA6)', 'Net-(J4-D--PadA7)')

s = open(sys.argv[1]).read()
x0, x1, y0, y1 = BOX


def inbox(p):
    return x0 <= p[0] <= x1 and y0 <= p[1] <= y1


items = []
for m in re.finditer(r'\(segment\s*\(start ([\d.-]+) ([\d.-]+)\)\s*'
                     r'\(end ([\d.-]+) ([\d.-]+)\)\s*\(width ([\d.-]+)\)\s*'
                     r'\(layer "([^"]+)"\)\s*\(net "([^"]*)"\)', s):
    p = (float(m.group(1)), float(m.group(2)))
    q = (float(m.group(3)), float(m.group(4)))
    net = m.group(7)
    if net not in NETS:
        continue
    if inbox(p) or inbox(q):
        items.append(('seg', net, m.group(6), p, q, float(m.group(5))))
for m in re.finditer(r'\(via\s*\(at ([\d.-]+) ([\d.-]+)\)\s*\(size ([\d.-]+)\)\s*'
                     r'\(drill ([\d.-]+)\)\s*\(layers ([^)]*)\)\s*\(net "([^"]*)"\)', s):
    p = (float(m.group(1)), float(m.group(2)))
    if m.group(6) in NETS and inbox(p):
        items.append(('via', m.group(6), 'all', p, p, float(m.group(3))))

items.sort(key=lambda t: (t[1], min(t[3][1], t[4][1]), min(t[3][0], t[4][0])))
print(f'{len(items)} items on J4 signal nets touching x {x0}..{x1}, y {y0}..{y1}')
for k, net, lay, p, q, w in items:
    full = inbox(p) and inbox(q)
    tag = 'IN ' if full else 'CUT'
    print(f'  {tag} {net:<24} {lay:<5} ({p[0]:8.3f},{p[1]:8.3f}) -> '
          f'({q[0]:8.3f},{q[1]:8.3f})  w{w}')
