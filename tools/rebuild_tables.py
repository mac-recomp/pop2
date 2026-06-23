#!/usr/bin/env python3
"""Rebuild tools/platform-tables.txt from per-level state dumps + start positions.

For each level it runs the reachability solver (tools/level_solve.py), seeded with
the geometry heuristic (tools/level_platforms.py). Where the movement model
actually traverses the level (its reachable-with-jumps world is a large fraction
of the walkable tiles) the solver's vetted+completed fill set is used -- it drops
any heuristic fill that would seal an intended drop and adds bridges/cushions so
the start can reach that world with no horizontal jumps. Where the model gets
stuck (vertical-only start chambers the model can't yet climb) it falls back to
the geometry heuristic, which over-fills safely but is not movement-verified.

Per-level "# L N:" comments record the provenance and the playtest review list
(rejected fills, still-unreachable spots).

  python3 tools/rebuild_tables.py <state-dump-dir> <starts.txt>
"""
import sys
import os
import re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_solve as S
import level_platforms as LP
import decode_level as D

HEADER = """# PoP2 platform-assist tables (auto-generated; see tools/rebuild_tables.py).
#
# "LEVEL N:" = floor cells to stamp so the level needs no horizontal jumps,
# "room:idx", idx = row*10 + col. Baked into the engine via
# tools/gen_platform_tables.py (run it after editing). "# L N:" notes how the
# table was built and what to re-check in a playtest.
"""


def walkable(buf):
    n = 0
    for r in range(1, 33):
        if not D.room_nonempty(buf, r):
            continue
        for c in range(10):
            for row in range(3):
                if (D.tile_at(buf, r, c, row) & 0xFF) in D.WALKABLE:
                    n += 1
    return n


def parse_starts(path):
    st = {}
    for line in open(path):
        m = re.match(r'L(\d+) .*col=(-?\d+) row=(-?\d+) room=(-?\d+)', line)
        if m:
            lvl, col, row, room = map(int, m.groups())
            st[lvl] = (room, col, row)
    return st


def grab(report, key):
    for line in report:
        if line.strip().startswith(key):
            return line.split(':', 1)[1].strip()
    return ''


def main():
    sd, starts_path = sys.argv[1], sys.argv[2]
    starts = parse_starts(starts_path)
    lines = [HEADER.rstrip('\n')]
    summary = []
    for lvl in range(1, 15):
        p = os.path.join(sd, f'lvl{lvl}.bin')
        if not os.path.exists(p):
            continue
        buf = open(p, 'rb').read()
        wlk = walkable(buf)
        seed, _ = LP.find_fills(buf)
        if lvl not in starts:
            fills, mode, note = seed, 'geometry', 'no start position'
            full = safe = unr = -1
            rej = unrch = ''
        else:
            room, col, row = starts[lvl]
            sfills, report = S.solve(buf, room, col, row, seed=seed)
            r0 = report[0]
            full = int(re.search(r'reachable_full=(\d+)', r0).group(1))
            fullst = int(re.search(r'safe_no_jump=\d+/(\d+)', r0).group(1))
            safe = int(re.search(r'safe_no_jump=(\d+)', r0).group(1))
            unr = int(re.search(r'still_unreached=(\d+)', r0).group(1))
            rej = grab(report, 'rejected (')
            unrch = grab(report, 'unreached without')
            model_ok = full >= 0.6 * wlk or full >= 120
            if model_ok:
                fills, mode = sfills, 'solver'
                note = f'reachable {safe}/{fullst} no-jump (of {full} world)'
            else:
                fills, mode = seed, 'geometry'
                note = f'model stuck (only {full}/{wlk} reachable) -> geometry fill'
        if fills:
            lines.append(f'LEVEL {lvl}: ' + ','.join(f'{r}:{i}' for r, i in fills))
        lines.append(f'# L{lvl} [{mode}] {note} | {len(fills)} cells')
        if mode == 'solver' and rej:
            lines.append(f'#   removed (would block a drop): {rej}')
        if mode == 'solver' and unrch:
            lines.append(f'#   still needs a jump here: {unrch}')
        summary.append((lvl, mode, len(fills), full, safe, unr))
    open('tools/platform-tables.txt', 'w').write('\n'.join(lines) + '\n')
    print('lvl mode      cells full safe unrch')
    for lvl, mode, n, full, safe, unr in summary:
        print(f'{lvl:3} {mode:9} {n:5} {full:4} {safe:4} {unr:5}')
    print('total cells:', sum(s[2] for s in summary))


if __name__ == '__main__':
    main()
