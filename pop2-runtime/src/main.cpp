// Smoke-test loader: map CODE segments into guest memory, build the A5
// world, and call the application entry point (jump table slot 0).
#include "pop2/cpu.h"
#include "pop2/mac.h"
#include "gen_decls.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Restore persisted save games before the guest runs: mount IDBFS at
// /data/persist, load it from IndexedDB, and copy any saves into the game
// volume root (/data/pop2), where SFGetFile / fs_first_pop2_save look. Awaits
// the async syncfs via Asyncify/JSPI. Paired with pop2_persist_root (file_mgr).
EM_ASYNC_JS(void, pop2_persist_init, (), {
  try { FS.mkdir('/data/persist'); } catch (e) {}
  try { FS.mount(IDBFS, {}, '/data/persist'); } catch (e) {}
  await new Promise(function (resolve) { FS.syncfs(true, function () { resolve(); }); });
  try {
    var names = FS.readdir('/data/persist');
    for (var i = 0; i < names.length; i++) {
      var f = names[i];
      if (f === '.' || f === '..') continue;
      try { FS.writeFile('/data/pop2/' + f, FS.readFile('/data/persist/' + f)); } catch (e) {}
    }
  } catch (e) {}
});
#endif

namespace fs = std::filesystem;
using namespace pop2;

// Guest memory layout: see pop2/mac.h (stack, A5 world, master pointers,
// heap) and recomp.py SEG_VBASE (CODE images at 0x200000 + seg*64K).
static uint32_t seg_vbase(int seg) { return 0x0020'0000 + uint32_t(seg) * 0x1'0000; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <extracted/app/CODE dir>\n", argv[0]);
        return 2;
    }
    runtime_init();
    mm_init();
    rm_init(fs::path(argv[1]).parent_path().string());  // extracted/app
    fs_init(argc > 2 ? argv[2]
                     : (fs::path(argv[1]) / ".." / ".." / ".." / ".." /
                        "extracted_sit" / "Prince of Persia 2").lexically_normal().string());
#ifdef __EMSCRIPTEN__
    pop2_persist_init();   // mount IDBFS + restore saved games before the guest runs
#endif

    // load CODE resources at their virtual bases
    int loaded = 0;
    for (auto& entry : fs::directory_iterator(argv[1])) {
        const std::string name = entry.path().filename().string();
        int seg = -1;
        if (sscanf(name.c_str(), "%d", &seg) != 1 || seg < 0) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        if (seg_vbase(seg) + data.size() > MEM_SIZE) fatal("segment %d too big", seg);
        std::memcpy(g_mem + seg_vbase(seg), data.data(), data.size());
        loaded++;
    }
    std::printf("loaded %d CODE segments\n", loaded);

    // A5 world: copy CODE 0 jump table image above A5 (loaded-form patching is
    // not needed by recompiled code, but data reads through A5 must see it).
    uint32_t below_a5 = 0;
    {
        std::ifstream f(fs::path(argv[1]) / "0.bin", std::ios::binary);
        std::vector<char> c0((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        if (c0.size() > 16)
            std::memcpy(g_mem + A5_BASE + 32, c0.data() + 16, c0.size() - 16);
        below_a5 = uint32_t((uint8_t(c0[4]) << 24) | (uint8_t(c0[5]) << 16) |
                            (uint8_t(c0[6]) << 8) | uint8_t(c0[7]));
    }

    // Classic low-memory globals the runtime startup reads.
    mem_write32(0x0108, MEM_SIZE);             // MemTop
    mem_write32(0x0114, HEAP_END);             // HeapEnd
    mem_write32(0x0130, HEAP_END);             // ApplLimit
    mem_write32(0x02A6, 0x2800);               // SysZone
    mem_write32(0x02AA, HEAP_BASE);            // ApplZone
    mem_write32(0x02AE, 0xFF0000);             // ROMBase
    mem_write32(0x031A, 0x00FFFFFF);           // Lo3Bytes
    mem_write32(0x0824, FB_BASE);              // ScrnBase
    mem_write32(0x0904, A5_BASE);              // CurrentA5
    mem_write32(0x0908, A5_BASE - below_a5);   // CurStackBase (= globals base)
    mem_write32(0x0A02, 0x00010001);           // OneOne
    mem_write32(0x0A06, 0xFFFFFFFF);           // MinusOne
    mem_write8(0x0260, 7);                     // SdVolume: speaker at max —
    // the MDRV synth's doubleback finalizer reads it raw and emits pure
    // silence when it is zero (f25_087c vs f25_0894)
    write_pstr(0x0910, "Prince of Persia 2");  // CurApName
    // Drive queue: one mounted drive #1 — Mohawk walks DrvQHdr to verify the
    // data volume sits on a real drive (drive number from HGetVInfo).
    constexpr uint32_t DRVQ_EL = 0x2F00;
    mem_write16(0x0308, 0);                    // DrvQHdr.qFlags
    mem_write32(0x030A, DRVQ_EL);              // DrvQHdr.qHead
    mem_write32(0x030E, DRVQ_EL);              // DrvQHdr.qTail
    mem_write32(DRVQ_EL + 0, 0);               // qLink: end of queue
    mem_write16(DRVQ_EL + 4, 1);               // qType: dQDrvSz2 valid
    mem_write16(DRVQ_EL + 6, 1);               // dQDrive = drive 1
    mem_write16(DRVQ_EL + 8, uint16_t(-37));   // dQRefNum (driver)
    mem_write16(DRVQ_EL + 10, 0);              // dQFSID: native file system
    mem_write16(DRVQ_EL + 12, 0x7FFF);         // dQDrvSz
    mem_write16(DRVQ_EL + 14, 0x7FFF);         // dQDrvSz2
    // Unit table for that driver (refnum -37 -> unit 36): Mohawk reads the
    // DRVR name to classify the drive (".Sony" = floppy, ".AppleCD" = CD);
    // give it a hard-disk driver so both special paths are skipped.
    constexpr uint32_t UNIT_TABLE = 0x2E00;    // 48 entries x 4
    constexpr uint32_t DCE_MASTER = 0x2EF0;    // master pointer of DCE handle
    constexpr uint32_t DCE        = 0x2E20;
    constexpr uint32_t DRVR_HDR   = 0x2E40;
    mem_write32(0x011C, UNIT_TABLE);           // UTableBase
    mem_write16(0x01D2, 48);                   // UnitNtryCnt
    mem_write32(UNIT_TABLE + 36 * 4, DCE_MASTER);
    mem_write32(DCE_MASTER, DCE);
    mem_write32(DCE + 0, DRVR_HDR);            // dCtlDriver (pointer form)
    mem_write16(DCE + 4, 0);                   // dCtlFlags: not RAM-based
    mem_write16(DCE + 24, uint16_t(-37));      // dCtlRefNum
    write_pstr(DRVR_HDR + 18, ".ATADisk");     // drvrName
    std::printf("A5 world: belowA5=%u, globals at %06X..%06X\n",
                below_a5, A5_BASE - below_a5, A5_BASE);

    Cpu cpu;
    cpu.a[5] = A5_BASE;
    cpu.a[7] = STACK_TOP;
    push32(cpu, 0);  // sentinel return address

    std::printf("calling entry point...\n");
    if (g_jt_count == 0) fatal("empty jump table");
    g_jt_entries[0].fn(cpu);

    std::printf("entry returned cleanly: d0=%08X\n", cpu.d[0]);
    return 0;
}
