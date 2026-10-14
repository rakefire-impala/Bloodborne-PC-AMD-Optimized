"""Generate the lamp travel layer (scripts/lamp_travel.py) from the game's files and check it:
every lamp's "Return to the Hunter's Dream?" dialog calls the travel group, which marks the lamp
itself for the port's menu and never warps to it, common.emevd starts one warp handler per lamp of
the Dream's own, and the menu file lists every headstone lamp with a name.
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

dream = F.Bnd4(F.Dcx((game / 'dvdroot_ps4' / L.DREAM_TALK).read_bytes()).data)
with tempfile.TemporaryDirectory(prefix='bb-lamp-travel-') as tmp:
    out = Path(tmp)
    count = L.generate(L.GameFiles(game), out)
    headstones = [L.Headstone(F.Esd(dream.files[dream.find(n)][1]), m) for n, m in L.HEADSTONES]
    everything = [(n, lamp) for _hs, lamps in L.menu_lamps(headstones) for n, lamp in lamps]
    menu = (out / L.MENU_FILE).read_text(encoding='utf-8').splitlines()
    rows = [line.split('\t') for line in menu]
    assert rows[0][0] == 'dream' and rows[0][1] and rows[1][0] == 'keys' and all(rows[1][1:]) and rows[-1][0] == 'flags', rows[0]
    assert [r[0] for r in rows].count('headstone') == len(L.HEADSTONES)
    listed = [r for r in rows if r[0] == 'lamp']
    assert len(listed) == len(everything) and all(r[3] for r in listed), listed
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
            assert esd.group(L.TRAVEL_GROUP) and esd.group(L.CHECK_GROUP), name
            calls = [(esd.cmds[ci][0], esd.cmds[ci][1]) for st in esd.states
                     for ci in esd.commands(st['entry'])]
            assert (6, L.TRAVEL_GROUP) in calls, name
            own = [n for n, lamp in everything if (lamp[3], lamp[4]) == here]
            sets, checks = [], []
            for si in esd.group_states(L.TRAVEL_GROUP):
                for ci in esd.commands(esd.states[si]['entry']):
                    args = [F.ez_eval(a) for a in esd.cmd_args(ci)]
                    if esd.cmds[ci][:2] == [1, 11]:
                        sets.append(args)
                    elif esd.cmds[ci][:2] == [6, L.CHECK_GROUP]:
                        checks.append(tuple(args[:2]))
            assert all([L.MENU_LAMPS + k, 1] in sets for k in own), f'{name} does not mark itself'
            assert sets[-3] == [L.MENU_REQUEST, 1], name
            assert here not in checks and len(checks) == len(everything) - len(own), name
    assert lamps == count, (lamps, count)

    common = F.Emevd(F.Dcx((out / 'dvdroot_ps4' / L.COMMON_EVENTS).read_bytes()).data)
    handler = common.event_ins(L.HANDLER_EVENT)
    assert [h[:2] for h in handler] == [(1003, 6), (1003, 8), (3, 0), (3, 0), (2003, 18), (1001, 0), (2003, 49)]
    assert handler[1][2] == bytes([0, 1, 21, 0])  # ends in the Hunter's Dream
    assert handler[2][2][:4] == bytes(4) and handler[3][2][:4] == bytes([0, 1, 0, 0])  # off, then on
    starts = [struct.unpack('<4i', a) for b, i, a in common.event_ins(0) if (b, i) == (2000, 0) and len(a) == 16
              and struct.unpack_from('<i', a, 4)[0] == L.HANDLER_EVENT]
    dream = L.destinations(F.Emevd(F.Dcx((game / 'dvdroot_ps4' / L.DREAM_EVENTS).read_bytes()).data))
    assert [(s[2], s[3]) for s in starts] == dream and len({s[0] for s in starts}) == len(starts)
print(f'PASS: {count} lamps open the travel menu ({len(everything)} lamps listed); {len(dream)} warp handlers in common.emevd')
