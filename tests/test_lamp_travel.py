"""Generate the lamp travel layer (scripts/lamp_travel.py) from the game's files and check it:
every lamp's "Return to the Hunter's Dream?" dialog calls the travel list, each headstone list
leaves out the lamp itself, and common.emevd starts one warp handler per lamp of the Dream's own.
Needs the game folder (BB_GAME_DIR, or the path given); skipped without.
"""
from pathlib import Path
import argparse
import os
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import fsformats as F  # noqa: E402
import lamp_travel as L  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('game', nargs='?', default=os.environ.get('BB_GAME_DIR', ''))
args = parser.parse_args()
if not args.game or not (Path(args.game) / 'dvdroot_ps4').is_dir():
    print('SKIP: no game folder (pass it or set BB_GAME_DIR)')
    sys.exit(0)
game = Path(args.game)

with tempfile.TemporaryDirectory(prefix='bb-lamp-travel-') as tmp:
    out = Path(tmp)
    count = L.generate(L.GameFiles(game), out)
    assert count >= 40, count
    lamps = 0
    for talk in sorted((out / 'dvdroot_ps4/script/talk').glob('*.dcx')):
        bnd = F.Bnd4(F.Dcx(talk.read_bytes()).data)
        for name, data in bnd.files:
            esd = F.Esd(data)
            here = L.lamp_identity(esd)
            if here is None:
                continue
            lamps += 1
            assert esd.write() == data
            assert esd.group(L.TOP_GROUP) and esd.group(L.CHECK_GROUP), name
            calls = [(esd.cmds[ci][0], esd.cmds[ci][1]) for st in esd.states
                     for ci in esd.commands(st['entry'])]
            assert (6, L.TOP_GROUP) in calls, name
            own = 70000000 + here[0] * 10 + here[1]
            for gid, first, n in esd.groups:
                if L.REGION_GROUP <= gid < L.REGION_GROUP + len(L.HEADSTONES):
                    for si in range(first, first + n):
                        for ci in esd.commands(esd.states[si]['entry']):
                            if esd.cmds[ci][:2] == [1, 19]:
                                assert F.ez_eval(esd.cmd_args(ci)[1]) != own, f'{name} lists itself'
    assert lamps == count, (lamps, count)

    common = F.Emevd(F.Dcx((out / 'dvdroot_ps4' / L.COMMON_EVENTS).read_bytes()).data)
    handler = common.event_ins(L.HANDLER_EVENT)
    assert [h[:2] for h in handler] == [(1003, 6), (1003, 8), (3, 0), (2003, 2), (2003, 18), (1001, 0), (2003, 49)]
    assert handler[1][2] == bytes([0, 1, 21, 0])  # ends in the Hunter's Dream
    starts = [struct.unpack('<4i', a) for b, i, a in common.event_ins(0) if (b, i) == (2000, 0) and len(a) == 16
              and struct.unpack_from('<i', a, 4)[0] == L.HANDLER_EVENT]
    dream = L.destinations(F.Emevd(F.Dcx((game / 'dvdroot_ps4' / L.DREAM_EVENTS).read_bytes()).data))
    assert [(s[2], s[3]) for s in starts] == dream and len({s[0] for s in starts}) == len(starts)
print(f'PASS: {count} lamps get the travel lists; {len(dream)} warp handlers in common.emevd')
