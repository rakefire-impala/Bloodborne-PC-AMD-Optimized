"""Round-trip the game's talk scripts (fsSL in BND4/DCX) and event scripts (EVD in DCX) through
scripts/fsformats.py: unmodified files must come back byte for byte, and an edited script must
read back with the edits. Needs the game folder (BB_GAME_DIR, or the path given); skipped without.
"""
from pathlib import Path
import argparse
import os
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import fsformats  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('game', nargs='?', default=os.environ.get('BB_GAME_DIR', ''))
args = parser.parse_args()
root = Path(args.game) / 'dvdroot_ps4' if args.game else None
if not root or not root.is_dir():
    print('SKIP: no game folder (pass it or set BB_GAME_DIR)')
    sys.exit(0)

talk = sorted((root / 'script' / 'talk').glob('*.talkesdbnd.dcx'))
events = sorted((root / 'event').glob('*.emevd.dcx'))
esd_count = 0
for path in talk:
    dcx = fsformats.Dcx(path.read_bytes())
    assert fsformats.Dcx(dcx.write()).data == dcx.data, path
    bnd = fsformats.Bnd4(dcx.data)
    assert bnd.write() == dcx.data, f'{path.name}: BND4 rewrite differs'
    for name, data in bnd.files:
        esd = fsformats.Esd(data)
        assert esd.write() == data, f'{path.name}/{name}: talk script rewrite differs'
        esd_count += 1
for path in events:
    dcx = fsformats.Dcx(path.read_bytes())
    ev = fsformats.Emevd(dcx.data)
    assert ev.write() == dcx.data, f'{path.name}: event script rewrite differs'

# Edits read back: a new talk script group, an event appended to and a new event.
sample = fsformats.Bnd4(fsformats.Dcx((root / 'script/talk/m24_01_00_00.talkesdbnd.dcx').read_bytes()).data)
esd = fsformats.Esd(sample.files[0][1])
end = esd.add_cond(fsformats.ez(1), passes=[(7, -1, [fsformats.ez(0)])])
go = esd.add_cond(fsformats.ez(1))
ids = esd.add_group(0x7FFFFF00, [(0, [], [], [], [go]), (1, [(1, 0, [fsformats.ez(5)])], [], [], [end])])
esd.set_target(go, ids[1])
again = fsformats.Esd(esd.write())
g = again.group(0x7FFFFF00)
assert g and g[2] == 2 and again.states[g[1] + 1]['id'] == 1
assert again.cmd_args(again.commands(again.states[g[1] + 1]['entry'])[0]) == [fsformats.ez(5)]
assert again.conds[again.state_conds(g[1])[0]]['target'] == g[1] + 1
assert fsformats.Esd(esd.write()).write() == esd.write()

common = fsformats.Emevd(fsformats.Dcx((root / 'event/common.emevd.dcx').read_bytes()).data)
before = len(common.event_ins(0))
common.append_to_event(0, [(2000, 0, bytes(16))])
common.add_event(7299, [(1001, 0, bytes(4))], params=[(0, 0, 0, 4)])
after = fsformats.Emevd(common.write())
assert len(after.event_ins(0)) == before + 1 and after.event_ins(7299) == [(1001, 0, bytes(4))]
assert after.event_ins(7200) == fsformats.Emevd(fsformats.Dcx((root / 'event/common.emevd.dcx').read_bytes()).data).event_ins(7200)

layouts = sorted((root / 'map' / 'MapStudio').glob('m[23]*.msb.dcx'))
for path in layouts:
    msb = fsformats.Msb(fsformats.Dcx(path.read_bytes()).data)
    assert msb.write() == msb.raw, f'{path.name}: map layout rewrite differs'
# An added region reads back where it was put; the rest stays (Old Yharnam: Darkbeast Paarl's fog).
msb = fsformats.Msb(fsformats.Dcx((root / 'map/MapStudio/m23_00_00_00.msb.dcx').read_bytes()).data)
regions, parts = msb.region_positions(), msb.part_positions()
msb.add_region(2302810, 2302990, (1.0, 2.0, 3.0), 45.0, 'test region with a longer name')
again = fsformats.Msb(msb.write())
assert again.region_positions() == regions + [(2302990, (1.0, 2.0, 3.0))]
assert again.part_positions() == parts and again.respawn_points() == msb.respawn_points()
print(f'PASS: {len(talk)} talk archives ({esd_count} scripts), {len(events)} event scripts and '
      f'{len(layouts)} map layouts round-trip; edits read back')
