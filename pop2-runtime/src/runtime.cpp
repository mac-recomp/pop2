// Runtime core: guest memory, dispatch plumbing, trap logging stub.
#include "pop2/cpu.h"
#include "pop2/mac.h"
#include "gen_decls.h"

#include <cstdarg>
#ifndef __EMSCRIPTEN__
#include <execinfo.h>   // glibc host backtrace (POP2_WATCH); absent under wasm
#endif
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <string>

namespace pop2 {

uint8_t* g_mem = nullptr;
uint32_t g_watch_addr = 0;

void watch_hit(uint32_t addr, uint32_t val) {
    static int s_hits = 0;
    static uint32_t s_last = 0xDEADBEEF;
    if (val == s_last) return;          // change-only: block copies rewrite same value
    s_last = val;
    if (s_hits >= 40) return;
    s_hits++;
    std::fprintf(stderr, "[watch] write %08X to %06X t=%u — host bt:\n", val, addr,
                 ticks_live());
#ifndef __EMSCRIPTEN__
    void* frames[14];
    int n = backtrace(frames, 14);
    backtrace_symbols_fd(frames, n, 2);
#endif
}

static std::unordered_map<uint32_t, GuestFn> s_vfns;
static std::unordered_map<uint16_t, GuestFn> s_jt;
const char* trap_display_name(uint16_t op);  // gen_trap_names.cpp

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "FATAL: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::abort();
}

bool g_trace_funcs = false;
static const char* s_trace_filter = nullptr;

void guest_trace(const char* fn, Cpu& cpu) {
    if (!s_trace_filter || std::strstr(fn, s_trace_filter))
        std::fprintf(stderr, "[fn] %s t=%u args=%08X %04X %04X %08X\n", fn,
                     ticks_live(), mem_read32(cpu.a[7] + 4),
                     mem_read16(cpu.a[7] + 8), mem_read16(cpu.a[7] + 10),
                     mem_read32(cpu.a[7] + 12));
    // dissolve-effect forensics: dump the offset table and pixel bases
    if (s_trace_filter && !std::strcmp(s_trace_filter, "f15_3050") &&
        !std::strcmp(fn, "f15_3050")) {
        uint32_t ctx = mem_read32(cpu.a[6] + 8);   // entered via trampoline
        uint32_t h = mem_read32(cpu.a[6] + 12);    // JMP: frame is the caller's
        uint32_t gw = ctx ? mem_read32(ctx + 20) : 0;
        uint32_t pm = gw ? mem_read32(mem_read32(gw + 2)) : 0;
        uint32_t srcbase = pm ? mem_read32(pm) : 0;
        uint32_t p = h ? mem_read32(h) : 0;
        std::fprintf(stderr, "[fx] gw=%06X srcbase=%06X srcrow=%04X tbl=%06X "
                     "hdr=%04X %04X sz=%d e0={%08X %08X} e1={%08X %08X} "
                     "e2={%08X %08X}\n", gw, srcbase,
                     pm ? mem_read16(pm + 4) : 0, p,
                     p ? mem_read16(p) : 0, p ? mem_read16(p + 2) : 0,
                     h ? mm_get_handle_size(h) : -1,
                     p ? mem_read32(p + 20) : 0, p ? mem_read32(p + 24) : 0,
                     p ? mem_read32(p + 28) : 0, p ? mem_read32(p + 32) : 0,
                     p ? mem_read32(p + 36) : 0, p ? mem_read32(p + 40) : 0);
        static bool dumped = false;
        if (!dumped && p) {
            dumped = true;
            int32_t sz = mm_get_handle_size(h);
            FILE* f = std::fopen("/tmp/dslv_table.bin", "wb");
            if (f && sz > 0) {
                uint32_t hdr[2] = {srcbase, mem_read32(gw + 2)};
                std::fwrite(hdr, 8, 1, f);
                std::fwrite(g_mem + (p & 0xFFFFFF), size_t(sz), 1, f);
                std::fclose(f);
            }
        }
    }
}

void runtime_init() {
    if (const char* w = std::getenv("POP2_WATCH"))
        g_watch_addr = uint32_t(std::strtoul(w, nullptr, 16));
    if (const char* tf = std::getenv("POP2_TRACE_FUNCS")) {
        g_trace_funcs = true;
        if (tf[0] && std::strcmp(tf, "1") != 0) s_trace_filter = tf;
    }
    g_mem = static_cast<uint8_t*>(std::calloc(MEM_SIZE, 1));
    if (!g_mem) fatal("cannot allocate guest memory");
    s_vfns.reserve(g_vfn_count * 2 + g_jt_count * 2);
    for (size_t i = 0; i < g_vfn_count; i++)
        s_vfns[g_vfns[i].vaddr] = g_vfns[i].fn;
    for (size_t i = 0; i < g_jt_count; i++) {
        s_jt[g_jt_entries[i].a5off] = g_jt_entries[i].fn;
        // function pointers taken from jump-table slots: A5 + a5off
        s_vfns[A5_BASE + g_jt_entries[i].a5off] = g_jt_entries[i].fn;
    }
}

void jt_call(Cpu& cpu, uint16_t a5_offset) {
    auto it = s_jt.find(a5_offset);
    if (it == s_jt.end()) fatal("jt_call: no entry at A5+%u", a5_offset);
    it->second(cpu);
}

void jt_jump(Cpu& cpu, uint16_t a5_offset) { jt_call(cpu, a5_offset); }

static GuestFn resolve_vfn(uint32_t vaddr) {
    for (int hops = 0; hops < 4; hops++) {
        auto it = s_vfns.find(vaddr);
        if (it != s_vfns.end()) return it->second;
        // chase JMP abs.l thunks living in data (Think C jump islands,
        // DREL-relocated pointers into the jump table)
        if (mem_read16(vaddr) == 0x4EF9) {
            vaddr = mem_read32(vaddr + 2);
            continue;
        }
        // heap trampoline built by the sound glue for its VBL task:
        // BSR.B +4; DC.L target; MOVEA.L (SP)+,A1; MOVEA.L (A1),A1; JMP (A1)
        if (mem_read16(vaddr) == 0x6104 &&
            mem_read32(vaddr + 6) == 0x225F2251 &&
            mem_read16(vaddr + 10) == 0x4ED1) {
            vaddr = mem_read32(vaddr + 2);
            continue;
        }
        break;
    }
    return nullptr;
}

// JSR/JMP to a GetTrapAddress stub: the guest return address is on top of
// the stack (above any Pascal args); pop it, run the trap, return natively.
static bool try_trap_stub(Cpu& cpu, uint32_t vaddr) {
    if (vaddr < TRAP_STUB_BASE || vaddr >= TRAP_STUB_END) return false;
    uint16_t trap = uint16_t(0xA000 | ((vaddr >> 4) & 0xFFF));
    (void)pop32(cpu);
    do_trap(cpu, trap);
    return true;
}

void call_virtual(Cpu& cpu, uint32_t vaddr) {
    // POP2_TRACE_DRIVER: log calls into the MDRV blob (selector + params)
    static const bool trace_drv = std::getenv("POP2_TRACE_DRIVER") != nullptr;
    if (trace_drv && vaddr >= 0x390000 && vaddr < 0x3A0000) {
        uint32_t sel = mem_read32(cpu.a[7] + 4);
        uint32_t p = mem_read32(cpu.a[7] + 8);
        std::fprintf(stderr, "[drv] call %06X sel=%08X p=%08X ra=%06X\n",
                     vaddr, sel, p, mem_read32(cpu.a[7]));
        // sample start/claim: dump the request block the game hands over
        if ((sel == 0x11 || sel == 0x12) && p && p < MEM_SIZE - 80) {
            std::fprintf(stderr, "[drv]   block:");
            for (int i = 0; i < 80; i += 4)
                std::fprintf(stderr, " %08X", mem_read32(p + i));
            std::fprintf(stderr, "\n");
        }
    }
    if (GuestFn fn = resolve_vfn(vaddr)) { fn(cpu); return; }
    if (try_trap_stub(cpu, vaddr)) return;
    fatal("call_virtual: no function at %06X (a7=%06X ra=%06X) "
          "bytes=%04X %04X %04X",
          vaddr, cpu.a[7], mem_read32(cpu.a[7]), mem_read16(vaddr),
          mem_read16(vaddr + 2), mem_read16(vaddr + 4));
}

void jump_virtual(Cpu& cpu, uint32_t vaddr) {
    if (GuestFn fn = resolve_vfn(vaddr)) { fn(cpu); return; }
    if (try_trap_stub(cpu, vaddr)) return;
    // Not a known entry: treat as "JMP <return address>" (the classic
    // pop-RA-into-An glue pattern) — native return resumes the caller there.
    static const bool trace_jv = std::getenv("POP2_TRACE_TRAPS") ||
                                 std::getenv("POP2_TRACE_JV");
    if (trace_jv) {
        static std::unordered_map<uint32_t, int> seen;  // log first 3 per site
        if (seen[vaddr]++ < 3)
            std::fprintf(stderr, "[jump_virtual] %06X not a function — "
                         "assuming return-to-caller (ra=%06X)\n", vaddr,
                         mem_read32(cpu.a[7]));
    }
}

void unimplemented(Cpu& cpu, const char* what, uint32_t vaddr) {
    fatal("unimplemented instruction '%s' at %06X", what, vaddr);
}

}  // namespace pop2
