#!/usr/bin/env python3
"""Emit a walkable route through a level for the in-game auto-navigator.

Builds the same global grid + reachability the solver uses (with the level's baked
platform fills applied), BFS from the start cell, and writes the path to the
farthest reachable cell as a sequence of waypoints. Each line:

    DIR room col row

DIR is the key(s) to hold to move from the previous waypoint to this one (U=climb
up, R/L=walk, combos like UR), and room/col/row is the target cell to detect
arrival. The harness (POP2_AUTONAV=file) follows it and reports reached/stuck/died.

  python3 tools/gen_path.py <state.bin> <room> <col> <row> <LEVEL_N> > path.txt
"""
import sys
import os
import re
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_solve as S
import decode_level as D


def table_fills(level):
    cells = set()
    for ln in open(os.path.join(os.path.dirname(__file__), 'platform-tables.txt')):
        m = re.match(r'LEVEL %d:\s*(.*)' % level, ln)
        if m:
            for tok in m.group(1).split(','):
                if tok.strip():
                    r, i = tok.strip().split(':')[:2]
                    cells.add((int(r), int(i)))
    return cells


def main():
    buf = open(sys.argv[1], 'rb').read()
    sr, sc, srow, level = (int(sys.argv[2]), int(sys.argv[3]),
                           int(sys.argv[4]), int(sys.argv[5]))
    g = S.Grid(buf, root=sr)
    # apply the baked fills as extra solid cells
    rc2cell = {rc: cell for cell, rc in g.cell2rc.items()}
    extra = {rc2cell[rc] for rc in table_fills(level) if rc in rc2cell}
    if sr not in g.pos:
        print("# start room not laid out", file=sys.stderr)
        return
    gx, gy = g.pos[sr]
    start = (gy * 3 + srow, gx * 10 + sc)
    if not g.stand(*start, extra):
        for r in range(3):
            for c in range(10):
                if g.stand(gy * 3 + r, gx * 10 + c, extra):
                    start = (gy * 3 + r, gx * 10 + c)
                    break
            else:
                continue
            break
    # BFS (no jumps, safe drops) recording parents
    parent = {start: None}
    q = deque([start])
    order = [start]
    while q:
        cell = q.popleft()
        for nb in S.neighbors(g, cell, extra, False, S.SAFE, cap=True):
            if nb not in parent and nb in g.tiles:
                parent[nb] = cell
                q.append(nb)
                order.append(nb)
    # farthest cell (BFS order last) = deepest route
    far = order[-1]
    path = []
    c = far
    while c is not None:
        path.append(c)
        c = parent[c]
    path.reverse()
    print("# level %d start=(%d,%d,%d) reachable=%d path_len=%d to far=%s"
          % (level, sr, sc, srow, len(parent), len(path),
             g.cell2rc.get(far)), file=sys.stderr)
    for a, b in zip(path, path[1:]):
        dgr, dgc = b[0] - a[0], b[1] - a[1]
        # up = climb (U); down = climb-down off a ledge (D, the kid won't walk off
        # one); plus the horizontal toward the target column.
        d = ('U' if dgr < 0 else 'D' if dgr > 0 else '') \
            + ('R' if dgc > 0 else 'L' if dgc < 0 else '')
        if not d:
            d = 'R'
        rc = g.cell2rc.get(b)
        if rc:
            print("%s %d %d %d" % (d, rc[0], rc[1] % 10, rc[1] // 10))


if __name__ == '__main__':
    main()
