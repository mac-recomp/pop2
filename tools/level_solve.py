#!/usr/bin/env python3
"""Reachability-based platform solver for a PoP2 level.

Idea: a level is a grid of 10x3-tile rooms wired by L/R/U/D links. Lay every
linked room into one global tile grid (BFS the links), then simulate the prince's
movement on it:

  * walk      -- step to an adjacent standable cell in the same row
  * climb up  -- step up one row to an adjacent ledge
  * drop      -- fall straight down a column to the first floor below; <=SAFE is
                 free (a controlled climb-down), deeper is a damaging/fatal fall
  * jump      -- (intended-path model only) clear a gap of a few columns

Reachability is computed two ways from the start cell:
  R_full   -- moves + jumps + survivable falls  = where the designer lets you go
  R_safe   -- moves + only safe (<=SAFE) drops, NO jumps, with platforms applied
             = where the prince can go with the assist and no horizontal jumps

The solver adds platforms so R_safe covers R_full: it bridges the gaps you would
otherwise jump and cushions the falls that would otherwise hurt. Crucially, a
platform is kept only if it does NOT shrink R_full (adding floor can only block a
fall, so this rejects any fill that would seal an intended drop) -- so the solver
can never create a softlock. Anything it cannot solve safely is reported.

Output: suggested fills as room:idx plus a report. Used by rebuild_tables.py.
"""
import sys
import os
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode_level as D

SAFE = 3        # fall of this many tiles or fewer is free (no damage)
FATAL = 8       # fall of this many tiles is death; 4..7 survivable with hp loss
JUMP = 4        # widest horizontal gap the intended path may jump


class Grid:
    """Linked rooms laid into one global tile grid (row down, col right)."""
    def __init__(self, buf, root=None):
        self.buf = buf
        self.rooms = [r for r in range(1, 33) if D.room_nonempty(buf, r)]
        self.pos = {}          # room -> (gx, gy) top-left in room units
        self._layout(root)
        self.tiles = {}        # (gr, gc) -> tile type
        self.cell2rc = {}      # (gr, gc) -> (room, idx)
        for room, (gx, gy) in self.pos.items():
            for r in range(3):
                for c in range(10):
                    gr, gc = gy * 3 + r, gx * 10 + c
                    self.tiles[(gr, gc)] = D.tile_at(buf, room, c, r) & 0xFF
                    self.cell2rc[(gr, gc)] = (room, r * 10 + c)

    def _layout(self, root=None):
        # BFS the L/R/U/D links from the start room (so the start and everything
        # link-connected to it lay out consistently); first assignment wins.
        if not self.rooms:
            return
        if root not in self.rooms:
            root = self.rooms[0]
        self.pos[root] = (0, 0)
        q = deque([root])
        while q:
            room = q.popleft()
            gx, gy = self.pos[room]
            L, R, U, Dn = D.links_of(self.buf, room)
            for nb, (dx, dy) in ((L, (-1, 0)), (R, (1, 0)),
                                 (U, (0, -1)), (Dn, (0, 1))):
                # follow links to ANY valid room (1..32), including tile-empty
                # passage rooms you fall through (e.g. L6 room 28 -D-> empty 14
                # -D-> room 10). A 0 link is "none" and is skipped.
                if 1 <= nb <= 32 and nb not in self.pos:
                    self.pos[nb] = (gx + dx, gy + dy)
                    q.append(nb)
        # rooms not reachable through links from root: drop a row below, side by
        # side, so they still get analysed (their own internal links still work).
        far = max((gy for _, gy in self.pos.values()), default=0) + 2
        x = 0
        for room in self.rooms:
            if room not in self.pos:
                self.pos[room] = (x, far)
                x += 1

    def stand(self, gr, gc, extra):
        t = self.tiles.get((gr, gc))
        if t is None:
            return (gr, gc) in extra
        return t in D.WALKABLE or (gr, gc) in extra

    def known(self, gr, gc):
        return (gr, gc) in self.tiles

    def drop_land(self, gr, gc, extra, limit):
        """First standable cell at column gc at row >= gr, within `limit` rows.
        Returns (land_row, dist) or (None, None)."""
        r = gr
        d = 0
        while d <= limit:
            if self.stand(r, gc, extra):
                return r, d
            if not self.known(r, gc) and r > gr:
                return None, None   # fell off the known map
            r += 1
            d += 1
        return None, None


def neighbors(g, cell, extra, allow_jump, fall_limit):
    gr, gc = cell
    out = []
    for dc in (-1, 1):
        nc = gc + dc
        if g.stand(gr, nc, extra):
            out.append((gr, nc))                      # walk
        else:
            land, dist = g.drop_land(gr, nc, extra, fall_limit)
            if land is not None and dist >= 1:
                out.append((land, nc))                # drop / fall
        if g.stand(gr - 1, nc, extra):
            out.append((gr - 1, nc))                  # climb up one (adjacent)
    # climb-grab a ledge two rows up (a room-seam climb: floor, a row of head-
    # room, floor above) when the way up is clear. Climbing/grabbing is not a
    # horizontal jump, so it is allowed in the no-jump model.
    for dc in (-1, 0, 1):
        if g.stand(gr - 2, gc + dc, extra) and not g.stand(gr - 1, gc + dc, extra):
            out.append((gr - 2, gc + dc))
    if allow_jump:
        for dc in (-1, 1):
            for k in range(2, JUMP + 1):
                tc = gc + dc * k
                # the cells jumped over must be non-floor (an actual gap)
                if any(g.stand(gr, gc + dc * j, extra) for j in range(1, k)):
                    break
                for dr in (0, -1, 1):
                    if g.stand(gr + dr, tc, extra):
                        out.append((gr + dr, tc))
    return out


def reach(g, start, extra, allow_jump, fall_limit):
    seen = {start}
    q = deque([start])
    while q:
        cell = q.popleft()
        for nb in neighbors(g, cell, extra, allow_jump, fall_limit):
            if nb not in seen:
                seen.add(nb)
                q.append(nb)
    return seen


def solve(buf, start_room, start_col, start_row, seed=None):
    g = Grid(buf, root=start_room)
    if start_room not in g.pos:
        return [], ['start room %d not in level' % start_room]
    gx, gy = g.pos[start_room]
    start = (gy * 3 + start_row, gx * 10 + start_col)
    if not g.stand(*start, set()):
        # snap to nearest standable in the start room
        for r in range(3):
            for c in range(10):
                if g.tiles.get((gy * 3 + r, gx * 10 + c)) in D.WALKABLE:
                    start = (gy * 3 + r, gx * 10 + c)
                    break
            else:
                continue
            break

    extra = set()                       # platform cells we add (global coords)
    full = reach(g, start, extra, True, FATAL - 1)  # intended reachable world
    report = []

    # rc->global for seeding the heuristic blanket fills (so casual play never
    # meets an unfilled pit), each vetted so it cannot seal an intended drop.
    rc2cell = {}
    for cell, rc in g.cell2rc.items():
        rc2cell[rc] = cell
    rejected = []
    for rc in (seed or []):
        cell = rc2cell.get(tuple(rc))
        if cell is None or cell in extra:
            continue
        if g.tiles.get(cell) in D.WALKABLE:
            continue                     # already solid
        cand = extra | {cell}
        if full.issubset(reach(g, start, cand, True, FATAL - 1)):
            extra = cand
        else:
            rejected.append(rc)          # this fill would block an intended drop

    def safe_to_add(cells):
        # adding floor can only block a fall; reject if it shrinks `full`.
        newfull = reach(g, start, extra | cells, True, FATAL - 1)
        return full.issubset(newfull)

    for _ in range(400):
        safe = reach(g, start, extra, False, SAFE)
        missing = [c for c in full if c not in safe and g.stand(*c, extra)]
        if not missing:
            break
        # find a safe-reachable cell that can reach a missing cell with one
        # jump/deep-fall, and bridge it.
        added = False
        for cell in list(safe):
            gr, gc = cell
            # horizontal jump-gap -> bridge along this row
            for dc in (-1, 1):
                for k in range(2, JUMP + 1):
                    tc = gc + dc * k
                    if any(g.stand(gr, gc + dc * j, extra) for j in range(1, k)):
                        break
                    if (gr, tc) in missing:
                        cells = {(gr, gc + dc * j) for j in range(1, k)}
                        if safe_to_add(cells):
                            extra |= cells
                            added = True
                        break
                if added:
                    break
            if added:
                break
            # deep fall -> cushion at SAFE depth so the drop is free
            for dc in (-1, 1):
                nc = gc + dc
                if g.stand(gr, nc, extra):
                    continue
                land, dist = g.drop_land(gr, nc, extra, FATAL)
                if land is not None and dist > SAFE and (land, nc) in missing:
                    cells = {(gr + SAFE, nc)}
                    if safe_to_add(cells):
                        extra |= cells
                        added = True
                        break
            if added:
                break
        if not added:
            break

    # Cushion pass: give every intended descent steeper than SAFE a rung SAFE
    # tiles down, iterated into a ladder, so no fall on a reachable path exceeds
    # SAFE (no damage / no splat). Only drops that LAND in the intended world
    # (`full`, computed with a survivable fall limit) get rungs -- a death pit is a
    # sheer drop past FATAL whose bottom is not in `full`, so it is left alone, and
    # the rung-lands-in-`full` test means the bottom stays reachable (drop off each
    # rung), so a ladder never softlocks. Every rung is safety-checked.
    cushions = 0
    cushion_cells = []
    for _ in range(600):
        safe = reach(g, start, extra, False, SAFE)
        added = False
        for cell in safe:
            gr, gc = cell
            for dc in (-1, 1):
                nc = gc + dc
                if g.stand(gr, nc, extra):
                    continue
                land, dist = g.drop_land(gr, nc, extra, FATAL - 1)
                if land is not None and dist > SAFE and (land, nc) in full:
                    rung = (gr + SAFE, nc)
                    if rung not in extra and rung in g.tiles and safe_to_add({rung}):
                        extra.add(rung)
                        cushions += 1
                        cushion_cells.append(g.cell2rc[rung])
                        added = True
                        break
            if added:
                break
        if not added:
            break

    safe = reach(g, start, extra, False, SAFE)
    full_stand = [c for c in full if g.stand(*c, extra)]
    unreached = [c for c in full_stand if c not in safe]
    fills = sorted({g.cell2rc[c] for c in extra if c in g.cell2rc})
    report.append('rooms=%d start=(r%d c%d row%d) reachable_full=%d '
                  'safe_no_jump=%d/%d still_unreached=%d fills=%d rejected_seed=%d '
                  'cushions=%d'
                  % (len(g.rooms), start_room, start_col, start_row,
                     len(full), len(safe & set(full_stand)), len(full_stand),
                     len(unreached), len(fills), len(rejected), cushions))
    if rejected:
        report.append('  rejected (would block an intended drop): '
                      + ",".join('%d:%d' % tuple(x) for x in rejected))
    if unreached:
        rcs = sorted({g.cell2rc[c] for c in unreached if c in g.cell2rc})
        report.append('  unreached without a jump: '
                      + ",".join('%d:%d' % rc for rc in rcs))
    if cushion_cells:
        report.append('  cushions: '
                      + ",".join('%d:%d' % rc for rc in cushion_cells))
    extra_outside = [c for c in extra if c not in g.cell2rc]
    if extra_outside:
        report.append('  WARN %d fills outside any room (off-grid)' % len(extra_outside))
    return fills, report


def main():
    import level_platforms as LP
    buf = open(sys.argv[1], 'rb').read()
    sr, sc, srow = (int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])) \
        if len(sys.argv) > 4 else (None, 0, 1)
    if sr is None:
        sr = Grid(buf).rooms[0]
    seed, _ = LP.find_fills(buf)         # heuristic blanket pit-fills, vetted inside
    fills, report = solve(buf, sr, sc, srow, seed=seed)
    for line in report:
        print(line, file=sys.stderr)
    print(",".join(f"{r}:{i}" for r, i in fills))


if __name__ == '__main__':
    main()
