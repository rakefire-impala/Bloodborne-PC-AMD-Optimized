"""Generate the boss retry layer (scripts/boss_retry.py) from the game's files and check it: every
gate's spot is in all of its map's layouts, a step in front of the fog on the side away from the
point beyond it; the retry warps to a lamp of the gate's map and on arrival makes a short warp to
the spot (never the full warp request, a second loading screen) and makes a lamp that is always
there the respawn point, not the one the boss leaves in its arena.
Needs the game folder (BB_GAME_DIR, or the path given); skipped without.
"""
from pathlib import Path
import argparse
import math
import os
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import boss_retry as B  # noqa: E402
import fsformats as F  # noqa: E402
import lamp_travel as L  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('game', nargs='?', default=os.environ.get('BB_GAME_DIR', ''))
args = parser.parse_args()
if not args.game or not (Path(args.game) / 'dvdroot_ps4').is_dir():
    print('SKIP: no game folder (pass it or set BB_GAME_DIR)')
    sys.exit(0)
game = Path(args.game)
files = L.GameFiles(game)

with tempfile.TemporaryDirectory(prefix='bb-boss-retry-') as tmp:
    out = Path(tmp)
    count = B.generate(files, out)
    rows = [line.split('\t') for line in (out / B.RETRY_FILE).read_text(encoding='utf-8').splitlines()]
    assert len([r for r in rows if r[0] == 'gate']) == count >= 20, count
    assert all(r[5] == '1' for r in rows if r[0] == 'gate')
    common = F.Emevd(F.Dcx((out / 'dvdroot_ps4' / B.COMMON_EVENTS).read_bytes()).data)
    layouts = {p.name: F.Msb(F.Dcx(p.read_bytes()).data) for p in (out / 'dvdroot_ps4/map/MapStudio').glob('*.msb.dcx')}
    for name in layouts:  # every variant of a gate's map
        stem = name[:10]
        assert {n for n in layouts if n.startswith(stem)} == {Path(p).name for p in files.map_layouts(stem + '00')}
    number, checked = 0, 0
    for path in files.event_scripts():
        events_name = path.split('/')[-1]
        if not events_name.startswith('m') or events_name.startswith('m29'):
            continue
        original = F.Emevd(F.Dcx(files(path)).data)
        flags = B.lamp_flags(original)
        for gate in B.find_gates(original, path):
            warp, arrival = (common.event_ins(B.FIRST_EVENT + B.EVENTS_PER_GATE * number + k) for k in range(2))
            number += 1
            assert warp[-1][:2] == (2003, 49), warp[-1]
            lamp = struct.unpack('<i', warp[-1][2])[0]
            assert lamp // 10000 == gate.area * 10 + gate.block, lamp
            moves = [(b, i, a) for b, i, a in arrival if b == 2004 and i in (3, 40, 41, 42)]
            assert [m[:2] for m in moves] == [(2004, 41)], moves
            player, kind, spot, dummy = struct.unpack('<4i', moves[0][2])
            assert (player, kind, dummy) == (B.PLAYER, 1, -1)
            home = [struct.unpack('<i', a)[0] for b, i, a in arrival if (b, i) == (2003, 23)]
            assert len(home) == 1 and flags.get(home[0], B.ALWAYS) == B.ALWAYS, (home, flags)
            for name, layout in layouts.items():
                if not name.startswith(events_name[:10]):
                    continue
                position = dict(layout.region_positions())[spot]
                fog, rotation = layout.part_transforms()[gate.fog]
                beyond = dict(layout.region_positions())[gate.point]
                nx, nz = math.sin(math.radians(rotation[1])), math.cos(math.radians(rotation[1]))
                side = lambda p: (p[0] - fog[0]) * nx + (p[2] - fog[2]) * nz  # noqa: E731
                assert side(position) * side(beyond) < 0, (name, gate.fog)  # opposite sides of the fog
                assert abs(abs(side(position)) - B.SPOT_DISTANCE) < 0.01 and abs(position[1] - fog[1]) < 1
                checked += 1
    assert number == count, (number, count)
print(f'PASS: {count} boss gates; {checked} spots in front of their fogs, short warps, lamps that are always there as respawn points')
