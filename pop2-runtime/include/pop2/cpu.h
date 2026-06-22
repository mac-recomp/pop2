// pop2 runtime: 68k CPU state, guest memory, CCR semantics.
//
// Guest model: big-endian 68020 user mode, flat 16 MB virtual space.
// Recompiled code manipulates Cpu directly; memory access via mem_read/write
// with byte swapping on little-endian hosts.

#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace pop2 {

inline constexpr uint32_t MEM_SIZE = 16u * 1024 * 1024;
inline constexpr uint32_t ADDR_MASK = MEM_SIZE - 1;

extern uint8_t* g_mem;  // allocated by runtime_init()

struct Cpu {
    uint32_t d[8]{};
    uint32_t a[8]{};
    uint32_t pc{};
    // CCR as separate flags (fast path; SR synthesis on demand)
    bool x{}, n{}, z{}, v{}, c{};
};

[[noreturn]] void fatal(const char* fmt, ...);
uint32_t ticks_live();   // live TickCount for raw low-mem Ticks reads

// ---------- memory ----------
inline uint8_t mem_read8(uint32_t addr) { return g_mem[addr & ADDR_MASK]; }
inline uint16_t mem_read16(uint32_t addr) {
    addr &= ADDR_MASK;
    return uint16_t(g_mem[addr] << 8 | g_mem[addr + 1]);
}
inline uint32_t mem_read32(uint32_t addr) {
    addr &= ADDR_MASK;
    return uint32_t(g_mem[addr]) << 24 | uint32_t(g_mem[addr + 1]) << 16 |
           uint32_t(g_mem[addr + 2]) << 8 | g_mem[addr + 3];
}
extern uint32_t g_watch_addr;        // POP2_WATCH=hex — log writes to it
void watch_hit(uint32_t addr, uint32_t val);

inline void mem_write8(uint32_t addr, uint8_t val) {
    addr &= ADDR_MASK;
    if (g_watch_addr && addr == g_watch_addr) watch_hit(addr, val);
    g_mem[addr] = val;
}
inline void mem_write16(uint32_t addr, uint16_t val) {
    addr &= ADDR_MASK;
    if (g_watch_addr && g_watch_addr - addr < 2) watch_hit(addr, val);
    g_mem[addr] = uint8_t(val >> 8); g_mem[addr + 1] = uint8_t(val);
}
inline void mem_write32(uint32_t addr, uint32_t val) {
    addr &= ADDR_MASK;
    if (g_watch_addr && g_watch_addr - addr < 4) watch_hit(addr, val);
    g_mem[addr] = uint8_t(val >> 24); g_mem[addr + 1] = uint8_t(val >> 16);
    g_mem[addr + 2] = uint8_t(val >> 8); g_mem[addr + 3] = uint8_t(val);
}

template <class T> inline T mem_read(uint32_t addr) {
    if constexpr (sizeof(T) == 1) return T(mem_read8(addr));
    else if constexpr (sizeof(T) == 2) return T(mem_read16(addr));
    else return T(mem_read32(addr));
}
template <class T> inline void mem_write(uint32_t addr, T val) {
    if constexpr (sizeof(T) == 1) mem_write8(addr, uint8_t(val));
    else if constexpr (sizeof(T) == 2) mem_write16(addr, uint16_t(val));
    else mem_write32(addr, uint32_t(val));
}

// ---------- register sub-width access ----------
// 68k: byte/word ops on Dn touch only low bits; An word ops sign-extend.
template <class T> inline T dreg_get(const Cpu& cpu, int r) { return T(cpu.d[r]); }
template <class T> inline void dreg_set(Cpu& cpu, int r, T val) {
    if constexpr (sizeof(T) == 1) cpu.d[r] = (cpu.d[r] & 0xFFFFFF00u) | uint8_t(val);
    else if constexpr (sizeof(T) == 2) cpu.d[r] = (cpu.d[r] & 0xFFFF0000u) | uint16_t(val);
    else cpu.d[r] = uint32_t(val);
}
inline void areg_set_w(Cpu& cpu, int r, uint16_t val) {  // sign-extends
    cpu.a[r] = uint32_t(int32_t(int16_t(val)));
}

// ---------- 68020 bitfields (bit 0 = MSB; fields wrap in registers) ----------
inline uint32_t bf_mask(int width) {
    return width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
}
inline uint32_t bf_reg_get(uint32_t r, int32_t off, int width) {
    unsigned o = unsigned(off) & 31;
    uint32_t rot = o ? (r << o) | (r >> (32 - o)) : r;
    return (width >= 32 ? rot : rot >> (32 - width)) & bf_mask(width);
}
inline uint32_t bf_reg_put(uint32_t r, int32_t off, int width, uint32_t val) {
    unsigned o = unsigned(off) & 31;
    uint32_t rot = o ? (r << o) | (r >> (32 - o)) : r;
    uint32_t m = bf_mask(width) << (32 - width);
    rot = (rot & ~m) | ((val << (32 - width)) & m);
    return o ? (rot >> o) | (rot << (32 - o)) : rot;
}
inline uint32_t bf_mem_get(uint32_t addr, int32_t off, int width) {
    int64_t bit = int64_t(addr) * 8 + off;
    uint32_t byte = uint32_t(bit >> 3);
    int sh = int(bit & 7);
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | mem_read8(byte + uint32_t(i));
    return uint32_t(v >> (40 - sh - width)) & bf_mask(width);
}
inline void bf_mem_put(uint32_t addr, int32_t off, int width, uint32_t val) {
    int64_t bit = int64_t(addr) * 8 + off;
    uint32_t byte = uint32_t(bit >> 3);
    int sh = int(bit & 7);
    int shift = 40 - sh - width;
    uint64_t m = uint64_t(bf_mask(width)) << shift;
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | mem_read8(byte + uint32_t(i));
    v = (v & ~m) | ((uint64_t(val) << shift) & m);
    for (int i = 4; i >= 0; i--) {
        mem_write8(byte + uint32_t(i), uint8_t(v));
        v >>= 8;
    }
}

// ---------- CCR helpers ----------
template <class T> inline void set_nz(Cpu& cpu, T r) {
    using S = std::make_signed_t<T>;
    cpu.n = S(r) < 0; cpu.z = r == 0;
}
template <class T> inline void set_nz_clear_vc(Cpu& cpu, T r) {
    set_nz(cpu, r); cpu.v = false; cpu.c = false;
}

template <class T> inline T op_add(Cpu& cpu, T a, T b) {
    T r = T(a + b);
    cpu.c = r < a; cpu.x = cpu.c;
    cpu.v = ((a ^ r) & (b ^ r)) >> (sizeof(T) * 8 - 1);
    set_nz(cpu, r);
    return r;
}
template <class T> inline T op_addx(Cpu& cpu, T a, T b) {
    T r = T(a + b + (cpu.x ? 1 : 0));
    bool carry = cpu.x ? r <= a && b != T(~a) : r < a;
    // simpler: compute in wider type
    uint64_t wide = uint64_t(a) + b + (cpu.x ? 1 : 0);
    carry = wide >> (sizeof(T) * 8);
    cpu.c = carry; cpu.x = carry;
    cpu.v = ((a ^ r) & (b ^ r)) >> (sizeof(T) * 8 - 1);
    cpu.n = std::make_signed_t<T>(r) < 0;
    if (r != 0) cpu.z = false;  // ADDX: Z only cleared, never set
    return r;
}
template <class T> inline T op_sub(Cpu& cpu, T a, T b) {  // a - b
    T r = T(a - b);
    cpu.c = b > a; cpu.x = cpu.c;
    cpu.v = ((a ^ b) & (a ^ r)) >> (sizeof(T) * 8 - 1);
    set_nz(cpu, r);
    return r;
}
template <class T> inline T op_subx(Cpu& cpu, T a, T b) {
    uint64_t wide = uint64_t(a) - b - (cpu.x ? 1 : 0);
    T r = T(wide);
    bool borrow = (wide >> 63) != 0 || (uint64_t(b) + (cpu.x ? 1 : 0)) > a;
    borrow = (uint64_t(b) + (cpu.x ? 1 : 0)) > uint64_t(a);
    cpu.c = borrow; cpu.x = borrow;
    cpu.v = ((a ^ b) & (a ^ r)) >> (sizeof(T) * 8 - 1);
    cpu.n = std::make_signed_t<T>(r) < 0;
    if (r != 0) cpu.z = false;
    return r;
}
template <class T> inline void op_cmp(Cpu& cpu, T a, T b) {  // a - b, no X
    T r = T(a - b);
    cpu.c = b > a;
    cpu.v = ((a ^ b) & (a ^ r)) >> (sizeof(T) * 8 - 1);
    set_nz(cpu, r);
}
template <class T> inline T op_neg(Cpu& cpu, T a) {
    T r = T(0 - a);
    cpu.c = a != 0; cpu.x = cpu.c;
    cpu.v = (a & r) >> (sizeof(T) * 8 - 1);
    set_nz(cpu, r);
    return r;
}

// shifts/rotates; count pre-masked to 0..63 by caller for register form
template <class T> inline T op_lsl(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    T r;
    if (cnt == 0) { cpu.c = false; r = a; }
    else if (cnt > BITS) { cpu.c = false; cpu.x = false; r = 0; }
    else {
        cpu.c = (a >> (BITS - cnt)) & 1; cpu.x = cpu.c;
        r = cnt == BITS ? T(0) : T(a << cnt);
    }
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_lsr(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    T r;
    if (cnt == 0) { cpu.c = false; r = a; }
    else if (cnt > BITS) { cpu.c = false; cpu.x = false; r = 0; }
    else {
        cpu.c = (a >> (cnt - 1)) & 1; cpu.x = cpu.c;
        r = cnt == BITS ? T(0) : T(a >> cnt);
    }
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_asl(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    T r;
    if (cnt == 0) { cpu.c = false; cpu.v = false; r = a; }
    else if (cnt >= BITS) {
        cpu.c = cnt == BITS ? (a & 1) : false; cpu.x = cpu.c;
        cpu.v = a != 0; r = 0;
    } else {
        cpu.c = (a >> (BITS - cnt)) & 1; cpu.x = cpu.c;
        // V set if sign changed at any point: top cnt+1 bits not all equal
        T mask = T(~T(0)) << (BITS - cnt - 1);
        T top = a & mask;
        cpu.v = !(top == 0 || top == mask);
        r = T(a << cnt);
    }
    set_nz(cpu, r);
    return r;
}
template <class T> inline T op_asr(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    using S = std::make_signed_t<T>;
    T r;
    if (cnt == 0) { cpu.c = false; r = a; }
    else if (cnt >= BITS) {
        r = T(S(a) >> (BITS - 1));
        cpu.c = r & 1; cpu.x = cpu.c;
    } else {
        cpu.c = (a >> (cnt - 1)) & 1; cpu.x = cpu.c;
        r = T(S(a) >> cnt);
    }
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_rol(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    T r = a;
    if (cnt) {
        cnt %= BITS;
        r = cnt ? T((a << cnt) | (a >> (BITS - cnt))) : a;
        cpu.c = r & 1;
    } else cpu.c = false;
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_ror(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    T r = a;
    if (cnt) {
        unsigned m = cnt % BITS;
        r = m ? T((a >> m) | (a << (BITS - m))) : a;
        cpu.c = std::make_signed_t<T>(r) < 0;
        cpu.c = (r >> (BITS - 1)) & 1;
    } else cpu.c = false;
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_roxl(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    unsigned m = cnt % (BITS + 1);
    T r = a; bool x = cpu.x;
    while (m--) {  // simple loop; shift counts are tiny
        bool newx = (r >> (BITS - 1)) & 1;
        r = T((r << 1) | (x ? 1 : 0));
        x = newx;
    }
    cpu.x = cnt ? x : cpu.x; cpu.c = x;
    cpu.v = false; set_nz(cpu, r);
    return r;
}
template <class T> inline T op_roxr(Cpu& cpu, T a, unsigned cnt) {
    constexpr unsigned BITS = sizeof(T) * 8;
    unsigned m = cnt % (BITS + 1);
    T r = a; bool x = cpu.x;
    while (m--) {
        bool newx = r & 1;
        r = T((r >> 1) | (T(x ? 1 : 0) << (BITS - 1)));
        x = newx;
    }
    cpu.x = cnt ? x : cpu.x; cpu.c = x;
    cpu.v = false; set_nz(cpu, r);
    return r;
}

// ---------- condition codes ----------
inline bool cc_hi(const Cpu& c) { return !c.c && !c.z; }
inline bool cc_ls(const Cpu& c) { return c.c || c.z; }
inline bool cc_cc(const Cpu& c) { return !c.c; }
inline bool cc_cs(const Cpu& c) { return c.c; }
inline bool cc_ne(const Cpu& c) { return !c.z; }
inline bool cc_eq(const Cpu& c) { return c.z; }
inline bool cc_vc(const Cpu& c) { return !c.v; }
inline bool cc_vs(const Cpu& c) { return c.v; }
inline bool cc_pl(const Cpu& c) { return !c.n; }
inline bool cc_mi(const Cpu& c) { return c.n; }
inline bool cc_ge(const Cpu& c) { return c.n == c.v; }
inline bool cc_lt(const Cpu& c) { return c.n != c.v; }
inline bool cc_gt(const Cpu& c) { return !c.z && c.n == c.v; }
inline bool cc_le(const Cpu& c) { return c.z || c.n != c.v; }

inline uint16_t get_ccr(const Cpu& c) {
    return uint16_t((c.x << 4) | (c.n << 3) | (c.z << 2) | (c.v << 1) | (c.c << 0));
}
inline void set_ccr(Cpu& c, uint16_t sr) {
    c.x = sr & 16; c.n = sr & 8; c.z = sr & 4; c.v = sr & 2; c.c = sr & 1;
}

// ---------- stack ----------
inline void push32(Cpu& cpu, uint32_t v) { cpu.a[7] -= 4; mem_write32(cpu.a[7], v); }
inline void push16(Cpu& cpu, uint16_t v) { cpu.a[7] -= 2; mem_write16(cpu.a[7], v); }
inline uint32_t pop32(Cpu& cpu) { uint32_t v = mem_read32(cpu.a[7]); cpu.a[7] += 4; return v; }
inline uint16_t pop16(Cpu& cpu) { uint16_t v = mem_read16(cpu.a[7]); cpu.a[7] += 2; return v; }

// ---------- control transfer plumbing (runtime.cpp) ----------
using GuestFn = void (*)(Cpu&);

void runtime_init();
void do_trap(Cpu& cpu, uint16_t opcode);          // A-line dispatch
void jt_call(Cpu& cpu, uint16_t a5_offset);       // JSR d16(A5)
void jt_jump(Cpu& cpu, uint16_t a5_offset);       // JMP d16(A5) (tail)
void call_virtual(Cpu& cpu, uint32_t vaddr);      // JSR (An) etc.
void jump_virtual(Cpu& cpu, uint32_t vaddr);      // JMP (An) — tail position
void unimplemented(Cpu& cpu, const char* what, uint32_t vaddr);

// guest function entry tracing (POP2_TRACE_FUNCS=1, or =substring filter)
extern bool g_trace_funcs;
void guest_trace(const char* fn, Cpu& cpu);
#define GUEST_TRACE(fn) do { if (g_trace_funcs) guest_trace(fn, cpu); } while (0)

}  // namespace pop2
