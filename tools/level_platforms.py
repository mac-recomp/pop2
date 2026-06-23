#!/usr/bin/env python3
"""Generate platform fills for a PoP2 level so it needs no horizontal jumps.

Reads a level-state blob (a POP2_DUMP_STATE dump) and scans every room's 10x3
tile grid for gaps a walking prince would otherwise have to jump:

  * interior gaps  -- an empty run in a row bounded on BOTH sides by a walkable
    tile in that same row (a pit in the walkway). Filled at any width; wider than
    MAX_PLAIN is still filled but FLAGGED, since a very wide opening might be an
    intended drop/passage rather than a walkway.

  * cross-room edge gaps -- an empty run reaching a room's right edge (col 9)
    where the right-neighbour room's SAME row has a walkway to connect to. The
    seam between the two walkways is filled. Guarded: if the neighbour's row has
    no walkable tile (a level change, not a flat walk) nothing is filled, so we
    never bridge into a wall/void. Every cross-room fill is FLAGGED.

The hazard the assist guards against is gravity (falling); closing these gaps
lets the prince walk straight across. Vertical drops (fatal-fall cushions) are
NOT auto-placed -- too risky without a playtest -- and remain a TODO.

idx = row*10 + col, matching POP2_PLATFORMS. Floor fill = tile type 1.

  python3 tools/level_platforms.py /tmp/lvl3.bin        # print fills + flags
"""
import sys
import os
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode_level as D

MAX_PLAIN = 4   # interior gaps wider than this are filled but flagged for review
MAX_EDGE = 5    # cross-room seam wider than this is skipped (likely a level change)


def _row(buf, room, row):
    return [D.tile_at(buf, room, c, row) & 0xFF for c in range(10)]


def _first_walk(t):
    for c in range(10):
        if t[c] in D.WALKABLE:
            return c
    return None


def find_fills(buf):
    """Return (fills, flags): fills = sorted list of (room, idx); flags = list of
    (room, idx, reason) for the cells a human should re-check in a playtest."""
    fills, flags = set(), {}
    rooms = [r for r in range(1, 33) if D.room_nonempty(buf, r)]
    rset = set(rooms)
    for room in rooms:
        L, R, U, Dn = D.links_of(buf, room)
        for row in range(3):
            t = _row(buf, room, row)
            c = 0
            while c < 10:
                if t[c] != 0:
                    c += 1
                    continue
                start = c
                while c < 10 and t[c] == 0:
                    c += 1
                end = c  # [start, end) is the empty run
                interior = start > 0 and end < 10
                if interior and t[start - 1] in D.WALKABLE and t[end] in D.WALKABLE:
                    width = end - start
                    for cc in range(start, end):
                        fills.add((room, row * 10 + cc))
                        if width > MAX_PLAIN:
                            flags[(room, row * 10 + cc)] = 'wide pit (%d)' % width
                elif end == 10 and start > 0 and t[start - 1] in D.WALKABLE \
                        and R in rset:
                    # gap reaching the right edge -> try to bridge into room R
                    rt = _row(buf, R, row)
                    b = _first_walk(rt)
                    if b is not None and (10 - start) + b <= MAX_EDGE:
                        for cc in range(start, 10):
                            fills.add((room, row * 10 + cc))
                            flags[(room, row * 10 + cc)] = 'edge->R%d' % R
                        for cc in range(0, b):
                            fills.add((R, row * 10 + cc))
                            flags[(R, row * 10 + cc)] = 'edge<-R%d' % room
    fl = [(r, i, flags[(r, i)]) for (r, i) in sorted(flags)]
    return sorted(fills), fl


def main():
    buf = open(sys.argv[1], 'rb').read()
    fills, flags = find_fills(buf)
    by = defaultdict(list)
    for r, i in fills:
        by[r].append(i)
    for r in sorted(by):
        print(f"  room {r:2}: {by[r]}", file=sys.stderr)
    print(f"  {len(fills)} cells across {len(by)} rooms, {len(flags)} flagged",
          file=sys.stderr)
    for r, i, why in flags:
        print(f"  FLAG room {r} idx {i}: {why}", file=sys.stderr)
    print(",".join(f"{r}:{i}" for r, i in fills))


if __name__ == '__main__':
    main()
