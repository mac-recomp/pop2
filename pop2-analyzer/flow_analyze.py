#!/usr/bin/env python3
"""Recursive-descent discovery over Mac 68k CODE segments.

Seeds: every jump-table entry from CODE 0. Follows branches, local calls and
fallthrough; records A-line traps only on reachable instruction boundaries.

Usage: flow_analyze.py <code_dir> [--json out.json] [--funcs]
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import m68k
from analyze_code import TRAP_NAMES, trap_name, load_segments, parse_code0


def analyze(segs):
    c0 = parse_code0(segs[0][1])
    # jump table: a5 offset -> (seg, code offset). Code offset is relative to
    # segment data INCLUDING its 4-byte header.
    jt_by_a5 = {}
    for e in c0["entries"]:
        if "seg" in e:
            jt_by_a5[e["a5call"]] = (e["seg"], e["off"] + 4)

    visited = {}          # (seg, off) -> Insn
    queue = []            # (seg, off)
    entry_funcs = set()   # function entry points
    edges = defaultdict(set)

    for a5off, (seg, off) in jt_by_a5.items():
        queue.append((seg, off))
        entry_funcs.add((seg, off))

    traps = defaultdict(list)      # opcode -> [(seg, off)]
    a5_calls = defaultdict(list)   # a5off -> [(seg, off)]
    indirect = []                  # unresolved JMP/JSR sites
    errors = []

    while queue:
        seg, off = queue.pop()
        if (seg, off) in visited or seg not in segs:
            continue
        data = segs[seg][1]
        if off >= len(data):
            errors.append((seg, off, "out of range"))
            continue
        try:
            insn = m68k.decode(data, off)
        except m68k.DecodeError as e:
            errors.append((seg, off, str(e)))
            continue
        visited[(seg, off)] = insn

        nxt = off + insn.size
        if insn.trap is not None:
            traps[insn.trap].append((seg, off))
        if insn.a5_call is not None:
            a5_calls[insn.a5_call].append((seg, off))
            tgt = jt_by_a5.get(insn.a5_call)
            if tgt:
                entry_funcs.add(tgt)
                edges[(seg, off)].add(tgt)
                queue.append(tgt)
        if insn.flow in (m68k.SEQ, m68k.ALINE):
            queue.append((seg, nxt))
        elif insn.flow == m68k.BRANCH:
            queue.append((seg, nxt))
            if insn.target is not None:
                queue.append((seg, insn.target))
        elif insn.flow == m68k.JUMP:
            if insn.target is not None:
                queue.append((seg, insn.target))
            elif insn.mnem == "jmp_ind":
                indirect.append((seg, off, insn.mnem))
        elif insn.flow == m68k.CALL:
            queue.append((seg, nxt))
            if insn.target is not None:
                entry_funcs.add((seg, insn.target))
                queue.append((seg, insn.target))
            elif insn.mnem in ("jsr_ind", "jsr_abs"):
                indirect.append((seg, off, insn.mnem))
        # RET, ILLEGAL: stop

    return {
        "code0": c0, "jt_by_a5": jt_by_a5, "visited": visited,
        "traps": traps, "a5_calls": a5_calls, "indirect": indirect,
        "errors": errors, "entry_funcs": entry_funcs,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code_dir", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    segs = load_segments(args.code_dir)
    res = analyze(segs)
    visited = res["visited"]

    print(f"jump table entries resolved: {len(res['jt_by_a5'])}")
    print(f"instructions decoded: {len(visited)}")
    print(f"function entries:     {len(res['entry_funcs'])}")
    print(f"indirect jmp/jsr:     {len(res['indirect'])}")
    print(f"decode errors:        {len(res['errors'])}")

    # coverage per segment
    cov = defaultdict(int)
    for (seg, off), insn in visited.items():
        cov[seg] += insn.size
    print(f"\n{'seg':>3} {'name':<16} {'size':>6} {'code':>6} {'cov%':>5}")
    for sid, (name, data) in segs.items():
        if sid == 0:
            continue
        size = len(data) - 4
        print(f"{sid:>3} {name:<16.16} {size:>6} {cov[sid]:>6} "
              f"{100.0 * cov[sid] / max(size, 1):>5.1f}")

    print(f"\nA-line traps on reachable paths: "
          f"{sum(len(v) for v in res['traps'].values())} sites, "
          f"{len(res['traps'])} unique")
    print(f"{'opcode':<8} {'count':>5}  name")
    unknown = 0
    for op in sorted(res["traps"], key=lambda o: -len(res["traps"][o])):
        nm = trap_name(op)
        if nm.startswith("unknown"):
            unknown += 1
        print(f"0x{op:04X}  {len(res['traps'][op]):>5}  {nm}")
    print(f"(unknown: {unknown})")

    if res["errors"]:
        print("\nfirst decode errors:")
        for seg, off, msg in res["errors"][:10]:
            print(f"  seg {seg} off {off:#x}: {msg}")

    if args.json:
        out = {
            "traps": {f"0x{op:04X}": {"name": trap_name(op),
                                      "count": len(sites),
                                      "sites": [[s, o] for s, o in sites]}
                      for op, sites in sorted(res["traps"].items())},
            "functions": sorted([s, o] for s, o in res["entry_funcs"]),
            "indirect": res["indirect"],
            "coverage": {str(k): v for k, v in cov.items()},
        }
        args.json.write_text(json.dumps(out, indent=1))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
