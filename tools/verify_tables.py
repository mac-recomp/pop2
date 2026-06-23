#!/usr/bin/env python3
"""Model-light verification of the platform tables, for every level.

Trusts the designer's room graph: the rooms reachable from the start are the ones
you can get to via the L/R/U/D links (BFS). Within those reachable rooms it lists
the horizontal jump-gaps (the same-row floor-gap-floor pits + cross-room edge
seams from level_platforms) and checks the current table bridges them all -- i.e.
no reachable room leaves a horizontal jump unfilled. This needs no movement
physics, so it covers the levels the reachability solver cannot yet traverse.

Reports per level: reachable rooms / total, gap cells needed vs covered, and any
missed gaps (needed in a reachable room but absent from the table).

  python3 tools/verify_tables.py <state-dump-dir> <starts.txt>
"""
import sys
import os
import re
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_platforms as LP
import decode_level as D


def reachable_rooms(buf, start_room):
    rooms = {r for r in range(1, 33) if D.room_nonempty(buf, r)}
    if start_room not in rooms:
        return set()
    seen = {start_room}
    q = deque([start_room])
    while q:
        room = q.popleft()
        for nb in D.links_of(buf, room):
            if nb in rooms and nb not in seen:
                seen.add(nb)
                q.append(nb)
    return seen


def parse_table(path):
    tab = {}
    for line in open(path):
        m = re.match(r'LEVEL (\d+):\s*(.*)', line)
        if not m:
            continue
        lvl = int(m.group(1))
        cells = set()
        for tok in m.group(2).split(','):
            tok = tok.strip()
            if tok:
                r, i = tok.split(':')[:2]
                cells.add((int(r), int(i)))
        tab[lvl] = cells
    return tab


def parse_starts(path):
    st = {}
    for line in open(path):
        m = re.match(r'L(\d+) .*room=(-?\d+)', line)
        if m:
            st[int(m.group(1))] = int(m.group(2))
    return st


def main():
    sd, starts_path = sys.argv[1], sys.argv[2]
    starts = parse_starts(starts_path)
    table = parse_table('tools/platform-tables.txt')
    print('lvl reach/tot  gaps cover miss')
    total_miss = 0
    for lvl in range(1, 15):
        p = os.path.join(sd, f'lvl{lvl}.bin')
        if not os.path.exists(p):
            continue
        buf = open(p, 'rb').read()
        allrooms = {r for r in range(1, 33) if D.room_nonempty(buf, r)}
        rr = reachable_rooms(buf, starts.get(lvl, -1))
        fills, _ = LP.find_fills(buf)
        need = {(r, i) for r, i in fills if r in rr}     # gaps in reachable rooms
        have = table.get(lvl, set())
        miss = need - have
        total_miss += len(miss)
        flag = '' if not miss else '  MISS ' + ','.join('%d:%d' % m for m in sorted(miss))[:60]
        print(f'{lvl:3} {len(rr):3}/{len(allrooms):<3}   {len(need):4} {len(need & have):4} {len(miss):4}{flag}')
    print('total missed horizontal gaps in reachable rooms:', total_miss)


if __name__ == '__main__':
    main()
