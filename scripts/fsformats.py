# SPDX-License-Identifier: GPL-2.0-or-later
"""Readers and writers for the FromSoftware containers the QoL generators edit (Bloodborne 1.09):
DCX (DFLT), BND4, EzState talk scripts (fsSL, 64-bit) and event scripts (EVD, 64-bit).

The writers keep every original table entry where it was and append new entries at the end of
each table, relocating the offsets that point past them, so an unmodified file is written back
byte for byte (tests/test_fsformats.py checks that on the game's files when they are available).
"""
import struct
import zlib

# --- DCX ------------------------------------------------------------------------------------------


class Dcx:
    """A DCX (DFLT) file: the original header is kept; sizes follow the new contents."""

    def __init__(self, raw: bytes):
        if raw[:4] != b'DCX\0':
            raise ValueError('not a DCX file')
        dcs = raw.index(b'DCS\0')
        self.size, compressed = struct.unpack_from('>II', raw, dcs + 4)
        dcp = raw.index(b'DCP\0')
        if raw[dcp + 4:dcp + 8] != b'DFLT':
            raise ValueError(f'unsupported DCX compression {raw[dcp + 4:dcp + 8]!r}')
        dca = raw.index(b'DCA\0')
        start = dca + struct.unpack_from('>I', raw, dca + 4)[0]
        self.header = raw[:start]
        self.dcs = dcs
        self.data = zlib.decompress(raw[start:start + compressed])
        if len(self.data) != self.size:
            raise ValueError('DCX size mismatch')

    def write(self, data: bytes = None) -> bytes:
        data = self.data if data is None else data
        packed = zlib.compress(data, 9)
        header = bytearray(self.header)
        struct.pack_into('>II', header, self.dcs + 4, len(data), len(packed))
        return bytes(header) + packed


# --- BND4 -----------------------------------------------------------------------------------------


def _reverse_bits(value):
    return int(f'{value:08b}'[::-1], 2)


class Bnd4:
    """A BND4 archive with 64-bit entries; names, ids and flags are kept, data is replaceable."""

    def __init__(self, raw: bytes):
        if raw[:4] != b'BND4':
            raise ValueError('not a BND4 archive')
        if raw[9] != 0:
            raise ValueError('big-endian BND4 is unsupported')
        bit_big_endian = not raw[10]
        fmt = raw[0x31]
        if not (bit_big_endian or ((fmt & 1) and not fmt & 0x80)):
            fmt = _reverse_bits(fmt)
        self.format = fmt
        self.unicode = raw[0x30] == 1
        count = struct.unpack_from('<i', raw, 0x0C)[0]
        self.entry_size = struct.unpack_from('<q', raw, 0x20)[0]
        self.data_start = struct.unpack_from('<q', raw, 0x28)[0]
        if fmt & 0x10 or not fmt & 0x20 or self.entry_size != 0x24:
            raise ValueError(f'unsupported BND4 layout (format {fmt:#x}, entries {self.entry_size:#x})')
        self.raw = raw
        self.files = []  # [name, data]
        for i in range(count):
            off = 0x40 + i * self.entry_size
            size, usize, data_off = struct.unpack_from('<qqi', raw, off + 8)
            name = None
            if fmt & 0x0C:
                name_off = struct.unpack_from('<i', raw, off + 0x20)[0]
                end = name_off
                while raw[end:end + 2] != b'\0\0':
                    end += 2
                name = raw[name_off:end].decode('utf-16-le' if self.unicode else 'shift_jis')
            self.files.append([name, raw[data_off:data_off + size]])

    def names(self):
        return [name for name, _ in self.files]

    def find(self, suffix: str):
        """Index of the file whose name ends in `suffix` (a backslash-separated path tail)."""
        for i, (name, _) in enumerate(self.files):
            if name and name.replace('\\', '/').split('/')[-1].lower() == suffix.lower():
                return i
        return None

    def write(self) -> bytes:
        out = bytearray(self.raw[:self.data_start])
        for i, (_, data) in enumerate(self.files):
            while len(out) % 16:
                out.append(0)
            entry = 0x40 + i * self.entry_size
            struct.pack_into('<qqi', out, entry + 8, len(data), len(data), len(out))
            out += data
        return bytes(out)


# --- FMG text tables ------------------------------------------------------------------------------


def fmg_texts(raw: bytes) -> dict:
    """{id: text} of an FMG (version 2: 64-bit string offsets, UTF-16)."""
    if raw[2] != 2:
        raise ValueError('unsupported FMG version')
    groups = struct.unpack_from('<i', raw, 0x0C)[0]
    offsets = struct.unpack_from('<q', raw, 0x18)[0]
    texts = {}
    for g in range(groups):
        first_index, first_id, last_id = struct.unpack_from('<iii', raw, 0x28 + g * 16)
        for k in range(last_id - first_id + 1):
            start = struct.unpack_from('<q', raw, offsets + (first_index + k) * 8)[0]
            if start:
                end = start
                while raw[end:end + 2] != b'\0\0':
                    end += 2
                texts[first_id + k] = raw[start:end].decode('utf-16-le')
    return texts


# --- EzState talk scripts (fsSL) ------------------------------------------------------------------

ESD_DATA = 0x6C
GROUP_SIZE, STATE_SIZE, COND_SIZE, CMD_SIZE, ARG_SIZE = 0x20, 0x48, 0x38, 0x18, 0x10


class Esd:
    """An fsSL talk script as tables of records that refer to each other by index.

    groups:   [id, first state index, state count]; each group's states are followed by a copy
              of its first state (the game's layout), regenerated on writing
    states:   {'id', 'conds': (condoffs index, n), 'entry'/'exit'/'while': (cmd index, n)}
    conds:    {'target': state index or None, 'pass': (cmd index, n), 'subs': (condoffs index, n),
               'eval': expression bytes index or None}
    cmds:     [bank, id, (arg index, n)]
    args:     [expression bytes index]
    condoffs: [cond index]
    exprs:    expression byte strings by (offset into the blob) -> kept in blob order
    """

    def __init__(self, raw: bytes):
        if raw[:4] != b'fsSL':
            raise ValueError('not an fsSL talk script')
        self.head = bytearray(raw[:ESD_DATA])
        h = struct.unpack_from('<26i', raw, 4)
        (_, _, _, _, data_size, _, sizes48, _, gsize, n_groups, ssize, n_states, csize, n_conds,
         msize, n_cmds, asize, n_args, condoffs_off, n_condoffs, name_block, name_len, *_rest) = h
        if (gsize, ssize, csize, msize, asize) != (GROUP_SIZE, STATE_SIZE, COND_SIZE, CMD_SIZE, ARG_SIZE):
            raise ValueError('unsupported fsSL record sizes')
        data = raw[ESD_DATA:ESD_DATA + data_size]
        self.data_head = bytearray(data[:0x48])
        groups_off, n_groups2, name_off, name_chars = struct.unpack_from('<qqqq', data, 0x18)
        if groups_off != (0x48 if n_groups else -1) or n_groups2 != n_groups:
            raise ValueError('unexpected fsSL group table')
        groups_off = 0x48
        S = groups_off + n_groups * GROUP_SIZE
        C = S + n_states * STATE_SIZE
        M = C + n_conds * COND_SIZE
        A = M + n_cmds * CMD_SIZE
        O = A + n_args * ARG_SIZE
        E = O + n_condoffs * 8
        if O != condoffs_off:
            raise ValueError('unexpected fsSL table layout')
        self.blob = data[E:name_block]
        self.name = data[name_off:name_off + name_chars * 2]
        self.name_gap = data[name_block:name_off]

        def index(off, base, size, count, what):
            if (off - base) % size or not 0 <= (off - base) // size < count:
                raise ValueError(f'{what} offset {off:#x} outside its table')
            return (off - base) // size

        def array(off, n, base, size, count, what):
            if n == 0:
                return (None, 0) if off == -1 else ('raw', off)
            return (index(off, base, size, count, what), n)

        def expr(off, length):
            if length == 0:
                return (None, 0) if off == -1 else ('raw', off)
            if not 0 <= off - E and off - E + length <= len(self.blob):
                raise ValueError('expression outside the blob')
            return (off - E, length)

        self.groups = []
        for i in range(n_groups):
            gid, s_off, s_count, s_off2 = struct.unpack_from('<4q', data, groups_off + i * GROUP_SIZE)
            if s_off != s_off2:
                raise ValueError('group initial state is not its first state')
            self.groups.append([gid, index(s_off, S, STATE_SIZE, n_states, 'state'), s_count])
        self.states = []
        for i in range(n_states):
            sid, c_off, c_n, e_off, e_n, x_off, x_n, w_off, w_n = struct.unpack_from('<9q', data, S + i * STATE_SIZE)
            self.states.append({'id': sid,
                                'conds': array(c_off, c_n, O, 8, n_condoffs, 'condition list'),
                                'entry': array(e_off, e_n, M, CMD_SIZE, n_cmds, 'commands'),
                                'exit': array(x_off, x_n, M, CMD_SIZE, n_cmds, 'commands'),
                                'while': array(w_off, w_n, M, CMD_SIZE, n_cmds, 'commands')})
        self.conds = []
        for i in range(n_conds):
            t, p_off, p_n, s_off, s_n, e_off, e_len = struct.unpack_from('<7q', data, C + i * COND_SIZE)
            self.conds.append({'target': None if t == -1 else index(t, S, STATE_SIZE, n_states, 'target'),
                               'pass': array(p_off, p_n, M, CMD_SIZE, n_cmds, 'commands'),
                               'subs': array(s_off, s_n, O, 8, n_condoffs, 'condition list'),
                               'eval': expr(e_off, e_len)})
        self.cmds = []
        for i in range(n_cmds):
            bank, cid, a_off, a_n = struct.unpack_from('<iiqq', data, M + i * CMD_SIZE)
            self.cmds.append([bank, cid, array(a_off, a_n, A, ARG_SIZE, n_args, 'arguments')])
        self.args = []
        for i in range(n_args):
            e_off, e_len = struct.unpack_from('<qq', data, A + i * ARG_SIZE)
            self.args.append(expr(e_off, e_len))
        self.condoffs = [index(struct.unpack_from('<q', data, O + i * 8)[0], C, COND_SIZE, n_conds, 'condition')
                         for i in range(n_condoffs)]
        # Group state lists are contiguous, each followed by its copy of the first state.
        for gid, first, count in self.groups:
            copy = first + count
            if copy >= n_states or self.states[copy] != self.states[first]:
                raise ValueError(f'group {gid:#x} has no copy of its first state')

    # -- reading helpers --
    def group(self, gid):
        for g in self.groups:
            if g[0] == gid:
                return g
        return None

    def group_states(self, gid):
        g = self.group(gid)
        return [] if g is None else list(range(g[1], g[1] + g[2]))

    def expression(self, ref):
        off, length = ref
        return b'' if off is None else self.blob[off:off + length]

    def commands(self, ref):
        start, n = ref
        return [] if start is None else list(range(start, start + n))

    def cmd_args(self, cmd_index):
        start, n = self.cmds[cmd_index][2]
        return [] if start is None else [self.expression(self.args[i]) for i in range(start, start + n)]

    def state_conds(self, state_index):
        start, n = self.states[state_index]['conds']
        return [] if start is None else [self.condoffs[i] for i in range(start, start + n)]

    def cond_subs(self, cond_index):
        start, n = self.conds[cond_index]['subs']
        return [] if start is None else [self.condoffs[i] for i in range(start, start + n)]

    # -- building --
    def add_expr(self, code: bytes):
        off = len(self.blob)
        self.blob += code
        return (off, len(code))

    def add_cmds(self, cmds):
        """[(bank, id, [expression bytes])] -> (cmd index, n)."""
        if not cmds:
            return (None, 0)
        start = len(self.cmds)
        for bank, cid, args in cmds:
            if args:
                a = len(self.args)
                for code in args:
                    self.args.append(self.add_expr(code))
                ref = (a, len(args))
            else:
                ref = (None, 0)
            self.cmds.append([bank, cid, ref])
        return (start, len(cmds))

    def add_cond(self, evaluator: bytes, target=None, passes=(), subs=()):
        """A condition; `target` is a state index (or None), `subs` condition indices."""
        index = len(self.conds)
        self.conds.append({'target': target, 'pass': self.add_cmds(list(passes)),
                           'subs': self.add_condoffs(list(subs)),
                           'eval': self.add_expr(evaluator) if evaluator else (None, 0)})
        return index

    def add_condoffs(self, conds):
        if not conds:
            return (None, 0)
        start = len(self.condoffs)
        self.condoffs.extend(conds)
        return (start, len(conds))

    def add_group(self, gid, states):
        """states: [(state id, entry, while, exit, [cond index])] in order; state 0 first.

        Returns {state id: state index}; condition targets may be patched afterwards with
        set_target (they are usually created before their states exist)."""
        if self.group(gid) is not None:
            raise ValueError(f'group {gid:#x} exists')
        first = len(self.states)
        indices = {}
        for sid, entry, while_, exit_, conds in states:
            indices[sid] = len(self.states)
            self.states.append({'id': sid, 'conds': self.add_condoffs(list(conds)),
                                'entry': self.add_cmds(entry), 'exit': self.add_cmds(exit_),
                                'while': self.add_cmds(while_)})
        self.states.append(dict(self.states[first]))
        self.groups.append([gid, first, len(states)])
        return indices

    def set_target(self, cond_index, state_index):
        self.conds[cond_index]['target'] = state_index

    def replace_state(self, state_index, entry=None, while_=None, exit_=None, conds=None):
        """Point an existing state at new commands/conditions (its old records stay unused)."""
        st = self.states[state_index]
        if entry is not None:
            st['entry'] = self.add_cmds(entry)
        if while_ is not None:
            st['while'] = self.add_cmds(while_)
        if exit_ is not None:
            st['exit'] = self.add_cmds(exit_)
        if conds is not None:
            st['conds'] = self.add_condoffs(list(conds))
        # A group's first state has a copy that must match it.
        for gid, first, count in self.groups:
            if first == state_index:
                self.states[first + count] = dict(st)

    # -- writing --
    def write(self) -> bytes:
        n_groups, n_states, n_conds = len(self.groups), len(self.states), len(self.conds)
        n_cmds, n_args, n_condoffs = len(self.cmds), len(self.args), len(self.condoffs)
        S = 0x48 + n_groups * GROUP_SIZE
        C = S + n_states * STATE_SIZE
        M = C + n_conds * COND_SIZE
        A = M + n_cmds * CMD_SIZE
        O = A + n_args * ARG_SIZE
        E = O + n_condoffs * 8
        name_block = E + len(self.blob)
        # The UTF-16 name starts at an even offset; the original padding is kept when it fits.
        pad = (-name_block) % 2
        gap = self.name_gap if len(self.name_gap) == pad else b'\0' * pad
        name_off = name_block + len(gap)
        data_size = name_off + len(self.name)

        def arr(ref, base, size):
            start, n = ref
            if start is None:
                return (-1, 0)
            if start == 'raw':
                return (n, 0)
            return (base + start * size, n)

        def ex(ref):
            off, length = ref
            if off is None:
                return (-1, 0)
            if off == 'raw':
                return (length, 0)
            return (E + off, length)

        data = bytearray(self.data_head)
        struct.pack_into('<qqqq', data, 0x18, 0x48 if n_groups else -1, n_groups, name_off,
                         len(self.name) // 2)
        for gid, first, count in self.groups:
            data += struct.pack('<4q', gid, S + first * STATE_SIZE, count, S + first * STATE_SIZE)
        for st in self.states:
            data += struct.pack('<9q', st['id'], *arr(st['conds'], O, 8), *arr(st['entry'], M, CMD_SIZE),
                                *arr(st['exit'], M, CMD_SIZE), *arr(st['while'], M, CMD_SIZE))
        for c in self.conds:
            target = -1 if c['target'] is None else S + c['target'] * STATE_SIZE
            data += struct.pack('<7q', target, *arr(c['pass'], M, CMD_SIZE), *arr(c['subs'], O, 8),
                                *ex(c['eval']))
        for bank, cid, ref in self.cmds:
            data += struct.pack('<ii', bank, cid) + struct.pack('<qq', *arr(ref, A, ARG_SIZE))
        for ref in self.args:
            data += struct.pack('<qq', *ex(ref))
        for c in self.condoffs:
            data += struct.pack('<q', C + c * COND_SIZE)
        data += self.blob + gap + self.name
        assert len(data) == data_size
        head = bytearray(self.head)
        struct.pack_into('<i', head, 0x14, data_size)
        struct.pack_into('<i', head, 0x28, n_groups)
        struct.pack_into('<i', head, 0x30, n_states)
        struct.pack_into('<i', head, 0x38, n_conds)
        struct.pack_into('<i', head, 0x40, n_cmds)
        struct.pack_into('<i', head, 0x48, n_args)
        struct.pack_into('<ii', head, 0x4C, O, n_condoffs)
        struct.pack_into('<i', head, 0x54, name_block)
        struct.pack_into('<i', head, 0x5C, data_size)
        struct.pack_into('<i', head, 0x64, data_size)
        return bytes(head) + bytes(data)


# EzState expression bytecode (Bloodborne): small integers are one byte (value + 64), others
# 0x82 + int32; a function call pushes its id and arguments, then 0x84 + argument count.
EZ_END, EZ_EQ, EZ_NE, EZ_OR, EZ_AND = 0xA1, 0x95, 0x96, 0x99, 0x98
EZ_ADD, EZ_MUL = 0x8C, 0x8F
EZ_SET_REG0, EZ_GET_REG0, EZ_ARG, EZ_CALL_RESULT, EZ_CALL_RUNNING = 0xA7, 0xAF, 0xB8, 0xB9, 0xBA
EZ_CONTINUE = 0xA6


def ez_int(value: int) -> bytes:
    if -64 <= value <= 63:
        return bytes([value + 64])
    return b'\x82' + struct.pack('<i', value)


def ez(*parts) -> bytes:
    """Concatenate expression pieces (ints become literals, bytes are kept) and end it."""
    out = bytearray()
    for part in parts:
        out += ez_int(part) if isinstance(part, int) else part
    return bytes(out) + bytes([EZ_END])


def ez_call(function: int, *args) -> bytes:
    out = bytearray(ez_int(function))
    for a in args:
        out += ez_int(a) if isinstance(a, int) else a
    return bytes(out) + bytes([0x84 + len(args)])


def ez_eval(code: bytes, state_args=()):
    """Value of a constant expression (literals, + - * /, state group arguments), else None."""
    stack = []
    i = 0
    while i < len(code):
        op = code[i]
        i += 1
        if op < 0x80:
            stack.append(op - 64)
        elif op == 0x82:
            stack.append(struct.unpack_from('<i', code, i)[0])
            i += 4
        elif op in (0x8C, 0x8D, 0x8E, 0x8F):
            b, a = stack.pop(), stack.pop()
            stack.append({0x8C: a + b, 0x8D: a - b, 0x8E: a // b if b else 0, 0x8F: a * b}[op])
        elif op == EZ_ARG:
            idx = stack.pop()
            if idx >= len(state_args):
                return None
            stack.append(state_args[idx])
        elif op == EZ_END:
            break
        else:
            return None
    return stack[-1] if len(stack) == 1 else None


# --- event scripts (EVD) --------------------------------------------------------------------------

EVD_HEADER, EVENT_SIZE, INS_SIZE, PARAM_SIZE = 0x90, 0x30, 0x20, 0x20


class Emevd:
    """An EVD event script: events own contiguous instruction runs and parameter runs."""

    def __init__(self, raw: bytes):
        if raw[:4] != b'EVD\0' or raw[5] != 0xFF:
            raise ValueError('not a 64-bit EVD event script')
        self.head = bytearray(raw[:EVD_HEADER])
        (n_events, ev_off, n_ins, ins_off, _, unk_off, n_layers, lay_off, n_params, par_off,
         n_links, link_off, arg_len, arg_off, str_len, str_off) = struct.unpack_from('<16q', raw, 0x10)
        if n_layers:
            raise ValueError('event scripts with instruction layers are unsupported')
        if ev_off != EVD_HEADER or ins_off != ev_off + n_events * EVENT_SIZE or arg_off != ins_off + n_ins * INS_SIZE \
                or par_off != arg_off + arg_len or link_off != par_off + n_params * PARAM_SIZE \
                or str_off != link_off + n_links * 8 or unk_off != arg_off or lay_off != arg_off:
            raise ValueError('unexpected EVD table layout')
        self.events = []  # [id, ins start, ins n, param start or None, param n, rest, pad]
        for i in range(n_events):
            eid, n, i_off, pn, p_off, rest, pad = struct.unpack_from('<5qii', raw, ev_off + i * EVENT_SIZE)
            if i_off % INS_SIZE or (pn and p_off % PARAM_SIZE):
                raise ValueError('misaligned event tables')
            self.events.append([eid, i_off // INS_SIZE, n, p_off // PARAM_SIZE if pn else p_off, pn, rest, pad])
        self.ins = []  # [bank, id, args bytes offset, args length, layer offset]
        for i in range(n_ins):
            bank, iid, length, off, layer = struct.unpack_from('<iiqqq', raw, ins_off + i * INS_SIZE)
            self.ins.append([bank, iid, off, length, layer])
        self.args = bytearray(raw[arg_off:arg_off + arg_len])
        self.args_added = False
        self.params = [raw[par_off + i * PARAM_SIZE:par_off + (i + 1) * PARAM_SIZE] for i in range(n_params)]
        self.links = raw[link_off:link_off + n_links * 8]
        self.strings = raw[str_off:str_off + str_len]
        self.tail = raw[str_off + str_len:]

    def event(self, eid):
        for e in self.events:
            if e[0] == eid:
                return e
        return None

    def event_ins(self, eid):
        e = self.event(eid)
        return [] if e is None else [(self.ins[i][0], self.ins[i][1], self.ins_args(i))
                                     for i in range(e[1], e[1] + e[2])]

    def ins_args(self, i):
        _, _, off, length, _ = self.ins[i]
        return bytes(self.args[off:off + length])

    def add_args(self, data: bytes):
        while len(self.args) % 4:
            self.args.append(0)
        off = len(self.args)
        self.args += data
        self.args_added = True
        return off

    def append_to_event(self, eid, instructions):
        """Add [(bank, id, args bytes)] at the end of an existing event."""
        e = self.event(eid)
        if e is None:
            raise ValueError(f'no event {eid}')
        at = e[1] + e[2]
        new = [[bank, iid, self.add_args(args), len(args), -1] for bank, iid, args in instructions]
        self.ins[at:at] = new
        for other in self.events:
            if other is not e and other[1] >= at and other[2]:
                other[1] += len(new)
        e[2] += len(new)

    def add_event(self, eid, instructions, params=(), rest=0):
        """A new event: [(bank, id, args bytes)], params [(instruction index, target byte,
        source byte, byte count)]."""
        if self.event(eid) is not None:
            raise ValueError(f'event {eid} exists')
        start = len(self.ins)
        for bank, iid, args in instructions:
            self.ins.append([bank, iid, self.add_args(args), len(args), -1])
        p_start = len(self.params) if params else -1
        for index, target, source, count in params:
            self.params.append(struct.pack('<qqqii', index, target, source, count, 0))
        self.events.append([eid, start, len(instructions), p_start, len(params), rest, 0])

    def write(self) -> bytes:
        # The game's files keep the argument block 16-byte aligned; added arguments do too.
        while self.args_added and len(self.args) % 16:
            self.args.append(0)
        n_events, n_ins, n_params = len(self.events), len(self.ins), len(self.params)
        ev_off = EVD_HEADER
        ins_off = ev_off + n_events * EVENT_SIZE
        arg_off = ins_off + n_ins * INS_SIZE
        par_off = arg_off + len(self.args)
        link_off = par_off + n_params * PARAM_SIZE
        str_off = link_off + len(self.links)
        size = str_off + len(self.strings) + len(self.tail)
        head = bytearray(self.head)
        struct.pack_into('<i', head, 0x0C, size)
        struct.pack_into('<16q', head, 0x10, n_events, ev_off, n_ins, ins_off, 0, arg_off, 0, arg_off,
                         n_params, par_off, len(self.links) // 8, link_off, len(self.args), arg_off,
                         len(self.strings), str_off)
        out = bytearray(head)
        for eid, start, n, p_start, pn, rest, pad in self.events:
            p = p_start * PARAM_SIZE if pn else p_start
            out += struct.pack('<5qii', eid, n, start * INS_SIZE, pn, p, rest, pad)
        for bank, iid, off, length, layer in self.ins:
            out += struct.pack('<iiqqq', bank, iid, length, off, layer)
        out += self.args
        for p in self.params:
            out += p
        out += self.links + self.strings + self.tail
        assert len(out) == size
        return bytes(out)
