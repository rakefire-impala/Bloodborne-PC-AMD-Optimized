# SPDX-License-Identifier: GPL-2.0-or-later
"""Lamp-to-lamp travel (launcher: Quality of Life). Generated from the player's own game files.

At a lamp, "Return to the Hunter's Dream?" becomes a list: Hunter's Dream (the game's own travel)
and the five headstones of the Dream, each opening that headstone's list of lit lamps. The game's
texts are reused, so the menus follow the game language.

How the game does it: a headstone's talk script (script/talk/m21_00_00_00, t210000-t210003 and
t210721) lists lamps with c1_19(index, name 7000MMMI, lit flag) and, once a lamp is picked, sets
the warp request flag 7210MMMI; the Hunter's Dream event script (event 12107000, one instance per
lamp) waits for that flag and warps with "respawn point warp" (2003[49]) to the lamp's region.

What this adds, in a mod layer that replaces whole files:
- each lamp's talk script (script/talk/<map>.talkesdbnd.dcx): the list menus, built from the
  headstones' own lists and their "lamp usable?" check (copied as is), minus the lamp itself;
- the common event script (event/common.emevd.dcx): event 7250, one instance per lamp, which
  outside the Hunter's Dream and outside co-op waits for that lamp's warp flag, clears it, plays
  the lamp travel animation and warps (the Dream keeps its own handlers).
Lamps show the travel dialog only in single player, so co-op never sets these flags.
"""
import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fsformats as F  # noqa: E402

DREAM_TALK = 'script/talk/m21_00_00_00.talkesdbnd.dcx'
DREAM_EVENTS = 'event/m21_00_00_00.emevd.dcx'
COMMON_EVENTS = 'event/common.emevd.dcx'
# Headstone talk scripts: (file, headstone name message, "has a lit lamp" flag).
HEADSTONES = [('t210000.esd', 70010100, 12105030), ('t210001.esd', 70010101, 12105031),
              ('t210002.esd', 70010102, 12105032), ('t210003.esd', 70010103, 12105033),
              ('t210721.esd', 70010104, 12105034)]
DREAM_NAME = 70002100          # "Hunter's Dream"
DREAM_QUESTION = 10010713      # "Return to the Hunter's Dream?"
WARP_EVENT = 12107000          # the Dream's per-lamp warp handler
HANDLER_EVENT = 7250           # ours, in common.emevd (7210-7299 are unused there)
TRAVEL_ANIMATION = 101160      # what the game's own lamp travel (common event 7200) plays
PLAYER = 10000
TOP_GROUP, REGION_GROUP, CHECK_GROUP = 0x7FFFFF80, 0x7FFFFF81, 0x7FFFFF8F

TRUE = F.ez(1)
EV_CALL_RESULT = lambda n: bytes([F.EZ_CALL_RESULT]) + F.ez_int(n) + bytes([F.EZ_EQ, F.EZ_END])  # noqa: E731
EV_CALL_DONE = bytes([F.EZ_CALL_RESULT, F.EZ_CALL_RUNNING, F.EZ_NE, F.EZ_END])
EV_SELECTED = lambda n: bytes([F.EZ_GET_REG0]) + F.ez_int(n) + bytes([F.EZ_EQ, F.EZ_END])  # noqa: E731


def ret(value):
    return (7, -1, [F.ez(value)])


class Headstone:
    """One headstone's lamp list, read from its talk script."""

    def __init__(self, esd: F.Esd, name_msg, flag):
        self.name_msg, self.flag = name_msg, flag
        self.lamps = []  # (index, name message, lit flag, map, lamp, "not usable" message)
        self.cancel = self.show = self.wait = None
        self.check_group = None
        for gid, first, count in esd.groups:
            for si in range(first, first + count):
                items = [ci for ci in esd.commands(esd.states[si]['entry']) if esd.cmds[ci][:2] == [1, 19]]
                if len(items) >= 4:
                    self._read(esd, gid, items)
                    return
        raise ValueError('no lamp list')

    def _read(self, esd, gid, items):
        entries = [[F.ez_eval(a) for a in esd.cmd_args(ci)] for ci in items]
        # The state that waits for the choice: its first condition stores it (SetREG0).
        for si in esd.group_states(gid):
            conds = esd.state_conds(si)
            if conds and F.EZ_SET_REG0 in esd.expression(esd.conds[conds[0]]['eval']):
                switch = si
                break
        else:
            raise ValueError('no selection state')
        self.cancel = esd.expression(esd.conds[esd.state_conds(switch)[0]]['eval'])
        self.show = [(b, c, esd.cmd_args(ci)) for ci in esd.commands(esd.states[switch]['entry'])
                     for b, c, _ in [esd.cmds[ci]]]
        self.wait = [(b, c, esd.cmd_args(ci)) for ci in esd.commands(esd.states[switch]['while'])
                     for b, c, _ in [esd.cmds[ci]]]
        targets = {}
        for ci in esd.state_conds(switch)[1:]:
            ev = esd.expression(esd.conds[ci]['eval'])
            if len(ev) >= 4 and ev[0] == F.EZ_GET_REG0 and ev[-2:] == bytes([F.EZ_EQ, F.EZ_END]):
                targets[F.ez_eval(ev[1:-2] + bytes([F.EZ_END]))] = esd.conds[ci]['target']
        for index, msg, flag in entries:
            target = targets.get(index)
            call = None
            if target is not None:
                for ci in esd.commands(esd.states[target]['entry']):
                    if esd.cmds[ci][0] == 6:
                        call = (esd.cmds[ci][1], [F.ez_eval(a) for a in esd.cmd_args(ci)])
            if not call or len(call[1]) != 3:
                raise ValueError(f'no destination for list entry {index}')
            self.check_group = call[0]
            area, lamp, unusable = call[1]
            if msg != 70000000 + area * 10 + lamp:
                raise ValueError(f'list entry {index} names another lamp')
            self.lamps.append((index, msg, flag, area, lamp, unusable))


def copy_group(src: F.Esd, gid, dst: F.Esd, new_gid):
    """Copy a state group (states, conditions, commands, expressions) into another script."""
    states = src.group_states(gid)
    pending = []

    def cond(ci):
        c = src.conds[ci]
        subs = [cond(s) for s in src.cond_subs(ci)]
        passes = [(src.cmds[k][0], src.cmds[k][1], src.cmd_args(k)) for k in src.commands(c['pass'])]
        new = dst.add_cond(src.expression(c['eval']), passes=passes, subs=subs)
        if c['target'] is not None:
            pending.append((new, src.states[c['target']]['id']))
        return new

    built = []
    for si in states:
        st = src.states[si]
        cmds = {k: [(src.cmds[i][0], src.cmds[i][1], src.cmd_args(i)) for i in src.commands(st[k])]
                for k in ('entry', 'while', 'exit')}
        built.append((st['id'], cmds['entry'], cmds['while'], cmds['exit'], [cond(c) for c in src.state_conds(si)]))
    ids = dst.add_group(new_gid, built)
    for new, sid in pending:
        dst.set_target(new, ids[sid])


def add_list_group(esd: F.Esd, gid, headstone: Headstone, items, on_pick):
    """A list menu state group: items [(index, message, flag)], on_pick(index) -> (entry
    commands, kind) where an int kind ends the group with that value and 'call' waits for a
    called group (result 1: back to the list; otherwise return 0). Cancel returns 1."""
    conds_by_state = {}
    pending = []

    def go(evaluator, target_sid, passes=()):
        c = esd.add_cond(evaluator, passes=passes)
        if target_sid is not None:
            pending.append((c, target_sid))
        return c

    def nested(evaluator, target_sid):
        return esd.add_cond(evaluator, subs=[go(TRUE, target_sid)])

    states = [(0, [], [], [], [go(TRUE, 1)]),
              (1, [(1, 20, [])], [], [], [go(TRUE, 2)]),
              (2, [(1, 19, [F.ez(i), F.ez(m), F.ez(f)]) for i, m, f in items], [], [], [go(TRUE, 3)])]
    switch = [go(headstone.cancel, 4)]
    picks = []
    for sid, (index, _m, _f) in enumerate(items, start=10):
        switch.append(go(EV_SELECTED(index), sid))
        entry, kind = on_pick(index)
        if kind == 'call':
            conds = [nested(EV_CALL_RESULT(1), 1), nested(EV_CALL_DONE, 5)]
        else:
            conds = [go(TRUE, None, passes=[ret(kind)])]
        picks.append((sid, entry, conds))
    states.append((3, headstone.show, headstone.wait, [], switch))
    states.append((4, [], [], [], [go(TRUE, None, passes=[ret(1)])]))
    states.append((5, [], [], [], [go(TRUE, None, passes=[ret(0)])]))
    for s, entry, conds in picks:
        states.append((s, entry, [], [], conds))
    ids = esd.add_group(gid, states)
    for c, target in pending:
        esd.set_target(c, ids[target])


def lamp_identity(esd: F.Esd):
    """(map, lamp) of a lamp's talk script, or None (group 0 calls its main group with
    6000, <6001 or name>, 6100, 6101, map, lamp, message)."""
    g = esd.group(0)
    if g is None:
        return None
    for si in esd.group_states(0):
        for ci in esd.commands(esd.states[si]['entry']):
            if esd.cmds[ci][0] == 6:
                vals = [F.ez_eval(a) for a in esd.cmd_args(ci)]
                if len(vals) == 7 and vals[0] == 6000 and vals[2:4] == [6100, 6101]:
                    return vals[4], vals[5]
    return None


def patch_lamp(esd: F.Esd, here, headstones, check_src):
    """Turn the lamp's "Return to the Hunter's Dream?" dialog into the travel lists."""
    for gid, first, count in esd.groups:
        for si in range(first, first + count):
            for ci in esd.commands(esd.states[si]['entry']):
                if esd.cmds[ci][:2] == [1, 17] and DREAM_QUESTION in [F.ez_eval(a) for a in esd.cmd_args(ci)]:
                    dialog = si
                    break
            else:
                continue
            break
        else:
            continue
        break
    else:
        raise ValueError('no "Return to the Hunter\'s Dream?" dialog')
    ok = cancel = None
    for c in esd.state_conds(dialog):
        ev = esd.expression(esd.conds[c]['eval'])
        if ev[:2] == bytes([F.ez_int(22)[0], 0x84]):
            ok = esd.conds[c]['target']
        elif ev[:2] == bytes([F.ez_int(21)[0], 0x84]):
            cancel = esd.conds[c]['target']
    if ok is None or cancel is None:
        raise ValueError('unexpected dialog conditions')
    wait = [(esd.cmds[i][0], esd.cmds[i][1], esd.cmd_args(i)) for i in esd.commands(esd.states[dialog]['while'])]

    copy_group(check_src, headstones[0].check_group, esd, CHECK_GROUP)
    regions = []
    for k, hs in enumerate(headstones):
        lamps = [lp for lp in hs.lamps if (lp[3], lp[4]) != here]
        if not lamps:
            continue
        gid = REGION_GROUP + k
        items = [(i + 1, msg, flag) for i, (_, msg, flag, _, _, _) in enumerate(lamps)]
        by_index = {i + 1: lp for i, lp in enumerate(lamps)}
        add_list_group(esd, gid, hs, items,
                       lambda n, by=by_index: ([(6, CHECK_GROUP, [F.ez(by[n][3]), F.ez(by[n][4]), F.ez(by[n][5])])], 'call'))
        regions.append((hs.name_msg, hs.flag, gid))
    top = [(1, DREAM_NAME, -1)] + [(k + 2, msg, flag) for k, (msg, flag, _) in enumerate(regions)]
    calls = {k + 2: gid for k, (_, _, gid) in enumerate(regions)}
    add_list_group(esd, TOP_GROUP, headstones[0], top,
                   lambda n: ([], 2) if n == 1 else ([(6, calls[n], [])], 'call'))

    # Result 1: cancelled; 2: Hunter's Dream (the game's own travel); otherwise a lamp was picked
    # and its warp flag set (the event script warps): the dialog ends as when cancelled.
    def nested(evaluator, target):
        return esd.add_cond(evaluator, subs=[esd.add_cond(TRUE, target=target)])
    esd.replace_state(dialog, entry=[(6, TOP_GROUP, [])], while_=wait,
                      conds=[nested(EV_CALL_RESULT(1), cancel), nested(EV_CALL_RESULT(2), ok),
                             nested(EV_CALL_DONE, cancel)])


def destinations(dream_events: F.Emevd):
    """[(warp flag, region)] from the Dream's initializations of its warp handler."""
    out = []
    for bank, iid, args in dream_events.event_ins(0):
        if (bank, iid) == (2000, 0) and len(args) == 20:
            slot, event, flag, _headstone, region = struct.unpack('<5i', args)
            if event == WARP_EVENT:
                out.append((flag, region))
    if not out:
        raise ValueError('no lamp warps in the Hunter\'s Dream event script')
    return out


def patch_common(common: F.Emevd, warps):
    template = common.event_ins(7200)  # the game's own lamp travel: multiplayer end, flag wait/clear
    if len(template) < 3 or [t[:2] for t in template[:3]] != [(1003, 6), (3, 0), (2003, 2)]:
        raise ValueError('unexpected lamp travel event 7200')
    common.add_event(HANDLER_EVENT, [
        template[0],                                        # end in co-op
        (1003, 8, bytes([0, 1, 21, 0])),                   # end in the Hunter's Dream (m21_00)
        template[1],                                        # wait for the warp flag
        template[2],                                        # clear it
        (2003, 18, struct.pack('<ii', PLAYER, TRAVEL_ANIMATION) + bytes(4)),
        (1001, 0, struct.pack('<f', 4.0)),
        (2003, 49, bytes(4)),                               # respawn point warp
    ], params=[(2, 4, 0, 4), (3, 0, 0, 4), (6, 0, 4, 4)])
    common.append_to_event(0, [(2000, 0, struct.pack('<4i', slot, HANDLER_EVENT, flag, region))
                               for slot, (flag, region) in enumerate(warps)])


def generate(source, out: Path):
    """source(relative path) -> bytes of the file in effect (mods included). Writes the layer
    under out/dvdroot_ps4 and returns the number of lamps changed."""
    dream_talk = F.Bnd4(F.Dcx(source(DREAM_TALK)).data)
    headstones, check_src = [], None
    for name, msg, flag in HEADSTONES:
        i = dream_talk.find(name)
        if i is None:
            continue
        esd = F.Esd(dream_talk.files[i][1])
        headstones.append(Headstone(esd, msg, flag))
        if check_src is None:
            check_src = esd
    if not headstones:
        raise ValueError('no headstones in the Hunter\'s Dream talk scripts')
    warps = destinations(F.Emevd(F.Dcx(source(DREAM_EVENTS)).data))

    root = out / 'dvdroot_ps4'
    lamps = 0
    for talk in source.talk_archives():
        if talk == DREAM_TALK:
            continue
        dcx = F.Dcx(source(talk))
        bnd = F.Bnd4(dcx.data)
        changed = False
        for entry in bnd.files:
            esd = F.Esd(entry[1])
            here = lamp_identity(esd)
            if here is None:
                continue
            patch_lamp(esd, here, headstones, check_src)
            entry[1] = esd.write()
            changed = True
            lamps += 1
        if changed:
            (root / talk).parent.mkdir(parents=True, exist_ok=True)
            (root / talk).write_bytes(dcx.write(bnd.write()))
    common_dcx = F.Dcx(source(COMMON_EVENTS))
    common = F.Emevd(common_dcx.data)
    patch_common(common, warps)
    (root / COMMON_EVENTS).parent.mkdir(parents=True, exist_ok=True)
    (root / COMMON_EVENTS).write_bytes(common_dcx.write(common.write()))
    return lamps


class GameFiles:
    """Files of the game folder with optional replacements {relative path: source path}."""

    def __init__(self, game: Path, replaced=None):
        self.game = Path(game)
        self.replaced = {k.casefold(): Path(v) for k, v in (replaced or {}).items()}

    def __call__(self, relative):
        key = ('dvdroot_ps4/' + relative).casefold()
        if key in self.replaced:
            return self.replaced[key].read_bytes()
        return (self.game / 'dvdroot_ps4' / relative).read_bytes()

    def talk_archives(self):
        names = {p.name for p in (self.game / 'dvdroot_ps4/script/talk').glob('*.talkesdbnd.dcx')}
        names |= {k.split('/')[-1] for k in self.replaced if k.startswith('dvdroot_ps4/script/talk/')
                  and k.endswith('.talkesdbnd.dcx')}
        return sorted(f'script/talk/{n}' for n in names)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(f'Lamp travel: {generate(GameFiles(args.game), args.out)} lamps')


if __name__ == '__main__':
    main()
