#!/usr/bin/env python3
"""68k → C++ static recompiler for Mac CODE segments.

Reads extracted CODE resources, partitions code into functions (entries =
jump-table slots + direct call targets), and emits one C++ translation unit
per segment plus dispatch tables.

Conventions (see pop2-runtime/include/pop2/cpu.h):
  - guest fn -> `void fN_XXXX(Cpu&)`; JSR pushes guest RA, RTS pops + returns.
  - segment N image loaded at SEG_VBASE(N); PC-relative ops use constants.
  - unsupported instructions emit `unimplemented(...)` and terminate the path.

Usage: recomp.py <code_dir> <out_dir>
"""

import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "pop2-analyzer"))
import m68k
from m68k import parse_ea, EA
from analyze_code import load_segments, parse_code0, trap_name


def SEG_VBASE(seg: int) -> int:
    return 0x0020_0000 + seg * 0x1_0000


TYPES = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
STYPES = {1: "int8_t", 2: "int16_t", 4: "int32_t"}


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------------------
# function discovery
# ---------------------------------------------------------------------------

def discover(segs):
    """Return (entries per seg, decode cache, jt map a5off->(seg,off))."""
    c0 = parse_code0(segs[0][1])
    jt = {}
    for e in c0["entries"]:
        if "seg" in e:
            jt[e["a5call"]] = (e["seg"], e["off"] + 4)

    cache = {}     # (seg, off) -> Insn
    entries = set(jt.values())
    for seg in segs:
        if seg >= 25:   # raw code blobs (decoded MDRV etc.): entry at offset 0
            entries.add((seg, 0))
    queue = list(entries)
    visited = set()
    while queue:
        seg, off = queue.pop()
        if (seg, off) in visited or seg not in segs:
            continue
        visited.add((seg, off))
        data = segs[seg][1]
        if off < 0 or off >= len(data):
            continue
        try:
            insn = m68k.decode(data, off)
        except m68k.DecodeError:
            continue
        cache[(seg, off)] = insn
        nxt = off + insn.size
        if insn.a5_call is not None and insn.a5_call in jt:
            entries.add(jt[insn.a5_call])
            queue.append(jt[insn.a5_call])
        if insn.mnem in ("lea", "pea"):
            # LEA fn(PC),An / PEA fn(PC) — address-of-function for callbacks;
            # treat in-segment even targets as potential entry points.
            mode, reg = (insn.words[0] >> 3) & 7, insn.words[0] & 7
            try:
                ea = parse_ea(data, off + 2, mode, reg, 0)
            except m68k.DecodeError:
                ea = None
            if ea and ea.kind == "pcdisp":
                tgt = off + 2 + ea.disp
                if 4 <= tgt < len(data) and tgt % 2 == 0:
                    entries.add((seg, tgt))
                    queue.append((seg, tgt))
        if insn.flow in (m68k.SEQ, m68k.ALINE):
            queue.append((seg, nxt))
        elif insn.flow == m68k.BRANCH:
            queue.append((seg, nxt))
            queue.append((seg, insn.target))
        elif insn.flow == m68k.JUMP:
            if insn.target is not None:
                queue.append((seg, insn.target))
            elif insn.mnem == "jmp_ind":
                jt_scan = scan_jump_table(data, insn)
                if jt_scan:
                    for _, bra_off in jt_scan[2]:
                        queue.append((seg, bra_off))
                elif ot := scan_offset_table(data, insn):
                    for _, tgt in ot[1]:
                        queue.append((seg, tgt))
                elif lt := scan_lea_adda_table(data, insn):
                    for _, tgt in lt[2]:
                        queue.append((seg, tgt))
        elif insn.flow == m68k.CALL:
            queue.append((seg, nxt))
            if insn.target is not None:
                entries.add((seg, insn.target))
                queue.append((seg, insn.target))
    return entries, cache, jt


def fn_name(seg, off):
    return f"f{seg}_{off:04x}"


def scan_jump_table(data, insn):
    """For `JMP d8(PC,Dn.W)` — or `LEA d16(PC),An; JMP d8(An,Dn.W)` —
    decode the BRA table that follows.

    Returns (case_step, [(case_value, bra_off), ...]) or None. Think C (and
    the HALESTORM driver blobs) emit the dispatch as a run of BRA.W (or
    BRA.B) at the indexed base.
    """
    op = insn.words[0]
    mode, reg = (op >> 3) & 7, op & 7
    if mode == 7 and reg == 3:
        try:
            ea = parse_ea(data, insn.addr + 2, 7, 3, 0)
        except m68k.DecodeError:
            return None
        if ea.idx_long or ea.idx_scale != 1 or ea.idx_reg >= 8:
            return None
        base = insn.addr + 2 + ea.disp
    elif mode == 6 and insn.addr >= 8:
        try:
            ea = parse_ea(data, insn.addr + 2, 6, reg, 0)
        except m68k.DecodeError:
            return None
        if ea.idx_long or ea.idx_scale != 1 or ea.idx_reg >= 8:
            return None
        lw = (data[insn.addr - 4] << 8) | data[insn.addr - 3]
        if lw != (0x41FA | (reg << 9)):    # LEA (d16,PC),An feeding the JMP
            return None
        d16 = (data[insn.addr - 2] << 8) | data[insn.addr - 1]
        if d16 & 0x8000:
            d16 -= 0x10000
        base = insn.addr - 2 + d16 + ea.disp
    else:
        return None
    if base < 4 or base >= len(data):
        return None
    w0 = (data[base] << 8) | data[base + 1] if base + 2 <= len(data) else 0
    if w0 == 0x6000:
        step = 4          # BRA.W entries
    elif (w0 >> 8) == 0x60 and (w0 & 0xFF) not in (0x00, 0xFF):
        step = 2          # BRA.B entries
    else:
        return None
    cases = []
    off = base
    while off + 2 <= len(data) and len(cases) < 512:
        w = (data[off] << 8) | data[off + 1]
        if step == 4 and w != 0x6000:
            break
        if step == 2 and ((w >> 8) != 0x60 or (w & 0xFF) in (0x00, 0xFF)):
            break
        cases.append((off - base, off))
        off += step
    return (ea.idx_reg, base, cases) if cases else None


def scan_offset_table(data, insn):
    """For `MOVE.W d8(PC,Dn.W),Dm` + `JMP d8'(PC,Dm.W)`: decode the
    offset-word table (Think C's other switch lowering — a table of signed
    word offsets relative to the JMP's PC base, used when BRA stubs would
    not reach).

    `insn` is the JMP. Returns (idx_reg_m, [(word, target_off), ...]) or
    None; the switch key is Dm's runtime value (the loaded offset word).
    """
    if insn.words[0] != 0x4EFB or insn.addr < 8:
        return None
    try:
        jea = parse_ea(data, insn.addr + 2, 7, 3, 0)
    except m68k.DecodeError:
        return None
    if jea.idx_long or jea.idx_scale != 1 or jea.idx_reg >= 8:
        return None
    mv = (data[insn.addr - 4] << 8) | data[insn.addr - 3]
    if (mv & 0xF1FF) != 0x303B:            # MOVE.W (d8,PC,Xn),Dm
        return None
    if (mv >> 9) & 7 != jea.idx_reg:       # table value must feed the JMP
        return None
    try:
        mea = parse_ea(data, insn.addr - 2, 7, 3, 0)
    except m68k.DecodeError:
        return None
    if mea.idx_long or mea.idx_scale != 1 or mea.idx_reg >= 8:
        return None
    table = insn.addr - 2 + mea.disp
    jbase = insn.addr + 2 + jea.disp
    if table < 4 or table >= len(data):
        return None
    cases = []
    end = len(data)                        # table ends at the first target
    pos = table
    while pos + 2 <= len(data) and pos < end and len(cases) < 256:
        w = (data[pos] << 8) | data[pos + 1]
        sw = w - 0x10000 if w & 0x8000 else w
        tgt = jbase + sw
        if tgt < 4 or tgt >= len(data) or tgt % 2 or tgt == insn.addr:
            break
        cases.append((sw, tgt))
        if tgt > table:
            end = min(end, tgt)
        pos += 2
    if len(cases) < 2:
        return None
    return (jea.idx_reg, cases)


def scan_lea_adda_table(data, insn):
    """For `LEA d16(PC),An; ADDA.W (0,An,Dn.W*2),An; JMP (An)` — or the
    plain-68000 flavor `LEA d16(PC),An; ADD.W Dn,Dn; ADDA.W (0,An,Dn.W),An;
    JMP (An)` — Think C's third switch lowering: a table of signed word
    offsets relative to the TABLE base (targets may sit below the table).
    The preceding bounds check `CMPI.W #max,Dn; BHI <default>` gives the
    exact entry count.

    `insn` is the JMP. Returns (an, table_off, [(idx, target_off), ...]) or
    None; the switch key is An's runtime value (vbase + table + word).
    """
    op = insn.words[0]
    if (op & 0xFFF8) != 0x4ED0 or insn.addr < 16:
        return None
    an = op & 7
    aw = (data[insn.addr - 4] << 8) | data[insn.addr - 3]
    ext = (data[insn.addr - 2] << 8) | data[insn.addr - 1]
    if aw != (0xD0F0 | (an << 9) | an):       # ADDA.W (d8,An,Xn),An
        return None
    dn = (ext >> 12) & 7
    if (ext & 0x8FFF) == 0x0200:              # Dn.W index, scale*2, d8=0
        lea_at = insn.addr - 8
    elif (ext & 0x8FFF) == 0x0000:            # Dn.W scale*1: index doubled
        dbl = (data[insn.addr - 6] << 8) | data[insn.addr - 5]
        if dbl != (0xD040 | (dn << 9) | dn):  # ADD.W Dn,Dn
            return None
        lea_at = insn.addr - 10
    else:
        return None
    if lea_at < 6:
        return None
    lw = (data[lea_at] << 8) | data[lea_at + 1]
    if lw != (0x41FA | (an << 9)):            # LEA (d16,PC),An
        return None
    d16 = (data[lea_at + 2] << 8) | data[lea_at + 3]
    if d16 & 0x8000:
        d16 -= 0x10000
    table = lea_at + 2 + d16
    if table < 4 or table + 4 > len(data):
        return None
    # bounds check just before the LEA gives the exact entry count:
    # either `CMPI.W #max,Dn; BHI` or `MOVEQ #max,Dm; CMP.L Dm,Dn; BHI`
    count = None
    for bhi_len in (2, 4):
        ba = lea_at - bhi_len
        if ba < 6:
            continue
        bw = (data[ba] << 8) | data[ba + 1]
        if (bw >> 8) != 0x62 or (bhi_len == 2) != ((bw & 0xFF) != 0):
            continue
        ca = ba - 4
        if ca >= 0:
            cw = (data[ca] << 8) | data[ca + 1]
            if cw == (0x0C40 | dn):
                count = ((data[ca + 2] << 8) | data[ca + 3]) + 1
                break
        ca = ba - 2
        if ca >= 2:
            cw = (data[ca] << 8) | data[ca + 1]
            mq = (data[ca - 2] << 8) | data[ca - 1]
            if ((cw & 0xF1F8) == 0xB080 and ((cw >> 9) & 7) == dn
                    and (mq & 0xF100) == 0x7000
                    and ((mq >> 9) & 7) == (cw & 7)):
                count = (mq & 0xFF) + 1
                break
    if count is None or count > 512 or table + count * 2 > len(data):
        return None
    cases = []
    for i in range(count):
        pos = table + i * 2
        w = (data[pos] << 8) | data[pos + 1]
        sw = w - 0x10000 if w & 0x8000 else w
        tgt = table + sw
        if tgt < 4 or tgt >= len(data) or tgt % 2 or tgt == insn.addr:
            return None
        cases.append((i, tgt))
    return (an, table, cases)


def local_walk(seg, entry_off, cache, entries, segdata):
    """Instructions belonging to fn at entry_off; follows local flow only.

    Returns (insns: {off: Insn}, labels: set, tails: set of (seg,off) called
    as tail). Branches to other entries become tail calls.
    """
    insns, labels, tails = {}, set(), set()
    queue = [entry_off]
    while queue:
        off = queue.pop()
        if off in insns:
            continue
        insn = cache.get((seg, off))
        if insn is None:
            continue   # decode hole; path will emit unimplemented at runtime
        insns[off] = insn
        nxt = off + insn.size

        def to(target):
            if target == entry_off:
                labels.add(target)
                queue.append(target)
            elif (seg, target) in entries:
                tails.add(target)
            else:
                labels.add(target)
                queue.append(target)

        if insn.flow in (m68k.SEQ, m68k.ALINE):
            queue.append(nxt)
        elif insn.flow == m68k.BRANCH:
            queue.append(nxt)
            to(insn.target)
        elif insn.flow == m68k.JUMP:
            if insn.target is not None:
                to(insn.target)
            elif insn.mnem == "jmp_ind":
                jt_scan = scan_jump_table(segdata, insn)
                if jt_scan:
                    for _, bra_off in jt_scan[2]:
                        labels.add(bra_off)
                        queue.append(bra_off)
                elif ot := scan_offset_table(segdata, insn):
                    for _, tgt in ot[1]:
                        to(tgt)
                elif lt := scan_lea_adda_table(segdata, insn):
                    for _, tgt in lt[2]:
                        to(tgt)
        elif insn.flow == m68k.CALL:
            queue.append(nxt)
    return insns, labels, tails


# ---------------------------------------------------------------------------
# EA code generation
# ---------------------------------------------------------------------------

class Emit:
    def __init__(self, seg, insn, vbase):
        self.seg = seg
        self.insn = insn
        self.vbase = vbase
        self.stmts = []
        self.tmp = 0

    def t(self):
        self.tmp += 1
        return f"t{self.tmp}"

    def __call__(self, s):
        self.stmts.append(s)


def step_for(ea, size):
    # byte ops on A7 keep the stack word-aligned
    return 2 if size == 1 and ea.reg == 7 else size


def ea_addr(e: Emit, ea: EA, size: int) -> str:
    """Emit address computation; returns C++ expr (uint32_t)."""
    if ea.kind == "ind":
        return f"cpu.a[{ea.reg}]"
    if ea.kind == "postinc":
        v = e.t()
        e(f"uint32_t {v} = cpu.a[{ea.reg}]; cpu.a[{ea.reg}] += {step_for(ea, size)};")
        return v
    if ea.kind == "predec":
        e(f"cpu.a[{ea.reg}] -= {step_for(ea, size)};")
        return f"cpu.a[{ea.reg}]"
    if ea.kind == "disp16":
        return f"(cpu.a[{ea.reg}] + {ea.disp})"
    if ea.kind in ("index", "pcindex"):
        if ea.kind == "index":
            base = f"cpu.a[{ea.reg}] + {ea.disp}"
        else:
            base = f"0x{(e.vbase + e.insn.addr + 2 + ea.disp) & 0xFFFFFFFF:x}u"
        idx = "0u" if ea.idx_none else index_expr(ea)
        if not ea.memind:
            return f"({base} + {idx})"
        v = e.t()
        if ea.memind == 1:      # pre-indexed: indirect through base+bd+idx
            e(f"uint32_t {v} = mem_read<uint32_t>({base} + {idx}) "
              f"+ {ea.od};")
        else:                   # post-indexed: index added after the load
            e(f"uint32_t {v} = mem_read<uint32_t>({base}) + {idx} "
              f"+ {ea.od};")
        return v
    if ea.kind == "absw" or ea.kind == "absl":
        return f"0x{ea.disp & 0xFFFFFFFF:x}u"
    if ea.kind == "pcdisp":
        return f"0x{(e.vbase + e.insn.addr + 2 + ea.disp) & 0xFFFFFFFF:x}u"
    raise Unsupported(f"ea_addr {ea.kind}")


def index_expr(ea: EA) -> str:
    r = ea.idx_reg
    base = f"cpu.d[{r}]" if r < 8 else f"cpu.a[{r - 8}]"
    if not ea.idx_long:
        base = f"uint32_t(int32_t(int16_t({base})))"
    if ea.idx_scale != 1:
        base = f"({base} * {ea.idx_scale})"
    return base


def ea_read(e: Emit, ea: EA, size: int, signed=False) -> str:
    T = TYPES[size]
    if ea.kind == "dreg":
        v = f"dreg_get<{T}>(cpu, {ea.reg})"
    elif ea.kind == "areg":
        v = f"{T}(cpu.a[{ea.reg}])"
    elif ea.kind == "imm":
        v = f"{T}(0x{ea.disp & (2**(size*8)-1):x}u)"
    else:
        v = f"mem_read<{T}>({ea_addr(e, ea, size)})"
    return f"{STYPES[size]}({v})" if signed else v


def ea_rmw(e: Emit, ea: EA, size: int):
    """Read-modify-write: returns (value_expr, write(valexpr)->None)."""
    T = TYPES[size]
    if ea.kind == "dreg":
        return (f"dreg_get<{T}>(cpu, {ea.reg})",
                lambda v: e(f"dreg_set<{T}>(cpu, {ea.reg}, {v});"))
    if ea.kind == "areg":
        return (f"{T}(cpu.a[{ea.reg}])",
                lambda v: e(f"cpu.a[{ea.reg}] = uint32_t({v});"))
    addr = ea_addr(e, ea, size)
    av = e.t()
    e(f"uint32_t {av} = {addr};")
    return (f"mem_read<{T}>({av})",
            lambda v: e(f"mem_write<{T}>({av}, {v});"))


def ea_write(e: Emit, ea: EA, size: int, val: str):
    T = TYPES[size]
    if ea.kind == "dreg":
        e(f"dreg_set<{T}>(cpu, {ea.reg}, {val});")
    elif ea.kind == "areg":
        e(f"cpu.a[{ea.reg}] = uint32_t({val});")
    else:
        e(f"mem_write<{T}>({ea_addr(e, ea, size)}, {val});")


# ---------------------------------------------------------------------------
# per-instruction translation
# ---------------------------------------------------------------------------

def translate(e: Emit, seg, insn, data, ctx):
    """Append C++ statements for one instruction to e. May raise Unsupported."""
    op = insn.words[0]
    pc = insn.addr
    hi = op >> 12
    mode, reg = (op >> 3) & 7, op & 7
    vnext = e.vbase + pc + insn.size

    def ea_at(off_words, m, r, size):
        return parse_ea(data, pc + 2 + off_words * 2, m, r, size)

    def ret_native():
        e("cpu.a[7] += 4; return;")

    def call_fn(tseg, toff):
        ctx["called"].add((tseg, toff))
        e(f"push32(cpu, 0x{vnext:x}u);")
        e(f"{fn_name(tseg, toff)}(cpu);")

    def tail_fn(tseg, toff):
        ctx["called"].add((tseg, toff))
        e(f"{fn_name(tseg, toff)}(cpu); return;")

    def resolve_branch(tgt):
        """Branch-target classification mirroring local_walk: returns a C++
        statement transferring control to tgt."""
        if tgt != ctx["entry"] and (seg, tgt) in ctx["entries"]:
            ctx["called"].add((seg, tgt))
            return f"{{ {fn_name(seg, tgt)}(cpu); return; }}"
        if tgt in ctx["insns"]:
            return f"goto L{tgt:x};"
        return (f"{{ unimplemented(cpu, \"branch-hole\", "
                f"0x{(e.vbase + tgt) & 0xFFFFFFFF:x}u); return; }}")

    # ----- A-line -----
    if hi == 0xA:
        if op in m68k.NO_RETURN_TRAPS:
            e(f"cpu.pc = 0x{e.vbase + pc:x}u; do_trap(cpu, 0x{op:04X}); return; /* {trap_name(op)} */")
        elif 0xAC00 <= op <= 0xAFFF:  # auto-pop
            e(f"cpu.pc = 0x{e.vbase + pc:x}u; (void)pop32(cpu); do_trap(cpu, 0x{op:04X}); return; /* {trap_name(op)} */")
        else:
            e(f"cpu.pc = 0x{vnext:x}u; do_trap(cpu, 0x{op:04X}); /* {trap_name(op)} */")
        return

    # ----- branches -----
    if hi == 0x6:
        cc = (op >> 8) & 0xF
        if cc == 1:  # BSR
            call_fn(seg, insn.target)
            return
        if cc == 0:  # BRA
            e(resolve_branch(insn.target))
            return
        cond = f"cc_{m68k.COND[cc]}(cpu)"
        e(f"if ({cond}) {resolve_branch(insn.target)}")
        return

    if hi == 0x5:
        ss = (op >> 6) & 3
        if ss == 3 and mode == 1:  # DBcc
            cc = (op >> 8) & 0xF
            cond = "false" if cc == 1 else f"cc_{m68k.COND[cc]}(cpu)"
            e(f"if (!({cond})) {{")
            e(f"  uint16_t w = uint16_t(cpu.d[{reg}]) - 1;")
            e(f"  dreg_set<uint16_t>(cpu, {reg}, w);")
            e(f"  if (w != 0xFFFF) {resolve_branch(insn.target)}")
            e("}")
            return
        if ss == 3:  # Scc
            cc = (op >> 8) & 0xF
            cond = {0: "true", 1: "false"}.get(cc, f"cc_{m68k.COND[cc]}(cpu)")
            ea = ea_at(0, mode, reg, 1)
            ea_write(e, ea, 1, f"uint8_t(({cond}) ? 0xFF : 0x00)")
            return
        # ADDQ/SUBQ
        data3 = (op >> 9) & 7 or 8
        size = m68k._SIZES[ss]
        sub = bool(op & 0x100)
        ea = ea_at(0, mode, reg, size)
        if ea.kind == "areg":  # no flags, always full 32-bit
            e(f"cpu.a[{reg}] {'-' if sub else '+'}= {data3};")
            return
        val, wr = ea_rmw(e, ea, size)
        T = TYPES[size]
        wr(f"op_{'sub' if sub else 'add'}<{T}>(cpu, {val}, {T}({data3}))")
        return

    # ----- MOVE -----
    if hi in (1, 2, 3):
        size = {1: 1, 3: 2, 2: 4}[hi]
        src = ea_at(0, mode, reg, size)
        dmode, dreg_n = (op >> 6) & 7, (op >> 9) & 7
        dst = ea_at(src.words, dmode, dreg_n, size)
        sval = ea_read(e, src, size)
        if dst.kind == "areg":  # MOVEA: sign-extend word, no flags
            if size == 2:
                e(f"cpu.a[{dreg_n}] = uint32_t(int32_t(int16_t({sval})));")
            else:
                e(f"cpu.a[{dreg_n}] = {sval};")
            return
        v = e.t()
        e(f"{TYPES[size]} {v} = {sval};")
        ea_write(e, dst, size, v)
        e(f"set_nz_clear_vc(cpu, {v});")
        return

    if hi == 0x7:  # MOVEQ
        d = (op >> 9) & 7
        val = op & 0xFF
        sval = val - 256 if val & 0x80 else val
        e(f"cpu.d[{d}] = uint32_t(int32_t({sval}));")
        e(f"set_nz_clear_vc(cpu, uint32_t(int32_t({sval})));")
        return

    # ----- group 4 misc -----
    if hi == 0x4:
        if insn.mnem == "lea":
            an = (op >> 9) & 7
            ea = ea_at(0, mode, reg, 0)
            e(f"cpu.a[{an}] = uint32_t({ea_addr(e, ea, 0)});")
            return
        if insn.mnem == "pea":
            ea = ea_at(0, mode, reg, 0)
            e(f"push32(cpu, uint32_t({ea_addr(e, ea, 0)}));")
            return
        if insn.mnem == "link":
            disp = m68k._s16(insn.words[1])
            e(f"push32(cpu, cpu.a[{reg}]);")
            e(f"cpu.a[{reg}] = cpu.a[7];")
            e(f"cpu.a[7] += {disp};")
            return
        if insn.mnem == "unlk":
            e(f"cpu.a[7] = cpu.a[{reg}];")
            e(f"cpu.a[{reg}] = pop32(cpu);")
            return
        if insn.mnem == "rts":
            ret_native()
            return
        if insn.mnem == "rtd":
            disp = m68k._s16(insn.words[1])
            e(f"cpu.a[7] += {4 + disp}; return;")
            return
        if insn.mnem == "nop":
            return
        if insn.mnem in ("jsr", "jsr_a5", "jsr_ind", "jsr_abs"):
            if insn.mnem == "jsr":
                call_fn(seg, insn.target)
            elif insn.mnem == "jsr_a5":
                tgt = ctx["jt"].get(insn.a5_call)
                if tgt is None:
                    raise Unsupported(f"jsr a5+{insn.a5_call} not in JT")
                call_fn(*tgt)
            else:
                ea = ea_at(0, mode, reg, 0)
                addr = ea_addr(e, ea, 0)
                e(f"push32(cpu, 0x{vnext:x}u);")
                e(f"call_virtual(cpu, uint32_t({addr}));")
            return
        if insn.mnem in ("jmp", "jmp_a5", "jmp_ind"):
            if insn.mnem == "jmp":
                e(resolve_branch(insn.target))
            elif insn.mnem == "jmp_a5":
                tgt = ctx["jt"].get(insn.a5_call)
                if tgt is None:
                    raise Unsupported(f"jmp a5+{insn.a5_call} not in JT")
                tail_fn(*tgt)
            else:
                jt_scan = scan_jump_table(data, insn)
                ot_scan = None if jt_scan else scan_offset_table(data, insn)
                lt_scan = (None if jt_scan or ot_scan
                           else scan_lea_adda_table(data, insn))
                if jt_scan:  # computed switch: JMP d8(PC,Dn.W) over BRA table
                    idx_reg, base, cases = jt_scan
                    e(f"switch (int32_t(int16_t(cpu.d[{idx_reg}]))) {{")
                    for case_val, bra_off in cases:
                        e(f"case {case_val}: {resolve_branch(bra_off)}")
                    e("}")
                    e(f"unimplemented(cpu, \"switch-index\", 0x{(e.vbase + pc) & 0xFFFFFFFF:x}u); return;")
                elif ot_scan:  # switch via offset-word table: key = loaded word
                    idx_reg, cases = ot_scan
                    seen = set()
                    e(f"switch (int32_t(int16_t(cpu.d[{idx_reg}]))) {{")
                    for word, tgt in cases:
                        if word in seen:
                            continue
                        seen.add(word)
                        e(f"case {word}: {resolve_branch(tgt)}")
                    e("}")
                    e(f"unimplemented(cpu, \"switch-offset\", 0x{(e.vbase + pc) & 0xFFFFFFFF:x}u); return;")
                elif lt_scan:  # switch via LEA/ADDA table: key = An (vaddr)
                    an, _, cases = lt_scan
                    seen = set()
                    e(f"switch (cpu.a[{an}]) {{")
                    for _, tgt in cases:
                        va = (e.vbase + tgt) & 0xFFFFFFFF
                        if va in seen:
                            continue
                        seen.add(va)
                        e(f"case 0x{va:x}u: {resolve_branch(tgt)}")
                    e("}")
                    e(f"unimplemented(cpu, \"switch-lea\", 0x{(e.vbase + pc) & 0xFFFFFFFF:x}u); return;")
                else:
                    ea = ea_at(0, mode, reg, 0)
                    addr = ea_addr(e, ea, 0)
                    e(f"jump_virtual(cpu, uint32_t({addr})); return;")
            return
        if insn.mnem in ("clr", "neg", "negx", "not", "tst"):
            ss = (op >> 6) & 3
            size = m68k._SIZES[ss]
            T = TYPES[size]
            ea = ea_at(0, mode, reg, size)
            if insn.mnem == "clr":
                ea_write(e, ea, size, f"{T}(0)")
                e("cpu.n = false; cpu.z = true; cpu.v = false; cpu.c = false;")
            elif insn.mnem == "tst":
                e(f"set_nz_clear_vc(cpu, {ea_read(e, ea, size)});")
            elif insn.mnem == "neg":
                val, wr = ea_rmw(e, ea, size)
                wr(f"op_neg<{T}>(cpu, {val})")
            elif insn.mnem == "not":
                val, wr = ea_rmw(e, ea, size)
                v = e.t()
                e(f"{T} {v} = {T}(~({val}));")
                wr(v)
                e(f"set_nz_clear_vc(cpu, {v});")
            else:  # negx: 0 - dst - X (op_subx has the Z-accumulate rule)
                val, wr = ea_rmw(e, ea, size)
                wr(f"op_subx<{T}>(cpu, {T}(0), {val})")
            return
        if insn.mnem == "ext":
            d = reg
            if op & 0x40:  # ext.l: word -> long
                e(f"cpu.d[{d}] = uint32_t(int32_t(int16_t(cpu.d[{d}])));")
                e(f"set_nz_clear_vc(cpu, cpu.d[{d}]);")
            else:          # ext.w: byte -> word
                e(f"dreg_set<uint16_t>(cpu, {d}, uint16_t(int16_t(int8_t(cpu.d[{d}]))));")
                e(f"set_nz_clear_vc(cpu, uint16_t(cpu.d[{d}]));")
            return
        if insn.mnem == "extb.l":
            e(f"cpu.d[{reg}] = uint32_t(int32_t(int8_t(cpu.d[{reg}])));")
            e(f"set_nz_clear_vc(cpu, cpu.d[{reg}]);")
            return
        if insn.mnem == "swap":
            e(f"cpu.d[{reg}] = (cpu.d[{reg}] >> 16) | (cpu.d[{reg}] << 16);")
            e(f"set_nz_clear_vc(cpu, cpu.d[{reg}]);")
            return
        if insn.mnem == "movem":
            to_mem = not (op & 0x400)
            size = 4 if op & 0x40 else 2
            T = TYPES[size]
            mask = insn.words[1]
            ea = ea_at(1, mode, reg, size)
            regs = []
            if ea.kind == "predec":
                for i in range(16):
                    if mask & (1 << i):
                        regs.append(15 - i)   # bit0=A7 ... bit15=D0
            else:
                for i in range(16):
                    if mask & (1 << i):
                        regs.append(i)        # bit0=D0 ... bit15=A7
            def regref(idx):
                return f"cpu.d[{idx}]" if idx < 8 else f"cpu.a[{idx - 8}]"
            if to_mem:
                if ea.kind == "predec":
                    for r_ in regs:
                        e(f"cpu.a[{reg}] -= {size};")
                        e(f"mem_write<{T}>(cpu.a[{reg}], {T}({regref(r_)}));")
                else:
                    base = e.t()
                    e(f"uint32_t {base} = {ea_addr(e, ea, size)};")
                    for k, r_ in enumerate(regs):
                        e(f"mem_write<{T}>({base} + {k * size}, {T}({regref(r_)}));")
            else:
                if ea.kind == "postinc":
                    for r_ in regs:
                        if size == 2:
                            e(f"{regref(r_)} = uint32_t(int32_t(int16_t(mem_read<uint16_t>(cpu.a[{reg}]))));")
                        else:
                            e(f"{regref(r_)} = mem_read<uint32_t>(cpu.a[{reg}]);")
                        e(f"cpu.a[{reg}] += {size};")
                else:
                    base = e.t()
                    e(f"uint32_t {base} = {ea_addr(e, ea, size)};")
                    for k, r_ in enumerate(regs):
                        if size == 2:
                            e(f"{regref(r_)} = uint32_t(int32_t(int16_t(mem_read<uint16_t>({base} + {k * size}))));")
                        else:
                            e(f"{regref(r_)} = mem_read<uint32_t>({base} + {k * size});")
            return
        if insn.mnem == "move_srccr":
            sub8 = op >> 8
            ss = (op >> 6) & 3
            if (op & 0xFFC0) == 0x40C0:   # MOVE from SR
                ea = ea_at(0, mode, reg, 2)
                ea_write(e, ea, 2, "get_ccr(cpu)")
                return
            if (op & 0xFFC0) == 0x44C0:   # MOVE to CCR
                ea = ea_at(0, mode, reg, 2)
                e(f"set_ccr(cpu, {ea_read(e, ea, 2)});")
                return
            if (op & 0xFFC0) == 0x46C0:   # MOVE to SR: keep the CCR half
                ea = ea_at(0, mode, reg, 2)
                e(f"set_ccr(cpu, {ea_read(e, ea, 2)});")
                return
            raise Unsupported("move to SR")
        if insn.mnem == "muldiv.l":
            ext = insn.words[1]
            dl = (ext >> 12) & 7
            signed = bool(ext & 0x800)
            sz64 = bool(ext & 0x400)
            is_div = bool(op & 0x40)
            if sz64:
                raise Unsupported("64-bit mul/div")
            ea = ea_at(1, mode, reg, 4)
            src = ea_read(e, ea, 4)
            if is_div:
                dh = ext & 7
                sv = e.t()
                e(f"uint32_t {sv} = {src};")
                e(f"if ({sv} == 0) fatal(\"div.l by zero at %x\", 0x{e.vbase + pc:x});")
                if signed:
                    q = e.t(); r_ = e.t()
                    e(f"int32_t {q} = int32_t(cpu.d[{dl}]) / int32_t({sv});")
                    e(f"int32_t {r_} = int32_t(cpu.d[{dl}]) % int32_t({sv});")
                    e(f"cpu.d[{dl}] = uint32_t({q});")
                    if dh != dl:
                        e(f"cpu.d[{dh}] = uint32_t({r_});")
                    e(f"set_nz_clear_vc(cpu, uint32_t({q}));")
                else:
                    q = e.t(); r_ = e.t()
                    e(f"uint32_t {q} = cpu.d[{dl}] / {sv};")
                    e(f"uint32_t {r_} = cpu.d[{dl}] % {sv};")
                    e(f"cpu.d[{dl}] = {q};")
                    if dh != dl:
                        e(f"cpu.d[{dh}] = {r_};")
                    e(f"set_nz_clear_vc(cpu, {q});")
            else:
                if signed:
                    e(f"cpu.d[{dl}] = uint32_t(int32_t(cpu.d[{dl}]) * int32_t({src}));")
                else:
                    e(f"cpu.d[{dl}] = cpu.d[{dl}] * {src};")
                e(f"set_nz_clear_vc(cpu, cpu.d[{dl}]);")
            return
        if insn.mnem == "chk":
            # CHK bounds trap not modeled; keep the EA read side effects
            ea = ea_at(0, mode, reg, 2)
            e(f"(void)({ea_read(e, ea, 2)});")
            return
        if insn.mnem == "bitop_imm" or insn.mnem == "bitop_dyn":
            pass  # handled in group 0 below via fallthrough
        raise Unsupported(insn.mnem)

    # ----- group 0: immediates & bit ops -----
    if hi == 0x0:
        if insn.mnem == "movep":
            # MOVEP: alternating-byte transfers (drivers banging VIA/ASC)
            dx = (op >> 9) & 7
            ay = op & 7
            opmode = (op >> 6) & 7
            d16 = insn.words[1]
            if d16 & 0x8000:
                d16 -= 0x10000
            a = e.t()
            e(f"uint32_t {a} = cpu.a[{ay}] + uint32_t(int32_t({d16}));")
            if opmode == 4:    # word, mem -> reg
                e(f"dreg_set<uint16_t>(cpu, {dx}, uint16_t("
                  f"(uint16_t(mem_read<uint8_t>({a})) << 8) | "
                  f"mem_read<uint8_t>({a} + 2)));")
            elif opmode == 5:  # long, mem -> reg
                e(f"cpu.d[{dx}] = (uint32_t(mem_read<uint8_t>({a})) << 24) | "
                  f"(uint32_t(mem_read<uint8_t>({a} + 2)) << 16) | "
                  f"(uint32_t(mem_read<uint8_t>({a} + 4)) << 8) | "
                  f"mem_read<uint8_t>({a} + 6);")
            elif opmode == 6:  # word, reg -> mem
                e(f"mem_write<uint8_t>({a}, uint8_t(cpu.d[{dx}] >> 8));")
                e(f"mem_write<uint8_t>({a} + 2, uint8_t(cpu.d[{dx}]));")
            else:              # long, reg -> mem
                e(f"mem_write<uint8_t>({a}, uint8_t(cpu.d[{dx}] >> 24));")
                e(f"mem_write<uint8_t>({a} + 2, uint8_t(cpu.d[{dx}] >> 16));")
                e(f"mem_write<uint8_t>({a} + 4, uint8_t(cpu.d[{dx}] >> 8));")
                e(f"mem_write<uint8_t>({a} + 6, uint8_t(cpu.d[{dx}]));")
            return
        if insn.mnem in ("bitop_dyn", "bitop_imm"):
            kind = (op >> 6) & 3   # 0=BTST 1=BCHG 2=BCLR 3=BSET
            if insn.mnem == "bitop_dyn":
                bn = f"cpu.d[{(op >> 9) & 7}]"
                ea = ea_at(0, mode, reg, 1)
            else:
                bn = str(insn.words[1] & 0xFF)
                ea = ea_at(1, mode, reg, 1)
            if ea.kind == "dreg":
                b = e.t()
                e(f"unsigned {b} = ({bn}) & 31;")
                e(f"cpu.z = !((cpu.d[{ea.reg}] >> {b}) & 1);")
                if kind == 1:
                    e(f"cpu.d[{ea.reg}] ^= (1u << {b});")
                elif kind == 2:
                    e(f"cpu.d[{ea.reg}] &= ~(1u << {b});")
                elif kind == 3:
                    e(f"cpu.d[{ea.reg}] |= (1u << {b});")
            else:
                b = e.t()
                e(f"unsigned {b} = ({bn}) & 7;")
                val, wr = ea_rmw(e, ea, 1)
                v = e.t()
                e(f"uint8_t {v} = {val};")
                e(f"cpu.z = !(({v} >> {b}) & 1);")
                if kind == 1:
                    wr(f"uint8_t({v} ^ (1u << {b}))")
                elif kind == 2:
                    wr(f"uint8_t({v} & ~(1u << {b}))")
                elif kind == 3:
                    wr(f"uint8_t({v} | (1u << {b}))")
            return
        if insn.mnem.endswith("_ccr"):
            imm = insn.words[1] & 0x1F
            opn = insn.mnem[:-4]
            cur = "get_ccr(cpu)"
            if opn == "ori":
                e(f"set_ccr(cpu, uint16_t({cur} | {imm}));")
            elif opn == "andi":
                e(f"set_ccr(cpu, uint16_t({cur} & {imm}));")
            else:
                e(f"set_ccr(cpu, uint16_t({cur} ^ {imm}));")
            return
        if insn.mnem.endswith("_sr"):
            # ORI/ANDI/EORI #imm,SR: interrupt-mask fiddling — no-op here,
            # but apply the CCR half (low byte) for flag correctness.
            imm = insn.words[1]
            opn = insn.mnem[:-3]
            cur = "get_ccr(cpu)"
            if opn == "ori":
                e(f"set_ccr(cpu, uint16_t({cur} | {imm & 0x1F}));")
            elif opn == "andi":
                e(f"set_ccr(cpu, uint16_t({cur} & {imm & 0x1F}));")
            else:
                e(f"set_ccr(cpu, uint16_t({cur} ^ {imm & 0x1F}));")
            return
        # ORI/ANDI/SUBI/ADDI/EORI/CMPI #imm,EA
        sub = (op >> 9) & 7
        ss = (op >> 6) & 3
        size = m68k._SIZES[ss]
        T = TYPES[size]
        immw = 2 if size == 4 else 1
        if size == 4:
            imm = (insn.words[1] << 16) | insn.words[2]
        else:
            imm = insn.words[1] & (0xFF if size == 1 else 0xFFFF)
        ea = ea_at(immw, mode, reg, size)
        lit = f"{T}(0x{imm:x}u)"
        if sub == 6:  # CMPI
            e(f"op_cmp<{T}>(cpu, {ea_read(e, ea, size)}, {lit});")
            return
        val, wr = ea_rmw(e, ea, size)
        if sub == 0:
            v = e.t(); e(f"{T} {v} = {T}({val} | {lit});"); wr(v)
            e(f"set_nz_clear_vc(cpu, {v});")
        elif sub == 1:
            v = e.t(); e(f"{T} {v} = {T}({val} & {lit});"); wr(v)
            e(f"set_nz_clear_vc(cpu, {v});")
        elif sub == 5:
            v = e.t(); e(f"{T} {v} = {T}({val} ^ {lit});"); wr(v)
            e(f"set_nz_clear_vc(cpu, {v});")
        elif sub == 2:
            wr(f"op_sub<{T}>(cpu, {val}, {lit})")
        elif sub == 3:
            wr(f"op_add<{T}>(cpu, {val}, {lit})")
        return

    # ----- dyadic groups -----
    if hi in (0x8, 0x9, 0xB, 0xC, 0xD):
        omode = (op >> 6) & 7
        dn = (op >> 9) & 7
        if insn.mnem in ("adda", "suba", "cmpa"):
            size = 2 if omode == 3 else 4
            ea = ea_at(0, mode, reg, size)
            src = ea_read(e, ea, size)
            if size == 2:
                src = f"uint32_t(int32_t(int16_t({src})))"
            if insn.mnem == "adda":
                e(f"cpu.a[{dn}] += {src};")
            elif insn.mnem == "suba":
                e(f"cpu.a[{dn}] -= {src};")
            else:
                e(f"op_cmp<uint32_t>(cpu, cpu.a[{dn}], {src});")
            return
        if insn.mnem == "mul.w":
            ea = ea_at(0, mode, reg, 2)
            src = ea_read(e, ea, 2)
            if op & 0x100:  # MULS
                e(f"cpu.d[{dn}] = uint32_t(int32_t(int16_t(cpu.d[{dn}])) * int32_t(int16_t({src})));")
            else:
                e(f"cpu.d[{dn}] = uint32_t(uint16_t(cpu.d[{dn}])) * uint32_t(uint16_t({src}));")
            e(f"set_nz_clear_vc(cpu, cpu.d[{dn}]);")
            return
        if insn.mnem == "div.w":
            ea = ea_at(0, mode, reg, 2)
            src = ea_read(e, ea, 2)
            sv = e.t()
            e(f"uint16_t {sv} = {src};")
            e(f"if ({sv} == 0) fatal(\"div.w by zero at %x\", 0x{e.vbase + pc:x});")
            if op & 0x100:  # DIVS
                q = e.t(); r_ = e.t()
                e(f"int32_t {q} = int32_t(cpu.d[{dn}]) / int16_t({sv});")
                e(f"int32_t {r_} = int32_t(cpu.d[{dn}]) % int16_t({sv});")
                e(f"if ({q} > 32767 || {q} < -32768) {{ cpu.v = true; cpu.c = false; }}")
                e(f"else {{ cpu.d[{dn}] = (uint32_t(uint16_t({r_})) << 16) | uint16_t({q}); "
                  f"cpu.v = false; cpu.c = false; cpu.n = int16_t({q}) < 0; cpu.z = int16_t({q}) == 0; }}")
            else:
                q = e.t(); r_ = e.t()
                e(f"uint32_t {q} = cpu.d[{dn}] / {sv};")
                e(f"uint32_t {r_} = cpu.d[{dn}] %% {sv};".replace("%%", "%"))
                e(f"if ({q} > 0xFFFF) {{ cpu.v = true; cpu.c = false; }}")
                e(f"else {{ cpu.d[{dn}] = (uint32_t({r_}) << 16) | uint16_t({q}); "
                  f"cpu.v = false; cpu.c = false; cpu.n = int16_t({q}) < 0; cpu.z = uint16_t({q}) == 0; }}")
            return
        if insn.mnem in ("addx", "subx"):
            ss = (op >> 6) & 3
            size = m68k._SIZES[ss]
            T = TYPES[size]
            ry, rx = reg, dn
            opf = "op_addx" if insn.mnem == "addx" else "op_subx"
            if mode == 0:  # Dy,Dx
                e(f"dreg_set<{T}>(cpu, {rx}, {opf}<{T}>(cpu, dreg_get<{T}>(cpu, {rx}), dreg_get<{T}>(cpu, {ry})));")
            else:          # -(Ay),-(Ax)
                e(f"cpu.a[{ry}] -= {size};")
                sv = e.t()
                e(f"{T} {sv} = mem_read<{T}>(cpu.a[{ry}]);")
                e(f"cpu.a[{rx}] -= {size};")
                dv = e.t()
                e(f"{T} {dv} = mem_read<{T}>(cpu.a[{rx}]);")
                e(f"mem_write<{T}>(cpu.a[{rx}], {opf}<{T}>(cpu, {dv}, {sv}));")
            return
        if insn.mnem == "cmpm":
            ss = (op >> 6) & 3
            size = m68k._SIZES[ss]
            T = TYPES[size]
            sv = e.t()
            e(f"{T} {sv} = mem_read<{T}>(cpu.a[{reg}]); cpu.a[{reg}] += {step_for(EA('postinc', reg=reg), size)};")
            dv = e.t()
            e(f"{T} {dv} = mem_read<{T}>(cpu.a[{dn}]); cpu.a[{dn}] += {step_for(EA('postinc', reg=dn), size)};")
            e(f"op_cmp<{T}>(cpu, {dv}, {sv});")
            return
        if insn.mnem == "exg":
            om = (op >> 3) & 0x1F
            t = e.t()
            if om == 0x08:
                e(f"uint32_t {t} = cpu.d[{dn}]; cpu.d[{dn}] = cpu.d[{reg}]; cpu.d[{reg}] = {t};")
            elif om == 0x09:
                e(f"uint32_t {t} = cpu.a[{dn}]; cpu.a[{dn}] = cpu.a[{reg}]; cpu.a[{reg}] = {t};")
            else:
                e(f"uint32_t {t} = cpu.d[{dn}]; cpu.d[{dn}] = cpu.a[{reg}]; cpu.a[{reg}] = {t};")
            return
        if insn.mnem in ("abcd", "sbcd"):
            raise Unsupported(insn.mnem)
        # ADD/SUB/AND/OR/CMP/EOR
        ss = omode & 3
        size = m68k._SIZES[ss]
        T = TYPES[size]
        to_ea = omode >= 4
        ea = ea_at(0, mode, reg, size)
        if hi == 0xB:
            if not to_ea:  # CMP EA,Dn
                e(f"op_cmp<{T}>(cpu, dreg_get<{T}>(cpu, {dn}), {ea_read(e, ea, size)});")
            else:          # EOR Dn,EA
                val, wr = ea_rmw(e, ea, size)
                v = e.t()
                e(f"{T} {v} = {T}({val} ^ dreg_get<{T}>(cpu, {dn}));")
                wr(v)
                e(f"set_nz_clear_vc(cpu, {v});")
            return
        cop = {0x8: "|", 0xC: "&"}.get(hi)
        if cop:  # OR / AND
            if to_ea:
                val, wr = ea_rmw(e, ea, size)
                v = e.t()
                e(f"{T} {v} = {T}({val} {cop} dreg_get<{T}>(cpu, {dn}));")
                wr(v)
                e(f"set_nz_clear_vc(cpu, {v});")
            else:
                v = e.t()
                e(f"{T} {v} = {T}(dreg_get<{T}>(cpu, {dn}) {cop} {ea_read(e, ea, size)});")
                e(f"dreg_set<{T}>(cpu, {dn}, {v});")
                e(f"set_nz_clear_vc(cpu, {v});")
            return
        opf = "op_add" if hi == 0xD else "op_sub"
        if to_ea:  # EA = EA op Dn
            val, wr = ea_rmw(e, ea, size)
            wr(f"{opf}<{T}>(cpu, {val}, dreg_get<{T}>(cpu, {dn}))")
        else:      # Dn = Dn op EA
            src = ea_read(e, ea, size)
            e(f"dreg_set<{T}>(cpu, {dn}, {opf}<{T}>(cpu, dreg_get<{T}>(cpu, {dn}), {src}));")
        return

    # ----- shifts -----
    if hi == 0xE:
        ss = (op >> 6) & 3
        left = bool(op & 0x100)
        if insn.mnem == "shift":
            size = m68k._SIZES[ss]
            T = TYPES[size]
            kind = (op >> 3) & 3
            name = ["as", "ls", "rox", "ro"][kind] + ("l" if left else "r")
            if op & 0x20:
                cnt = f"(cpu.d[{(op >> 9) & 7}] & 63)"
            else:
                cnt = str((op >> 9) & 7 or 8)
            e(f"dreg_set<{T}>(cpu, {reg}, op_{name}<{T}>(cpu, dreg_get<{T}>(cpu, {reg}), {cnt}));")
            return
        if insn.mnem == "shift_mem":
            kind = (op >> 9) & 3
            name = ["as", "ls", "rox", "ro"][kind] + ("l" if left else "r")
            ea = ea_at(0, mode, reg, 2)
            val, wr = ea_rmw(e, ea, 2)
            wr(f"op_{name}<uint16_t>(cpu, {val}, 1)")
            return
        if insn.mnem == "bitfield":
            # 68020 bitfields. sub-op in opcode bits 11-8; ext word:
            # Dn(14-12), Do(11), offset(10-6), Dw(5), width(4-0).
            sub = (op >> 8) & 0xF
            ext = (data[pc + 2] << 8) | data[pc + 3]
            dn = (ext >> 12) & 7
            offx = (f"int32_t(cpu.d[{(ext >> 6) & 7}])"
                    if ext & 0x800 else str((ext >> 6) & 31))
            wx = (f"((int(cpu.d[{ext & 7}] - 1) & 31) + 1)"
                  if ext & 0x20 else str(((ext & 31) - 1) % 32 + 1))
            o, w, f = e.t(), e.t(), e.t()
            e(f"int32_t {o} = {offx}; int {w} = {wx};")
            if mode == 0:   # register field (wraps, offset mod 32)
                getf = f"bf_reg_get(cpu.d[{reg}], {o}, {w})"
                putf = lambda v: (
                    f"cpu.d[{reg}] = bf_reg_put(cpu.d[{reg}], {o}, {w}, {v});")
            else:           # memory field
                ea = ea_at(1, mode, reg, 0)
                addr = ea_addr(e, ea, 0)
                a = e.t()
                e(f"uint32_t {a} = uint32_t({addr});")
                getf = f"bf_mem_get({a}, {o}, {w})"
                putf = lambda v: f"bf_mem_put({a}, {o}, {w}, {v});"
            if sub == 0xF:  # BFINS: flags come from the inserted field
                e(f"uint32_t {f} = cpu.d[{dn}] & bf_mask({w});")
            else:
                e(f"uint32_t {f} = {getf};")
            e(f"cpu.n = ({f} >> ({w} - 1)) & 1; cpu.z = {f} == 0; "
              f"cpu.v = false; cpu.c = false;")
            if sub == 0x9:      # BFEXTU
                e(f"cpu.d[{dn}] = {f};")
            elif sub == 0xB:    # BFEXTS
                s = e.t()
                e(f"uint32_t {s} = 1u << ({w} - 1);")
                e(f"cpu.d[{dn}] = ({f} ^ {s}) - {s};")
            elif sub == 0xA:    # BFCHG
                e(putf(f"~{f}"))
            elif sub == 0xC:    # BFCLR
                e(putf("0u"))
            elif sub == 0xE:    # BFSET
                e(putf("0xFFFFFFFFu"))
            elif sub == 0xD:    # BFFFO
                e(f"int i_{f} = 0;")
                e(f"while (i_{f} < {w} && !(({f} >> ({w} - 1 - i_{f})) & 1)) "
                  f"i_{f}++;")
                e(f"cpu.d[{dn}] = uint32_t({o} + i_{f});")
            elif sub == 0xF:    # BFINS
                e(putf(f"cpu.d[{dn}]"))
            # sub 0x8 = BFTST: flags only
            return
        raise Unsupported(insn.mnem)

    raise Unsupported(insn.mnem)


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

HEADER = """\
// GENERATED by recomp.py — do not edit.
#include "pop2/cpu.h"
#include "gen_decls.h"
using namespace pop2;
"""


def write_if_changed(path: Path, content: str):
    """Skip rewriting identical files so ninja recompiles only real changes."""
    if path.exists() and path.read_text() == content:
        return
    path.write_text(content)


def generate(code_dir: Path, out_dir: Path):
    segs = load_segments(code_dir)
    entries, cache, jt = discover(segs)
    out_dir.mkdir(parents=True, exist_ok=True)

    stats = {"insns": 0, "ok": 0, "unimpl": defaultdict(int)}
    all_fns = []          # (seg, off)
    called = set()

    decls = ["// GENERATED — function declarations", "#pragma once",
             '#include "pop2/cpu.h"', "namespace pop2 {",
             "struct JtEntry { uint16_t a5off; uint32_t vaddr; void (*fn)(Cpu&); };",
             "struct VFnEntry { uint32_t vaddr; void (*fn)(Cpu&); };",
             "extern const JtEntry g_jt_entries[]; extern const size_t g_jt_count;",
             "extern const VFnEntry g_vfns[]; extern const size_t g_vfn_count;"]
    for seg, off in sorted(entries):
        decls.append(f"void {fn_name(seg, off)}(Cpu&);")
    decls.append("}")

    by_seg = defaultdict(list)
    for seg, off in entries:
        by_seg[seg].append(off)

    for seg in sorted(by_seg):
        name, data = segs[seg]
        vbase = SEG_VBASE(seg)
        lines = [HEADER, f"// segment {seg} {name!r}", "namespace pop2 {"]
        for entry in sorted(by_seg[seg]):
            insns, labels, tails = local_walk(seg, entry, cache, entries, data)
            ctx = {"insns": insns, "jt": jt, "called": called,
                   "entry": entry, "entries": entries}
            lines.append(f"\nvoid {fn_name(seg, entry)}(Cpu& cpu) {{")
            lines.append(f'  GUEST_TRACE("{fn_name(seg, entry)}");')
            # local_walk may pull in blocks that live BELOW the entry point
            # (shared tails reached by backward branches, e.g. a common
            # "bump counter; rts" epilogue). Instructions are emitted in
            # address order, so without an explicit jump the function would
            # start executing the pulled-in tail instead of its entry.
            if insns and min(insns) < entry:
                lines.append(f"  goto L{entry:x};")
            for off in sorted(insns):
                insn = insns[off]
                stats["insns"] += 1
                if off in labels or off == entry:
                    lines.append(f"L{off:x}: (void)0;")
                em = Emit(seg, insn, vbase)
                try:
                    translate(em, seg, insn, data, ctx)
                    stats["ok"] += 1
                    body = " ".join(em.stmts)
                    # raw reads of low-mem Ticks (0x16A) must see live time:
                    # the MDRV synth busy-waits on it for its CPU benchmark
                    body = body.replace("mem_read<uint32_t>(0x16au)",
                                        "ticks_live()")
                    lines.append(f"  /*{off:05x} {insn.mnem}*/ {{ {body} }}")
                except (Unsupported, m68k.DecodeError) as ex:
                    stats["unimpl"][str(ex)] += 1
                    lines.append(
                        f"  /*{off:05x}*/ unimplemented(cpu, \"{insn.mnem}\", "
                        f"0x{vbase + off:x}u); return;")
            lines.append("}")
        lines.append("}  // namespace pop2")
        (out_dir / f"seg{seg:02d}.cpp").write_text("\n".join(lines))

    # dispatch tables
    tbl = [HEADER, "namespace pop2 {",
           "const JtEntry g_jt_entries[] = {"]
    for a5off, (seg, off) in sorted(jt.items()):
        if (seg, off) in entries:
            tbl.append(f"  {{{a5off}, 0x{SEG_VBASE(seg) + off:x}u, &{fn_name(seg, off)}}},")
    tbl.append("};")
    tbl.append("const size_t g_jt_count = sizeof(g_jt_entries)/sizeof(g_jt_entries[0]);")
    tbl.append("const VFnEntry g_vfns[] = {")
    for seg, off in sorted(entries):
        tbl.append(f"  {{0x{SEG_VBASE(seg) + off:x}u, &{fn_name(seg, off)}}},")
    tbl.append("};")
    tbl.append("const size_t g_vfn_count = sizeof(g_vfns)/sizeof(g_vfns[0]);")
    tbl.append("}  // namespace pop2")
    (out_dir / "gen_tables.cpp").write_text("\n".join(tbl))

    (out_dir / "gen_decls.h").write_text("\n".join(decls))

    # trap display names for runtime diagnostics
    from analyze_code import TRAP_NAMES
    tn = [HEADER, "namespace pop2 {",
          "const char* trap_display_name(uint16_t op) {",
          "  switch (op) {"]
    for opn, nm in sorted(TRAP_NAMES.items()):
        tn.append(f'    case 0x{opn:04X}: return "{nm}";')
    tn.append('    default: return "?";')
    tn.append("  }\n}\n}  // namespace pop2")
    (out_dir / "gen_trap_names.cpp").write_text("\n".join(tn))

    total = stats["insns"]
    ok = stats["ok"]
    print(f"functions: {len(entries)}")
    print(f"instructions: {total}, translated: {ok} ({100.0*ok/total:.2f}%)")
    if stats["unimpl"]:
        print("top unimplemented:")
        for what, n in sorted(stats["unimpl"].items(), key=lambda kv: -kv[1])[:15]:
            print(f"  {n:>6}  {what}")


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
