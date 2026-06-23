// A-line trap dispatcher.
//
// OS traps (0xA000-0xA7FF): register-based; identity = opcode with bits 9-10
// (flags: SYS/CLEAR for Memory Manager, ASYNC/IMMED for files) masked off.
// Toolbox traps (0xA800-0xABFF): Pascal stack convention — args pushed
// left-to-right, callee pops, result written to the slot below the args.
#include "pop2/mac.h"

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pop2 {

const char* trap_display_name(uint16_t op);

extern int g_post_load_pump;   // video.cpp: post-load sim/render-gate window

static const bool s_trace = std::getenv("POP2_TRACE_TRAPS") != nullptr;

// ---- UI state (headless for now) ----
static std::map<int16_t, uint32_t> s_menus;   // menuID -> MenuHandle
static uint32_t s_cur_port = 0;               // GrafPtr
static uint32_t s_qd_theport = 0;             // guest &qd.thePort (InitGraf arg)

static void set_cur_port(uint32_t port) {
    s_cur_port = port;
    if (s_qd_theport) mem_write32(s_qd_theport, port);
}
static uint32_t s_front_window = 0;
static std::map<uint32_t, uint32_t> s_win_palette;  // window -> PaletteHandle

static uint32_t new_region(int16_t t = 0, int16_t l = 0,
                           int16_t b = 0, int16_t r = 0) {
    uint32_t h = mm_new_handle(10, true);
    uint32_t p = mem_read32(h);
    mem_write16(p, 10);            // rgnSize
    mem_write16(p + 2, uint16_t(t));
    mem_write16(p + 4, uint16_t(l));
    mem_write16(p + 6, uint16_t(b));
    mem_write16(p + 8, uint16_t(r));
    return h;
}

// Main screen GDevice: 512x384, 8-bit indexed CLUT — built lazily.
static uint32_t main_gdevice() {
    static uint32_t s_gd = 0;
    if (s_gd) return s_gd;
    // ColorTable: 8 + 256*8 bytes, grayscale ramp placeholder
    uint32_t clut = mm_new_handle(8 + 256 * 8, true);
    uint32_t cp = mem_read32(clut);
    mem_write32(cp, 1);                  // ctSeed
    mem_write16(cp + 6, 255);            // ctSize = count-1
    for (int i = 0; i < 256; i++) {
        uint32_t ep = cp + 8 + uint32_t(i) * 8;
        mem_write16(ep, uint16_t(i));
        uint16_t v = uint16_t(i * 257);
        mem_write16(ep + 2, v); mem_write16(ep + 4, v); mem_write16(ep + 6, v);
    }
    // PixMap (50 bytes)
    uint32_t pm = mm_new_handle(50, true);
    uint32_t pp = mem_read32(pm);
    mem_write32(pp, FB_BASE);            // baseAddr
    mem_write16(pp + 4, 0x8000 | SCREEN_W);   // rowBytes | pixmap flag
    mem_write16(pp + 6, 0); mem_write16(pp + 8, 0);
    mem_write16(pp + 10, SCREEN_H); mem_write16(pp + 12, SCREEN_W);
    mem_write32(pp + 22, 0x00480000);    // hRes 72.0
    mem_write32(pp + 26, 0x00480000);    // vRes
    mem_write16(pp + 30, 0);             // pixelType: indexed
    mem_write16(pp + 32, 8);             // pixelSize
    mem_write16(pp + 34, 1);             // cmpCount
    mem_write16(pp + 36, 8);             // cmpSize
    mem_write32(pp + 42, clut);          // pmTable
    // GDevice (62 bytes)
    uint32_t gd = mm_new_handle(62, true);
    uint32_t gp = mem_read32(gd);
    mem_write16(gp + 4, 0);              // gdType: clutType
    mem_write16(gp + 20, 0xA801);        // gdFlags: color|mainScreen|screenDevice|screenActive
    mem_write32(gp + 22, pm);            // gdPMap
    mem_write16(gp + 34, 0); mem_write16(gp + 36, 0);
    mem_write16(gp + 38, SCREEN_H); mem_write16(gp + 40, SCREEN_W);  // gdRect
    s_gd = gd;
    return gd;
}

// Build a WindowRecord (156 bytes, zeroed) as a NewPtr block; portRect from
// the WIND resource if available, else 512x384. `color` makes a CGrafPort
// window (portPixMap handle + portVersion C000) over the same screen pixels.
static uint32_t make_window(int16_t wind_id, uint32_t storage,
                            bool color = false) {
    uint32_t w = storage ? storage : mm_new_ptr(160, true);
    int16_t top = 0, left = 0, bottom = SCREEN_H, right = SCREEN_W;
    if (uint32_t h = rm_get_resource(0x57494E44 /*WIND*/, wind_id, false)) {
        uint32_t p = mem_read32(h);
        int16_t t = int16_t(mem_read16(p)), l = int16_t(mem_read16(p + 2));
        int16_t b = int16_t(mem_read16(p + 4)), r = int16_t(mem_read16(p + 6));
        bottom = int16_t(b - t); right = int16_t(r - l);
        rm_release_resource(h);
    }
    if (color) {
        // screen PixMap (one shared handle): baseAddr=FB, 8 bpp, 512x384
        static uint32_t s_screen_pm = 0;
        if (!s_screen_pm) {
            s_screen_pm = mm_new_handle(50, true);
            uint32_t pp = mem_read32(s_screen_pm);
            mem_write32(pp, FB_BASE);
            mem_write16(pp + 4, uint16_t(0x8000 | SCREEN_W));
            mem_write16(pp + 6, 0); mem_write16(pp + 8, 0);
            mem_write16(pp + 10, SCREEN_H); mem_write16(pp + 12, SCREEN_W);
            mem_write32(pp + 22, 0x00480000);
            mem_write32(pp + 26, 0x00480000);
            mem_write16(pp + 32, 8);            // pixelSize
            mem_write16(pp + 34, 1); mem_write16(pp + 36, 8);
            uint32_t mgd = mem_read32(main_gdevice());
            mem_write32(pp + 42, mem_read32(mem_read32(mgd + 22) + 42));
        }
        mem_write32(w + 2, s_screen_pm);
        mem_write16(w + 6, 0xC000);             // portVersion: CGrafPort
    } else {
        // GrafPort: device@0, portBits@2 {baseAddr,rowBytes,bounds}
        mem_write32(w + 2, FB_BASE);
        mem_write16(w + 6, SCREEN_W);
        mem_write16(w + 8, 0); mem_write16(w + 10, 0);
        mem_write16(w + 12, SCREEN_H); mem_write16(w + 14, SCREEN_W);
    }
    mem_write16(w + 16, 0); mem_write16(w + 18, 0);
    mem_write16(w + 20, uint16_t(bottom)); mem_write16(w + 22, uint16_t(right));
    mem_write32(w + 24, new_region(0, 0, bottom, right));  // visRgn
    mem_write32(w + 28, new_region(-32768, -32768, 32767, 32767));  // clipRgn
    mem_write16(w + 68, 0);   // txFont
    mem_write16(w + 72, 1);   // txMode: srcOr (OpenPort default per IM)
    mem_write32(w + 80, video_match_color(0, 0, 0));        // fgColor pixel
    mem_write32(w + 84, video_match_color(255, 255, 255));  // bkColor pixel
    mem_write16(w + 108, 8);  // windowKind: userKind
    mem_write8(w + 110, 1);   // visible
    s_front_window = w;
    if (!s_cur_port) set_cur_port(w);
    return w;
}

static uint32_t s_tick_offset = 0;   // assist: rewind the game clock to add time
static double   s_speed     = 1.0;   // assist: game-speed multiplier
static double   s_scaled    = 0.0;   // accumulated speed-adjusted ticks
static uint32_t s_last_raw  = 0;
static uint32_t raw_ticks() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return uint32_t(duration_cast<milliseconds>(steady_clock::now() -
                                                start).count() * 60 / 1000);
}
// Speed-adjusted, monotonic game clock (before the +time offset): every real
// tick advances it by s_speed ticks, so the game's TickCount-paced logic runs
// faster/slower while the real-time present throttle keeps the display at 60fps.
static uint32_t scaled_ticks() {
    uint32_t raw = raw_ticks();
    s_scaled += double(raw - s_last_raw) * s_speed;
    s_last_raw = raw;
    return uint32_t(s_scaled);
}
static uint32_t ticks_now() {
    uint32_t s = scaled_ticks();
    uint32_t t = s > s_tick_offset ? s - s_tick_offset : 0;   // offset capped on set
    mem_write32(0x16A, t);   // low-mem Ticks: the MDRV synth polls it raw
    return t;
}

uint32_t ticks_live() { return ticks_now(); }

// Assist: grant `minutes` of game time by rewinding the clock the game reads for
// its time limit (the game tracks elapsed = TickCount - start_tick). Ticks run
// 60/sec, so a minute is 3600 ticks. Capped so TickCount stays >= 60 and can
// never underflow or stall — the offset can't exceed the real elapsed ticks
// (and the early game, where it would, has ample time anyway).
void time_add_minutes(int minutes) {
    if (minutes <= 0) return;
    uint32_t s = scaled_ticks();
    uint32_t cap = s > 60 ? s - 60 : 0;
    uint32_t want = s_tick_offset + uint32_t(minutes) * 3600u;
    s_tick_offset = want < cap ? want : cap;
}

// Assist: set the game-speed multiplier (1.0 = normal). The accumulator banks
// the time elapsed at the old speed before switching, so the clock stays
// monotonic across changes (no jump). Clamped to a sane, playable range.
void time_set_speed(double mult) {
    if (mult < 0.25) mult = 0.25;
    if (mult > 3.0) mult = 3.0;
    scaled_ticks();
    s_speed = mult;
}

static int64_t now_us() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - start).count();
}

// ---- Time Manager: tasks fire for real (guest callback, A1 = TMTask) ----
struct TmTask {
    int64_t due_us = 0;
    bool active = false;
    bool prime_us = false;   // primed with negative count (microseconds)
};
static std::map<uint32_t, TmTask> s_tm_tasks;   // TMTask ptr -> state

void tm_fire_due(Cpu& cpu) {
    static bool s_firing = false;
    if (s_firing || s_tm_tasks.empty()) return;
    s_firing = true;
    // We only get polled at trap sites — once per ~16ms frame during
    // gameplay (the pacing loop busy-waits on raw Ticks reads). Half a
    // frame of lookahead makes fires land symmetrically around their due
    // time, so delta-paced clients (the Mohawk sound service) track real
    // time instead of running ~0.7x slow.
    int64_t now = now_us() + 8000;
    // snapshot keys: a fired task may InsTime/RmvTime, invalidating iterators
    std::vector<uint32_t> tps;
    tps.reserve(s_tm_tasks.size());
    for (auto& [tp, t] : s_tm_tasks)
        if (t.active && t.due_us <= now) tps.push_back(tp);
    for (uint32_t tp : tps) {
        auto it = s_tm_tasks.find(tp);
        if (it == s_tm_tasks.end()) continue;   // removed by an earlier task
        TmTask& t = it->second;
        if (!t.active || t.due_us > now) continue;
        uint32_t addr = mem_read32(tp + 6);     // tmAddr
        if (!addr) continue;                    // not wired up yet: stay due
        t.active = false;
        mem_write16(tp + 4,                     // qType bit15 off: expired
                    uint16_t(mem_read16(tp + 4) & 0x7FFF));
        mem_write32(tp + 10, 0);                // tmCount: no unexpired time
        mem_write32(tp + 14, uint32_t(t.due_us));  // tmWakeUp: expiry stamp
        static int s_fired = 0;
        static const bool trace_tm = std::getenv("POP2_TRACE_TM") != nullptr;
        if (trace_tm)
            std::fprintf(stderr, "[time] fire task=%06X late=%lldms t=%u "
                         "rec30=%08X\n", tp,
                         (long long)((now - t.due_us) / 1000), ticks_now(),
                         mem_read32(tp + 30));
        else if (s_fired < 5 && ++s_fired)
            std::fprintf(stderr, "[time] firing task=%06X addr=%06X\n",
                         tp, addr);
        Cpu saved = cpu;                        // interrupt context: save all
        push32(cpu, 0);                         // fake RA for the callee's RTS
        cpu.a[1] = tp;                          // TM passes the task in A1
        g_interrupt_depth++;
        call_virtual(cpu, addr);
        g_interrupt_depth--;
        cpu = saved;
    }
    s_firing = false;
}

int g_interrupt_depth = 0;   // guest "interrupt-time" callbacks in progress

// ---- Vertical Retrace Manager: VBL tasks tick at 60 Hz (A0 = VBLTask) ----
// VBLTask: +6 vblAddr, +10 vblCount (ticks; fires at 0, task re-arms itself)
static std::map<uint32_t, int64_t> s_vbl_tasks;   // VBLTask ptr -> last tick
static void vbl_debug_dump();
void vbl_fire_due(Cpu& cpu) {
    static bool s_firing = false;
    if (s_firing || s_vbl_tasks.empty()) return;
    s_firing = true;
    vbl_debug_dump();
    int64_t now = now_us();
    std::vector<uint32_t> tps;
    tps.reserve(s_vbl_tasks.size());
    for (auto& [tp, last] : s_vbl_tasks) tps.push_back(tp);
    for (uint32_t tp : tps) {
        auto it = s_vbl_tasks.find(tp);
        if (it == s_vbl_tasks.end()) continue;  // VRemove'd by an earlier task
        int64_t ticks = (now - it->second) / 16667;   // 60 Hz ticks elapsed
        if (ticks <= 0) continue;
        if (ticks > 8) { ticks = 8; it->second = now - 8 * 16667; }
        for (int64_t t = 0; t < ticks; t++) {
            it->second += 16667;
            uint16_t cnt = mem_read16(tp + 10);
            if (!cnt) continue;                 // dormant until re-armed
            --cnt;
            mem_write16(tp + 10, cnt);
            if (cnt) continue;
            uint32_t addr = mem_read32(tp + 6);
            if (!addr) continue;
            static int s_fired = 0;
            if (s_fired < 3 && ++s_fired)
                std::fprintf(stderr, "[vbl] firing task=%06X addr=%06X\n",
                             tp, addr);
            Cpu saved = cpu;                    // interrupt context: save all
            push32(cpu, 0);                     // fake RA for the callee's RTS
            cpu.a[0] = tp;                      // VBL passes the task in A0
            g_interrupt_depth++;
            call_virtual(cpu, addr);            // handler re-arms vblCount
            g_interrupt_depth--;
            cpu = saved;
            it = s_vbl_tasks.find(tp);          // handler may have removed it
            if (it == s_vbl_tasks.end()) break;
        }
    }
    s_firing = false;
}

// ---- Sound Manager double-buffer pump --------------------------------------
// SndPlayDoubleBuffer: the HALESTORM driver renders synth slices into two
// ping-pong buffers; the Sound Manager is supposed to call dbhDoubleBack at
// interrupt time whenever a buffer drains. We approximate the drain clock in
// real time (frames / sampleRate) from the regular tick sites. The doubleback
// advances the MIDI sequencer — it is the heartbeat of music AND of the NIS
// cutscene scripting.
struct DblBuf {
    uint32_t chan = 0, hdr = 0;
    int cur = 0;
    int64_t next_due = 0;
};
static std::vector<DblBuf> s_dblbufs;

static int64_t dblbuf_duration_us(uint32_t hdr, uint32_t bufp) {
    uint32_t frames = mem_read32(bufp);
    uint32_t rate = mem_read16(hdr + 8);    // integer part of Fixed
    if (!rate) rate = 22050;
    if (!frames) frames = 512;
    return int64_t(frames) * 1000000 / rate;
}

void snd_chan_pump(Cpu& cpu);
void sfx_mix_into(uint8_t* buf, uint32_t bytes, int rate, int bits, int chans);

uint64_t g_dbl_fires = 0, g_dbl_frames = 0;

void snd_pump(Cpu& cpu) {
    snd_chan_pump(cpu);
    static bool s_pumping = false;
    if (s_pumping || s_dblbufs.empty()) return;
    s_pumping = true;
    for (auto& db : s_dblbufs) {
        int chans = int16_t(mem_read16(db.hdr + 0));
        int bits = int16_t(mem_read16(db.hdr + 2));
        int rate = mem_read16(db.hdr + 8);      // integer part of Fixed
        if (chans < 1) chans = 1;
        if (bits != 8 && bits != 16) bits = 8;
        if (!rate) rate = 22050;
        // a fired buffer's rendered samples go to the SDL queue; while the
        // device accepts them, audio appetite IS the pacing (sequencer and
        // sound stay locked to real time). Time-based pacing is the
        // headless fallback.
        for (int fired = 0; fired < 8; fired++) {
            uint32_t bufp = mem_read32(db.hdr + 12 + 4 * uint32_t(db.cur));
            uint32_t doubleback = mem_read32(db.hdr + 20);
            if (!bufp || !doubleback || !db.next_due) { db.next_due = 0; break; }
            uint32_t frames = mem_read32(bufp);
            uint32_t bytes = frames * uint32_t(chans) * uint32_t(bits / 8);
            bool by_audio = audio_queued_bytes() > 0 || fired == 0;
            if (by_audio) {
                // keep ~4 buffers queued; stop when the device is sated
                uint32_t target = bytes * 4;
                if (audio_queued_bytes() >= target) break;
            } else if (now_us() < db.next_due) {
                break;
            }
            mem_write32(bufp + 4, mem_read32(bufp + 4) & ~1u);  // ~ready
            Cpu saved = cpu;
            push32(cpu, db.chan);            // pascal: push args left to right
            push32(cpu, bufp);
            push32(cpu, 0);                  // fake RA
            g_interrupt_depth++;
            call_virtual(cpu, doubleback);
            g_interrupt_depth--;
            cpu = saved;
            // the doubleback just refilled bufp: ship it to the speakers
            frames = mem_read32(bufp);
            bytes = frames * uint32_t(chans) * uint32_t(bits / 8);
            g_dbl_fires++;
            g_dbl_frames += frames;
            if (frames && bytes < 0x40000) {
                sfx_mix_into(g_mem + ((bufp + 16) & ADDR_MASK), bytes, rate,
                             bits, chans);
                audio_queue(g_mem + ((bufp + 16) & ADDR_MASK), bytes, rate,
                            bits, chans);
            }
            db.cur ^= 1;
            uint32_t nbuf = mem_read32(db.hdr + 12 + 4 * uint32_t(db.cur));
            if (!nbuf) { db.next_due = 0; break; }
            db.next_due += dblbuf_duration_us(db.hdr, nbuf);
            // resync if we are hopelessly behind (paused in a debugger etc.)
            if (now_us() - db.next_due > 500000)
                db.next_due = now_us();
        }
    }
    s_pumping = false;
}

// ---- Sound Manager channels: bufferCmd playback + callBackCmd completion ---
// PoP2 SFX go through seg21's channel objects: it queues a bufferCmd with a
// stdSH SoundHeader, then a callBackCmd; the channel callback (installed via
// SndNewChannel) clears the engine's "voice busy" flags. Without the callback
// every f5_52c8(id) poll answers "still playing" forever and the death /
// jingle / story-page flows freeze. We track per-channel busy time from the
// sample duration, fire guest callbacks when due, and mix the PCM into the
// outgoing music buffers so the effects are audible.
struct SfxVoice {
    std::vector<uint8_t> pcm;   // 8-bit unsigned mono
    double pos = 0, step = 1;   // resample cursor (sfx rate / out rate)
};
struct SndChanState {
    uint32_t cb = 0;
    int64_t busy_until = 0;
    struct PendingCb { int64_t due; uint16_t p1; uint32_t p2; };
    std::vector<PendingCb> cbs;
};
static std::map<uint32_t, SndChanState> s_snd_chans;
static std::vector<SfxVoice> s_sfx_voices;

// mix active SFX voices into an 8-bit unsigned buffer about to be queued
void sfx_mix_into(uint8_t* buf, uint32_t bytes, int rate, int bits, int chans) {
    if (s_sfx_voices.empty() || bits != 8 || chans != 1 || !rate) return;
    for (auto& v : s_sfx_voices) {
        for (uint32_t i = 0; i < bytes && v.pos < double(v.pcm.size()); i++) {
            int s = int(buf[i]) + int(v.pcm[size_t(v.pos)]) - 128;
            buf[i] = uint8_t(s < 0 ? 0 : s > 255 ? 255 : s);
            v.pos += v.step;
        }
    }
    s_sfx_voices.erase(std::remove_if(s_sfx_voices.begin(), s_sfx_voices.end(),
                                      [](const SfxVoice& v) {
                                          return v.pos >= double(v.pcm.size());
                                      }),
                       s_sfx_voices.end());
}

void snd_chan_pump(Cpu& cpu) {
    static bool s_inside = false;
    if (s_inside || s_snd_chans.empty()) return;
    s_inside = true;
    int64_t now = now_us();
    for (auto& [chan, st] : s_snd_chans) {
        while (!st.cbs.empty() && st.cbs.front().due <= now) {
            auto pc = st.cbs.front();
            st.cbs.erase(st.cbs.begin());
            uint32_t cb = mem_read32(chan + 8);   // game may repoint callBack
            if (!cb) cb = st.cb;
            if (!cb) continue;
            mem_write16(chan + 20, 13);           // cmdInProgress: callBackCmd
            mem_write16(chan + 22, pc.p1);
            mem_write32(chan + 24, pc.p2);
            static int s_logged = 0;
            if (s_logged < 8 && ++s_logged)
                std::fprintf(stderr, "[snd] callback chan=%06X cb=%06X p1=%d\n",
                             chan, cb, pc.p1);
            Cpu saved = cpu;
            push32(cpu, chan);                    // pascal: args left to right
            push32(cpu, chan + 20);               // SndCommand*
            push32(cpu, 0);                       // fake RA
            g_interrupt_depth++;
            call_virtual(cpu, cb);
            g_interrupt_depth--;
            cpu = saved;
        }
    }
    s_inside = false;
}

// POP2_TRACE_VBL: once a second, dump each VBL task's live vblCount
static void vbl_debug_dump() {
    static const bool on = std::getenv("POP2_TRACE_VBL") != nullptr;
    if (!on) return;
    static int64_t s_last = 0;
    int64_t now = now_us();
    if (now - s_last < 1000000) return;
    s_last = now;
    for (auto& [tp, last] : s_vbl_tasks)
        std::fprintf(stderr, "[vbl] task=%06X count=%d addr=%06X\n",
                     tp, mem_read16(tp + 10), mem_read32(tp + 6));
}

// ---------------------------------------------------------------------------
// OS traps
// ---------------------------------------------------------------------------
static void os_trap(Cpu& cpu, uint16_t op) {
    const bool clear = op & 0x200;
    switch (op & 0xF9FF) {
    // ---- Memory Manager ----
    case 0xA122: {  // NewHandle
        uint32_t h;
        // f11_39de unpacks the MDRV synth driver into a fresh handle; pin
        // that one to the recompiled blob's base (seg 25) so the unpacked
        // code/data land where the native functions expect them.
        if (cpu.pc == 0x2B3A08 && cpu.d[0] >= 0x100 &&
            cpu.d[0] - 0x100 <= 0xFF00) {
            h = mm_new_handle_at(0x390000, cpu.d[0]);
            std::fprintf(stderr,
                         "[snd] MDRV unpack handle pinned at 390000 "
                         "(%u bytes)\n", cpu.d[0]);
        } else {
            h = mm_new_handle(cpu.d[0], clear);
        }
        cpu.a[0] = h;
        cpu.d[0] = h ? 0 : uint32_t(int32_t(memFullErr));
        return;
    }
    case 0xA023: mm_dispose_handle(cpu.a[0]); cpu.d[0] = 0; return;
    case 0xA025: {  // GetHandleSize
        int32_t s = mm_get_handle_size(cpu.a[0]);
        cpu.d[0] = uint32_t(s < 0 ? s : s);
        return;
    }
    case 0xA024:    // SetHandleSize
        cpu.d[0] = mm_set_handle_size(cpu.a[0], cpu.d[0])
                       ? 0 : uint32_t(int32_t(memFullErr));
        return;
    case 0xA029: mm_hset_state(cpu.a[0], mm_hget_state(cpu.a[0]) | 0x80); cpu.d[0] = 0; return;  // HLock
    case 0xA02A: mm_hset_state(cpu.a[0], mm_hget_state(cpu.a[0]) & ~0x80); cpu.d[0] = 0; return; // HUnlock
    case 0xA049: mm_hset_state(cpu.a[0], mm_hget_state(cpu.a[0]) | 0x40); cpu.d[0] = 0; return;  // HPurge
    case 0xA04A: mm_hset_state(cpu.a[0], mm_hget_state(cpu.a[0]) & ~0x40); cpu.d[0] = 0; return; // HNoPurge
    case 0xA069: cpu.d[0] = mm_hget_state(cpu.a[0]); return;             // HGetState
    case 0xA06A: mm_hset_state(cpu.a[0], uint8_t(cpu.d[0])); cpu.d[0] = 0; return;  // HSetState
    case 0xA064: cpu.d[0] = 0; return;                                   // MoveHHi
    case 0xA11E: {  // NewPtr
        uint32_t p = mm_new_ptr(cpu.d[0], clear);
        cpu.a[0] = p;
        cpu.d[0] = p ? 0 : uint32_t(int32_t(memFullErr));
        return;
    }
    case 0xA01F: mm_dispose_ptr(cpu.a[0]); cpu.d[0] = 0; return;
    case 0xA021: {  // GetPtrSize
        int32_t s = mm_get_ptr_size(cpu.a[0]);
        cpu.d[0] = uint32_t(s);
        return;
    }
    case 0xA02E: {  // BlockMove
        uint32_t src = cpu.a[0] & ADDR_MASK, dst = cpu.a[1] & ADDR_MASK;
        uint32_t n = cpu.d[0];
        if (n && src + n <= MEM_SIZE && dst + n <= MEM_SIZE)
            std::memmove(g_mem + dst, g_mem + src, n);
        cpu.d[0] = 0;
        return;
    }
    case 0xA036: cpu.d[0] = 0; return;                                   // MoreMasters
    case 0xA063: cpu.d[0] = 0; return;                                   // MaxApplZone
    case 0xA02C: cpu.d[0] = 0; return;                                   // InitApplZone
    case 0xA019: cpu.d[0] = 0; return;                                   // InitZone
    case 0xA02D: cpu.d[0] = 0; return;                                   // SetApplLimit
    case 0xA040: cpu.d[0] = 0; return;                                   // ResrvMem
    case 0xA01C: cpu.d[0] = mm_free_mem(); return;                       // FreeMem
    case 0xA11D: cpu.d[0] = mm_free_mem(); cpu.a[0] = 0; return;         // MaxMem
    case 0xA061: cpu.d[0] = mm_free_mem(); return;                       // MaxBlock
    case 0xA162: cpu.d[0] = mm_free_mem(); cpu.a[0] = mm_free_mem(); return;  // PurgeSpace
    case 0xA065: cpu.d[0] = cpu.a[7] - LOWMEM_TOP; return;               // StackSpace
    case 0xA055: return;                                                 // StripAddress: D0 -> D0
    case 0xA128: cpu.a[0] = mm_recover_handle(cpu.a[0]); return;         // RecoverHandle
    case 0xA126:                                                         // HandleZone
    case 0xA11A: cpu.a[0] = 0x2800; cpu.d[0] = 0; return;                // GetZone
    case 0xA01B: cpu.d[0] = 0; return;                                   // SetZone
    // ---- File Manager (param block in A0) ----
    case 0xA014: {  // GetVol: ioNamePtr@18, ioVRefNum@22
        uint32_t pb = cpu.a[0];
        uint32_t namep = mem_read32(pb + 18);
        if (namep) write_pstr(namep, "PoP2");
        mem_write16(pb + 22, uint16_t(-1));   // fake vRefNum
        mem_write16(pb + 16, 0);              // ioResult
        cpu.d[0] = 0;
        return;
    }
    case 0xA015: cpu.d[0] = 0; mem_write16(cpu.a[0] + 16, 0); return;    // SetVol
    case 0xA000:    // Open / HOpen (H-variants carry ioDirID at offset 48)
    case 0xA00A: {  // OpenRF / HOpenRF
        uint32_t pb = cpu.a[0];
        uint32_t namep = mem_read32(pb + 18);
        std::string name = namep ? read_pstr(namep) : "";
        if (!name.empty() && name[0] == '.') {
            // driver open (.Sony, .Sound, ...): hand out the unit-table
            // refnum we expose for our fake drive, or a generic one
            int16_t dref = name == ".ATADisk" ? -37 : -5;
            std::fprintf(stderr, "[fs] OpenDriver('%s') -> %d\n",
                         name.c_str(), dref);
            mem_write16(pb + 24, uint16_t(dref));
            mem_write16(pb + 16, 0);
            cpu.d[0] = 0;
            return;
        }
        int32_t dirID = (op & 0x0200) ? int32_t(mem_read32(pb + 48)) : 0;
        int16_t vref = int16_t(mem_read16(pb + 22));
        int16_t ref = fs_open_at(vref, dirID, name, (op & 0xFF) == 0x0A);
        int16_t err = ref < 0 ? ref : 0;
        if (ref >= 0) mem_write16(pb + 24, uint16_t(ref));
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA008: {  // Create / HCreate: make the data file
        uint32_t pb = cpu.a[0];
        uint32_t namep = mem_read32(pb + 18);
        std::string name = namep ? read_pstr(namep) : "";
        int32_t dirID = (op & 0x0200) ? int32_t(mem_read32(pb + 48)) : 0;
        SpecInfo si = fs_make_spec(int16_t(mem_read16(pb + 22)), dirID, name);
        int16_t err;
        if (si.exists) err = dupFNErr;
        else if (si.err != noErr && si.err != fnfErr) err = si.err;
        else err = fs_create_file(si.parID, si.name, 0, 0);
        std::fprintf(stderr, "[fs] Create('%s') -> %d\n", name.c_str(), err);
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA00C: {  // GetFileInfo / HGetFileInfo: stat by name into the pblock
        uint32_t pb = cpu.a[0];
        uint32_t namep = mem_read32(pb + 18);
        std::string name = namep ? read_pstr(namep) : "";
        int32_t dirID = (op & 0x0200) ? int32_t(mem_read32(pb + 48)) : 0;
        int16_t dirIdx = int16_t(mem_read16(pb + 28));
        SpecInfo si = fs_make_spec(int16_t(mem_read16(pb + 22)), dirID, name);
        int16_t err = dirIdx > 0 ? fnfErr        // indexed walk: unsupported
                      : !si.exists ? (si.err == noErr ? fnfErr : si.err)
                      : si.is_dir  ? fnfErr
                                   : noErr;
        if (err == noErr) {
            int32_t len = 0, rlen = 0;
            fs_stat_host(si.host, nullptr, &len, &rlen);
            mem_write8(pb + 30, 0);                   // ioFlAttrib: unlocked
            mem_write8(pb + 31, 0);                   // ioFlVersNum
            for (int i = 0; i < 16; i++) mem_write8(pb + 32 + i, 0);  // FndrInfo
            mem_write32(pb + 48, 0);                  // ioFlNum
            mem_write16(pb + 52, 0);                  // ioFlStBlk
            mem_write32(pb + 54, uint32_t(len));      // ioFlLgLen
            mem_write32(pb + 58, uint32_t(len));      // ioFlPyLen
            mem_write16(pb + 62, 0);                  // ioFlRStBlk
            mem_write32(pb + 64, uint32_t(rlen));     // ioFlRLgLen
            mem_write32(pb + 68, uint32_t(rlen));     // ioFlRPyLen
            mem_write32(pb + 72, 0);                  // ioFlCrDat
            mem_write32(pb + 76, 0);                  // ioFlMdDat
        }
        std::fprintf(stderr, "[fs] GetFileInfo('%s') -> %d\n", name.c_str(), err);
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA00D: {  // SetFileInfo / HSetFileInfo: accept type/creator, no store
        uint32_t pb = cpu.a[0];
        uint32_t namep = mem_read32(pb + 18);
        std::string name = namep ? read_pstr(namep) : "";
        int32_t dirID = (op & 0x0200) ? int32_t(mem_read32(pb + 48)) : 0;
        SpecInfo si = fs_make_spec(int16_t(mem_read16(pb + 22)), dirID, name);
        int16_t err = si.exists ? noErr : (si.err == noErr ? fnfErr : si.err);
        std::fprintf(stderr, "[fs] SetFileInfo('%s' type=%08X creator=%08X) -> %d\n",
                     name.c_str(), mem_read32(pb + 32), mem_read32(pb + 36), err);
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA004: case 0xA005: case 0xA006: {  // Control / Status / KillIO
        uint32_t pb = cpu.a[0];
        if (s_trace)
            std::fprintf(stderr, "[fs] %s(ref=%d csCode=%d)\n",
                         (op & 0xFF) == 4   ? "Control"
                         : (op & 0xFF) == 5 ? "Status"
                                            : "KillIO",
                         int16_t(mem_read16(pb + 24)),
                         int16_t(mem_read16(pb + 26)));
        mem_write16(pb + 16, 0);    // ioResult: noErr (csParam left as-is)
        cpu.d[0] = 0;
        return;
    }
    case 0xA001: {  // Close
        uint32_t pb = cpu.a[0];
        int16_t err = fs_close(int16_t(mem_read16(pb + 24)));
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA002: {  // Read: ioRefNum@24, ioBuffer@32, ioReqCount@36, ioPosMode@44, ioPosOffset@46
        uint32_t pb = cpu.a[0];
        uint32_t act = 0;
        int32_t err = fs_read(int16_t(mem_read16(pb + 24)), mem_read32(pb + 32),
                              mem_read32(pb + 36), mem_read16(pb + 44),
                              int32_t(mem_read32(pb + 46)), &act);
        mem_write32(pb + 40, act);            // ioActCount
        mem_write32(pb + 46, uint32_t(fs_get_fpos(int16_t(mem_read16(pb + 24)))));
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(err);
        return;
    }
    case 0xA003: {  // Write — pretend success
        uint32_t pb = cpu.a[0];
        uint32_t act = 0;
        int32_t err = fs_write(int16_t(mem_read16(pb + 24)), mem_read32(pb + 32),
                               mem_read32(pb + 36), mem_read16(pb + 44),
                               int32_t(mem_read32(pb + 46)), &act);
        mem_write32(pb + 40, act);
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(err);
        return;
    }
    case 0xA011: {  // GetEOF -> ioMisc@28
        uint32_t pb = cpu.a[0];
        int32_t sz = fs_get_eof(int16_t(mem_read16(pb + 24)));
        mem_write32(pb + 28, uint32_t(sz < 0 ? 0 : sz));
        int16_t err = sz < 0 ? int16_t(-38) : 0;
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA012: cpu.d[0] = 0; mem_write16(cpu.a[0] + 16, 0); return;    // SetEOF
    case 0xA044: {  // SetFPos
        uint32_t pb = cpu.a[0];
        int32_t err = fs_set_fpos(int16_t(mem_read16(pb + 24)),
                                  mem_read16(pb + 44),
                                  int32_t(mem_read32(pb + 46)));
        mem_write32(pb + 46, uint32_t(fs_get_fpos(int16_t(mem_read16(pb + 24)))));
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(err);
        return;
    }
    case 0xA018: {  // GetFPos
        uint32_t pb = cpu.a[0];
        mem_write32(pb + 46, uint32_t(fs_get_fpos(int16_t(mem_read16(pb + 24)))));
        mem_write16(pb + 44, 0);
        mem_write16(pb + 16, 0);
        cpu.d[0] = 0;
        return;
    }
    case 0xA013: cpu.d[0] = 0; return;                                   // FlushVol
    case 0xA060: {  // FSDispatch (HFS): selector in D0.W
        uint16_t sel = uint16_t(cpu.d[0]);
        uint32_t pb = cpu.a[0];
        int16_t err = 0;
        if (s_trace) {
            uint32_t namep = mem_read32(pb + 18);
            std::fprintf(stderr, "[fs] FSDispatch sel=0x%02X dirIndex=%d "
                         "name='%s' pc=%06X\n", sel,
                         int16_t(mem_read16(pb + 28)),
                         namep ? read_pstr(namep).c_str() : "",
                         cpu.pc);
        }
        switch (sel) {
        case 0x01: break;                    // OpenWD: keep vRefNum as wdRefNum
        case 0x02: break;                    // CloseWD
        case 0x07:                           // GetWDInfo
            mem_write16(pb + 32, uint16_t(-1));   // ioWDVRefNum
            mem_write32(pb + 48, 2);              // ioWDDirID = root
            break;
        case 0x1A: {                         // OpenDF (data fork)
            uint32_t namep = mem_read32(pb + 18);
            std::string name = namep ? read_pstr(namep) : "";
            int32_t dirID = (op & 0x0200) ? int32_t(mem_read32(pb + 48)) : 0;
            int16_t ref = fs_open_at(int16_t(mem_read16(pb + 22)), dirID,
                                     name, false);
            if (ref >= 0) mem_write16(pb + 24, uint16_t(ref));
            err = ref < 0 ? ref : 0;
            break;
        }
        case 0x08: {                         // GetFCBInfo: by ioRefNum
            int16_t ref = int16_t(mem_read16(pb + 24));
            if (int16_t(mem_read16(pb + 28)) > 0) {   // ioFCBIndx: no enumeration
                err = paramErr;
                break;
            }
            std::string fname;
            int32_t parID = 0, eofv = 0, posv = 0;
            if (!fs_fcb_info(ref, &fname, &parID, &eofv, &posv)) {
                err = fnOpnErr;
                break;
            }
            uint32_t namep = mem_read32(pb + 18);
            if (namep) write_pstr(namep, fname);
            mem_write32(pb + 32, uint32_t(uint16_t(ref)));  // ioFCBFlNm
            mem_write16(pb + 36, 0);                  // ioFCBFlags
            mem_write16(pb + 38, 0);                  // ioFCBStBlk
            mem_write32(pb + 40, uint32_t(eofv));     // ioFCBEOF
            mem_write32(pb + 44, uint32_t(eofv));     // ioFCBPLen
            mem_write32(pb + 48, uint32_t(posv));     // ioFCBCrPs
            mem_write16(pb + 52, uint16_t(-1));       // ioFCBVRefNum
            mem_write32(pb + 54, 512);                // ioFCBClpSiz
            mem_write32(pb + 58, uint32_t(parID));    // ioFCBParID
            if (s_trace)
                std::fprintf(stderr, "[fs] GetFCBInfo(ref %d) -> '%s' parID %d "
                             "eof %d\n", ref, fname.c_str(), parID, eofv);
            break;
        }
        case 0x09: {                         // GetCatInfo
            int16_t dirIndex = int16_t(mem_read16(pb + 28));
            uint32_t namep = mem_read32(pb + 18);
            int32_t dirID = int32_t(mem_read32(pb + 48));
            if (dirIndex > 0) { err = fnfErr; break; }   // no enumeration
            if (dirIndex < 0) {              // describe directory by ioDirID
                std::string dname;
                int32_t parID = 0;
                if (!fs_dir_info(dirID, &dname, &parID)) {
                    err = dirNFErr;
                    break;
                }
                if (namep) write_pstr(namep, dname);
                mem_write8(pb + 30, 0x10);   // ioFlAttrib: directory
                mem_write32(pb + 48, uint32_t(dirID ? dirID : 2));
                mem_write16(pb + 52, 30);    // ioDrNmFls
                mem_write32(pb + 100, uint32_t(parID));  // ioDrParID
                break;
            }
            std::string name = namep ? read_pstr(namep) : "";
            SpecInfo s = fs_make_spec(int16_t(mem_read16(pb + 22)), dirID, name);
            bool is_dir = false;
            int32_t dlen = 0, rlen = 0;
            if (!s.exists ||
                !fs_stat_host(s.host, &is_dir, &dlen, &rlen)) {
                err = fnfErr;
                break;
            }
            mem_write8(pb + 30, is_dir ? 0x10 : 0x00);   // ioFlAttrib
            if (is_dir) {
                mem_write32(pb + 48, uint32_t(s.dir_id));  // ioDrDirID
                mem_write16(pb + 52, 30);                  // ioDrNmFls
                mem_write32(pb + 100, uint32_t(s.parID));  // ioDrParID
            } else {
                uint32_t type, creator;
                fs_get_finfo(s.host, &type, &creator);
                mem_write32(pb + 32, type);              // fdType
                mem_write32(pb + 36, creator);           // fdCreator
                mem_write32(pb + 48, 1000);              // ioFlNum
                mem_write32(pb + 54, uint32_t(dlen));    // ioFlLgLen
                mem_write32(pb + 58, uint32_t(dlen));    // ioFlPyLen
                mem_write32(pb + 64, uint32_t(rlen));    // ioFlRLgLen
                mem_write32(pb + 68, uint32_t(rlen));    // ioFlRPyLen
                mem_write32(pb + 100, uint32_t(s.parID));  // ioFlParID
            }
            break;
        }
        default:
            fatal("FSDispatch selector 0x%02X unimplemented at pc=%06X",
                  sel, cpu.pc);
        }
        mem_write16(pb + 16, uint16_t(err));
        cpu.d[0] = uint32_t(int32_t(err));
        return;
    }
    case 0xA007: {  // GetVolInfo / HGetVInfo (bit 9 masked off)
        uint32_t pb = cpu.a[0];
        int16_t volIndex = int16_t(mem_read16(pb + 28));
        if (volIndex > 1) {                  // only one volume
            mem_write16(pb + 16, uint16_t(-35));  // nsvErr
            cpu.d[0] = uint32_t(int32_t(-35));
            return;
        }
        uint32_t namep = mem_read32(pb + 18);
        if (s_trace)
            std::fprintf(stderr, "[fs] %s(volIndex %d, vRefNum %d)\n",
                         (op & 0x0200) ? "HGetVInfo" : "GetVolInfo", volIndex,
                         int16_t(mem_read16(pb + 22)));
        if (namep) write_pstr(namep, "PoP2");
        mem_write16(pb + 22, uint16_t(-1));  // ioVRefNum
        mem_write32(pb + 30, 0xA7C30000);    // ioVCrDate (some 1993 date)
        mem_write32(pb + 34, 0xA7C30000);    // ioVLsMod
        mem_write16(pb + 38, 0);             // ioVAtrb: not locked
        mem_write16(pb + 40, 100);           // ioVNmFls
        mem_write16(pb + 42, 0);             // ioVBitMap
        mem_write16(pb + 44, 0);             // ioAllocPtr
        mem_write16(pb + 46, 0x7FFF);        // ioVNmAlBlks
        mem_write32(pb + 48, 0x8000);        // ioVAlBlkSiz (32K)
        mem_write32(pb + 52, 0x8000);        // ioVClpSiz
        mem_write16(pb + 56, 16);            // ioAlBlSt
        mem_write32(pb + 58, 2000);          // ioVNxtCNID
        mem_write16(pb + 62, 0x7FFF);        // ioVFrBlk: ~1 GB free
        if (op & 0x0200) {                   // HGetVInfo extension
            mem_write16(pb + 64, 0x4244);    // ioVSigWord: HFS
            mem_write16(pb + 66, 1);         // ioVDrvInfo: drive 1
            mem_write16(pb + 68, uint16_t(-37));  // ioVDRefNum: driver
            mem_write16(pb + 70, 0);         // ioVFSID: local
            mem_write32(pb + 72, 0);         // ioVBkUp
            mem_write16(pb + 76, 0);         // ioVSeqNum
            mem_write32(pb + 78, 1);         // ioVWrCnt
            mem_write32(pb + 82, 100);       // ioVFilCnt
            mem_write32(pb + 86, 10);        // ioVDirCnt
        }
        mem_write16(pb + 16, 0);
        cpu.d[0] = 0;
        return;
    }
    // ---- misc OS ----
    case 0xA05C:    // MemoryDispatch (Hold/Unhold/Lock/UnlockMemory...) —
        cpu.d[0] = 0;   // flat memory, everything is always locked: noErr
        return;
    case 0xA05D: {  // SwapMMUMode: D0.B = wanted mode -> previous mode
        // returns via MOVEQ on real ROMs: the FULL D0 becomes the old mode.
        // The PoP2 blitter relies on this clearing D0's high bits before its
        // 32-bit row loop — preserving them sprayed fills across the heap.
        static uint8_t s_mode = 1;             // we are always "32-bit clean"
        uint8_t want = uint8_t(cpu.d[0]);
        cpu.d[0] = s_mode;
        s_mode = want;
        return;
    }
    case 0xA032: cpu.d[0] = 0; return;                                   // FlushEvents
    case 0xA12F: {  // PPostEvent: A0.W = event code, D0.L = event message;
        // returns A0 = EvQElPtr, D0 = OSErr. Register convention per the recomp
        // call site (seg02 f2_32xx). PoP2 posts an event on a mouse click here;
        // leaving it unimplemented abort()ed the whole instance. We deliver the
        // event through the runtime queue and hand back a scratch EvQEl the
        // caller fills in (it is never read back — video_next_event synthesizes
        // when/where/modifiers).
        video_post_event(uint16_t(cpu.a[0]), cpu.d[0]);
        cpu.a[0] = EVQEL_SCRATCH;
        cpu.d[0] = 0;  // noErr
        return;
    }
    case 0xA03B: {  // Delay: A0 = ticks
        uint32_t t = cpu.a[0];
#ifdef __EMSCRIPTEN__
        emscripten_sleep(t * 1000 / 60);  // Asyncify: yield instead of block
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(t * 1000 / 60));
#endif
        cpu.d[0] = ticks_now();
        return;
    }
    // ---- Time Manager ----
    case 0xA058: {  // InsTime / InsXTime: A0 = TMTask*
        static const bool trace_tm = std::getenv("POP2_TRACE_TM") != nullptr;
        if (trace_tm)                           // else this floods every TM call
            std::fprintf(stderr, "[time] InsTime task=%06X tmAddr=%06X pc=%06X\n",
                         cpu.a[0], mem_read32(cpu.a[0] + 6), cpu.pc);
        s_tm_tasks[cpu.a[0]];                   // register, inactive
        mem_write16(cpu.a[0] + 4, 1);           // qType: vType, not active
        mem_write32(cpu.a[0] + 10, 0);          // tmCount starts clean
        cpu.d[0] = 0;
        return;
    }
    case 0xA059: {  // RmvTime: tmCount = unexpired time, in the primed form
        uint32_t tmCount = 0;
        auto it = s_tm_tasks.find(cpu.a[0]);
        if (it != s_tm_tasks.end() && it->second.active) {
            int64_t left = it->second.due_us - now_us();
            if (left < 0) left = 0;
            tmCount = it->second.prime_us ? uint32_t(-left)
                                          : uint32_t(left / 1000);
        }
        mem_write32(cpu.a[0] + 10, tmCount);
        mem_write16(cpu.a[0] + 4,               // no longer active
                    uint16_t(mem_read16(cpu.a[0] + 4) & 0x7FFF));
        s_tm_tasks.erase(cpu.a[0]);
        cpu.d[0] = 0;
        return;
    }
    case 0xA198: cpu.d[0] = 0; return;  // HWPriv: cache flush etc. — no-op
    case 0xA033: {  // VInstall: A0 = VBLTask*
        static const bool trace_vbl = std::getenv("POP2_TRACE_VBL") != nullptr;
        if (trace_vbl)
            std::fprintf(stderr, "[vbl] VInstall task=%06X vblAddr=%06X count=%d\n",
                         cpu.a[0], mem_read32(cpu.a[0] + 6),
                         mem_read16(cpu.a[0] + 10));
        s_vbl_tasks.emplace(cpu.a[0], now_us());
        cpu.d[0] = 0;
        return;
    }
    case 0xA034: s_vbl_tasks.erase(cpu.a[0]); cpu.d[0] = 0; return;      // VRemove
    case 0xA05A: {  // PrimeTime: A0 = TMTask*, D0 = ms (>0) or -us (<0)
        int32_t count = int32_t(cpu.d[0]);
        TmTask& t = s_tm_tasks[cpu.a[0]];
        static const bool trace_tm = std::getenv("POP2_TRACE_TM") != nullptr;
        if (trace_tm)
            std::fprintf(stderr, "[time] Prime task=%06X count=%d t=%u%s\n",
                         cpu.a[0], count, ticks_now(),
                         t.active ? " (active: kept)" : "");
        // Classic TM: priming an already-active task does NOT reset its
        // deadline. The Mohawk timer service relies on this — its client
        // re-primes the real interval from inside the callback, then the
        // service tail issues a junk-valued fallback prime; last-write-wins
        // here stretched every sound/music countdown ~25x (death restart
        // took 80s, story pages ~2min).
        if (t.active) { cpu.d[0] = 0; return; }
        t.due_us = now_us() + (count >= 0 ? int64_t(count) * 1000
                                          : -int64_t(count));
        t.active = true;
        t.prime_us = count < 0;
        mem_write16(cpu.a[0] + 4,               // qType bit15: task active
                    uint16_t(mem_read16(cpu.a[0] + 4) | 0x8000));
        mem_write32(cpu.a[0] + 14, uint32_t(t.due_us));  // tmWakeUp
        cpu.d[0] = 0;
        return;
    }
    case 0xA047:    // SetTrapAddress — record what the game patches
        std::fprintf(stderr, "[trap] SetTrapAddress: trap 0x%04X -> %06X\n",
                     uint16_t(cpu.d[0]), cpu.a[0]);
        cpu.d[0] = 0;
        return;
    case 0xA146: {  // GetTrapAddress / GetOSTrapAddress / GetToolTrapAddress
        uint16_t num = uint16_t(cpu.d[0]);
        uint16_t full;
        if ((num & 0xF000) == 0xA000) full = num;
        else if (op & 0x0400 || num >= 0x100)         // tool variant / big num
            full = uint16_t(0xA800 | (num & 0x7FF));
        else
            full = uint16_t(0xA000 | (num & 0xFF));
        cpu.a[0] = trap_stub_addr(full);
        return;
    }
    case 0xA1AD: {  // Gestalt
        uint32_t sel = cpu.d[0];
        uint32_t resp = 0;
        int32_t err = 0;
        switch (sel) {
        case 0x73797376: resp = 0x0700; break;        // 'sysv' System 7.0
        case 0x70726f63: resp = 4; break;             // 'proc' 68030
        case 0x71640120: resp = 1; break;             // 'qd  '?? (not real)
        default:
            err = -5551;                              // gestaltUndefSelectorErr
            std::fprintf(stderr, "[trap] Gestalt('%s') -> undef\n",
                         type_to_str(sel).c_str());
        }
        cpu.a[0] = resp;
        cpu.d[0] = uint32_t(err);
        return;
    }
    case 0xA090: {  // SysEnvirons: A0 = SysEnvRec*, D0.W = version
        // SysEnvRec is EXACTLY 16 bytes; hasFPU/hasColorQD are BYTES.
        // (A word-sized overflow here used to smash the caller's saved A6.)
        uint32_t p = cpu.a[0];
        mem_write16(p + 0, 2);        // environsVersion
        mem_write16(p + 2, 3);        // machineType
        mem_write16(p + 4, 0x0700);   // systemVersion
        mem_write16(p + 6, 4);        // processor: 68030
        mem_write8(p + 8, 0);         // hasFPU
        mem_write8(p + 9, 1);         // hasColorQD: the game gates ALL its
                                      // Palette Manager work on this
        mem_write16(p + 10, 2);       // keyboardType
        mem_write16(p + 12, 0);       // atDrvrVersNum
        mem_write16(p + 14, 0);       // sysVRefNum
        cpu.d[0] = 0;
        return;
    }
    }
    fatal("unimplemented OS trap %s (0x%04X) at pc=%06X",
          trap_display_name(op), op, cpu.pc);
}

// ---------------------------------------------------------------------------
// Toolbox traps
// ---------------------------------------------------------------------------
static void tb_trap(Cpu& cpu, uint16_t op) {
    switch (op) {
    // ---- Resource Manager ----
    case 0xA9A0: {  // GetResource(type:4, id:2): Handle
        int16_t id = int16_t(arg_w(cpu));
        uint32_t type = arg_l(cpu);
        uint32_t h = rm_get_resource(type, id, false);
        if (s_trace)
            std::fprintf(stderr, "[trap]   GetResource('%s', %d) -> %06X\n",
                         type_to_str(type).c_str(), id, h);
        ret_l(cpu, h);
        return;
    }
    case 0xA80C: {  // Get1Resource
        int16_t id = int16_t(arg_w(cpu));
        uint32_t type = arg_l(cpu);
        ret_l(cpu, rm_get_resource(type, id, true));
        return;
    }
    case 0xA9A1: {  // GetNamedResource(type:4, name: Str255 ptr): Handle
        uint32_t namep = arg_l(cpu);
        uint32_t type = arg_l(cpu);
        ret_l(cpu, rm_get_named_resource(type, read_pstr(namep)));
        return;
    }
    case 0xA9A2: (void)arg_l(cpu); return;                  // LoadResource
    case 0xA9A3: rm_release_resource(arg_l(cpu)); return;   // ReleaseResource
    case 0xA992: rm_detach_resource(arg_l(cpu)); return;    // DetachResource
    case 0xA9A4: ret_w(cpu, uint16_t(rm_home_res_file(arg_l(cpu)))); return;
    case 0xA9A5: ret_l(cpu, uint32_t(rm_size_rsrc(arg_l(cpu)))); return;  // SizeRsrc
    case 0xA821: ret_l(cpu, uint32_t(rm_size_rsrc(arg_l(cpu)))); return;  // MaxSizeRsrc
    case 0xA9AF: ret_w(cpu, uint16_t(rm_res_error())); return;
    case 0xA994: ret_w(cpu, uint16_t(rm_cur_res_file())); return;
    case 0xA997: {  // OpenResFile(name: Str255 ptr): Integer
        uint32_t namep = arg_l(cpu);
        ret_w(cpu, uint16_t(rm_open_res_file(read_pstr(namep))));
        return;
    }
    case 0xA81A: {  // HOpenResFile(vRef:2, dirID:4, name:4, perm:2): Integer
        (void)arg_w(cpu);
        uint32_t namep = arg_l(cpu);
        (void)arg_l(cpu);
        (void)arg_w(cpu);
        ret_w(cpu, uint16_t(rm_open_res_file(read_pstr(namep))));
        return;
    }
    case 0xA99A: rm_close_res_file(int16_t(arg_w(cpu))); return;  // CloseResFile
    case 0xA998: rm_use_res_file(int16_t(arg_w(cpu))); return;    // UseResFile
    case 0xA99B: (void)arg_w(cpu); return;                        // SetResLoad
    case 0xA993: (void)arg_w(cpu); return;                        // SetResPurge
    case 0xA99C: {  // CountResources(type): Integer
        uint32_t type = arg_l(cpu);
        ret_w(cpu, uint16_t(rm_count_resources(type, false)));
        return;
    }
    case 0xA80D: {  // Count1Resources
        uint32_t type = arg_l(cpu);
        ret_w(cpu, uint16_t(rm_count_resources(type, true)));
        return;
    }
    case 0xA99D: {  // GetIndResource(type:4, index:2): Handle
        int16_t idx = int16_t(arg_w(cpu));
        uint32_t type = arg_l(cpu);
        ret_l(cpu, rm_get_ind_resource(type, idx, false));
        return;
    }
    case 0xA9A8: {  // GetResInfo(h:4, VAR id:4, VAR type:4, VAR name:4)
        uint32_t namep = arg_l(cpu);
        uint32_t typep = arg_l(cpu);
        uint32_t idp = arg_l(cpu);
        uint32_t h = arg_l(cpu);
        int16_t id = 0; uint32_t type = 0; std::string name;
        rm_get_res_info(h, &id, &type, &name);
        if (idp) mem_write16(idp, uint16_t(id));
        if (typep) mem_write32(typep, type);
        if (namep) write_pstr(namep, name);
        return;
    }
    case 0xA9A6: (void)arg_l(cpu); ret_w(cpu, 0); return;   // GetResAttrs
    case 0xA9A7: { (void)arg_w(cpu); (void)arg_l(cpu); return; }  // SetResAttrs
    case 0xA995: ret_w(cpu, 0); return;                     // InitResources
    case 0xA996: return;                                    // RsrcZoneInit
    case 0xA822: {  // ResourceDispatch: selector in D0.W
        uint16_t sel = uint16_t(cpu.d[0]);
        if (sel == 1) {  // ReadPartialResource(h, offset, buffer, count)
            uint32_t count = arg_l(cpu);
            uint32_t buffer = arg_l(cpu);
            uint32_t offset = arg_l(cpu);
            uint32_t h = arg_l(cpu);
            int32_t hsize = mm_get_handle_size(h);
            if (h && hsize >= 0 && int32_t(offset) <= hsize) {
                uint32_t n = std::min<uint32_t>(count, uint32_t(hsize) - offset);
                std::memmove(g_mem + (buffer & ADDR_MASK),
                             g_mem + ((mem_read32(h) + offset) & ADDR_MASK), n);
                rm_set_res_error(n == count ? 0 : eofErr);
            } else {
                rm_set_res_error(resNotFound);
            }
            return;
        }
        if (sel == 3) {  // SetResourceSize(h, newSize)
            uint32_t newSize = arg_l(cpu);
            uint32_t h = arg_l(cpu);
            rm_set_res_error(mm_set_handle_size(h, newSize) ? 0 : memFullErr);
            return;
        }
        fatal("ResourceDispatch selector %u unimplemented at pc=%06X",
              sel, cpu.pc);
    }
    case 0xA9B1: {  // CreateResFile(name): make a fresh dump dir for it
        uint32_t namep = arg_l(cpu);
        rm_create_res_file(namep ? read_pstr(namep) : "");
        return;
    }
    case 0xA9EA: {  // Pack3 — Standard File; selector word on top of stack
        uint16_t sel = arg_w(cpu);
        std::fprintf(stderr, "[sf] Pack3 sel=%u\n", sel);
        switch (sel) {
        case 1: case 3: {   // SFPutFile / SFPPutFile (prompt, origName)
            uint32_t replyp = arg_l(cpu);
            (void)arg_l(cpu);                      // dlgHook
            uint32_t origp = arg_l(cpu);
            (void)arg_l(cpu);                      // prompt
            (void)arg_l(cpu);                      // where (Point by value)
            std::string name = origp ? read_pstr(origp) : "";
            if (name.empty()) name = "Saved Game";
            std::string slot = fs_take_save_override();   // web save-manager:
            if (!slot.empty()) name = slot;               // save to chosen slot
            mem_write8(replyp, 1);                 // good
            mem_write8(replyp + 1, 0);
            mem_write32(replyp + 2, 0);            // fType
            mem_write16(replyp + 6, 0);            // vRefNum
            mem_write16(replyp + 8, 0);            // version
            write_pstr(replyp + 10, name);
            std::fprintf(stderr, "[sf] SFPutFile -> '%s'\n", name.c_str());
            return;
        }
        case 2: case 4: {   // SFGetFile / SFPGetFile
            uint32_t replyp = arg_l(cpu);
            (void)arg_l(cpu);                      // dlgHook
            (void)arg_l(cpu);                      // typeList
            (void)arg_w(cpu);                      // numTypes
            (void)arg_l(cpu);                      // fileFilter
            (void)arg_l(cpu);                      // prompt(2)/unused
            (void)arg_l(cpu);                      // where
            // No picker UI: open the slot the web save-manager chose, else the
            // first saved game (a plain file with the 'POP2' magic), or cancel.
            std::string name = fs_take_save_override();
            if (name.empty()) name = fs_first_pop2_save();
            if (!name.empty()) {
                // Clear the kid's HP-change machinery before the load so a level-14
                // finale's HP-drain state can't leak into a non-finale level opened
                // next in the same session. a5-20462 gates the per-tick HP-delta
                // routine (f2_68c0); a5-20656 is the pending delta f2_54d6 applies.
                // Both live outside the kid block the save restores, so they would
                // otherwise persist across the load. A finale save re-arms them
                // within a few ticks through its own env8582==6 path, so the actual
                // level-14 finale still drains.
                mem_write16(A5_BASE - 20462, 0);
                mem_write16(A5_BASE - 20656, 0);
                // a5-20462 also gates the main loop's per-frame render; after a
                // load it can stay 0 and the loaded level sits frozen. Have
                // video_pump re-assert it for a window of frames so the camera
                // settles and the scene draws (a5-20656 stays 0 -> no drain leak).
                g_post_load_pump = 180;
            }
            mem_write8(replyp, name.empty() ? 0 : 1);
            mem_write8(replyp + 1, 0);
            mem_write32(replyp + 2, 0x50533247);   // fType 'PS2G' (unused)
            mem_write16(replyp + 6, 0);            // vRefNum: default volume
            mem_write16(replyp + 8, 0);
            write_pstr(replyp + 10, name);
            std::fprintf(stderr, "[sf] SFGetFile -> '%s'\n",
                         name.empty() ? "(cancel)" : name.c_str());
            return;
        }
        default:
            fatal("Pack3 selector %u unimplemented", sel);
        }
        return;
    }
    case 0xA999: { (void)arg_w(cpu); return; }              // UpdateResFile
    case 0xA9B0:                                            // WriteResource
    case 0xA9AA: {  // ChangedResource: persist into the dump tree
        uint32_t h = arg_l(cpu);
        rm_changed_resource(h);
        return;
    }
    case 0xA9AB: {  // AddResource(h, type, id, name)
        uint32_t namep = arg_l(cpu);
        int16_t id = int16_t(arg_w(cpu));
        uint32_t type = arg_l(cpu);
        uint32_t h = arg_l(cpu);
        rm_add_resource(h, type, id, namep ? read_pstr(namep) : "");
        return;
    }
    case 0xA9AD: {  // RemoveResource(h)
        uint32_t h = arg_l(cpu);
        rm_remove_resource(h);
        return;
    }
    // ---- launch / app ----
    case 0xA9F5: {  // GetAppParms(VAR name, VAR refnum, VAR param)
        uint32_t paramp = arg_l(cpu);
        uint32_t refp = arg_l(cpu);
        uint32_t namep = arg_l(cpu);
        if (namep) write_pstr(namep, "Prince of Persia 2");
        if (refp) mem_write16(refp, 2);
        if (paramp) mem_write32(paramp, 0);
        return;
    }
    case 0xA9F4:    // ExitToShell
        std::fprintf(stderr, "[trap] ExitToShell at pc=%06X — clean exit\n", cpu.pc);
        std::exit(0);
    case 0xA9F1: (void)arg_l(cpu); return;                  // UnloadSeg
    // ---- init stubs ----
    case 0xA86E: {  // InitGraf(globalPtr)
        uint32_t qd = arg_l(cpu);
        s_qd_theport = qd;                  // SetPort keeps guest thePort live
        mem_write32(cpu.a[5], qd);          // [A5] -> QD globals (thePort slot)
        mem_write32(qd, 0);                 // thePort = nil
        mem_write32(qd - 126, 1);           // randSeed
        // patterns: white @-8, black @-16, gray @-24
        for (int i = 0; i < 8; i++) {
            mem_write8(qd - 8 + i, 0x00);
            mem_write8(qd - 16 + i, 0xFF);
            mem_write8(qd - 24 + i, (i & 1) ? 0x55 : 0xAA);
            mem_write8(qd - 32 + i, (i & 1) ? 0x22 : 0x88);
            mem_write8(qd - 40 + i, (i & 1) ? 0xDD : 0x77);
        }
        for (int i = 0; i < 68; i++) mem_write8(qd - 108 + i, 0);  // arrow
        mem_write32(qd - 122, FB_BASE);     // screenBits.baseAddr
        mem_write16(qd - 118, SCREEN_W);    // rowBytes
        mem_write16(qd - 116, 0);           // bounds.top
        mem_write16(qd - 114, 0);           // bounds.left
        mem_write16(qd - 112, SCREEN_H);    // bounds.bottom
        mem_write16(qd - 110, SCREEN_W);    // bounds.right
        mem_write32(qd - 126, 1);           // randSeed
        std::fprintf(stderr, "[trap] InitGraf(qd=%06X) — MILESTONE\n", qd);
        return;
    }
    case 0xA8FE: std::fprintf(stderr, "[trap] InitFonts [stub]\n"); return;
    case 0xA912: std::fprintf(stderr, "[trap] InitWindows [stub]\n"); return;
    case 0xA930: std::fprintf(stderr, "[trap] InitMenus [stub]\n"); return;
    case 0xA9CC: std::fprintf(stderr, "[trap] TEInit [stub]\n"); return;
    case 0xA97B: (void)arg_l(cpu);
        std::fprintf(stderr, "[trap] InitDialogs [stub]\n"); return;
    case 0xA850: return;                                    // InitCursor
    case 0xA852: return;                                    // HideCursor
    case 0xA853: return;                                    // ShowCursor
    // ---- Dialog Manager (headless) ----
    case 0xA98B: {  // ParamText(p0..p3) — log for diagnostics
        std::string p[4];
        for (int i = 3; i >= 0; i--) {
            uint32_t sp = arg_l(cpu);
            if (sp) p[i] = read_pstr(sp);
        }
        std::fprintf(stderr, "[dlg] ParamText('%s','%s','%s','%s')\n",
                     p[0].c_str(), p[1].c_str(), p[2].c_str(), p[3].c_str());
        return;
    }
    case 0xA990: {  // GetIText(item: Handle, VAR text: Str255)
        uint32_t text = arg_l(cpu);
        uint32_t item = arg_l(cpu);
        uint32_t p = item ? mem_read32(item) : 0;
        int32_t n = item ? mm_get_handle_size(item) : 0;
        if (n < 0) n = 0;
        if (n > 255) n = 255;
        mem_write8(text, uint8_t(n));
        for (int32_t i = 0; i < n; i++)
            mem_write8(text + 1 + i, mem_read8(p + i));
        return;
    }
    case 0xA98F: {  // SetIText(item: Handle, text: Str255)
        uint32_t text = arg_l(cpu);
        uint32_t item = arg_l(cpu);
        if (!item) return;
        uint8_t n = text ? mem_read8(text) : 0;
        mm_set_handle_size(item, n);
        uint32_t p = mem_read32(item);
        for (int i = 0; i < n; i++)
            mem_write8(p + i, mem_read8(text + 1 + i));
        return;
    }
    case 0xA985:    // Alert(id, filter): Integer
    case 0xA986:    // StopAlert
    case 0xA987:    // NoteAlert
    case 0xA988: {  // CautionAlert
        (void)arg_l(cpu);
        int16_t id = int16_t(arg_w(cpu));
        // 406 = "pack data to save disk space?" — decline (writes are no-ops)
        int16_t item = (id == 406) ? 2 : 1;
        // POP2_ALERT_<id>=<item> overrides the auto-answer for experiments
        {
            char env[32];
            std::snprintf(env, sizeof env, "POP2_ALERT_%d", id);
            if (const char* v = std::getenv(env)) item = int16_t(std::atoi(v));
        }
        std::fprintf(stderr, "[dlg] Alert(%d) -> item %d (auto)\n", id, item);
        {                // guest backtrace: scan the stack for code addresses
            std::fprintf(stderr, "[dlg]   mohawk lastErr=%d (a5-3512)\n",
                         int16_t(mem_read16(A5_BASE - 3512)));
            std::fprintf(stderr, "[dlg]   stack RAs:");
            for (uint32_t sp = cpu.a[7]; sp < cpu.a[7] + 0x200; sp += 2) {
                uint32_t v = mem_read32(sp);
                if (v >= 0x200000 && v < 0x380000 && (v & 1) == 0)
                    std::fprintf(stderr, " %06X", v);
            }
            std::fprintf(stderr, "\n");
        }
        ret_w(cpu, uint16_t(item));
        return;
    }
    // ---- Dialog Manager (minimal: the sword/story interlude pages) ----
    case 0xA97C: {  // GetNewDialog(id:2, dStorage:4, behind:4): DialogPtr
        (void)arg_l(cpu);                       // behind
        uint32_t storage = arg_l(cpu);
        int16_t id = int16_t(arg_w(cpu));
        if (!storage) storage = mm_new_ptr(176, true);
        uint32_t d = make_window(id, storage, true);
        mem_write32(d + 156, mm_new_handle(4, true));   // items list
        mem_write16(d + 168, 1);                        // aDefItem
        std::fprintf(stderr, "[dlg] GetNewDialog(%d) -> %06X\n", id, d);
        ret_l(cpu, d);
        return;
    }
    case 0xA991: {  // ModalDialog(filterProc:4, VAR itemHit:2)
        uint32_t hitp = arg_l(cpu);
        (void)arg_l(cpu);                       // filterProc
        // pump real events so the page shows; any key/click (or ~2.5 s)
        // dismisses with the default item
        uint32_t t0 = ticks_now();
        MacEvent ev;
        while (ticks_now() - t0 < 150) {
            tm_fire_due(cpu); vbl_fire_due(cpu); snd_pump(cpu);
            if (video_next_event(0xFFFF, &ev, true) &&
                (ev.what == 1 || ev.what == 3)) break;
        }
        if (hitp) mem_write16(hitp, 1);
        std::fprintf(stderr, "[dlg] ModalDialog -> item 1\n");
        return;
    }
    case 0xA983: (void)arg_l(cpu); return;      // DisposDialog
    case 0xA98D: {  // GetDItem(dlg:4, item:2, VAR type:2, VAR hdl:4, VAR box:8)
        uint32_t boxp = arg_l(cpu);
        uint32_t hdlp = arg_l(cpu);
        uint32_t typep = arg_l(cpu);
        (void)arg_w(cpu);                       // itemNo
        (void)arg_l(cpu);                       // dialog
        if (typep) mem_write16(typep, 8);       // statText
        if (hdlp) mem_write32(hdlp, mm_new_handle(4, true));
        if (boxp) {
            mem_write16(boxp, 10); mem_write16(boxp + 2, 10);
            mem_write16(boxp + 4, 60); mem_write16(boxp + 6, 500);
        }
        return;
    }
    // ---- Menu Manager (headless) ----
    case 0xA9C0: {  // GetNewMBar(id:2): Handle — load MBAR, preload its MENUs
        int16_t id = int16_t(arg_w(cpu));
        uint32_t h = rm_get_resource(0x4D424152 /*MBAR*/, id, false);
        if (h) {
            uint32_t p = mem_read32(h);
            int n = int(mem_read16(p));
            for (int i = 0; i < n; i++) {
                int16_t mid = int16_t(mem_read16(p + 2 + i * 2));
                uint32_t mh = rm_get_resource(0x4D454E55 /*MENU*/, mid, false);
                if (mh) s_menus[mid] = mh;
            }
        }
        ret_l(cpu, h);
        return;
    }
    case 0xA9BF: {  // GetRMenu(id:2): MenuHandle
        int16_t id = int16_t(arg_w(cpu));
        uint32_t h = rm_get_resource(0x4D454E55, id, false);
        if (h) s_menus[id] = h;
        ret_l(cpu, h);
        return;
    }
    case 0xA949: {  // GetMHandle(id:2): MenuHandle
        int16_t id = int16_t(arg_w(cpu));
        auto it = s_menus.find(id);
        ret_l(cpu, it == s_menus.end() ? 0 : it->second);
        return;
    }
    case 0xA93C: (void)arg_l(cpu); return;                  // SetMenuBar
    case 0xA93B: ret_l(cpu, 0); return;                     // GetMenuBar
    case 0xA934: return;                                    // ClearMenuBar
    case 0xA935: { (void)arg_w(cpu); (void)arg_l(cpu); return; }  // InsertMenu
    case 0xA936: (void)arg_w(cpu); return;                  // DeleteMenu
    case 0xA937: return;                                    // DrawMenuBar
    case 0xA81D: return;                                    // InvalMenuBar
    case 0xA938: (void)arg_w(cpu); return;                  // HiliteMenu
    case 0xA939: case 0xA93A: { (void)arg_w(cpu); (void)arg_l(cpu); return; }  // Enable/DisableItem
    case 0xA945: { (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu); return; }  // CheckItem
    case 0xA944: { (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu); return; }  // SetItmMark
    case 0xA948: (void)arg_l(cpu); return;                  // CalcMenuSize
    case 0xA94D: { (void)arg_l(cpu); (void)arg_l(cpu); return; }  // AddResMenu
    case 0xA950: (void)arg_l(cpu); ret_w(cpu, 0); return;   // CountMItems
    case 0xA93D: (void)arg_l(cpu); ret_l(cpu, 0); return;   // MenuSelect(pt): no choice
    case 0xA93E: {  // MenuKey(ch): LongInt — match key equivalents
        uint8_t ch = uint8_t(arg_w(cpu) & 0xFF);
        if (ch >= 'a' && ch <= 'z') ch = uint8_t(ch - 32);
        for (const auto& [mid, mh] : s_menus) {
            uint32_t p = mem_read32(mh);
            uint32_t q = p + 14;                 // title (Pascal string)
            q += 1 + mem_read8(q);
            for (int item = 1; mem_read8(q); item++) {
                uint8_t il = mem_read8(q);
                uint8_t key = mem_read8(q + 1 + il + 1);
                if (key == ch) {
                    std::fprintf(stderr, "[menu] MenuKey('%c') -> %d:%d\n",
                                 ch, mid, item);
                    ret_l(cpu, (uint32_t(uint16_t(mid)) << 16) | uint32_t(item));
                    return;
                }
                q += uint32_t(5 + il);
            }
        }
        ret_l(cpu, 0);
        return;
    }
    case 0xA94C: (void)arg_w(cpu); return;                  // FlashMenuBar
    // ---- QuickDraw ports / regions ----
    case 0xA873: set_cur_port(arg_l(cpu)); return;          // SetPort
    case 0xA874: {  // GetPort(VAR port)
        uint32_t pp = arg_l(cpu);
        mem_write32(pp, s_cur_port);
        return;
    }
    // text state -> GrafPort fields
    case 0xA887: {  // TextFont
        uint16_t v = arg_w(cpu);
        if (s_cur_port) mem_write16(s_cur_port + 68, v);
        return;
    }
    case 0xA888: {  // TextFace
        uint16_t v = arg_w(cpu);
        if (s_cur_port) mem_write16(s_cur_port + 70, v);
        return;
    }
    case 0xA889: {  // TextMode
        uint16_t v = arg_w(cpu);
        if (s_cur_port) mem_write16(s_cur_port + 72, v);
        return;
    }
    case 0xA88A: {  // TextSize
        uint16_t v = arg_w(cpu);
        if (s_cur_port) mem_write16(s_cur_port + 74, v);
        return;
    }
    case 0xA88B: {  // GetFontInfo(VAR fi) — matches the built-in 8x16 font
        uint32_t fip = arg_l(cpu);
        mem_write16(fip, uint16_t(qd_ascent()));
        mem_write16(fip + 2, uint16_t(qd_descent()));
        mem_write16(fip + 4, uint16_t(qd_char_width()));  // widMax
        mem_write16(fip + 6, 0);                          // leading
        return;
    }
    // pen / drawing state (headless no-ops where harmless)
    case 0xA893: {  // MoveTo(h,v)
        uint16_t v = arg_w(cpu), h = arg_w(cpu);
        if (s_cur_port) {
            mem_write16(s_cur_port + 48, v);   // pnLoc.v
            mem_write16(s_cur_port + 50, h);   // pnLoc.h
        }
        return;
    }
    case 0xA894: {  // Move(dh,dv) — relative pen move
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        if (s_cur_port) {
            mem_write16(s_cur_port + 48,
                        uint16_t(int16_t(mem_read16(s_cur_port + 48)) + dv));
            mem_write16(s_cur_port + 50,
                        uint16_t(int16_t(mem_read16(s_cur_port + 50)) + dh));
        }
        return;
    }
    case 0xA891: {  // LineTo(h,v)
        int16_t v = int16_t(arg_w(cpu)), h = int16_t(arg_w(cpu));
        if (s_cur_port) qd_line_to(s_cur_port, h, v);
        return;
    }
    case 0xA892: {  // Line(dh,dv)
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        if (s_cur_port)
            qd_line_to(s_cur_port,
                       int16_t(int16_t(mem_read16(s_cur_port + 50)) + dh),
                       int16_t(int16_t(mem_read16(s_cur_port + 48)) + dv));
        return;
    }
    case 0xA898: {  // GetPenState(VAR ps: {pnLoc, pnSize, pnMode, pnPat})
        uint32_t ps = arg_l(cpu);
        if (s_cur_port)
            for (int i = 0; i < 18; i += 2)     // copy port+48..65
                mem_write16(ps + i, mem_read16(s_cur_port + 48 + i));
        return;
    }
    case 0xA899: {  // SetPenState(ps*)
        uint32_t ps = arg_l(cpu);
        if (s_cur_port)
            for (int i = 0; i < 18; i += 2)
                mem_write16(s_cur_port + 48 + i, mem_read16(ps + i));
        return;
    }
    case 0xA89B: (void)arg_l(cpu); return;                  // PenSize(Point)
    case 0xA89C: (void)arg_w(cpu); return;                  // PenMode
    case 0xA89D: (void)arg_l(cpu); return;                  // PenPat(pat*)
    case 0xA89E: return;                                    // PenNormal
    case 0xA896: return;                                    // HidePen
    case 0xA897: return;                                    // ShowPen
    case 0xA87C: (void)arg_l(cpu); return;                  // BackPat
    case 0xA862: case 0xA863: {  // ForeColor / BackColor (classic constants)
        uint32_t code = arg_l(cpu);
        uint8_t r = 0, g = 0, b = 0;
        switch (code) {
        case 30: r = g = b = 255; break;            // white
        case 33: break;                             // black
        case 69: r = g = 255; break;                // yellow
        case 137: r = b = 255; break;               // magenta
        case 205: r = 255; break;                   // red
        case 273: g = b = 255; break;               // cyan
        case 341: g = 255; break;                   // green
        case 409: b = 255; break;                   // blue
        }
        if (s_cur_port)
            mem_write32(s_cur_port + (op == 0xA862 ? 80 : 84),
                        video_match_color(r, g, b));
        return;
    }
    case 0xAA14: case 0xAA15: {  // RGBForeColor / RGBBackColor (RGBColor*)
        uint32_t cp = arg_l(cpu);
        uint8_t pix = video_match_color(uint8_t(mem_read16(cp) >> 8),
                                        uint8_t(mem_read16(cp + 2) >> 8),
                                        uint8_t(mem_read16(cp + 4) >> 8));
        if (std::getenv("POP2_TRACE_TEXT"))
            std::fprintf(stderr, "[color] %s(%04X,%04X,%04X) -> %u port=%06X\n",
                         op == 0xAA14 ? "fg" : "bk", mem_read16(cp),
                         mem_read16(cp + 2), mem_read16(cp + 4), pix,
                         s_cur_port);
        if (s_cur_port)
            mem_write32(s_cur_port + (op == 0xAA14 ? 80 : 84), pix);
        return;
    }
    case 0xA878: { (void)arg_w(cpu); (void)arg_w(cpu); return; }  // SetOrigin
    case 0xA8A1: case 0xA8A2: case 0xA8A3: case 0xA8A4:     // Frame/Paint/Erase/InvertRect
        qd_rect_op(s_cur_port, arg_l(cpu), op - 0xA8A1); return;
    case 0xA8A5: {  // FillRect(r, pat)
        uint32_t pat = arg_l(cpu);
        qd_rect_op(s_cur_port, arg_l(cpu), 4, pat);
        return;
    }
    case 0xA87B: {  // ClipRect(r*)
        uint32_t rp = arg_l(cpu);
        if (s_cur_port) {
            uint32_t rgn = mem_read32(s_cur_port + 28);
            if (rgn) {
                uint32_t p = mem_read32(rgn);
                for (int i = 0; i < 8; i += 2)
                    mem_write16(p + 2 + i, mem_read16(rp + i));
            }
        }
        return;
    }
    case 0xA879: (void)arg_l(cpu); return;                  // SetClip
    case 0xA87A: {  // GetClip(rgn)
        uint32_t dst = arg_l(cpu);
        if (s_cur_port && dst) {
            uint32_t src = mem_read32(s_cur_port + 28);
            if (src) {
                uint32_t sp = mem_read32(src), dp = mem_read32(dst);
                for (int i = 0; i < 10; i += 2)
                    mem_write16(dp + i, mem_read16(sp + i));
            }
        }
        return;
    }
    // rect / point utilities
    case 0xA8A7: {  // SetRect(r*, l, t, r, b)
        int16_t b = int16_t(arg_w(cpu)), r = int16_t(arg_w(cpu));
        int16_t t = int16_t(arg_w(cpu)), l = int16_t(arg_w(cpu));
        uint32_t rp = arg_l(cpu);
        mem_write16(rp, uint16_t(t)); mem_write16(rp + 2, uint16_t(l));
        mem_write16(rp + 4, uint16_t(b)); mem_write16(rp + 6, uint16_t(r));
        return;
    }
    case 0xA8A8: {  // OffsetRect(r*, dh, dv)
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        uint32_t rp = arg_l(cpu);
        mem_write16(rp, uint16_t(int16_t(mem_read16(rp)) + dv));
        mem_write16(rp + 2, uint16_t(int16_t(mem_read16(rp + 2)) + dh));
        mem_write16(rp + 4, uint16_t(int16_t(mem_read16(rp + 4)) + dv));
        mem_write16(rp + 6, uint16_t(int16_t(mem_read16(rp + 6)) + dh));
        return;
    }
    case 0xA8A9: {  // InsetRect(r*, dh, dv)
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        uint32_t rp = arg_l(cpu);
        mem_write16(rp, uint16_t(int16_t(mem_read16(rp)) + dv));
        mem_write16(rp + 2, uint16_t(int16_t(mem_read16(rp + 2)) + dh));
        mem_write16(rp + 4, uint16_t(int16_t(mem_read16(rp + 4)) - dv));
        mem_write16(rp + 6, uint16_t(int16_t(mem_read16(rp + 6)) - dh));
        return;
    }
    case 0xA8AA: {  // SectRect(a*, b*, dst*): Boolean
        uint32_t dp = arg_l(cpu), bp = arg_l(cpu), ap = arg_l(cpu);
        int16_t t = std::max(int16_t(mem_read16(ap)), int16_t(mem_read16(bp)));
        int16_t l = std::max(int16_t(mem_read16(ap + 2)), int16_t(mem_read16(bp + 2)));
        int16_t b = std::min(int16_t(mem_read16(ap + 4)), int16_t(mem_read16(bp + 4)));
        int16_t r = std::min(int16_t(mem_read16(ap + 6)), int16_t(mem_read16(bp + 6)));
        bool ok = t < b && l < r;
        if (!ok) t = l = b = r = 0;
        mem_write16(dp, uint16_t(t)); mem_write16(dp + 2, uint16_t(l));
        mem_write16(dp + 4, uint16_t(b)); mem_write16(dp + 6, uint16_t(r));
        ret_w(cpu, ok ? 0x0100 : 0);
        return;
    }
    case 0xA8AB: {  // UnionRect(a*, b*, dst*)
        uint32_t dp = arg_l(cpu), bp = arg_l(cpu), ap = arg_l(cpu);
        int16_t t = std::min(int16_t(mem_read16(ap)), int16_t(mem_read16(bp)));
        int16_t l = std::min(int16_t(mem_read16(ap + 2)), int16_t(mem_read16(bp + 2)));
        int16_t b = std::max(int16_t(mem_read16(ap + 4)), int16_t(mem_read16(bp + 4)));
        int16_t r = std::max(int16_t(mem_read16(ap + 6)), int16_t(mem_read16(bp + 6)));
        mem_write16(dp, uint16_t(t)); mem_write16(dp + 2, uint16_t(l));
        mem_write16(dp + 4, uint16_t(b)); mem_write16(dp + 6, uint16_t(r));
        return;
    }
    case 0xA8AD: {  // PtInRect(pt, r*): Boolean
        uint32_t rp = arg_l(cpu);
        uint32_t pt = arg_l(cpu);
        int16_t v = int16_t(pt >> 16), h = int16_t(pt);
        bool in = v >= int16_t(mem_read16(rp)) && v < int16_t(mem_read16(rp + 4)) &&
                  h >= int16_t(mem_read16(rp + 2)) && h < int16_t(mem_read16(rp + 6));
        ret_w(cpu, in ? 0x0100 : 0);
        return;
    }
    case 0xA8A6: {  // EqualRect(a*, b*): Boolean
        uint32_t bp = arg_l(cpu), ap = arg_l(cpu);
        bool eq = true;
        for (int i = 0; i < 8; i += 2)
            if (mem_read16(ap + i) != mem_read16(bp + i)) { eq = false; break; }
        ret_w(cpu, eq ? 0x0100 : 0);
        return;
    }
    case 0xA8AE: {  // EmptyRect(r*): Boolean
        uint32_t rp = arg_l(cpu);
        bool empty = int16_t(mem_read16(rp)) >= int16_t(mem_read16(rp + 4)) ||
                     int16_t(mem_read16(rp + 2)) >= int16_t(mem_read16(rp + 6));
        ret_w(cpu, empty ? 0x0100 : 0);
        return;
    }
    case 0xA87E: {  // AddPt(src, VAR dst)
        uint32_t dp = arg_l(cpu);
        uint32_t src = arg_l(cpu);
        mem_write16(dp, uint16_t(int16_t(mem_read16(dp)) + int16_t(src >> 16)));
        mem_write16(dp + 2, uint16_t(int16_t(mem_read16(dp + 2)) + int16_t(src)));
        return;
    }
    case 0xA87F: {  // SubPt(src, VAR dst)
        uint32_t dp = arg_l(cpu);
        uint32_t src = arg_l(cpu);
        mem_write16(dp, uint16_t(int16_t(mem_read16(dp)) - int16_t(src >> 16)));
        mem_write16(dp + 2, uint16_t(int16_t(mem_read16(dp + 2)) - int16_t(src)));
        return;
    }
    case 0xA880: {  // SetPt(VAR pt, h, v)
        int16_t v = int16_t(arg_w(cpu)), h = int16_t(arg_w(cpu));
        uint32_t pp = arg_l(cpu);
        mem_write16(pp, uint16_t(v)); mem_write16(pp + 2, uint16_t(h));
        return;
    }
    case 0xA881: {  // EqualPt(a, b): Boolean
        uint32_t b = arg_l(cpu), a = arg_l(cpu);
        ret_w(cpu, a == b ? 0x0100 : 0);
        return;
    }
    case 0xA870: (void)arg_l(cpu); return;                  // LocalToGlobal (origin 0,0)
    case 0xA871: (void)arg_l(cpu); return;                  // GlobalToLocal
    case 0xA856: return;                                    // ObscureCursor
    case 0xA851: (void)arg_l(cpu); return;                  // SetCursor
    case 0xA9B9: {  // GetCursor(id): CursHandle
        int16_t id = int16_t(arg_w(cpu));
        ret_l(cpu, rm_get_resource(0x43555253 /*CURS*/, id, false));
        return;
    }
    case 0xA861: {  // Random(): Integer — LCG over QD randSeed at [[A5]]-126
        uint32_t seedp = mem_read32(cpu.a[5]) - 126;
        int64_t seed = int32_t(mem_read32(seedp));
        seed = (seed * 16807) % 0x7FFFFFFF;
        if (seed <= 0) seed += 0x7FFFFFFF;
        mem_write32(seedp, uint32_t(seed));
        uint16_t r = uint16_t(seed);
        if (r == 0x8000) r = 0;   // documented quirk: -32768 never returned
        ret_w(cpu, r);
        return;
    }
    // ---- bit/logical utilities (OS Utilities) ----
    case 0xA858: {  // BitAnd(v1:l, v2:l): LongInt
        uint32_t b = arg_l(cpu), a = arg_l(cpu);
        ret_l(cpu, a & b);
        return;
    }
    case 0xA859: {  // BitXor
        uint32_t b = arg_l(cpu), a = arg_l(cpu);
        ret_l(cpu, a ^ b);
        return;
    }
    case 0xA85A: ret_l(cpu, ~arg_l(cpu)); return;           // BitNot
    case 0xA85B: {  // BitOr
        uint32_t b = arg_l(cpu), a = arg_l(cpu);
        ret_l(cpu, a | b);
        return;
    }
    case 0xA85C: {  // BitShift(v:l, count:w): LongInt; +left, -right logical
        int16_t cnt = int16_t(arg_w(cpu));
        uint32_t v = arg_l(cpu);
        cnt = int16_t(cnt % 32);
        ret_l(cpu, cnt >= 0 ? v << cnt : v >> -cnt);
        return;
    }
    case 0xA85D: {  // BitTst(bytePtr:l, bitNum:l): Boolean — MSB-first
        uint32_t bit = arg_l(cpu);
        uint32_t p = arg_l(cpu);
        uint8_t byte = mem_read8(p + bit / 8);
        ret_w(cpu, (byte >> (7 - bit % 8)) & 1 ? 0x0100 : 0);
        return;
    }
    case 0xA85E: {  // BitSet(bytePtr:l, bitNum:l)
        uint32_t bit = arg_l(cpu);
        uint32_t p = arg_l(cpu);
        mem_write8(p + bit / 8,
                   uint8_t(mem_read8(p + bit / 8) | (1 << (7 - bit % 8))));
        return;
    }
    case 0xA85F: {  // BitClr(bytePtr:l, bitNum:l)
        uint32_t bit = arg_l(cpu);
        uint32_t p = arg_l(cpu);
        mem_write8(p + bit / 8,
                   uint8_t(mem_read8(p + bit / 8) & ~(1 << (7 - bit % 8))));
        return;
    }
    // text drawing
    case 0xA884: {  // DrawString(s)
        uint32_t sp = arg_l(cpu);
        if (sp) qd_draw_text(s_cur_port, sp + 1, mem_read8(sp));
        return;
    }
    case 0xA883: {  // DrawChar(ch:w)
        uint16_t ch = arg_w(cpu);
        uint32_t tmp = cpu.a[7] - 2;     // scratch below SP for the byte
        mem_write8(tmp, uint8_t(ch));
        qd_draw_text(s_cur_port, tmp, 1);
        return;
    }
    case 0xA885: {  // DrawText(buf, firstByte:w, count:w)
        uint16_t count = arg_w(cpu);
        uint16_t first = arg_w(cpu);
        uint32_t buf = arg_l(cpu);
        if (buf) qd_draw_text(s_cur_port, buf + first, count);
        return;
    }
    case 0xA88C: {  // StringWidth(s): Integer
        uint32_t sp = arg_l(cpu);
        uint16_t len = sp ? mem_read8(sp) : 0;
        ret_w(cpu, uint16_t(len * qd_char_width()));
        return;
    }
    case 0xA88D:    // CharWidth
        (void)arg_w(cpu); ret_w(cpu, uint16_t(qd_char_width())); return;
    case 0xA886: {  // TextWidth(buf, first, count): Integer
        uint16_t count = arg_w(cpu);
        (void)arg_w(cpu); (void)arg_l(cpu);
        ret_w(cpu, uint16_t(count * qd_char_width()));
        return;
    }
    case 0xA8D8: ret_l(cpu, new_region()); return;          // NewRgn
    case 0xA8D9: mm_dispose_handle(arg_l(cpu)); return;     // DisposRgn
    case 0xA8DF: {  // RectRgn(rgn, rect)
        uint32_t rp = arg_l(cpu);
        uint32_t rgn = arg_l(cpu);
        uint32_t p = mem_read32(rgn);
        mem_write16(p, 10);
        for (int i = 0; i < 8; i += 2)
            mem_write16(p + 2 + i, mem_read16(rp + i));
        return;
    }
    case 0xA8DE: {  // SetRecRgn(rgn, t..r) — SetRectRgn(rgn, l, t, r, b)
        int16_t b = int16_t(arg_w(cpu)), r = int16_t(arg_w(cpu));
        int16_t t = int16_t(arg_w(cpu)), l = int16_t(arg_w(cpu));
        uint32_t rgn = arg_l(cpu);
        uint32_t p = mem_read32(rgn);
        mem_write16(p, 10);
        mem_write16(p + 2, uint16_t(t)); mem_write16(p + 4, uint16_t(l));
        mem_write16(p + 6, uint16_t(b)); mem_write16(p + 8, uint16_t(r));
        return;
    }
    case 0xA8EC: {  // CopyBits(srcBits*, dstBits*, srcRect*, dstRect*, mode:w, maskRgn)
        (void)arg_l(cpu);                      // maskRgn (ignored)
        (void)arg_w(cpu);                      // mode (srcCopy assumed)
        uint32_t drp = arg_l(cpu);
        uint32_t srp = arg_l(cpu);
        uint32_t dbp = arg_l(cpu);
        uint32_t sbp = arg_l(cpu);
        // resolve a BitMap* / CGrafPort-portBits* to {base,rowBytes,bounds}
        struct Bits { uint32_t base; uint32_t row; int16_t t, l; };
        auto resolve = [](uint32_t bp) -> Bits {
            uint16_t rb = mem_read16(bp + 4);
            if ((rb & 0xC000) == 0xC000) {     // CGrafPort: PixMap handle at bp
                uint32_t pm = mem_read32(mem_read32(bp));
                return {mem_read32(pm), uint32_t(mem_read16(pm + 4) & 0x3FFF),
                        int16_t(mem_read16(pm + 6)), int16_t(mem_read16(pm + 8))};
            }
            if (rb & 0x8000) {                 // direct PixMap*
                return {mem_read32(bp), uint32_t(rb & 0x3FFF),
                        int16_t(mem_read16(bp + 6)), int16_t(mem_read16(bp + 8))};
            }
            return {mem_read32(bp), rb,        // plain BitMap (8bpp in our world)
                    int16_t(mem_read16(bp + 6)), int16_t(mem_read16(bp + 8))};
        };
        Bits src = resolve(sbp), dst = resolve(dbp);
        int16_t st = int16_t(mem_read16(srp)), sl = int16_t(mem_read16(srp + 2));
        int16_t sb = int16_t(mem_read16(srp + 4)), sr = int16_t(mem_read16(srp + 6));
        int16_t dt = int16_t(mem_read16(drp)), dl = int16_t(mem_read16(drp + 2));
        int16_t db = int16_t(mem_read16(drp + 4)), dr = int16_t(mem_read16(drp + 6));
        int sw = sr - sl, sh = sb - st, dw = dr - dl, dh = db - dt;
        if (std::getenv("POP2_TRACE_BLT")) {
            static int s_n = 0;
            if (s_n < 400 && ++s_n)
                std::fprintf(stderr, "[cb] %06X(%08X row%u)->%06X(%08X row%u) "
                             "%dx%d@%d,%d -> %d,%d t=%u\n", sbp, src.base,
                             src.row, dbp, dst.base, dst.row, sw, sh, sl, st,
                             dl, dt, ticks_now());
        }
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !src.base || !dst.base)
            return;
        for (int y = 0; y < dh; y++) {
            int sy = (sh == dh) ? y : y * sh / dh;
            uint32_t srow = src.base + uint32_t(st - src.t + sy) * src.row;
            uint32_t drow = dst.base + uint32_t(dt - dst.t + y) * dst.row;
            uint8_t* s = g_mem + ((srow + uint32_t(sl - src.l)) & ADDR_MASK);
            uint8_t* d = g_mem + ((drow + uint32_t(dl - dst.l)) & ADDR_MASK);
            if (sw == dw) {
                std::memmove(d, s, size_t(dw));
            } else {
                for (int x = 0; x < dw; x++) d[x] = s[x * sw / dw];
            }
        }
        return;
    }
    // rectangular-region combinators: treat regions as their bounding boxes
    case 0xA8E5:    // UnionRgn(a, b, dst)
    case 0xA8E4:    // SectRgn
    case 0xA8E6:    // DiffRgn
    case 0xA8E7: {  // XorRgn
        uint32_t dst = arg_l(cpu);
        uint32_t b = arg_l(cpu);
        uint32_t a = arg_l(cpu);
        uint32_t ap = mem_read32(a), bp = mem_read32(b), dp = mem_read32(dst);
        int16_t at = int16_t(mem_read16(ap + 2)), al = int16_t(mem_read16(ap + 4));
        int16_t ab = int16_t(mem_read16(ap + 6)), ar = int16_t(mem_read16(ap + 8));
        int16_t bt = int16_t(mem_read16(bp + 2)), bl = int16_t(mem_read16(bp + 4));
        int16_t bb = int16_t(mem_read16(bp + 6)), br = int16_t(mem_read16(bp + 8));
        bool a_empty = at >= ab || al >= ar, b_empty = bt >= bb || bl >= br;
        int16_t t, l, btm, r;
        if ((op & 0xFF) == 0xE4) {           // intersection
            t = std::max(at, bt); l = std::max(al, bl);
            btm = std::min(ab, bb); r = std::min(ar, br);
            if (t >= btm || l >= r) t = l = btm = r = 0;
        } else if ((op & 0xFF) == 0xE6) {    // difference: approximate with a
            t = at; l = al; btm = ab; r = ar;
            if (a_empty) t = l = btm = r = 0;
        } else {                             // union / xor: bbox union
            if (a_empty && b_empty) { t = l = btm = r = 0; }
            else if (a_empty) { t = bt; l = bl; btm = bb; r = br; }
            else if (b_empty) { t = at; l = al; btm = ab; r = ar; }
            else {
                t = std::min(at, bt); l = std::min(al, bl);
                btm = std::max(ab, bb); r = std::max(ar, br);
            }
        }
        mem_write16(dp, 10);
        mem_write16(dp + 2, uint16_t(t)); mem_write16(dp + 4, uint16_t(l));
        mem_write16(dp + 6, uint16_t(btm)); mem_write16(dp + 8, uint16_t(r));
        return;
    }
    case 0xA8DC: {  // CopyRgn(src, dst)
        uint32_t dst = arg_l(cpu);
        uint32_t src = arg_l(cpu);
        uint32_t sp = mem_read32(src);
        uint32_t size = mem_read16(sp);
        if (size < 10) size = 10;
        if (size > 10 && !mm_set_handle_size(dst, size)) size = 10;
        uint32_t dp = mem_read32(dst);
        for (uint32_t i = 0; i < size; i += 2)
            mem_write16(dp + i, mem_read16(sp + i));
        if (size == 10) mem_write16(dp, 10);
        return;
    }
    case 0xA8DD: {  // SetEmptyRgn(rgn)
        uint32_t p = mem_read32(arg_l(cpu));
        mem_write16(p, 10);
        for (int i = 2; i < 10; i += 2) mem_write16(p + i, 0);
        return;
    }
    case 0xA8E0: {  // OffsetRgn(rgn, dh, dv)
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        uint32_t p = mem_read32(arg_l(cpu));
        mem_write16(p + 2, uint16_t(int16_t(mem_read16(p + 2)) + dv));
        mem_write16(p + 4, uint16_t(int16_t(mem_read16(p + 4)) + dh));
        mem_write16(p + 6, uint16_t(int16_t(mem_read16(p + 6)) + dv));
        mem_write16(p + 8, uint16_t(int16_t(mem_read16(p + 8)) + dh));
        if (mem_read16(p) > 10) {            // shift span data too
            uint16_t n = mem_read16(p + 10);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t sp = p + 12 + i * 6;
                mem_write16(sp, uint16_t(int16_t(mem_read16(sp)) + dv));
                mem_write16(sp + 2, uint16_t(int16_t(mem_read16(sp + 2)) + dh));
                mem_write16(sp + 4, uint16_t(int16_t(mem_read16(sp + 4)) + dh));
            }
        }
        return;
    }
    case 0xA8D2: case 0xA8D3: case 0xA8D4: case 0xA8D5: {
        // Frame/Erase/InvertRgn — rect approximation; Paint honors span data
        uint32_t p = mem_read32(arg_l(cpu));
        if (op == 0xA8D3) qd_paint_rgn(s_cur_port, p);
        else qd_rect_op(s_cur_port, p + 2, op - 0xA8D2);
        return;
    }
    case 0xA8D7: {  // BitMapToRegion(rgn, bMap*): OSErr
        // Real region from a 1-bit bitmap: per-row spans of set bits. The
        // game's only call site is the shape-silhouette painter (f17_5996
        // mask path: steam over potions, gates) — PaintRgn fills the spans.
        // Region layout: rgnSize.w, bbox.rect, count.w, {v,h1,h2}* (ours).
        uint32_t bm = arg_l(cpu);
        uint32_t rgn = arg_l(cpu);
        uint32_t base = mem_read32(bm);
        uint32_t rowb = mem_read16(bm + 4) & 0x3FFF;
        int16_t bt = int16_t(mem_read16(bm + 6)), bl = int16_t(mem_read16(bm + 8));
        int16_t bb = int16_t(mem_read16(bm + 10)), br = int16_t(mem_read16(bm + 12));
        int w = br - bl, h = bb - bt;
        std::vector<int16_t> spans;          // {v, h1, h2} triplets
        int16_t mt = 0, ml = 0, mb = 0, mr = 0;
        if (base && rowb && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            for (int y = 0; y < h; y++) {
                uint32_t row = base + uint32_t(y) * rowb;
                for (int x = 0; x < w;) {
                    if (!(mem_read8(row + uint32_t(x >> 3)) &
                          (0x80u >> (x & 7)))) { x++; continue; }
                    int x0 = x;
                    while (x < w && (mem_read8(row + uint32_t(x >> 3)) &
                                     (0x80u >> (x & 7)))) x++;
                    if (spans.empty()) {
                        mt = int16_t(bt + y); ml = int16_t(bl + x0);
                        mb = int16_t(bt + y + 1); mr = int16_t(bl + x);
                    } else {
                        ml = std::min(ml, int16_t(bl + x0));
                        mb = int16_t(bt + y + 1);
                        mr = std::max(mr, int16_t(bl + x));
                    }
                    spans.push_back(int16_t(bt + y));
                    spans.push_back(int16_t(bl + x0));
                    spans.push_back(int16_t(bl + x));
                }
            }
        }
        uint32_t n = uint32_t(spans.size() / 3);
        uint32_t total = n ? 12 + n * 6 : 10;
        if (total > 32000 || !mm_set_handle_size(rgn, total)) {
            // fallback: bounding box only
            uint32_t p = mem_read32(rgn);
            mem_write16(p, 10);
            mem_write16(p + 2, uint16_t(n ? mt : 0));
            mem_write16(p + 4, uint16_t(n ? ml : 0));
            mem_write16(p + 6, uint16_t(n ? mb : 0));
            mem_write16(p + 8, uint16_t(n ? mr : 0));
            ret_w(cpu, 0);
            return;
        }
        uint32_t p = mem_read32(rgn);
        mem_write16(p, uint16_t(total));
        mem_write16(p + 2, uint16_t(mt)); mem_write16(p + 4, uint16_t(ml));
        mem_write16(p + 6, uint16_t(mb)); mem_write16(p + 8, uint16_t(mr));
        if (n) {
            mem_write16(p + 10, uint16_t(n));
            for (uint32_t i = 0; i < spans.size(); i++)
                mem_write16(p + 12 + i * 2, uint16_t(spans[i]));
        }
        ret_w(cpu, 0);
        return;
    }
    case 0xA8E1: {  // InsetRgn(rgn, dh, dv) — degrades span data to its bbox
        int16_t dv = int16_t(arg_w(cpu)), dh = int16_t(arg_w(cpu));
        uint32_t p = mem_read32(arg_l(cpu));
        mem_write16(p, 10);
        mem_write16(p + 2, uint16_t(int16_t(mem_read16(p + 2)) + dv));
        mem_write16(p + 4, uint16_t(int16_t(mem_read16(p + 4)) + dh));
        mem_write16(p + 6, uint16_t(int16_t(mem_read16(p + 6)) - dv));
        mem_write16(p + 8, uint16_t(int16_t(mem_read16(p + 8)) - dh));
        return;
    }
    case 0xA8E8: {  // PtInRgn(pt, rgn): Boolean
        uint32_t p = mem_read32(arg_l(cpu));
        uint32_t pt = arg_l(cpu);
        int16_t v = int16_t(pt >> 16), h = int16_t(pt & 0xFFFF);
        bool in = v >= int16_t(mem_read16(p + 2)) &&
                  v < int16_t(mem_read16(p + 6)) &&
                  h >= int16_t(mem_read16(p + 4)) &&
                  h < int16_t(mem_read16(p + 8));
        ret_w(cpu, in ? 0x0100 : 0);
        return;
    }
    case 0xA8E2: {  // EmptyRgn(rgn): Boolean
        uint32_t p = mem_read32(arg_l(cpu));
        bool empty = int16_t(mem_read16(p + 2)) >= int16_t(mem_read16(p + 6)) ||
                     int16_t(mem_read16(p + 4)) >= int16_t(mem_read16(p + 8));
        ret_w(cpu, empty ? 0x0100 : 0);
        return;
    }
    case 0xA8E3: {  // EqualRgn(a, b): Boolean — rect regions only
        uint32_t pb = mem_read32(arg_l(cpu));
        uint32_t pa = mem_read32(arg_l(cpu));
        bool eq = true;
        for (int i = 2; i < 10 && eq; i += 2)
            eq = mem_read16(pa + uint32_t(i)) == mem_read16(pb + uint32_t(i));
        ret_w(cpu, eq ? 0x0100 : 0);
        return;
    }
    // ---- TextEdit (minimal: records exist, no display) ----
    case 0xA9D2: {  // TENew(destRect*, viewRect*): TEHandle
        uint32_t viewp = arg_l(cpu);
        uint32_t destp = arg_l(cpu);
        uint32_t te = mm_new_handle(0x100, true);
        uint32_t tp = mem_read32(te);
        for (int i = 0; i < 8; i += 2) {
            mem_write16(tp + i, mem_read16(destp + i));      // destRect
            mem_write16(tp + 8 + i, mem_read16(viewp + i));  // viewRect
        }
        mem_write16(tp + 24, 12);                  // lineHeight
        mem_write16(tp + 26, 10);                  // fontAscent
        mem_write32(tp + 62, mm_new_handle(0, false));  // hText
        mem_write32(tp + 82, s_cur_port);          // inPort
        ret_l(cpu, te);
        return;
    }
    case 0xA9CD: mm_dispose_handle(arg_l(cpu)); return;     // TEDispose
    case 0xA9CF: {  // TESetText(text, len:l, hTE)
        uint32_t te = arg_l(cpu);
        uint32_t len = arg_l(cpu);
        uint32_t text = arg_l(cpu);
        uint32_t tp = mem_read32(te);
        uint32_t ht = mem_read32(tp + 62);
        mm_set_handle_size(ht, len);
        std::memcpy(g_mem + (mem_read32(ht) & ADDR_MASK),
                    g_mem + (text & ADDR_MASK), len);
        mem_write16(tp + 60, uint16_t(len));       // teLength
        return;
    }
    case 0xA9D0: (void)arg_l(cpu); return;                  // TECalText
    case 0xA9D1:    // TESetSelect(start:l, end:l, hTE)
        (void)arg_l(cpu); (void)arg_l(cpu); (void)arg_l(cpu);
        return;
    case 0xA9D3: (void)arg_l(cpu); (void)arg_l(cpu); return;  // TEUpdate
    case 0xA9D8: case 0xA9D9: case 0xA9DA:                  // TEActivate/Deactivate/Idle
        (void)arg_l(cpu);
        return;
    case 0xA9DC: {  // TEKey(key:w, hTE)
        uint32_t te = arg_l(cpu);
        uint16_t key = arg_w(cpu) & 0xFF;
        uint32_t tp = mem_read32(te);
        uint32_t ht = mem_read32(tp + 62);
        int32_t len = mm_get_handle_size(ht);
        if (key == 8) {                            // backspace
            if (len > 0) mm_set_handle_size(ht, uint32_t(len - 1));
        } else {
            mm_set_handle_size(ht, uint32_t(len + 1));
            mem_write8(mem_read32(ht) + uint32_t(len), uint8_t(key));
        }
        mem_write16(tp + 60, uint16_t(mm_get_handle_size(ht)));
        return;
    }
    case 0xA9D4:    // TEClick(pt:l, extend:w, hTE)
        (void)arg_l(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
        return;
    case 0xA9D5: case 0xA9D6: case 0xA9DB:                  // TECopy/Cut/Paste
        (void)arg_l(cpu);
        return;
    case 0xA9D7: {  // TEDelete(hTE) — clears the selection; we clear all
        uint32_t te = arg_l(cpu);
        uint32_t tp = mem_read32(te);
        mm_set_handle_size(mem_read32(tp + 62), 0);
        mem_write16(tp + 60, 0);
        return;
    }
    case 0xA9DD:    // TEScroll(dh:w, dv:w, hTE)
        (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
        return;
    case 0xA9DE: {  // TEInsert(text, len:l, hTE) — append (no selection model)
        uint32_t te = arg_l(cpu);
        uint32_t len = arg_l(cpu);
        uint32_t text = arg_l(cpu);
        uint32_t tp = mem_read32(te);
        uint32_t ht = mem_read32(tp + 62);
        int32_t old = mm_get_handle_size(ht);
        if (old < 0) old = 0;
        mm_set_handle_size(ht, uint32_t(old) + len);
        uint32_t p = mem_read32(ht);
        for (uint32_t i = 0; i < len; i++)
            mem_write8(p + uint32_t(old) + i, mem_read8(text + i));
        mem_write16(tp + 60, uint16_t(mm_get_handle_size(ht)));
        return;
    }
    case 0xA9CB: {  // TEGetText(hTE): CharsHandle
        uint32_t te = arg_l(cpu);
        ret_l(cpu, mem_read32(mem_read32(te) + 62));
        return;
    }
    case 0xA9CE: {  // TextBox(text, len:l, box*, just:w)
        int16_t just = int16_t(arg_w(cpu));
        uint32_t box = arg_l(cpu);
        uint32_t len = arg_l(cpu);
        uint32_t text = arg_l(cpu);
        qd_text_box(s_cur_port, text, len, box, just);
        return;
    }
    // ---- fixed-point math ----
    case 0xA868: {  // FixMul(a, b): Fixed
        int32_t b = int32_t(arg_l(cpu)), a = int32_t(arg_l(cpu));
        ret_l(cpu, uint32_t((int64_t(a) * b) >> 16));
        return;
    }
    case 0xA84D: {  // FixDiv(a, b): Fixed
        int32_t b = int32_t(arg_l(cpu)), a = int32_t(arg_l(cpu));
        int64_t q = b ? ((int64_t(a) << 16) / b)
                      : (a < 0 ? int64_t(INT32_MIN) : INT32_MAX);
        ret_l(cpu, uint32_t(q));
        return;
    }
    case 0xA869: {  // FixRatio(numer:w, denom:w): Fixed
        int16_t den = int16_t(arg_w(cpu)), num = int16_t(arg_w(cpu));
        int64_t q = den ? ((int64_t(num) << 16) / den)
                        : (num < 0 ? int64_t(INT32_MIN) : INT32_MAX);
        ret_l(cpu, uint32_t(q));
        return;
    }
    case 0xA86C: {  // FixRound(f): Integer
        int32_t f = int32_t(arg_l(cpu));
        ret_w(cpu, uint16_t(int16_t((f + 0x8000) >> 16)));
        return;
    }
    case 0xA84A: {  // FracMul(a, b): Fract (2.30)
        int32_t b = int32_t(arg_l(cpu)), a = int32_t(arg_l(cpu));
        ret_l(cpu, uint32_t((int64_t(a) * b) >> 30));
        return;
    }
    case 0xA84B: {  // FracDiv
        int32_t b = int32_t(arg_l(cpu)), a = int32_t(arg_l(cpu));
        int64_t q = b ? ((int64_t(a) << 30) / b)
                      : (a < 0 ? int64_t(INT32_MIN) : INT32_MAX);
        ret_l(cpu, uint32_t(q));
        return;
    }
    // ---- handle utilities (register-based despite Toolbox numbers) ----
    case 0xA9E1: {  // HandToHand: A0 = src handle -> A0 = fresh copy
        uint32_t src = cpu.a[0];
        int32_t size = mm_get_handle_size(src);
        if (size < 0) { cpu.d[0] = uint32_t(int32_t(nilHandleErr)); return; }
        uint32_t dst = mm_new_handle(uint32_t(size), false);
        if (!dst) { cpu.d[0] = uint32_t(int32_t(memFullErr)); return; }
        std::memcpy(g_mem + (mem_read32(dst) & ADDR_MASK),
                    g_mem + (mem_read32(src) & ADDR_MASK), size_t(size));
        cpu.a[0] = dst;
        cpu.d[0] = 0;
        return;
    }
    case 0xA9E3: {  // PtrToHand: A0 = ptr, D0 = len -> A0 = new handle
        uint32_t ptr = cpu.a[0];
        uint32_t len = cpu.d[0];
        uint32_t dst = mm_new_handle(len, false);
        if (!dst) { cpu.d[0] = uint32_t(int32_t(memFullErr)); return; }
        std::memcpy(g_mem + (mem_read32(dst) & ADDR_MASK),
                    g_mem + (ptr & ADDR_MASK), len);
        cpu.a[0] = dst;
        cpu.d[0] = 0;
        return;
    }
    // ---- Window Manager (headless) ----
    case 0xA909:    // CalcVis(window)
    case 0xA90B:    // ClipAbove(window)
    case 0xA90E:    // SaveOld(window)
        (void)arg_l(cpu);
        return;
    case 0xA90A:    // CalcVisBehind(window, clobbered)
    case 0xA90C:    // PaintOne(window, clobbered)
    case 0xA90D:    // PaintBehind(window, clobbered)
    case 0xA90F:    // DrawNew(window, update)
        (void)arg_l(cpu);
        (void)arg_l(cpu);
        return;
    case 0xA9BD: {  // GetNewWindow(id:2, storage:4, behind:4): WindowPtr
        (void)arg_l(cpu);
        uint32_t storage = arg_l(cpu);
        int16_t id = int16_t(arg_w(cpu));
        uint32_t w = make_window(id, storage);
        std::fprintf(stderr, "[trap] GetNewWindow(%d) -> %06X\n", id, w);
        ret_l(cpu, w);
        return;
    }
    // ---- Color QuickDraw devices ----
    case 0xAA2A: ret_l(cpu, main_gdevice()); return;        // GetMainDevice
    case 0xAA29: ret_l(cpu, main_gdevice()); return;        // GetDeviceList
    case 0xAA32: ret_l(cpu, main_gdevice()); return;        // GetGDevice
    case 0xAA2B: (void)arg_l(cpu); ret_l(cpu, 0); return;   // GetNextDevice
    case 0xAA31: (void)arg_l(cpu); return;                  // SetGDevice
    case 0xAA27: (void)arg_l(cpu); ret_l(cpu, main_gdevice()); return;  // GetMaxDevice
    case 0xAA28: ret_l(cpu, 100); return;                   // GetCTSeed
    case 0xAA2C: {  // TestDeviceAttribute(gd:4, attr:2): Boolean
        int16_t attr = int16_t(arg_w(cpu));
        uint32_t gd = arg_l(cpu);
        uint16_t flags = gd ? mem_read16(mem_read32(gd) + 20) : 0;
        uint8_t res = (attr >= 0 && attr < 16 && (flags & (1u << attr))) ? 1 : 0;
        // result is a Boolean: byte... returned as word on stack
        ret_w(cpu, uint16_t(res ? 0x0100 : 0));
        return;
    }
    case 0xA915: (void)arg_l(cpu); return;                  // ShowWindow
    case 0xA916: (void)arg_l(cpu); return;                  // HideWindow
    case 0xA91B: {  // MoveWindow(w, h, v, front)
        (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
        return;
    }
    case 0xA91D: {  // SizeWindow(w, w16, h16, update)
        (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
        return;
    }
    case 0xA908: { (void)arg_w(cpu); (void)arg_l(cpu); return; }  // ShowHide
    case 0xA91C: { (void)arg_w(cpu); (void)arg_l(cpu); return; }  // HiliteWindow
    case 0xA922: (void)arg_l(cpu); return;                  // BeginUpdate
    case 0xA923: (void)arg_l(cpu); return;                  // EndUpdate
    case 0xA904: (void)arg_l(cpu); return;                  // DrawGrowIcon
    case 0xA91F: (void)arg_l(cpu); return;                  // SelectWindow
    case 0xA920: (void)arg_l(cpu); return;                  // BringToFront
    case 0xA924: ret_l(cpu, s_front_window); return;        // FrontWindow
    case 0xA92C: {  // FindWindow(pt, VAR window): part code
        uint32_t wp = arg_l(cpu);
        (void)arg_l(cpu);                                   // point
        if (wp) mem_write32(wp, s_front_window);
        ret_w(cpu, s_front_window ? 3 : 0);                 // inContent
        return;
    }
    case 0xA918: {  // SetWRefCon(w, refcon)
        uint32_t rc = arg_l(cpu);
        uint32_t w = arg_l(cpu);
        mem_write32(w + 152, rc);
        return;
    }
    case 0xA917: {  // GetWRefCon(w): LongInt
        uint32_t w = arg_l(cpu);
        ret_l(cpu, mem_read32(w + 152));
        return;
    }
    case 0xA91A: { (void)arg_l(cpu); (void)arg_l(cpu); return; }  // SetWTitle
    case 0xA92D: (void)arg_l(cpu); return;                  // CloseWindow
    case 0xA914: (void)arg_l(cpu); return;                  // DisposWindow
    // ---- QDOffscreen (GWorlds) ----
    case 0xAB1D: {
        uint16_t sel = uint16_t(cpu.d[0]);
        switch (sel) {
        case 0x0000: {  // NewGWorld(VAR gw, depth, bounds*, ctab, gdev, flags): QDErr
            uint32_t flags = arg_l(cpu);
            uint32_t gdev = arg_l(cpu);
            uint32_t ctab = arg_l(cpu);
            uint32_t boundsp = arg_l(cpu);
            int16_t depth = int16_t(arg_w(cpu));
            uint32_t gwp = arg_l(cpu);
            (void)flags; (void)gdev;
            int16_t t = int16_t(mem_read16(boundsp)), l = int16_t(mem_read16(boundsp + 2));
            int16_t b = int16_t(mem_read16(boundsp + 4)), r = int16_t(mem_read16(boundsp + 6));
            int w = r - l, h = b - t;
            if (depth == 0) depth = 8;
            // 32-Bit QD pads GWorld rows by 16 bytes past 4-byte alignment.
            // The shipped DSLV dissolve tables bake this stride (528 for the
            // 512-wide story-page buffer) — without the pad pages 4+ of the
            // intro dissolve into diagonal shear.
            uint32_t row = ((uint32_t(w) * uint32_t(depth) / 8 + 3) & ~3u) + 16;
            uint32_t pixels = mm_new_ptr(row * uint32_t(h), true);
            uint32_t pm = mm_new_handle(50, true);
            uint32_t pp = mem_read32(pm);
            mem_write32(pp, pixels);
            mem_write16(pp + 4, uint16_t(0x8000 | row));
            for (int i = 0; i < 8; i += 2)
                mem_write16(pp + 6 + i, mem_read16(boundsp + i));
            mem_write32(pp + 22, 0x00480000); mem_write32(pp + 26, 0x00480000);
            mem_write16(pp + 32, uint16_t(depth));
            mem_write16(pp + 34, 1); mem_write16(pp + 36, uint16_t(depth));
            if (!ctab) {  // share main device's color table
                uint32_t mgd = mem_read32(main_gdevice());
                ctab = mem_read32(mem_read32(mgd + 22) + 42);
            }
            mem_write32(pp + 42, ctab);
            uint32_t gw = mm_new_ptr(160, true);   // CGrafPort-like
            mem_write32(gw + 2, pm);               // portPixMap
            mem_write16(gw + 6, 0xC000);           // portVersion: CGrafPort mark
            for (int i = 0; i < 8; i += 2)
                mem_write16(gw + 16 + i, mem_read16(boundsp + i));
            mem_write32(gw + 24, new_region(t, l, b, r));      // visRgn
            // clipRgn must bound the pixels: the game's blitter clips with
            // clipRgn ONLY — wide-open here let full-screen shapes spray
            // beyond small GWorlds straight over the heap
            mem_write32(gw + 28, new_region(t, l, b, r));
            mem_write16(gw + 72, 1);  // txMode: srcOr (OpenCPort default)
            mem_write32(gw + 80, video_match_color(0, 0, 0));        // fgColor
            mem_write32(gw + 84, video_match_color(255, 255, 255));  // bkColor
            mem_write32(gwp, gw);
            qd_register_gworld(pixels, row, w, h);
            std::fprintf(stderr, "[gworld] NewGWorld %dx%d depth %d -> %06X\n",
                         w, h, depth, gw);
            ret_w(cpu, 0);  // noErr
            return;
        }
        case 0x0001: (void)arg_l(cpu); ret_w(cpu, 0x0100); return;  // LockPixels -> true
        case 0x0002: (void)arg_l(cpu); return;                       // UnlockPixels
        case 0x0003: {  // UpdateGWorld(VAR gw, depth, bounds, ctab, gdev, flags): GWorldFlags
            (void)arg_l(cpu); (void)arg_l(cpu); (void)arg_l(cpu);
            (void)arg_l(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
            ret_l(cpu, 0);
            return;
        }
        case 0x0004: {  // DisposeGWorld(gw) — free pixels, pixmap, regions
            uint32_t gw = arg_l(cpu);
            if (!gw) return;
            uint32_t pm = mem_read32(gw + 2);
            if (pm) {
                uint32_t pp = mem_read32(pm);
                uint32_t pixels = pp ? mem_read32(pp) : 0;
                if (pixels) {
                    qd_unregister_gworld(pixels);
                    mm_dispose_ptr(pixels);
                }
                mm_dispose_handle(pm);
            }
            mm_dispose_handle(mem_read32(gw + 24));
            mm_dispose_handle(mem_read32(gw + 28));
            if (s_cur_port == gw) set_cur_port(s_front_window);
            mm_dispose_ptr(gw);
            return;
        }
        case 0x0005: {  // GetGWorld(VAR port, VAR gd)
            uint32_t gdp = arg_l(cpu);
            uint32_t pp = arg_l(cpu);
            if (pp) mem_write32(pp, s_cur_port);
            if (gdp) mem_write32(gdp, main_gdevice());
            return;
        }
        case 0x0006: {  // SetGWorld(port, gd)
            (void)arg_l(cpu);
            set_cur_port(arg_l(cpu));
            return;
        }
        case 0x000F: {  // GetPixBaseAddr(pm): Ptr
            uint32_t pm = arg_l(cpu);
            ret_l(cpu, pm ? mem_read32(mem_read32(pm)) : 0);
            return;
        }
        case 0x0012: {  // GetGWorldDevice(gw): GDHandle
            (void)arg_l(cpu);
            ret_l(cpu, main_gdevice());
            return;
        }
        }
        fatal("QDExtensions selector 0x%04X unimplemented at pc=%06X", sel, cpu.pc);
    }
    case 0xAA52: {  // _HighLevelFSDispatch: FSSpec File Manager, selector in D0
        uint16_t sel = uint16_t(cpu.d[0]);
        // FSSpec layout: vRefNum:2, parID:4, name:Str63
        auto read_spec = [](uint32_t sp, int32_t* parID, std::string* name) {
            if (parID) *parID = int32_t(mem_read32(sp + 2));
            if (name) *name = read_pstr(sp + 6);
        };
        switch (sel) {
        case 0x0001: {  // FSMakeFSSpec(vRefNum:w, dirID:l, name:p, spec:p): OSErr
            uint32_t specp = arg_l(cpu);
            uint32_t namep = arg_l(cpu);
            int32_t dirID = int32_t(arg_l(cpu));
            int16_t vref = int16_t(arg_w(cpu));
            std::string name = namep ? read_pstr(namep) : "";
            SpecInfo s = fs_make_spec(vref, dirID, name);
            if (specp && (s.err == noErr || s.err == fnfErr)) {
                mem_write16(specp, uint16_t(-1));        // our single volume
                mem_write32(specp + 2, uint32_t(s.parID));
                std::string leaf = s.name.size() > 63 ? s.name.substr(0, 63)
                                                      : s.name;
                write_pstr(specp + 6, leaf);
            }
            if (s_trace)
                std::fprintf(stderr, "[fs] FSMakeFSSpec(%d,%d,'%s') -> err %d "
                             "(parID %d, '%s')\n", vref, dirID, name.c_str(),
                             s.err, s.parID, s.name.c_str());
            ret_w(cpu, uint16_t(s.err));
            return;
        }
        case 0x0002:    // FSpOpenDF(spec:p, perm:w, refNum:p): OSErr
        case 0x0003: {  // FSpOpenRF
            uint32_t refp = arg_l(cpu);
            (void)arg_w(cpu);
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            int16_t ref = fs_open_at(-1, parID, name, sel == 0x0003);
            if (ref >= 0 && refp) mem_write16(refp, uint16_t(ref));
            ret_w(cpu, uint16_t(ref < 0 ? ref : noErr));
            return;
        }
        case 0x0004: {  // FSpCreate(spec:p, creator:l, type:l, script:w): OSErr
            (void)arg_w(cpu);
            uint32_t type = arg_l(cpu);
            uint32_t creator = arg_l(cpu);
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            ret_w(cpu, uint16_t(fs_create_file(parID, name, creator, type)));
            return;
        }
        case 0x0005: {  // FSpDirCreate(spec:p, script:w, createdDirID:p): OSErr
            uint32_t outp = arg_l(cpu);
            (void)arg_w(cpu);
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            int32_t new_id = 0;
            int16_t err = fs_create_dir(parID, name, &new_id);
            if (outp && new_id) mem_write32(outp, uint32_t(new_id));
            ret_w(cpu, uint16_t(err));
            return;
        }
        case 0x0006: {  // FSpDelete(spec:p): OSErr
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            ret_w(cpu, uint16_t(fs_delete_spec(parID, name)));
            return;
        }
        case 0x0007:    // FSpGetFInfo(spec:p, fndrInfo:p): OSErr
        case 0x0008: {  // FSpSetFInfo(spec:p, fndrInfo:p): OSErr
            uint32_t fip = arg_l(cpu);
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            SpecInfo s = fs_make_spec(-1, parID, name);
            if (!s.exists) { ret_w(cpu, uint16_t(fnfErr)); return; }
            if (sel == 0x0007) {
                uint32_t type, creator;
                fs_get_finfo(s.host, &type, &creator);
                mem_write32(fip, type);
                mem_write32(fip + 4, creator);
                for (int i = 8; i < 16; i += 2) mem_write16(fip + i, 0);
            } else {
                fs_set_finfo(s.host, mem_read32(fip), mem_read32(fip + 4));
            }
            ret_w(cpu, uint16_t(noErr));
            return;
        }
        case 0x0009:    // FSpSetFLock(spec:p): OSErr — no-op
        case 0x000A: {  // FSpRstFLock(spec:p): OSErr — no-op
            (void)arg_l(cpu);
            ret_w(cpu, uint16_t(noErr));
            return;
        }
        case 0x000D: {  // FSpOpenResFile(spec:p, perm:w): refnum
            (void)arg_w(cpu);
            uint32_t specp = arg_l(cpu);
            int32_t parID; std::string name;
            read_spec(specp, &parID, &name);
            ret_w(cpu, uint16_t(rm_open_res_file(name)));
            return;
        }
        }
        fatal("HighLevelFSDispatch selector 0x%04X unimplemented at pc=%06X",
              sel, cpu.pc);
    }
    // ---- Sound Manager channels (driver output; accept commands) ----
    case 0xA807: {  // SndNewChannel(VAR chan:l, synth:w, init:l, cb:l): OSErr
        uint32_t cb = arg_l(cpu);
        uint32_t init = arg_l(cpu);
        uint16_t synth = arg_w(cpu);
        uint32_t chanp = arg_l(cpu);
        uint32_t chan = chanp ? mem_read32(chanp) : 0;
        if (!chan) {
            chan = mm_new_ptr(1064, true);
            if (chanp) mem_write32(chanp, chan);
        }
        mem_write32(chan + 8, cb);              // SndChannel.callBack
        s_snd_chans[chan].cb = cb;
        std::fprintf(stderr, "[snd] SndNewChannel chan=%06X synth=%d "
                     "init=%08X cb=%06X\n", chan, synth, init, cb);
        ret_w(cpu, 0);
        return;
    }
    case 0xA801: {  // SndDisposeChannel(chan:l, quietNow:w): OSErr
        (void)arg_w(cpu);
        uint32_t chan = arg_l(cpu);
        std::fprintf(stderr, "[snd] SndDisposeChannel chan=%06X\n", chan);
        s_dblbufs.erase(std::remove_if(s_dblbufs.begin(), s_dblbufs.end(),
                                       [chan](const DblBuf& db) {
                                           return db.chan == chan;
                                       }),
                        s_dblbufs.end());
        s_snd_chans.erase(chan);
        ret_w(cpu, 0);
        return;
    }
    case 0xA803: case 0xA804: {  // SndDoCommand(chan,cmd,noWait) / SndDoImmediate(chan,cmd)
        bool immediate = (op & 0xFF) == 0x04;
        if (!immediate) (void)arg_w(cpu);
        uint32_t cmdp = arg_l(cpu);
        uint32_t chan = arg_l(cpu);
        uint16_t cmd = mem_read16(cmdp);
        uint16_t p1 = mem_read16(cmdp + 2);
        uint32_t p2 = mem_read32(cmdp + 4);
        static int s_logged = 0;
        if (s_logged < 20 && ++s_logged)
            std::fprintf(stderr, "[snd] Snd%s chan=%06X cmd=%d p1=%d p2=%08X\n",
                         immediate ? "DoImmediate" : "DoCommand", chan, cmd,
                         p1, p2);
        SndChanState& st = s_snd_chans[chan];
        int64_t now = now_us();
        switch (cmd) {
        case 3: case 4:                  // quietCmd / flushCmd
            st.cbs.clear();
            st.busy_until = now;
            break;
        case 80: case 81: {              // soundCmd / bufferCmd: p2 = SoundHeader
            if (!p2) break;
            uint32_t data = mem_read32(p2);        // samplePtr; 0 = inline
            uint32_t len = mem_read32(p2 + 4);
            uint32_t rate = mem_read16(p2 + 8);    // integer part of Fixed
            uint8_t encode = mem_read8(p2 + 20);
            if (!data) data = p2 + 22;
            if (!rate) rate = 22050;
            if (encode != 0x00) {                  // only stdSH handled
                static int s_enc = 0;
                if (s_enc < 4 && ++s_enc)
                    std::fprintf(stderr, "[snd] bufferCmd encode=%02X "
                                 "unsupported (len=%u)\n", encode, len);
                len = 0;
            }
            int64_t dur = len ? int64_t(len) * 1000000 / rate : 0;
            int64_t start = st.busy_until > now ? st.busy_until : now;
            st.busy_until = start + dur;
            if (len && len < 0x100000) {
                SfxVoice v;
                v.pcm.assign(g_mem + (data & ADDR_MASK),
                             g_mem + ((data + len) & ADDR_MASK));
                v.step = double(rate) / 22254.0;   // music stream rate
                s_sfx_voices.push_back(std::move(v));
            }
            break;
        }
        case 13:                         // callBackCmd: fire when queue drains
            st.cbs.push_back({st.busy_until > now ? st.busy_until : now,
                              p1, p2});
            break;
        default:
            break;
        }
        ret_w(cpu, 0);
        return;
    }
    // ---- Sound Manager / MIDI Manager dispatch ----
    case 0xA800: {  // _SoundDispatch: full selector in D0 (group in low word)
        uint32_t sel = cpu.d[0];
        switch (sel) {
        case 0x000C0008:    // SndSoundManagerVersion: NumVersion
            ret_l(cpu, 0x03008000);   // Sound Manager 3.0
            return;
        case 0x022C0018: {  // GetDefaultOutputVolume(VAR vol:l): OSErr
            uint32_t vp = arg_l(cpu);
            if (vp) mem_write32(vp, 0x01000100);   // full volume L/R
            ret_w(cpu, 0);
            return;
        }
        case 0x02300018:    // SetDefaultOutputVolume(vol:l): OSErr
            (void)arg_l(cpu);
            ret_w(cpu, 0);
            return;
        case 0x00200008: {  // SndPlayDoubleBuffer(chan:l, hdr:l): OSErr
            uint32_t hdr = arg_l(cpu);
            uint32_t chan = arg_l(cpu);
            uint32_t buf0 = mem_read32(hdr + 12);
            std::fprintf(stderr, "[snd] SndPlayDoubleBuffer chan=%06X hdr=%06X"
                         " rate=%u frames=%u doubleback=%06X\n",
                         chan, hdr, mem_read16(hdr + 8), mem_read32(buf0),
                         mem_read32(hdr + 20));
            DblBuf db;
            db.chan = chan;
            db.hdr = hdr;
            db.cur = 0;
            db.next_due = now_us() + dblbuf_duration_us(hdr, buf0);
            s_dblbufs.push_back(db);
            ret_w(cpu, 0);
            return;
        }
        // Apple MIDI Manager (group 0x0004): report absent — the game falls
        // back to its own MDRV driver path
        case 0x00040004: {  // MIDISignIn(clientID:l, refCon:l, icon:l, name:p): OSErr
            (void)arg_l(cpu); (void)arg_l(cpu); (void)arg_l(cpu); (void)arg_l(cpu);
            std::fprintf(stderr, "[snd] MIDISignIn -> no MIDI Manager\n");
            ret_w(cpu, uint16_t(-1));
            return;
        }
        case 0x00080004:    // MIDISignOut(clientID:l)
            (void)arg_l(cpu);
            return;
        case 0x001C0004: {  // MIDIAddPort(clientID:l, bufSize:w, VAR ref:l, params:l): OSErr
            (void)arg_l(cpu); (void)arg_l(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
            ret_w(cpu, uint16_t(-1));
            return;
        }
        case 0x004C0004:    // MIDIRemovePort(ref:w): OSErr
            (void)arg_w(cpu);
            ret_w(cpu, 0);
            return;
        }
        fatal("SoundDispatch selector 0x%08X unimplemented at pc=%06X",
              sel, cpu.pc);
    }
    // ---- menu color tables ----
    case 0xAA46: {  // GetNewCWindow(id:2, storage:4, behind:4): CWindowPtr
        (void)arg_l(cpu);
        uint32_t storage = arg_l(cpu);
        int16_t id = int16_t(arg_w(cpu));
        uint32_t w = make_window(id, storage, true);
        std::fprintf(stderr, "[trap] GetNewCWindow(%d) -> %06X\n", id, w);
        ret_l(cpu, w);
        return;
    }
    case 0xAA47: (void)arg_l(cpu); return;                  // SetMCInfo
    case 0xAA48: ret_l(cpu, 0); return;                     // GetMCInfo
    case 0xAA3F: {  // SetEntries(start:w, count-1:w, aTable: cSpecArray*)
        uint32_t tab = arg_l(cpu);
        int16_t cnt = int16_t(arg_w(cpu));
        int16_t start = int16_t(arg_w(cpu));
        for (int i = 0; i <= cnt; i++) {
            uint32_t sp = tab + uint32_t(i) * 8;
            int idx = start < 0 ? int16_t(mem_read16(sp)) : start + i;
            video_set_color(idx, uint8_t(mem_read16(sp + 2) >> 8),
                            uint8_t(mem_read16(sp + 4) >> 8),
                            uint8_t(mem_read16(sp + 6) >> 8));
        }
        return;
    }
    case 0xAA40: ret_w(cpu, 0); return;                     // QDError -> noErr
    // ---- Palette Manager ----
    case 0xAA90: return;                                    // InitPalettes
    case 0xAA91: {  // NewPalette(entries:2, srcColors:4, usage:2, tolerance:2)
        int16_t tol = int16_t(arg_w(cpu));
        int16_t usage = int16_t(arg_w(cpu));
        uint32_t src = arg_l(cpu);
        int16_t n = int16_t(arg_w(cpu));
        uint32_t pal = mm_new_handle(16 + uint32_t(n) * 14, true);
        uint32_t pp = mem_read32(pal);
        mem_write16(pp, uint16_t(n));
        for (int i = 0; i < n; i++) {
            uint32_t ep = pp + 16 + uint32_t(i) * 14;
            if (src) {  // copy RGB from source color table
                uint32_t cp = mem_read32(src);
                int16_t ctSize = int16_t(mem_read16(cp + 6));
                if (i <= ctSize) {
                    uint32_t sp = cp + 8 + uint32_t(i) * 8 + 2;
                    for (int k = 0; k < 6; k += 2)
                        mem_write16(ep + k, mem_read16(sp + k));
                }
            }
            mem_write16(ep + 6, uint16_t(usage));
            mem_write16(ep + 8, uint16_t(tol));
        }
        ret_l(cpu, pal);
        return;
    }
    case 0xAA92: {  // GetNewPalette(id:2): load 'pltt' resource
        int16_t id = int16_t(arg_w(cpu));
        ret_l(cpu, rm_get_resource(0x706C7474 /*pltt*/, id, false));
        return;
    }
    case 0xAA93: mm_dispose_handle(arg_l(cpu)); return;     // DisposePalette
    case 0xAA94: {  // ActivatePalette(w): push window's palette to the screen
        uint32_t w = arg_l(cpu);
        uint32_t pal = s_win_palette.count(w) ? s_win_palette[w] : 0;
        if (pal) {
            uint32_t pp = mem_read32(pal);
            int16_t n = int16_t(mem_read16(pp));
            for (int i = 0; i < n && i < 256; i++) {
                uint32_t ep = pp + 16 + uint32_t(i) * 14;
                video_set_color(i, uint8_t(mem_read16(ep) >> 8),
                                uint8_t(mem_read16(ep + 2) >> 8),
                                uint8_t(mem_read16(ep + 4) >> 8));
            }
            std::fprintf(stderr, "[pal] ActivatePalette: %d entries\n", n);
        }
        return;
    }
    case 0xAA95: {  // SetPalette(w:4, pal:4, updates:2)
        (void)arg_w(cpu);
        uint32_t pal = arg_l(cpu);
        uint32_t w = arg_l(cpu);
        s_win_palette[w] = pal;
        return;
    }
    case 0xAA96: {  // GetPalette(w)
        uint32_t w = arg_l(cpu);
        ret_l(cpu, s_win_palette.count(w) ? s_win_palette[w] : 0);
        return;
    }
    case 0xAA97: case 0xAA98: { (void)arg_w(cpu); return; } // PmFore/BackColor
    case 0xAA9A: {  // AnimatePalette(w, ctab, srcIndex, dstIndex, count)
        int16_t count = int16_t(arg_w(cpu));
        int16_t dst = int16_t(arg_w(cpu));
        int16_t src = int16_t(arg_w(cpu));
        uint32_t ctab = arg_l(cpu);
        uint32_t w = arg_l(cpu);
        if (ctab) {
            uint32_t cp = mem_read32(ctab);
            int16_t ctSize = int16_t(mem_read16(cp + 6));
            uint32_t pal = s_win_palette.count(w) ? s_win_palette[w] : 0;
            uint32_t pp = pal ? mem_read32(pal) : 0;
            for (int i = 0; i < count; i++) {
                int s = src + i;
                if (s > ctSize) break;
                uint32_t sp = cp + 8 + uint32_t(s) * 8 + 2;
                video_set_color(dst + i, uint8_t(mem_read16(sp) >> 8),
                                uint8_t(mem_read16(sp + 2) >> 8),
                                uint8_t(mem_read16(sp + 4) >> 8));
                if (pp) {  // keep the palette record in sync
                    uint32_t ep = pp + 16 + uint32_t(dst + i) * 14;
                    for (int k = 0; k < 6; k += 2)
                        mem_write16(ep + k, mem_read16(sp + k));
                }
            }
        }
        return;
    }
    case 0xAA9C: {  // SetEntryColor(pal, entry, rgb*)
        uint32_t rgbp = arg_l(cpu);
        int16_t entry = int16_t(arg_w(cpu));
        uint32_t pal = arg_l(cpu);
        if (pal && entry >= 0) {
            uint32_t ep = mem_read32(pal) + 16 + uint32_t(entry) * 14;
            for (int k = 0; k < 6; k += 2)
                mem_write16(ep + k, mem_read16(rgbp + k));
        }
        return;
    }
    case 0xAA9E: {  // SetEntryUsage(pal, entry, usage, tolerance)
        (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_w(cpu); (void)arg_l(cpu);
        return;
    }
    case 0xAA9F: {  // CTab2Palette(ctab, pal, usage, tolerance)
        int16_t tol = int16_t(arg_w(cpu));
        int16_t usage = int16_t(arg_w(cpu));
        uint32_t pal = arg_l(cpu);
        uint32_t ctab = arg_l(cpu);
        if (pal && ctab) {
            uint32_t cp = mem_read32(ctab), pp = mem_read32(pal);
            int16_t ctSize = int16_t(mem_read16(cp + 6));
            int16_t n = int16_t(mem_read16(pp));
            for (int i = 0; i <= ctSize && i < n; i++) {
                uint32_t sp = cp + 8 + uint32_t(i) * 8 + 2;
                uint32_t ep = pp + 16 + uint32_t(i) * 14;
                for (int k = 0; k < 6; k += 2)
                    mem_write16(ep + k, mem_read16(sp + k));
                mem_write16(ep + 6, uint16_t(usage));
                mem_write16(ep + 8, uint16_t(tol));
            }
        }
        return;
    }
    case 0xAAA0: {  // Palette2CTab(pal, ctab)
        uint32_t ctab = arg_l(cpu);
        uint32_t pal = arg_l(cpu);
        if (pal && ctab) {
            uint32_t pp = mem_read32(pal);
            int16_t n = int16_t(mem_read16(pp));
            if (n > 256) n = 256;
            if (mm_get_handle_size(ctab) < int32_t(8 + uint32_t(n) * 8))
                mm_set_handle_size(ctab, 8 + uint32_t(n) * 8);
            uint32_t cp = mem_read32(ctab);
            mem_write32(cp, mem_read32(cp) + 1);        // ctSeed changes
            mem_write16(cp + 4, 0);
            mem_write16(cp + 6, uint16_t(n - 1));       // ctSize
            for (int i = 0; i < n; i++) {
                uint32_t ep = pp + 16 + uint32_t(i) * 14;
                uint32_t dp = cp + 8 + uint32_t(i) * 8;
                mem_write16(dp, uint16_t(i));
                for (int k = 0; k < 6; k += 2)
                    mem_write16(dp + 2 + k, mem_read16(ep + k));
            }
        }
        return;
    }
    // ---- Event Manager (SDL-backed) ----
    case 0xA976: {  // GetKeys(VAR keyMap[16])
        uint32_t km = arg_l(cpu);
        uint8_t keys[16];
        video_get_keys(keys);
        for (int i = 0; i < 16; i++) mem_write8(km + i, keys[i]);
        return;
    }
    case 0xA974: ret_w(cpu, video_button() ? 0x0100 : 0); return;  // Button
    case 0xA973: ret_w(cpu, video_button() ? 0x0100 : 0); return;  // StillDown
    case 0xA977: ret_w(cpu, video_button() ? 0x0100 : 0); return;  // WaitMouseUp
    case 0xA972: {  // GetMouse(VAR pt) — local coords ~ global (origin 0,0)
        uint32_t pp = arg_l(cpu);
        int16_t h = 0, v = 0;
        video_get_mouse(&h, &v);
        mem_write16(pp, uint16_t(v));
        mem_write16(pp + 2, uint16_t(h));
        return;
    }
    case 0xA970:    // GetNextEvent(mask, VAR ev): Boolean
    case 0xA971: {  // EventAvail: peeks without removing
        uint32_t evp = arg_l(cpu);
        uint16_t mask = arg_w(cpu);
        tm_fire_due(cpu); vbl_fire_due(cpu); snd_pump(cpu);  // interrupt time
        MacEvent ev;
        bool got = video_next_event(mask, &ev, op == 0xA970);
        static const bool trace_ev = std::getenv("POP2_TRACE_EVENTS") != nullptr;
        if (trace_ev && got)
            std::fprintf(stderr, "[ev] %s pc=%06X mask=%04X what=%u msg=%08X\n",
                         op == 0xA970 ? "GNE" : "EA", cpu.pc, mask, ev.what,
                         ev.message);
        if (evp) {
            mem_write16(evp, ev.what);
            mem_write32(evp + 2, ev.message);
            mem_write32(evp + 6, got ? ev.when : ticks_now());
            mem_write16(evp + 10, uint16_t(ev.where_v));
            mem_write16(evp + 12, uint16_t(ev.where_h));
            mem_write16(evp + 14, ev.modifiers);
        }
        ret_w(cpu, got ? 0x0100 : 0);
        return;
    }
    case 0xA860: {  // WaitNextEvent(mask, VAR ev, sleep, mouseRgn): Boolean
        (void)arg_l(cpu);
        (void)arg_l(cpu);
        uint32_t evp = arg_l(cpu);
        uint16_t mask = arg_w(cpu);
        // Copy-protection bypass: (a5-22232) is the "manual symbol already
        // verified this session" flag, checked once at f2_1a36 entry. The
        // "Select the symbol on page N of the manual" grid blocks forever
        // without the physical manual, so keep the flag pinned to 1 from the
        // menu event loop (runs before any New/Open) — the standard
        // preservation bypass. POP2_REQUIRE_PROTECTION=1 restores the gate.
        static const bool keep_prot = std::getenv("POP2_REQUIRE_PROTECTION");
        if (!keep_prot) mem_write16(A5_BASE - 22232, 1);
        tm_fire_due(cpu); vbl_fire_due(cpu); snd_pump(cpu);  // interrupt time
        MacEvent ev;
        bool got = video_next_event(mask, &ev);
        static const bool trace_wne = std::getenv("POP2_TRACE_EVENTS") != nullptr;
        if (trace_wne && got)
            std::fprintf(stderr, "[ev] WNE pc=%06X mask=%04X what=%u msg=%08X"
                         " mods=%04X\n", cpu.pc, mask, ev.what, ev.message,
                         ev.modifiers);
        if (evp) {
            mem_write16(evp, ev.what);
            mem_write32(evp + 2, ev.message);
            mem_write32(evp + 6, got ? ev.when : ticks_now());
            mem_write16(evp + 10, uint16_t(ev.where_v));
            mem_write16(evp + 12, uint16_t(ev.where_h));
            mem_write16(evp + 14, ev.modifiers);
        }
#ifndef __EMSCRIPTEN__
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        // No idle sleep under Emscripten: WaitNextEvent already runs through
        // video_pump(), which yields to the browser once per frame at its
        // present throttle. An extra emscripten_sleep here fired on every idle
        // poll, and the browser's ~4ms setTimeout clamp stacked several of them
        // per frame — capping the game near 40fps. The frame-paced yield alone
        // holds ~60 and keeps the tab responsive.
        ret_w(cpu, got ? 0x0100 : 0);
        return;
    }
    case 0xA9B4:    // SystemTask
        video_pump(); tm_fire_due(cpu); vbl_fire_due(cpu); snd_pump(cpu);
        return;
    // ---- misc ----
    case 0xA975:  // TickCount
        video_pump(); tm_fire_due(cpu); vbl_fire_due(cpu); snd_pump(cpu);
        ret_l(cpu, ticks_now()); return;
    case 0xA9C8: (void)arg_w(cpu); return;                  // SysBeep
    case 0xA9C6: {  // Secs2Date(secs:l, VAR d: DateTimeRec*) — Mac epoch 1904
        uint32_t dp = arg_l(cpu);
        uint32_t secs = arg_l(cpu);
        time_t ut = time_t(int64_t(secs) - 2082844800LL);
        struct tm tmv {};
        localtime_r(&ut, &tmv);
        mem_write16(dp + 0, uint16_t(tmv.tm_year + 1900));
        mem_write16(dp + 2, uint16_t(tmv.tm_mon + 1));
        mem_write16(dp + 4, uint16_t(tmv.tm_mday));
        mem_write16(dp + 6, uint16_t(tmv.tm_hour));
        mem_write16(dp + 8, uint16_t(tmv.tm_min));
        mem_write16(dp + 10, uint16_t(tmv.tm_sec));
        mem_write16(dp + 12, uint16_t(tmv.tm_wday + 1));   // 1 = Sunday
        return;
    }
    case 0xA9C7: {  // Date2Secs(d: DateTimeRec*, VAR secs:l)
        uint32_t sp = arg_l(cpu);
        uint32_t dp = arg_l(cpu);
        struct tm tmv {};
        tmv.tm_year = int(mem_read16(dp)) - 1900;
        tmv.tm_mon = int(mem_read16(dp + 2)) - 1;
        tmv.tm_mday = int(mem_read16(dp + 4));
        tmv.tm_hour = int(mem_read16(dp + 6));
        tmv.tm_min = int(mem_read16(dp + 8));
        tmv.tm_sec = int(mem_read16(dp + 10));
        tmv.tm_isdst = -1;
        mem_write32(sp, uint32_t(int64_t(mktime(&tmv)) + 2082844800LL));
        return;
    }
    }
    fatal("unimplemented Toolbox trap %s (0x%04X) at pc=%06X a7=%06X",
          trap_display_name(op), op, cpu.pc, cpu.a[7]);
}

void do_trap(Cpu& cpu, uint16_t opcode) {
    uint16_t op = opcode;
    if (op >= 0xAC00) op &= ~0x0400;   // strip auto-pop bit
    if (s_trace)
        std::fprintf(stderr, "[trap] %s (0x%04X) pc=%06X\n",
                     trap_display_name(op), op, cpu.pc);
    if (op >= 0xA800) {
        tb_trap(cpu, op);
    } else {
        os_trap(cpu, op);
        // register-based OS traps return their result in D0 and exit with
        // the condition codes reflecting it (callers do BNE right after)
        set_nz_clear_vc(cpu, cpu.d[0]);
    }
}

}  // namespace pop2
