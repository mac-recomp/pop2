#!/usr/bin/env python3
"""Generate platform fills for a PoP2 level so it needs no horizontal jumps.

Reads a level-state blob (a POP2_DUMP_STATE dump) and scans every room's 10x3
tile grid for short runs of empty space that are bounded on both sides, in the
same row, by a walkable tile -- i.e. a pit you would otherwise have to jump.
Each such gap is emitted as a floor fill.  The hazard the platform assist
guards against is gravity (falling), so closing these gaps lets the player walk
straight across instead of making a horizontal jump.

Output (stdout): a POP2_PLATFORMS string, "room:idx,room:idx,..." where
idx = row*10 + col.  A per-room summary goes to stderr.

  python3 tools/level_platforms.py /tmp/lvl3.bin
"""
import sys
import os
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode_level as D

MAX_GAP = 4  # fill only short pits; wider openings are passages / climb-down shafts


def find_fills(buf):
    """Return a list of (room, idx) cells to fill with floor."""
    plats = []
    for room in range(1, 33):
        if not D.room_nonempty(buf, room):
            continue
        for row in range(3):
            t = [D.tile_at(buf, room, c, row) & 0xFF for c in range(10)]
            c = 0
            while c < 10:
                if t[c] != 0:
                    c += 1
                    continue
                start = c
                while c < 10 and t[c] == 0:
                    c += 1
                end = c  # [start, end) is the run of space
                interior = start > 0 and end < 10
                if (interior and (end - start) <= MAX_GAP
                        and t[start - 1] in D.WALKABLE and t[end] in D.WALKABLE):
                    plats += [(room, row * 10 + cc) for cc in range(start, end)]
    return plats


def main():
    buf = open(sys.argv[1], 'rb').read()
    plats = find_fills(buf)
    by = defaultdict(list)
    for r, i in plats:
        by[r].append(i)
    for r in sorted(by):
        print(f"  room {r:2}: {by[r]}", file=sys.stderr)
    print(f"  {len(plats)} cells across {len(by)} rooms", file=sys.stderr)
    print(",".join(f"{r}:{i}" for r, i in plats))


if __name__ == '__main__':
    main()
