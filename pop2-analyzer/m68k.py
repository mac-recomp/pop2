"""M68k instruction decoder: lengths, mnemonics, control flow.

Scope: user-mode 68000/68010/68020 instructions as emitted by mid-90s Mac
compilers (Think C / CodeWarrior / MPW). Decodes enough for recursive-descent
discovery and later recompilation: exact instruction length, flow kind,
branch/call targets, A-line traps, A5-relative jump-table calls.

Reference: Motorola M68000 Family Programmer's Reference Manual.
"""

from dataclasses import dataclass, field

# flow kinds
SEQ = "seq"          # falls through
BRANCH = "branch"    # conditional: target + fallthrough
JUMP = "jump"        # unconditional: target (None = indirect)
CALL = "call"        # subroutine call: target (None = indirect) + fallthrough
RET = "ret"          # RTS/RTE/RTR/RTD
ALINE = "aline"      # A-line trap
ILLEGAL = "illegal"  # undecodable / unsupported

COND = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
        "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]

NO_RETURN_TRAPS = {0xA9F4, 0xA9F2}  # _ExitToShell, _Launch


@dataclass
class Insn:
    addr: int
    size: int                  # bytes, including extensions
    mnem: str
    flow: str = SEQ
    target: int | None = None  # branch/jump/call target (segment-local offset)
    trap: int | None = None    # A-line opcode
    a5_call: int | None = None # JSR/JMP d16(A5): the A5 displacement
    cond: str | None = None
    words: tuple = field(default_factory=tuple)  # raw opcode words


class DecodeError(Exception):
    pass


def _r16(mem: bytes, off: int) -> int:
    if off < 0 or off + 2 > len(mem):
        raise DecodeError(f"read out of range at {off:#x}")
    return (mem[off] << 8) | mem[off + 1]


def _s8(v):  return v - 256 if v & 0x80 else v
def _s16(v): return v - 65536 if v & 0x8000 else v


def _ea_ext_words(mode: int, reg: int, size: int, mem: bytes, off: int) -> int:
    """Number of extension WORDS for an effective address.

    size: operand bytes (1/2/4) — used for immediate mode.
    off: position right after opword (start of this EA's extensions).
    """
    if mode in (0, 1, 2, 3, 4):
        return 0
    if mode == 5:                       # d16(An)
        return 1
    if mode == 6:                       # d8(An,Xn) brief or full (68020)
        ext = _r16(mem, off)
        if ext & 0x100:                 # full extension word
            n = 1
            bd = (ext >> 4) & 3
            od = ext & 3
            n += (0, 0, 1, 2)[bd]
            n += (0, 0, 1, 2)[od]
            return n
        return 1
    if mode == 7:
        if reg == 0:                    # abs.W
            return 1
        if reg == 1:                    # abs.L
            return 2
        if reg == 2:                    # d16(PC)
            return 1
        if reg == 3:                    # d8(PC,Xn) brief/full
            ext = _r16(mem, off)
            if ext & 0x100:
                n = 1
                n += (0, 0, 1, 2)[(ext >> 4) & 3]
                n += (0, 0, 1, 2)[ext & 3]
                return n
            return 1
        if reg == 4:                    # #imm
            return 2 if size == 4 else 1
    raise DecodeError(f"bad EA mode {mode}/{reg}")


def _ea_target(mode: int, reg: int, mem: bytes, off: int, pc_base: int):
    """Static target for JMP/JSR EAs, or ('a5', disp) for d16(A5), else None."""
    if mode == 5:
        disp = _s16(_r16(mem, off))
        if reg == 5:                    # d16(A5) — jump table
            return ("a5", disp)
        return None
    if mode == 7:
        if reg == 0:
            return ("abs", _s16(_r16(mem, off)) & 0xFFFFFFFF)
        if reg == 1:
            return ("abs", (_r16(mem, off) << 16) | _r16(mem, off + 2))
        if reg == 2:
            return ("rel", pc_base + _s16(_r16(mem, off)))
    return None


_SIZES = {0: 1, 1: 2, 2: 4}  # standard ss field


@dataclass
class EA:
    """Decoded effective address operand."""
    kind: str          # dreg areg ind postinc predec disp16 index absw absl
                       # pcdisp pcindex imm
    reg: int = 0       # An/Dn number where applicable
    disp: int = 0      # displacement (signed) / absolute address / immediate
    idx_reg: int = 0   # index register number (0-7 Dn, 8-15 An)
    idx_long: bool = False
    idx_scale: int = 1
    words: int = 0     # extension words consumed
    memind: int = 0    # 68020 memory indirect: 0 none, 1 pre, 2 post
    od: int = 0        # outer displacement (memory indirect)
    idx_none: bool = False  # index suppressed (full extension word)


def parse_ea(mem: bytes, off: int, mode: int, reg: int, size: int) -> EA:
    """Parse one EA's extension words at `off` (just past prior words)."""
    if mode == 0:
        return EA("dreg", reg=reg)
    if mode == 1:
        return EA("areg", reg=reg)
    if mode == 2:
        return EA("ind", reg=reg)
    if mode == 3:
        return EA("postinc", reg=reg)
    if mode == 4:
        return EA("predec", reg=reg)
    if mode == 5:
        return EA("disp16", reg=reg, disp=_s16(_r16(mem, off)), words=1)
    if mode == 6 or (mode == 7 and reg == 3):
        ext = _r16(mem, off)
        kind = "index" if mode == 6 else "pcindex"
        if ext & 0x100:
            # 68020 full extension word: sized base displacement, optional
            # index suppression and memory indirection.
            if ext & 0x80:
                raise DecodeError(f"base-suppressed ea at {off:#x}")
            iis = ext & 7
            if iis == 4:
                raise DecodeError(f"reserved I/IS at {off:#x}")
            bd_size = (ext >> 4) & 3
            if bd_size == 0:
                raise DecodeError(f"reserved bd size at {off:#x}")
            pos, bd = off + 2, 0
            if bd_size == 2:
                bd = _s16(_r16(mem, pos))
                pos += 2
            elif bd_size == 3:
                v = (_r16(mem, pos) << 16) | _r16(mem, pos + 2)
                bd = v - 0x100000000 if v & 0x80000000 else v
                pos += 4
            memind, od = 0, 0
            if iis:
                memind = 1 if iis < 4 else 2    # pre / post indexed
                od_size = iis & 3
                if od_size == 2:
                    od = _s16(_r16(mem, pos))
                    pos += 2
                elif od_size == 3:
                    v = (_r16(mem, pos) << 16) | _r16(mem, pos + 2)
                    od = v - 0x100000000 if v & 0x80000000 else v
                    pos += 4
            words = (pos - off) // 2
            idx_none = bool(ext & 0x40)
            if idx_none and not memind:         # plain base + disp
                if mode == 6:
                    return EA("disp16", reg=reg, disp=bd, words=words)
                return EA("pcdisp", disp=bd, words=words)
            return EA(kind, reg=reg, disp=bd, idx_reg=(ext >> 12) & 0xF,
                      idx_long=bool(ext & 0x800),
                      idx_scale=1 << ((ext >> 9) & 3), words=words,
                      memind=memind, od=od, idx_none=idx_none)
        idx_reg = (ext >> 12) & 0xF
        idx_long = bool(ext & 0x800)
        scale = 1 << ((ext >> 9) & 3)
        d8 = _s8(ext & 0xFF)
        return EA(kind, reg=reg, disp=d8, idx_reg=idx_reg,
                  idx_long=idx_long, idx_scale=scale, words=1)
    if mode == 7:
        if reg == 0:
            return EA("absw", disp=_s16(_r16(mem, off)) & 0xFFFFFFFF, words=1)
        if reg == 1:
            return EA("absl", disp=(_r16(mem, off) << 16) | _r16(mem, off + 2),
                      words=2)
        if reg == 2:
            return EA("pcdisp", disp=_s16(_r16(mem, off)), words=1)
        if reg == 4:
            if size == 4:
                return EA("imm", disp=(_r16(mem, off) << 16) | _r16(mem, off + 2),
                          words=2)
            v = _r16(mem, off)
            if size == 1:
                v &= 0xFF
            return EA("imm", disp=v, words=1)
    raise DecodeError(f"parse_ea bad mode {mode}/{reg}")


def decode(mem: bytes, pc: int) -> Insn:
    """Decode one instruction at segment offset pc."""
    op = _r16(mem, pc)
    hi = op >> 12

    def ins(words, mnem, **kw):
        return Insn(addr=pc, size=words * 2, mnem=mnem,
                    words=tuple(_r16(mem, pc + i * 2) for i in range(words)), **kw)

    # ---------- A-line ----------
    if hi == 0xA:
        if op in NO_RETURN_TRAPS:
            return ins(1, f"trap_{op:04X}", flow=JUMP, trap=op)
        if 0xAC00 <= op <= 0xAFFF:      # auto-pop toolbox trap: returns to caller's caller
            return ins(1, f"trap_{op:04X}", flow=RET, trap=op)
        return ins(1, f"trap_{op:04X}", flow=ALINE, trap=op)

    # ---------- F-line ----------
    if hi == 0xF:
        raise DecodeError(f"F-line {op:04X} at {pc:#x}")

    mode = (op >> 3) & 7
    reg = op & 7

    # ---------- group 0: immediates / bit ops / MOVEP ----------
    if hi == 0x0:
        if op & 0x0138 == 0x0108:       # MOVEP
            return ins(2, "movep")
        if op & 0x0100:                 # dynamic bit ops BTST/BCHG/BCLR/BSET Dn,EA
            n = 1 + _ea_ext_words(mode, reg, 1, mem, pc + 2)
            return ins(n, "bitop_dyn")
        sub = (op >> 9) & 7
        ss = (op >> 6) & 3
        if sub == 4:                    # static bit ops #imm
            n = 2 + _ea_ext_words(mode, reg, 1, mem, pc + 4)
            return ins(n, "bitop_imm")
        if sub in (0, 1, 2, 3, 5, 6) and ss != 3:
            # ORI/ANDI/SUBI/ADDI/EORI/CMPI
            names = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}
            size = _SIZES[ss]
            immw = 2 if size == 4 else 1
            if op in (0x003C, 0x023C, 0x0A3C):   # to CCR
                return ins(2, names[sub] + "_ccr")
            if op in (0x007C, 0x027C, 0x0A7C):   # to SR
                return ins(2, names[sub] + "_sr")
            n = 1 + immw + _ea_ext_words(mode, reg, size, mem, pc + 2 + immw * 2)
            return ins(n, names[sub] + ".bwl"[ss])
        raise DecodeError(f"group0 {op:04X} at {pc:#x}")

    # ---------- groups 1-3: MOVE ----------
    if hi in (1, 2, 3):
        size = {1: 1, 3: 2, 2: 4}[hi]
        src_n = _ea_ext_words(mode, reg, size, mem, pc + 2)
        dmode = (op >> 6) & 7
        dreg = (op >> 9) & 7
        dst_n = _ea_ext_words(dmode, dreg, size, mem, pc + 2 + src_n * 2)
        return ins(1 + src_n + dst_n, "move" + {1: ".b", 3: ".w", 2: ".l"}[hi])

    # ---------- group 4: miscellaneous ----------
    if hi == 0x4:
        if op == 0x4AFC:
            return ins(1, "illegal", flow=ILLEGAL)
        if op & 0xFFF0 == 0x4E40:
            return ins(1, f"trap#{op & 15}")
        if op & 0xFFF8 == 0x4E50:       # LINK An,#d16
            return ins(2, "link")
        if op & 0xFFF8 == 0x4E58:
            return ins(1, "unlk")
        if op & 0xFFF8 == 0x4808:       # LINK.L (68020)
            return ins(3, "link.l")
        if op & 0xFFF0 == 0x4E60:
            return ins(1, "move_usp")
        if op == 0x4E70: return ins(1, "reset")
        if op == 0x4E71: return ins(1, "nop")
        if op == 0x4E72: return ins(2, "stop", flow=ILLEGAL)
        if op == 0x4E73: return ins(1, "rte", flow=RET)
        if op == 0x4E74: return ins(2, "rtd", flow=RET)
        if op == 0x4E75: return ins(1, "rts", flow=RET)
        if op == 0x4E76: return ins(1, "trapv")
        if op == 0x4E77: return ins(1, "rtr", flow=RET)
        if op in (0x4E7A, 0x4E7B):
            return ins(2, "movec")
        if op & 0xFFC0 == 0x4E80:       # JSR
            n = 1 + _ea_ext_words(mode, reg, 0, mem, pc + 2)
            t = _ea_target(mode, reg, mem, pc + 2, pc + 2)
            if t and t[0] == "a5":
                return ins(n, "jsr_a5", flow=CALL, a5_call=t[1])
            if t and t[0] == "rel":
                return ins(n, "jsr", flow=CALL, target=t[1])
            if t and t[0] == "abs":
                return ins(n, "jsr_abs", flow=CALL)   # absolute: runtime addr
            return ins(n, "jsr_ind", flow=CALL)
        if op & 0xFFC0 == 0x4EC0:       # JMP
            n = 1 + _ea_ext_words(mode, reg, 0, mem, pc + 2)
            t = _ea_target(mode, reg, mem, pc + 2, pc + 2)
            if t and t[0] == "a5":
                return ins(n, "jmp_a5", flow=JUMP, a5_call=t[1])
            if t and t[0] == "rel":
                return ins(n, "jmp", flow=JUMP, target=t[1])
            return ins(n, "jmp_ind", flow=JUMP)       # indirect / abs
        if op & 0xFB80 == 0x4880 and mode == 0:       # EXT.W/L (and EXTB.L 68020 0x49C0)
            return ins(1, "ext")
        if op & 0xFFF8 == 0x49C0:
            return ins(1, "extb.l")
        if op & 0xFB80 == 0x4880:       # MOVEM
            size = 2 if not (op & 0x40) else 4
            n = 2 + _ea_ext_words(mode, reg, size, mem, pc + 4)
            return ins(n, "movem")
        if op & 0xFFC0 == 0x4840:
            if mode == 0:
                return ins(1, "swap")
            if mode == 1:
                return ins(1, "bkpt")
            n = 1 + _ea_ext_words(mode, reg, 0, mem, pc + 2)   # PEA
            return ins(n, "pea")
        if op & 0xFFC0 == 0x4800:       # NBCD
            n = 1 + _ea_ext_words(mode, reg, 1, mem, pc + 2)
            return ins(n, "nbcd")
        if op & 0xFF00 == 0x4A00:       # TST / TAS
            ss = (op >> 6) & 3
            if ss == 3:                 # TAS
                n = 1 + _ea_ext_words(mode, reg, 1, mem, pc + 2)
                return ins(n, "tas")
            n = 1 + _ea_ext_words(mode, reg, _SIZES[ss], mem, pc + 2)
            return ins(n, "tst")
        if op & 0xFF00 in (0x4000, 0x4200, 0x4400, 0x4600):  # NEGX/CLR/NEG/NOT + MOVE sr/ccr
            ss = (op >> 6) & 3
            name = {0x40: "negx", 0x42: "clr", 0x44: "neg", 0x46: "not"}[op >> 8]
            if ss == 3:                 # MOVE from SR / from CCR / to CCR / to SR
                sz = 2
                n = 1 + _ea_ext_words(mode, reg, sz, mem, pc + 2)
                return ins(n, "move_srccr")
            n = 1 + _ea_ext_words(mode, reg, _SIZES[ss], mem, pc + 2)
            return ins(n, name)
        if op & 0xF1C0 == 0x41C0:       # LEA
            n = 1 + _ea_ext_words(mode, reg, 0, mem, pc + 2)
            return ins(n, "lea")
        if op & 0xF140 == 0x4100:       # CHK.W/.L
            ss = 2 if (op & 0x80) else 4
            n = 1 + _ea_ext_words(mode, reg, ss, mem, pc + 2)
            return ins(n, "chk")
        if op & 0xFF80 == 0x4C00:       # MULS.L/MULU.L/DIVS.L/DIVU.L (68020)
            n = 2 + _ea_ext_words(mode, reg, 4, mem, pc + 4)
            return ins(n, "muldiv.l")
        raise DecodeError(f"group4 {op:04X} at {pc:#x}")

    # ---------- group 5: ADDQ/SUBQ/Scc/DBcc ----------
    if hi == 0x5:
        ss = (op >> 6) & 3
        if ss == 3:
            cc = (op >> 8) & 0xF
            if mode == 1:               # DBcc
                disp = _s16(_r16(mem, pc + 2))
                return ins(2, f"db{COND[cc]}", flow=BRANCH,
                           target=pc + 2 + disp, cond=COND[cc])
            if mode == 7 and reg in (2, 3, 4):  # TRAPcc (68020)
                extra = {2: 1, 3: 2, 4: 0}[reg]
                return ins(1 + extra, f"trap{COND[cc]}")
            n = 1 + _ea_ext_words(mode, reg, 1, mem, pc + 2)   # Scc
            return ins(n, f"s{COND[cc]}")
        name = "addq" if not (op & 0x100) else "subq"
        n = 1 + _ea_ext_words(mode, reg, _SIZES[ss], mem, pc + 2)
        return ins(n, name)

    # ---------- group 6: Bcc/BSR/BRA ----------
    if hi == 0x6:
        cc = (op >> 8) & 0xF
        disp8 = op & 0xFF
        if disp8 == 0:
            disp = _s16(_r16(mem, pc + 2)); n = 2
        elif disp8 == 0xFF:
            disp = ((_r16(mem, pc + 2) << 16) | _r16(mem, pc + 4))
            disp = disp - (1 << 32) if disp & (1 << 31) else disp; n = 3
        else:
            disp = _s8(disp8); n = 1
        target = pc + 2 + disp
        if cc == 0:
            return ins(n, "bra", flow=JUMP, target=target)
        if cc == 1:
            return ins(n, "bsr", flow=CALL, target=target)
        return ins(n, f"b{COND[cc]}", flow=BRANCH, target=target, cond=COND[cc])

    # ---------- group 7: MOVEQ ----------
    if hi == 0x7:
        if op & 0x100:
            raise DecodeError(f"group7 {op:04X} at {pc:#x}")
        return ins(1, "moveq")

    # ---------- groups 8,9,B,C,D: standard dyadic ----------
    if hi in (0x8, 0x9, 0xB, 0xC, 0xD):
        omode = (op >> 6) & 7
        names = {0x8: "or", 0x9: "sub", 0xB: "cmp/eor", 0xC: "and", 0xD: "add"}
        if hi in (0x9, 0xD) and omode in (3, 7):     # ADDA/SUBA
            size = 2 if omode == 3 else 4
            n = 1 + _ea_ext_words(mode, reg, size, mem, pc + 2)
            return ins(n, names[hi] + "a")
        if hi == 0xB and omode in (3, 7):            # CMPA
            size = 2 if omode == 3 else 4
            n = 1 + _ea_ext_words(mode, reg, size, mem, pc + 2)
            return ins(n, "cmpa")
        if hi == 0x8 and omode in (3, 7):            # DIVU/DIVS .W
            n = 1 + _ea_ext_words(mode, reg, 2, mem, pc + 2)
            return ins(n, "div.w")
        if hi == 0xC and omode in (3, 7):            # MULU/MULS .W
            n = 1 + _ea_ext_words(mode, reg, 2, mem, pc + 2)
            return ins(n, "mul.w")
        if hi in (0x9, 0xD) and omode in (4, 5, 6) and mode in (0, 1) and (op & 0x100):
            return ins(1, names[hi] + "x")           # ADDX/SUBX
        if hi == 0xB and omode in (4, 5, 6) and mode == 1:
            return ins(1, "cmpm")
        if hi == 0xC and (op & 0x1F0) in (0x100, 0x140, 0x180):
            if (op & 0xF0) == 0x00 and omode == 4:
                return ins(1, "abcd")
            return ins(1, "exg")                     # EXG
        if hi == 0x8 and omode == 4 and mode in (0, 1):
            return ins(1, "sbcd")
        ss = omode & 3
        if ss == 3:
            raise DecodeError(f"dyadic ss=3 {op:04X} at {pc:#x}")
        n = 1 + _ea_ext_words(mode, reg, _SIZES[ss], mem, pc + 2)
        return ins(n, names[hi])

    # ---------- group E: shifts/rotates/bitfields ----------
    if hi == 0xE:
        ss = (op >> 6) & 3
        if ss == 3:
            if op & 0x0800:                          # bitfield ops (68020)
                n = 2 + _ea_ext_words(mode, reg, 0, mem, pc + 4)
                return ins(n, "bitfield")
            n = 1 + _ea_ext_words(mode, reg, 2, mem, pc + 2)   # memory shift
            return ins(n, "shift_mem")
        return ins(1, "shift")
    raise DecodeError(f"unhandled {op:04X} at {pc:#x}")
