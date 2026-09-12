#!/usr/bin/env python3
"""Delete vias at given board coordinates.

No MCP tool can remove a via -- `remove_traces` explicitly does not touch them
-- so a rip-and-re-route of the J4 fan-out leaves orphan vias stranded.  This
is the sanctioned direct writer for that, in the style of padangle.py.

Every via it removes is named on the command line; it never removes anything
else, and it refuses a coordinate that does not match exactly one via, so a
typo cannot silently delete the wrong copper.

A doubled via (two stacked at the same coordinate) makes the count 2, and the
plain form then refuses.  That is the point of the guard, so the escape is
explicit rather than automatic: `X,Y*N` means "this coordinate, and I expect
exactly N vias there".  Deleting all of a stack still requires naming it.

Usage: delvia.py PCB X,Y [X,Y*N] ...
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                        # noqa: E402
from mcp_server_kicad import _cst as C        # noqa: E402


def main():
    pcb = sys.argv[1]
    want = []
    for a in sys.argv[2:]:
        n = 1
        if '*' in a:
            a, cnt = a.rsplit('*', 1)
            n = int(cnt)
        x, y = a.split(',')
        want.append((float(x), float(y), n))
    if not want:
        sys.exit('nothing to delete')

    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]

    # index every via by its (x, y)
    found = {}
    for v in root.find_all('via'):
        at = v.find('at')
        found.setdefault((round(F.fnum(at.atoms[1].text), 3),
                          round(F.fnum(at.atoms[2].text), 3)), []).append(v)

    drop = []
    for x, y, n in want:
        key = (round(x, 3), round(y, 3))
        hits = found.get(key, [])
        if len(hits) != n:
            sys.exit('via at %s matched %d vias, expected %d -- refusing to write'
                     % (key, len(hits), n))
        drop.append((key, hits[0]))

    for key, v in drop:
        root.remove_child(v)
        print('  delete via at %s' % (key,))

    open(pcb, 'wb').write(C.serialize(doc))
    print('removed %d vias from %s' % (len(drop), pcb))


main()
