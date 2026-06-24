#!/usr/bin/env python3
"""Decode a PoP2 level-state blob (POP2_DUMP_STATE) into room maps.

Layout (verified on levels 1/3/4, sessions 16-17):
  tiles  state + (room-1)*60          30 BE words, 10x3 row-major, rooms 1-based
  anims  state + 1920 + (room-1)*120  4 bytes/tile: word state + word 0x000D
                                      gate: high byte of word0 = position
                                      plate: low byte of word0 = event number
  events state + 5760 + ev*10         {+0 room, +2 idx, +4 counter, +6 f6,
                                       +8 next}  next=-3 end, room=-2 empty
  links  state + 8312 + room*8        4 words: left,right,up,down (0/-1 none)
  env    state + 8582                 word: 1 shore, 3 caverns, 5 rooftops

Usage: decode_level.py state.bin [--rooms 1,2,..] [--events] [--graph]
"""
import argparse
import struct
import sys

TILE = {
    0: '...', 1: 'flr', 2: 'spk', 3: 'pil', 4: 'GAT', 5: 'st5', 6: 'st6',
    7: 'tp7', 8: 'bpB', 9: 'bpT', 10: 'pot', 11: 'lse', 12: 'c12', 13: 'c13',
    14: 'deb', 15: 'c15', 16: 'dr1', 17: 'dr2', 18: 'c18', 19: 'trc',
    20: 'WAL', 21: 'c21', 22: 'c22', 23: 'bc1', 24: 'bc2', 25: 'c25',
    30: 'c30', 33: 'p33', 34: 'PLT', 49: 'win',
}
# 5/6 are buttons/pressure-plates (classic PoP "raise"/"drop" buttons) -- the
# prince stands on them, so they are walkable surfaces for traversal/gap analysis.
# bpT/bpB (9/8) are the top and bottom halves of a 2-tile-tall block: the prince
# stands at the bpB (lower) level, NOT on bpT -- confirmed in-game (walking off a
# floor onto a bp column climbs DOWN to the bpB row). So bpB(8) is walkable and
# bpT(9) is not (it is the block's upper body, a non-standing surface).
WALKABLE = {1, 3, 4, 5, 6, 8, 10, 11, 14, 16, 17, 19, 23, 24, 33, 34, 49}


def w16(buf, off, signed=False):
    if off + 2 > len(buf):
        return 0
    v = struct.unpack_from('>h' if signed else '>H', buf, off)[0]
    return v


def tile_at(buf, room, col, row):
    return w16(buf, (room - 1) * 60 + (row * 10 + col) * 2)


def anim_at(buf, room, col, row):
    return w16(buf, 1920 + (room - 1) * 120 + (row * 10 + col) * 4)


def links_of(buf, room):
    base = 8312 + room * 8
    return [w16(buf, base + i * 2, signed=True) for i in range(4)]


def room_nonempty(buf, room):
    return any(w16(buf, (room - 1) * 60 + i * 2) for i in range(30))


def event(buf, ev):
    base = 5760 + ev * 10
    return (w16(buf, base, signed=True), w16(buf, base + 2, signed=True),
            w16(buf, base + 4, signed=True), w16(buf, base + 6, signed=True),
            w16(buf, base + 8, signed=True))


def fmt_tile(buf, room, col, row):
    t = tile_at(buf, room, col, row)
    code, mod = t & 0xFF, t >> 8
    name = TILE.get(code, '%3d' % code if code < 100 else '%03x' % code)
    return name, code, mod


def npcs_of(buf, room):
    """NPC room table: state+8422+room*192 = word count + 5x 38-byte records.
    Record: +0 tile idx (row*10+col), +2 x, +4 facing, +8 frame."""
    base = 8422 + room * 192
    cnt = w16(buf, base)
    out = []
    for k in range(min(cnt, 5)):
        rec = base + 2 + k * 38
        idx = w16(buf, rec, signed=True)
        out.append((idx % 10, idx // 10, w16(buf, rec + 2, signed=True),
                    w16(buf, rec + 4, signed=True),
                    w16(buf, rec + 8, signed=True)))
    return out


def print_room(buf, room):
    links = links_of(buf, room)
    print(f'-- room {room}  L={links[0]} R={links[1]} U={links[2]} D={links[3]}')
    notes = []
    for col, row, x, facing, frame in npcs_of(buf, room):
        notes.append(f'   NPC   ({col},{row}) x={x} face={facing} frm={frame}')
    for row in range(3):
        cells = []
        for col in range(10):
            name, code, mod = fmt_tile(buf, room, col, row)
            anim = anim_at(buf, room, col, row)
            mark = ' '
            if code in (5, 6, 15, 34):          # switch family: low byte = event
                kind = {5: 'sw5', 6: 'drop?', 15: 'raise', 34: 'plate'}[code]
                notes.append(f'   {kind} ({col},{row}) ev{anim & 0xFF}'
                             f' anim={anim:04x}')
                mark = '*'
            elif code == 4:
                notes.append(f'   gate  ({col},{row}) pos={anim:04x}')
            elif code in (16, 17):
                notes.append(f'   door  ({col},{row}) anim={anim:04x}')
            elif code == 11:
                ev = anim & 0xFF
                if ev:
                    notes.append(f'   loose ({col},{row}) ev{ev} anim={anim:04x}')
            if mod:
                cells.append(f'{name}{mod:x}'[:4].ljust(4))
            else:
                cells.append(name.ljust(4))
            _ = mark
        print('   ' + ' '.join(cells))
    for n in notes:
        print(n)


def describe_target(buf, room, idx):
    col, row = idx % 10, idx // 10
    t = tile_at(buf, room, col, row) & 0xFF
    name = TILE.get(t, str(t))
    return f'r{room}({col},{row})={name}'


def print_events(buf):
    print('== events (non-empty) ==')
    for ev in range(256):
        room, idx, counter, f6, nxt = event(buf, ev)
        if room == -2 or room == 0 and idx == 0 and counter == 0 and nxt == 0:
            continue
        chain = [describe_target(buf, room, idx)] if room > 0 else [f'room={room}']
        seen, cur = {ev}, nxt
        while cur >= 0 and cur not in seen and cur < 256:
            seen.add(cur)
            r2, i2, _, _, nxt2 = event(buf, cur)
            chain.append(f'ev{cur}:' + (describe_target(buf, r2, i2)
                                        if r2 > 0 else f'room={r2}'))
            cur = nxt2
        tail = '' if nxt == -3 else f' next={nxt}'
        print(f'  ev{ev:3d}: cnt={counter:3d} f6={f6:04x}{tail}  -> '
              + ' -> '.join(chain))


def print_graph(buf, rooms):
    print('== room graph (L,R,U,D) ==')
    for room in rooms:
        l, r, u, d = links_of(buf, room)
        def n(x):
            return str(x) if x > 0 else '-'
        print(f'  r{room:<2} L:{n(l):<3} R:{n(r):<3} U:{n(u):<3} D:{n(d)}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('blob')
    ap.add_argument('--rooms', help='comma list; default = non-empty rooms')
    ap.add_argument('--events', action='store_true')
    ap.add_argument('--graph', action='store_true')
    args = ap.parse_args()
    buf = open(args.blob, 'rb').read()
    env = w16(buf, 8582)
    rooms = ([int(x) for x in args.rooms.split(',')] if args.rooms else
             [r for r in range(1, 33) if room_nonempty(buf, r)])
    print(f'env8582={env}  rooms={rooms}')
    if args.graph:
        print_graph(buf, rooms)
    for room in rooms:
        print_room(buf, room)
    if args.events:
        print_events(buf)


if __name__ == '__main__':
    main()
