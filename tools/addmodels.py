#!/usr/bin/env python3
"""Give the six model-less footprints on SWC a 3D model.

Punch item 8.  Neither KiCad's official 3D packages nor the app bundle ship a
model for any of these:

    J1/J2/J3/J5  TerminalBlock_CUI.3dshapes  -- the directory does not exist
                 upstream at all (gitlab kicad-packages3D returns 404)
    J4           Connector_USB.3dshapes has 12 .step files, none of them the
                 HRO TYPE-C-31-M-12
    F1           Fuse.3dshapes carries only the 0402/0603/0805/1206/1210 chip
                 fuses, not the 1812

So the models were pulled from the LCSC/EasyEDA library (easyeda2kicad) for the
actual purchased part number and copied into the project's own 3dmodels/
directory, which is referenced as ${KIPRJMOD}/3dmodels/...  That keeps the
design self-contained and survives a KiCad upgrade, which editing the app
bundle would not.

OFFSETS.  The vendor model is authored in *EasyEDA's own* frame, which is not
always the frame of the .kicad_mod easyeda2kicad emits beside it -- for the
2-pole block it is 2.54 mm out from its own footprint.  So the offsets below
are NOT taken from EasyEDA's footprints.  Each is measured off the model's own
geometry, using a landmark that also exists in KiCad's footprint, read from the
STEP's CARTESIAN_POINTs (see modelcheck.py) -- except J4, whose STEP frame turns
out to differ from the frame KiCad renders in by ~2.1 mm, so its offset was
measured off a render instead (see below):

    J1/J2/J3/J5  the through-pins are the STEP's deepest points.  Clustering
                 their X gives the pin row directly:
                    2-pole  pins at -5.08,  0.00   -> dx = +5.08
                    3-pole  pins at -5.08,  0.00, +5.08  -> dx = +5.08
                    4-pole  pins at -7.62, -2.54, +2.54, +7.62 -> dx = +7.62
                 The same points cluster on Y = +-0.4 about zero, so dy = 0:
                 the pin row is already on KiCad's y = 0.
    F1           the body is symmetric, X -2.25..+2.25, and KiCad's pads are
                 symmetric about 0 too, so the offset is zero.  (Sizing the
                 offset off pad 1 instead would shift the body 0.29 mm.)
    J4           rotation (0, 0, 180) and dy = -1.07.  Measured, not derived.

                 The STEP's own coordinate frame is NOT the frame KiCad renders
                 in: KiCad applies the STEP's internal placement, and for this
                 vendor file that is worth about 2.1 mm in Y.  So aligning the
                 raw CARTESIAN_POINT extents against the footprint does not
                 work -- the alignment has to be measured off a render.

                 Method: render the top view twice with dy 4 mm apart and diff.
                 Whatever moves by exactly the dy delta is the model; whatever
                 stays put is the board.  At rot 0 the model's gold contact
                 column tracked dy 1:1 (board x 13.23..13.76 at dy -1.15,
                 9.21..9.78 at dy -5.15) while the footprint's own SH pads held
                 still at 21.73..23.81.  So the model's gold -- its SMT contact
                 tails -- sits at the model's +Y end, and under rot 0 it never
                 came near the pads at any dy.

                 The tails must land on the footprint's signal pads, which are
                 INBOARD: board x 22.97..24.42, centres 23.695.  The F.Fab body
                 is 16.00..23.30, its front face exactly on the x = 16.000 edge.
                 Only one rotation can satisfy both:

                     rot 0    shell 16.00..23.24 wants dy = +8.31, which
                              throws the tails to board x 6.07..6.60 -- clear
                              off the board.  Impossible.
                     rot 180  dy = -1.07 gives shell 16.00..23.24 and tails
                              23.28..23.85, squarely inside the pad column.

                 The render confirms it: shell measured 16.00..23.24 against the
                 footprint's own F.Fab body 16.00..23.30 (the 0.06 is the model's
                 own shell-vs-body drafting difference).  The offset is a pure
                 translation, 1:1 -- 6.04 mm of dy moved the shell 6.01 mm.

                 The note that used to live here had rot (0,0,0) with dy = +0.30,
                 arguing that the STEP's +Y face is the open mouth.  That was
                 inverted: +Y is the end the gold contact tails emerge from.
                 Under rot 0 the mouth points inboard and the tails hang off the
                 board -- the "facing inward, completely reversed" that got
                 reported.  It was also still inset ~6 mm too far, because the
                 mouth could not reach the edge with the tails on the pads.


Usage: addmodels.py PCB
"""
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fieldplace as F                                    # noqa: E402
from mcp_server_kicad import _cst as C                    # noqa: E402

PCB = ('<repo-root>'
       '/SWC.kicad_pcb')
MODELS = ('<repo-root>'
          '/3dmodels')

TB2 = 'TerminalBlock_CUI_TB007-508-02_1x02_P5.08mm_Horizontal'
TB3 = 'TerminalBlock_CUI_TB007-508-03_1x03_P5.08mm_Horizontal'
TB4 = 'TerminalBlock_CUI_TB007-508-04_1x04_P5.08mm_Horizontal'

# ref -> (step file in 3dmodels/, offset xyz as written to the file,
#         rotate xyz as written to the file)
PLAN = {
    'J1': (TB2, (5.08, 0.0, 0.0), (0, 0, 0)),
    'J2': (TB3, (5.08, 0.0, 0.0), (0, 0, 0)),
    'J3': (TB3, (5.08, 0.0, 0.0), (0, 0, 0)),
    'J5': (TB4, (7.62, 0.0, 0.0), (0, 0, 0)),
    'F1': ('Fuse_1812_4532Metric', (0.0, 0.0, 0.0), (0, 0, 0)),
    'J4': ('USB_C_Receptacle_HRO_TYPE-C-31-M-12', (0.0, -1.07, 0.0),
           (0, 0, 180)),
}


def num(v):
    """Format a coordinate the way KiCad writes it: no trailing .0 noise."""
    s = ('%.4f' % v).rstrip('0').rstrip('.')
    return s if s not in ('', '-0') else '0'


def xyz(sep, vals):
    return C.List(sep, [C.Atom(b'xyz', b'')] +
                  [C.Atom(num(v).encode(), b' ') for v in vals], b'')


def model_node(path, offset, rotate):
    """The exact shape KiCad 10 writes, matching the blocks already on the
    board: (offset (xyz ...)) rather than the pre-9 (at (xyz ...))."""
    return C.List(b'\n\t', [
        C.Atom(b'model', b''),
        C.Atom(('"%s"' % path).encode(), b' '),
        C.List(b'\n\t\t', [C.Atom(b'offset', b''), xyz(b'\n\t\t\t', offset)],
               b'\n\t\t'),
        C.List(b'\n\t\t', [C.Atom(b'scale', b''),
                           xyz(b'\n\t\t\t', (1, 1, 1))], b'\n\t\t'),
        C.List(b'\n\t\t', [C.Atom(b'rotate', b''),
                           xyz(b'\n\t\t\t', rotate)], b'\n\t\t'),
    ], b'\n\t')


def main():
    pcb = sys.argv[1] if len(sys.argv) > 1 else PCB
    doc = C.parse(open(pcb, 'rb').read())
    root = doc.children[0]

    seen = set()
    n = 0
    for fp in F.children(root, 'footprint'):
        ref = ''
        for p in F.children(fp, 'property'):
            if p.atoms[1].text.strip('"') == 'Reference':
                ref = p.atoms[2].text.strip('"')
        if ref not in PLAN:
            continue
        seen.add(ref)
        name, offset, rotate = PLAN[ref]
        for m in F.children(fp, 'model'):        # idempotent
            fp.remove_child(m)
        fp.children.append(model_node(
            '${KIPRJMOD}/3dmodels/%s.step' % name, offset, rotate))
        print('  %-4s <- %s.step  offset %s rotate %s'
              % (ref, name, offset, rotate))
        n += 1

    missing = set(PLAN) - seen
    if missing:
        raise SystemExit('NOT FOUND on board: %s' % sorted(missing))
    open(pcb, 'wb').write(C.serialize(doc))
    print('addmodels: %d footprints given a 3D model' % n)


if __name__ == '__main__':
    main()
