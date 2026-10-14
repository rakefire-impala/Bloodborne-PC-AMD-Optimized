# SPDX-License-Identifier: GPL-2.0-or-later
"""Lamp-to-lamp travel (launcher: Quality of Life). Generated from the player's own game files.

At a lamp, "Return to the Hunter's Dream?" opens the port's travel menu (gpu/shim/bbport_lamp_menu):
a tab per headstone of the Hunter's Dream with that headstone's lamps, "???" for lamps not lit yet,
and the Hunter's Dream on Square. The menu's texts are the game's own, in the game's language.

How the game does it: a headstone's talk script (script/talk/m21_00_00_00, t210000-t210003 and
t210721) lists lamps with c1_19(index, name 7000MMMI, lit flag) and, once a lamp is picked, calls a
check that shows a message if the lamp is unusable and otherwise sets the warp request flag
7210MMMI; the Hunter's Dream event script (event 12107000, one instance per lamp) waits for that
flag and warps with "respawn point warp" (2003[49]) to the lamp's region. The destination's arrival
event (common 7300) clears the flag.

What this adds, in a mod layer that replaces whole files:
- each lamp's talk script (script/talk/<map>.talkesdbnd.dcx): the dialog calls a new state group
  that asks the port for a choice through event flags (MENU_* below) and runs the headstone's own
  "lamp usable?" check (copied as is) on the lamp picked;
- the common event script (event/common.emevd.dcx): event 7250, one instance per lamp, which
  outside the Hunter's Dream and outside co-op waits for that lamp's warp flag, plays the lamp
  travel animation and warps (the Dream keeps its own handlers);
- lamp_menu.txt beside dvdroot_ps4: the menu's lamps and texts for the port (BB_LAMP_MENU).
Lamps show the travel dialog only in single player, so co-op never sets these flags.
"""
import argparse
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fsformats as F  # noqa: E402

DREAM_TALK = 'script/talk/m21_00_00_00.talkesdbnd.dcx'
DREAM_EVENTS = 'event/m21_00_00_00.emevd.dcx'
COMMON_EVENTS = 'event/common.emevd.dcx'
MENU_FILE = 'lamp_menu.txt'
# Headstone talk scripts and their names (Yharnam, Frontier, Unseen, Nightmare, Hunter's Nightmare).
HEADSTONES = [('t210000.esd', 70010100), ('t210001.esd', 70010101), ('t210002.esd', 70010102),
              ('t210003.esd', 70010103), ('t210721.esd', 70010104)]
DREAM_NAME = 70002100          # "Hunter's Dream"
DREAM_QUESTION = 10010713      # "Return to the Hunter's Dream?"
WARP_EVENT = 12107000          # the Dream's per-lamp warp handler
HANDLER_EVENT = 7250           # ours, in common.emevd (7210-7299 are unused there)
TRAVEL_ANIMATION = 101160      # what the game's own lamp travel (common event 7200) plays
PLAYER = 10000
TRAVEL_GROUP, CHECK_GROUP = 0x7FFFFF80, 0x7FFFFF8F
# Event flags shared with the port (src/runtime_lamp.c); 6920-6999 are used by no game file.
# The talk script clears them, marks its own lamp and sets MENU_REQUEST; the port shows the menu
# and sets the lamp picked (or MENU_DREAM) and then MENU_DONE.
MENU_REQUEST, MENU_DONE, MENU_DREAM, MENU_LAMPS = 6920, 6921, 6922, 6928
MENU_LAMP_LIMIT = 6984
# Game language (BB_LANGUAGE, the PS4 system language) -> message folder. The overlay's font has
# no CJK glyphs: those languages get the English names.
MESSAGE_FOLDERS = {0: 'engus', 1: 'engus', 2: 'frafr', 3: 'spaes', 4: 'deude', 5: 'itait', 6: 'nldnl',
                   7: 'porpt', 8: 'rusru', 9: 'engus', 10: 'engus', 11: 'engus', 12: 'finfi',
                   13: 'swese', 14: 'dandk', 15: 'norno', 16: 'polpl', 17: 'porbr', 18: 'enggb',
                   19: 'turtr', 20: 'spaar'}
TEXT_TABLES = ('イベントテキスト', 'SP_キーガイド')  # event texts, key guide (menu.msgbnd)
KEY_SELECT, KEY_RETURN = 200000, 110000

TRUE = F.ez(1)
EV_CALL_RESULT = lambda n: bytes([F.EZ_CALL_RESULT]) + F.ez_int(n) + bytes([F.EZ_EQ, F.EZ_END])  # noqa: E731
EV_CALL_DONE = bytes([F.EZ_CALL_RESULT, F.EZ_CALL_RUNNING, F.EZ_NE, F.EZ_END])
EV_FLAG_ON = lambda flag: F.ez(F.ez_call(15, flag), bytes([F.EZ_CONTINUE]), 1, bytes([F.EZ_EQ]))  # noqa: E731


def ret(value):
    return (7, -1, [F.ez(value)])


def set_flag(flag, value):
    return (1, 11, [F.ez(flag), F.ez(value)])


class Headstone:
    """One headstone's lamp list, read from its talk script."""

    def __init__(self, esd: F.Esd, name_msg):
        self.name_msg = name_msg
        self.lamps = []  # (index, name message, lit flag, map, lamp, "not usable" message)
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


def menu_lamps(headstones):
    """[(headstone, [(menu index, lamp entry)])]: every lamp of the Dream's headstones, numbered
    in list order for its MENU_LAMPS flag."""
    out, n = [], 0
    for hs in headstones:
        lamps = []
        for lamp in hs.lamps:
            lamps.append((n, lamp))
            n += 1
        out.append((hs, lamps))
    if MENU_LAMPS + n > MENU_LAMP_LIMIT:
        raise ValueError(f'{n} lamps do not fit the menu flags')
    return out


def add_travel_group(esd: F.Esd, here, tabs):
    """The state group the lamp's dialog calls: asks the port for a choice and returns 1 when
    cancelled, 2 for the Hunter's Dream, 0 once a picked lamp went through the usable check
    (which sets its warp flag, or shows why it cannot be used)."""
    pending = []

    def go(evaluator, target_sid, passes=()):
        c = esd.add_cond(evaluator, passes=passes)
        if target_sid is not None:
            pending.append((c, target_sid))
        return c

    everything = [(n, lamp) for _hs, lamps in tabs for n, lamp in lamps]
    ask = [set_flag(MENU_DONE, 0), set_flag(MENU_DREAM, 0)]
    ask += [set_flag(MENU_LAMPS + n, 0) for n, _ in everything]
    ask += [set_flag(MENU_LAMPS + n, 1) for n, lamp in everything if (lamp[3], lamp[4]) == here]
    ask.append(set_flag(MENU_REQUEST, 1))
    answer = [go(EV_FLAG_ON(MENU_DREAM), None, passes=[set_flag(MENU_DREAM, 0), ret(2)])]
    picks = []
    for n, lamp in everything:
        if (lamp[3], lamp[4]) == here:
            continue
        sid = 10 + n
        answer.append(go(EV_FLAG_ON(MENU_LAMPS + n), sid, passes=[set_flag(MENU_LAMPS + n, 0)]))
        _index, _msg, _flag, area, number, unusable = lamp
        check = [(6, CHECK_GROUP, [F.ez(area), F.ez(number), F.ez(unusable)])]
        done = esd.add_cond(EV_CALL_DONE, subs=[go(TRUE, 3)])
        picks.append((sid, check, [done]))
    answer.append(go(TRUE, None, passes=[ret(1)]))
    states = [(0, [], [], [], [go(TRUE, 1)]),
              (1, ask, [], [], [go(EV_FLAG_ON(MENU_DONE), 2)]),
              (2, [set_flag(MENU_REQUEST, 0), set_flag(MENU_DONE, 0)], [], [], answer),
              (3, [], [], [], [go(TRUE, None, passes=[ret(0)])])]
    states += [(sid, entry, [], [], conds) for sid, entry, conds in picks]
    ids = esd.add_group(TRAVEL_GROUP, states)
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


def patch_lamp(esd: F.Esd, here, tabs, check_src, check_group):
    """Turn the lamp's "Return to the Hunter's Dream?" dialog into the port's travel menu."""
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

    copy_group(check_src, check_group, esd, CHECK_GROUP)
    add_travel_group(esd, here, tabs)

    # Result 1: cancelled; 2: Hunter's Dream (the game's own travel); otherwise a lamp was picked
    # and its warp flag set (the event script warps): the dialog ends as when cancelled.
    def nested(evaluator, target):
        return esd.add_cond(evaluator, subs=[esd.add_cond(TRUE, target=target)])
    esd.replace_state(dialog, entry=[(6, TRAVEL_GROUP, [])], while_=wait,
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
    wait_on = template[1]
    if wait_on[2][:4] != bytes([0, 1, 0, 0]):
        raise ValueError('unexpected lamp travel flag wait')
    common.add_event(HANDLER_EVENT, [
        template[0],                                        # end in co-op
        (1003, 8, bytes([0, 1, 21, 0])),                   # end in the Hunter's Dream (m21_00)
        # A warp leaves its flag on until the destination's arrival event (common 7300) clears
        # it, so a handler starting in that map would warp again: wait for it to be off first.
        (wait_on[0], wait_on[1], bytes([0, 0, 0, 0]) + wait_on[2][4:]),
        wait_on,                                            # wait for the warp flag
        (2003, 18, struct.pack('<ii', PLAYER, TRAVEL_ANIMATION) + bytes(4)),
        (1001, 0, struct.pack('<f', 4.0)),
        (2003, 49, bytes(4)),                               # respawn point warp
    ], params=[(2, 4, 0, 4), (3, 4, 0, 4), (6, 0, 4, 4)])
    common.append_to_event(0, [(2000, 0, struct.pack('<4i', slot, HANDLER_EVENT, flag, region))
                               for slot, (flag, region) in enumerate(warps)])


def menu_texts(source, language):
    """{message id: text} of the event texts and key guide in the game language (English if
    unavailable)."""
    folder = MESSAGE_FOLDERS.get(language, 'engus')
    for name in dict.fromkeys((folder, 'engus')):
        try:
            bnd = F.Bnd4(F.Dcx(source(f'msg/{name}/menu.msgbnd.dcx')).data)
        except OSError:
            continue
        texts = {}
        for path, data in bnd.files:
            if path and path.replace('\\', '/').split('/')[-1].split('.')[0] in TEXT_TABLES:
                texts.update(F.fmg_texts(data))
        if texts:
            return texts
    raise ValueError('no menu texts')


def write_menu(path: Path, tabs, texts):
    """The port's menu: tab lines (headstone name) followed by their lamps (menu flag, lit flag,
    name). Tab separated, UTF-8."""
    def text(msg):
        return texts.get(msg, '').replace('\t', ' ').replace('\n', ' ').strip()
    lines = [f'dream\t{text(DREAM_NAME)}', f'keys\t{text(KEY_SELECT)}\t{text(KEY_RETURN)}']
    for hs, lamps in tabs:
        lines.append(f'headstone\t{text(hs.name_msg)}')
        for n, (_index, msg, lit, _area, _lamp, _unusable) in lamps:
            lines.append(f'lamp\t{MENU_LAMPS + n}\t{lit}\t{text(msg)}')
    lines.append(f'flags\t{MENU_REQUEST}\t{MENU_DONE}\t{MENU_DREAM}')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def generate(source, out: Path, language=1):
    """source(relative path) -> bytes of the file in effect (mods included). Writes the layer
    under out/dvdroot_ps4 and the menu to out/lamp_menu.txt; returns the number of lamps changed."""
    dream_talk = F.Bnd4(F.Dcx(source(DREAM_TALK)).data)
    headstones, check_src, check_group = [], None, None
    for name, msg in HEADSTONES:
        i = dream_talk.find(name)
        if i is None:
            continue
        esd = F.Esd(dream_talk.files[i][1])
        headstones.append(Headstone(esd, msg))
        if check_src is None:
            check_src, check_group = esd, headstones[-1].check_group
    if not headstones:
        raise ValueError('no headstones in the Hunter\'s Dream talk scripts')
    tabs = menu_lamps(headstones)
    warps = destinations(F.Emevd(F.Dcx(source(DREAM_EVENTS)).data))
    texts = menu_texts(source, language)

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
            patch_lamp(esd, here, tabs, check_src, check_group)
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
    write_menu(out / MENU_FILE, tabs, texts)  # last: its presence means the layer is complete
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


def game_language():
    try:
        return int(os.environ.get('BB_LANGUAGE', '1'))
    except ValueError:
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(f'Lamp travel: {generate(GameFiles(args.game), args.out, game_language())} lamps')


if __name__ == '__main__':
    main()
