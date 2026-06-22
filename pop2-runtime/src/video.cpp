// SDL2 video/input: presents the 512x384 8-bit guest framebuffer (FB_BASE)
// through the runtime palette, and feeds keyboard/mouse state to the Event
// Manager traps. Initialized lazily on the first pump so resource-dump tools
// and headless runs (POP2_NO_VIDEO=1, or no display) keep working.
#include <algorithm>
#include "pop2/mac.h"

#include <SDL2/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace pop2 {

namespace {

constexpr int kWidth = SCREEN_W;    // 512 — PoP2's native screen (see mac.h)
constexpr int kHeight = SCREEN_H;   // 384

enum class State { Uninit, On, Off };
State s_state = State::Uninit;

SDL_Window* s_win = nullptr;
SDL_Renderer* s_ren = nullptr;
SDL_Texture* s_tex = nullptr;

// ARGB8888; starts as the Mac default ramp (0=white .. 255=black) so color
// matching works before the game activates its palette, headless included.
uint32_t s_pal[256];
const bool s_pal_init = [] {
    for (int i = 0; i < 256; i++) {
        uint32_t v = uint32_t(255 - i);
        s_pal[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
    return true;
}();
uint8_t s_keymap[16];           // Mac KeyMap bits
bool s_button = false;
int16_t s_mouse_h = SCREEN_W / 2, s_mouse_v = SCREEN_H / 2;
uint32_t s_last_present = 0;    // SDL_GetTicks of last frame
std::deque<MacEvent> s_events;

// SDL scancode -> Mac virtual key code (classic US layout)
int mac_vkey(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_A: return 0x00;
    case SDL_SCANCODE_S: return 0x01;
    case SDL_SCANCODE_D: return 0x02;
    case SDL_SCANCODE_F: return 0x03;
    case SDL_SCANCODE_H: return 0x04;
    case SDL_SCANCODE_G: return 0x05;
    case SDL_SCANCODE_Z: return 0x06;
    case SDL_SCANCODE_X: return 0x07;
    case SDL_SCANCODE_C: return 0x08;
    case SDL_SCANCODE_V: return 0x09;
    case SDL_SCANCODE_B: return 0x0B;
    case SDL_SCANCODE_Q: return 0x0C;
    case SDL_SCANCODE_W: return 0x0D;
    case SDL_SCANCODE_E: return 0x0E;
    case SDL_SCANCODE_R: return 0x0F;
    case SDL_SCANCODE_Y: return 0x10;
    case SDL_SCANCODE_T: return 0x11;
    case SDL_SCANCODE_1: return 0x12;
    case SDL_SCANCODE_2: return 0x13;
    case SDL_SCANCODE_3: return 0x14;
    case SDL_SCANCODE_4: return 0x15;
    case SDL_SCANCODE_6: return 0x16;
    case SDL_SCANCODE_5: return 0x17;
    case SDL_SCANCODE_EQUALS: return 0x18;
    case SDL_SCANCODE_9: return 0x19;
    case SDL_SCANCODE_7: return 0x1A;
    case SDL_SCANCODE_MINUS: return 0x1B;
    case SDL_SCANCODE_8: return 0x1C;
    case SDL_SCANCODE_0: return 0x1D;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
    case SDL_SCANCODE_O: return 0x1F;
    case SDL_SCANCODE_U: return 0x20;
    case SDL_SCANCODE_LEFTBRACKET: return 0x21;
    case SDL_SCANCODE_I: return 0x22;
    case SDL_SCANCODE_P: return 0x23;
    case SDL_SCANCODE_RETURN: return 0x24;
    case SDL_SCANCODE_L: return 0x25;
    case SDL_SCANCODE_J: return 0x26;
    case SDL_SCANCODE_APOSTROPHE: return 0x27;
    case SDL_SCANCODE_K: return 0x28;
    case SDL_SCANCODE_SEMICOLON: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2A;
    case SDL_SCANCODE_COMMA: return 0x2B;
    case SDL_SCANCODE_SLASH: return 0x2C;
    case SDL_SCANCODE_N: return 0x2D;
    case SDL_SCANCODE_M: return 0x2E;
    case SDL_SCANCODE_PERIOD: return 0x2F;
    case SDL_SCANCODE_TAB: return 0x30;
    case SDL_SCANCODE_SPACE: return 0x31;
    case SDL_SCANCODE_GRAVE: return 0x32;
    case SDL_SCANCODE_BACKSPACE: return 0x33;
    case SDL_SCANCODE_ESCAPE: return 0x35;
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return 0x37;
    case SDL_SCANCODE_LSHIFT: return 0x38;
    case SDL_SCANCODE_CAPSLOCK: return 0x39;
    case SDL_SCANCODE_LALT: return 0x3A;
    case SDL_SCANCODE_LCTRL: return 0x3B;
    case SDL_SCANCODE_RSHIFT: return 0x3C;
    case SDL_SCANCODE_RALT: return 0x3D;
    case SDL_SCANCODE_RCTRL: return 0x3E;
    case SDL_SCANCODE_KP_PERIOD: return 0x41;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x43;
    case SDL_SCANCODE_KP_PLUS: return 0x45;
    case SDL_SCANCODE_KP_DIVIDE: return 0x4B;
    case SDL_SCANCODE_KP_ENTER: return 0x4C;
    case SDL_SCANCODE_KP_MINUS: return 0x4E;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_1: return 0x53;
    case SDL_SCANCODE_KP_2: return 0x54;
    case SDL_SCANCODE_KP_3: return 0x55;
    case SDL_SCANCODE_KP_4: return 0x56;
    case SDL_SCANCODE_KP_5: return 0x57;
    case SDL_SCANCODE_KP_6: return 0x58;
    case SDL_SCANCODE_KP_7: return 0x59;
    case SDL_SCANCODE_KP_8: return 0x5B;
    case SDL_SCANCODE_KP_9: return 0x5C;
    case SDL_SCANCODE_F5: return 0x60;
    case SDL_SCANCODE_F6: return 0x61;
    case SDL_SCANCODE_F7: return 0x62;
    case SDL_SCANCODE_F3: return 0x63;
    case SDL_SCANCODE_F8: return 0x64;
    case SDL_SCANCODE_F9: return 0x65;
    case SDL_SCANCODE_F11: return 0x67;
    case SDL_SCANCODE_F10: return 0x6D;
    case SDL_SCANCODE_F12: return 0x6F;
    case SDL_SCANCODE_F4: return 0x76;
    case SDL_SCANCODE_F2: return 0x78;
    case SDL_SCANCODE_F1: return 0x7A;
    case SDL_SCANCODE_LEFT: return 0x7B;
    case SDL_SCANCODE_RIGHT: return 0x7C;
    case SDL_SCANCODE_DOWN: return 0x7D;
    case SDL_SCANCODE_UP: return 0x7E;
    default: return -1;
    }
}

// ---- gamepad: d-pad/left stick -> arrows, A -> Shift, Start -> Return ----
SDL_GameController* s_pad = nullptr;
bool s_pad_shift = false;
bool s_pad_keys[4];             // left, right, up, down logical state

bool s_fake_cmd = false;   // POP2_AUTOKEY ":cmd" holds a synthetic cmdKey
bool s_fake_shift = false; // synthetic shiftKey while autokey holds vk 0x38

uint16_t mac_modifiers() {
    SDL_Keymod m = SDL_GetModState();
    uint16_t r = 0;
    if (!s_button) r |= 0x0080;            // btnState: 1 = up
    if (m & KMOD_GUI) r |= 0x0100;         // cmdKey
    if (m & KMOD_SHIFT) r |= 0x0200;       // shiftKey
    if (m & KMOD_CAPS) r |= 0x0400;        // alphaLock
    if (m & KMOD_ALT) r |= 0x0800;         // optionKey
    if (m & KMOD_CTRL) r |= 0x1000;        // controlKey
    if (s_fake_cmd) r |= 0x0100;
    if (s_fake_shift) r |= 0x0200;
    if (s_pad_shift) r |= 0x0200;          // gamepad A acts as Shift
    return r;
}

// rough vkey -> Mac ASCII for the event message low byte
uint8_t mac_char(SDL_Keysym ks) {
    if (ks.sym >= 32 && ks.sym < 127) {
        char c = char(ks.sym);
        if ((ks.mod & KMOD_SHIFT) && c >= 'a' && c <= 'z') c = char(c - 32);
        return uint8_t(c);
    }
    switch (ks.scancode) {
    case SDL_SCANCODE_RETURN: return 13;
    case SDL_SCANCODE_KP_ENTER: return 3;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_ESCAPE: return 27;
    case SDL_SCANCODE_LEFT: return 28;
    case SDL_SCANCODE_RIGHT: return 29;
    case SDL_SCANCODE_UP: return 30;
    case SDL_SCANCODE_DOWN: return 31;
    default: return 0;
    }
}

void push_event(uint16_t what, uint32_t message) {
    MacEvent ev;
    ev.what = what;
    ev.message = message;
    ev.when = uint32_t(SDL_GetTicks() * 60 / 1000);
    ev.where_v = s_mouse_v;
    ev.where_h = s_mouse_h;
    ev.modifiers = mac_modifiers();
    if (s_events.size() > 64) s_events.pop_front();
    s_events.push_back(ev);
}

bool s_linear = false;          // F8 toggles; POP2_FILTER=linear|nearest
bool s_integer = false;         // F9 toggles integer-only scaling
bool s_fullscreen = false;      // F11 toggles

void make_texture() {
    if (s_tex) SDL_DestroyTexture(s_tex);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, s_linear ? "linear" : "nearest");
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
}

bool init_video() {
    if (std::getenv("POP2_NO_VIDEO")) return false;
    // Let SIGINT/SIGTERM keep their default die-now semantics. SDL would
    // otherwise swallow them into an SDL_QUIT event, so a `timeout`-killed
    // test run survives as an orphan whenever the pump isn't being polled.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[video] SDL_Init failed: %s — headless\n",
                     SDL_GetError());
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        std::fprintf(stderr, "[video] no game controller support: %s\n",
                     SDL_GetError());
    if (const char* f = std::getenv("POP2_FILTER"))
        s_linear = std::strcmp(f, "linear") == 0;
    int scale = 1;
    if (const char* sc = std::getenv("POP2_SCALE"))
        scale = std::max(1, std::atoi(sc));
    s_win = SDL_CreateWindow("Prince of Persia 2",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             kWidth * scale, kHeight * scale,
                             SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!s_win) return false;
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    SDL_RenderSetLogicalSize(s_ren, kWidth, kHeight);   // aspect-true scaling
    make_texture();
    std::fprintf(stderr, "[video] SDL window up (%dx%d, scale x%d) — "
                 "F8 filter, F9 integer scale, F11 fullscreen, gamepad ok\n",
                 kWidth, kHeight, scale);
    return true;
}

void pad_apply(int idx, int vk, uint8_t ch, bool want) {
    if (s_pad_keys[idx] == want) return;
    s_pad_keys[idx] = want;
    if (want) {
        s_keymap[vk >> 3] |= uint8_t(1u << (vk & 7));
        push_event(3, uint32_t(vk << 8) | ch);
    } else {
        s_keymap[vk >> 3] &= uint8_t(~(1u << (vk & 7)));
        push_event(4, uint32_t(vk << 8) | ch);
    }
}

void pad_update() {
    if (!s_pad) return;
    auto btn = [&](SDL_GameControllerButton b) {
        return SDL_GameControllerGetButton(s_pad, b) != 0;
    };
    int16_t ax = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ay = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    const int dz = 10000;
    pad_apply(0, 0x7B, 28, btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -dz);
    pad_apply(1, 0x7C, 29, btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > dz);
    pad_apply(2, 0x7E, 30, btn(SDL_CONTROLLER_BUTTON_DPAD_UP) ||
                               btn(SDL_CONTROLLER_BUTTON_B) || ay < -dz);
    pad_apply(3, 0x7D, 31, btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > dz);
    // A = Shift (careful step / grab / drink) — a modifier, no key event
    bool shift = btn(SDL_CONTROLLER_BUTTON_A) ||
                 SDL_GameControllerGetAxis(
                     s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dz;
    if (shift != s_pad_shift) {
        s_pad_shift = shift;
        const int vk = 0x38;
        if (shift) s_keymap[vk >> 3] |= uint8_t(1u << (vk & 7));
        else s_keymap[vk >> 3] &= uint8_t(~(1u << (vk & 7)));
    }
    static bool s_start_was = false, s_back_was = false;
    bool start = btn(SDL_CONTROLLER_BUTTON_START);
    if (start != s_start_was) {
        s_start_was = start;
        push_event(start ? 3 : 4, uint32_t(0x24 << 8) | 13);
    }
    bool back = btn(SDL_CONTROLLER_BUTTON_BACK);
    if (back != s_back_was) {
        s_back_was = back;
        push_event(back ? 3 : 4, uint32_t(0x35 << 8) | 27);
    }
}

// POP2_DUMP_FB=path:N — one PPM of frame N; path:eN — every N frames
// (path_f<frame>.ppm), each dump also writes the registered GWorlds.
void maybe_dump(int frame) {
    static const char* spec = std::getenv("POP2_DUMP_FB");
    static bool done = false;
    if (!spec || done) return;
    std::string s(spec);
    auto colon = s.rfind(':');
    std::string path = colon == std::string::npos ? s : s.substr(0, colon);
    std::string arg = colon == std::string::npos ? "60" : s.substr(colon + 1);
    bool every = !arg.empty() && arg[0] == 'e';
    int n = std::atoi(arg.c_str() + (every ? 1 : 0));
    if (n <= 0) n = 60;
    if (every) {
        if (frame % n) return;
        char buf[64];
        std::snprintf(buf, sizeof buf, "_f%05d", frame);
        path += buf;
        path += ".ppm";
    } else {
        if (frame < n) return;
        done = true;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
    const uint8_t* fb = g_mem + FB_BASE;
    for (int i = 0; i < kWidth * kHeight; i++) {
        // the game's CLUT never assigns index 0, so 0 means "not yet drawn"
        // (only transient now the framebuffer equals the 512x384 game rect)
        uint32_t c = fb[i] ? s_pal[fb[i]] : 0xFF000000u;
        uint8_t rgb[3] = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[video] frame %d dumped to %s\n", frame, path.c_str());
    qd_dump_gworlds(path.c_str());
}

void present() {
    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(s_tex, nullptr, &pixels, &pitch) != 0) return;
    const uint8_t* fb = g_mem + FB_BASE;
    for (int y = 0; y < kHeight; y++) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(pixels) + size_t(y) * pitch);
        const uint8_t* src = fb + size_t(y) * kWidth;
        for (int x = 0; x < kWidth; x++)
            dst[x] = src[x] ? s_pal[src[x]] : 0xFF000000u;
    }
    SDL_UnlockTexture(s_tex);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr);
    SDL_RenderPresent(s_ren);
    static int s_frame = 0;
    maybe_dump(++s_frame);
    // Optional kid-state trace: POP2_DUMP_KID=N prints the player struct
    // (a5-20690: +2 anim +4 x +6 y +22 room) every N frames — a deterministic
    // check that movement input actually moves the character.
    static const int kid_n = [] {
        const char* e = std::getenv("POP2_DUMP_KID");
        return e ? std::atoi(e) : 0;
    }();
    if (kid_n > 0 && s_frame % kid_n == 0) {
        uint32_t kb = 0x080000u - 20690;
        uint32_t env = mem_read32(0x080000u - 20500);
        int16_t estate = env ? int16_t(mem_read16(env + 8582)) : -999;
        std::fprintf(stderr, "[kid] f%d anim=%d x=%d y=%d col=%d row=%d room=%d "
                     "fallPh=%d seq=%d fallY=%d env8582=%d hp=%d fc=%d mods=%04x vs=%d\n",
                     s_frame, int16_t(mem_read16(kb + 2)), int16_t(mem_read16(kb + 4)),
                     int16_t(mem_read16(kb + 6)), int16_t(mem_read16(kb + 12)),
                     int16_t(mem_read16(kb + 14)), int16_t(mem_read16(kb + 22)),
                     int16_t(mem_read16(kb + 28)), int16_t(mem_read16(0x080000u - 20556)),
                     int16_t(mem_read16(0x080000u - 20560)), estate,
                     int16_t(mem_read16(0x080000u - 20536)),   // hp
                     int16_t(mem_read16(0x080000u - 20484)),   // fall counter
                     mem_read16(0x080000u - 21316),            // last-event modifiers
                     int16_t(mem_read16(0x080000u - 20526)));  // vertical state
        // SFX gate telemetry: the death/jingle flow polls f5_52c8 on the two
        // ids below until both leave the driver's sample slots (a5-8060/-8058)
        static uint64_t pf = 0, pframes = 0;
        std::fprintf(stderr, "[sfx] f%d gate=%d,%d slots=%d,%d dbl=+%llu "
                     "(+%llufr) q=%u\n", s_frame,
                     int16_t(mem_read16(0x080000u - 17306)),
                     int16_t(mem_read16(0x080000u - 17304)),
                     int16_t(mem_read16(0x080000u - 8060)),
                     int16_t(mem_read16(0x080000u - 8058)),
                     (unsigned long long)(g_dbl_fires - pf),
                     (unsigned long long)(g_dbl_frames - pframes),
                     audio_queued_bytes());
        pf = g_dbl_fires;
        pframes = g_dbl_frames;
    }
    // POP2_DUMP_TILE=off[,off...] — with DUMP_KID, also print level-state
    // words at the given byte offsets (tile anim states live at
    // state+1920+(room-1)*120+idx*4, idx = row*10+col).
    static const char* tl_env = std::getenv("POP2_DUMP_TILE");
    if (tl_env && kid_n > 0 && s_frame % kid_n == 0) {
        uint32_t st = mem_read32(0x080000u - 20500);
        if (st) {
            std::fprintf(stderr, "[tile] f%d", s_frame);
            for (const char* p = tl_env; *p;) {
                int off = std::atoi(p);
                std::fprintf(stderr, " @%d=%04x", off,
                             mem_read16(st + uint32_t(off)));
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            std::fprintf(stderr, "\n");
        }
    }
    // POP2_POKE_STATE=ms:off:word[,ms:off:word...] — at time ms, write a
    // 16-bit word into the level-state blob at byte offset off. Dev tool
    // (e.g. zero an NPC room-table count to suppress a hostile spawn).
    static const char* poke_env = std::getenv("POP2_POKE_STATE");
    if (poke_env) {
        static int s_pk_idx = 0;
        const char* spec = poke_env;
        for (int i = 0; i < s_pk_idx && spec; i++) {
            spec = std::strchr(spec, ',');
            if (spec) spec++;
        }
        if (spec && *spec) {
            int ms = 0, off = 0, val = 0, dur = 0;
            int got = std::sscanf(spec, "%d:%d:%d:h%d", &ms, &off, &val, &dur);
            if (got >= 3) {
                uint32_t t = SDL_GetTicks();
                if (t >= uint32_t(ms)) {
                    uint32_t st = mem_read32(0x080000u - 20500);
                    if (st) {
                        mem_write16(st + uint32_t(off), uint16_t(val));
                        static bool s_pk_logged = false;
                        if (!s_pk_logged) {
                            std::fprintf(stderr, "[poke] state+%d = %04x\n",
                                         off, uint16_t(val));
                            s_pk_logged = true;
                        }
                        // hammer until ms+dur so a level (re)load can't
                        // re-init the word behind our back
                        if (t >= uint32_t(ms) + uint32_t(dur)) {
                            s_pk_idx++;
                            s_pk_logged = false;
                        }
                    }
                }
            } else {
                s_pk_idx++;
            }
        }
    }

    // POP2_POKE_A5=ms:negoff:word[:hN][,...] — timed 16-bit writes to A5
    // globals: address = A5_BASE - negoff (e.g. 20536 = hp). Optional hN
    // hammers the write for N ms (pin a value against per-tick rewrites).
    // POP2_AUTOBATTLE=room — while the kid is in that room and standing
    // (frame 15) or holding the flame en-garde (frame 171), keep feeding
    // the fireball control (control_shift = -2) so he draws and lunges
    static const char* autobattle = std::getenv("POP2_AUTOBATTLE");
    if (autobattle) {
        int abroom = std::atoi(autobattle);
        if (int16_t(mem_read16(0x080000u - 20418)) == abroom &&
            int16_t(mem_read16(0x080000u - 20544)) == abroom) {
            uint16_t f = mem_read16(0x080000u - 20556);
            bool boss_dead = mem_read16(0x080000u - 20990) == 185;
            int16_t kx = int16_t(mem_read16(0x080000u - 20562));
            if (boss_dead) {
                // victory: stand still and let the scene play out
            } else if (f == 15 || f == 171) {
                uint16_t bf = mem_read16(0x080000u - 20990);
                if (bf >= 121 && bf <= 132) {
                    // boss grab animation: back off instead of trading
                    mem_write16(0x080000u - 13462, 1u);
                } else if (kx > 356 + 78) {
                    mem_write16(0x080000u - 13462, 0xFFFFu);   // close in
                } else if (kx < 356 - 78) {
                    mem_write16(0x080000u - 13462, 1u);
                } else {
                    mem_write16(0x080000u - 13460, 0xFFFEu);   // lunge
                }
            }
        }
    }

    static const char* pokea5_env = std::getenv("POP2_POKE_A5");
    if (pokea5_env) {
        // all specs are evaluated every frame: each one writes from its ms
        // until ms+dur (parallel hammers, unlike the sequential state pokes)
        static uint32_t s_pa_done = 0;   // bitmask of finished specs
        uint32_t t = SDL_GetTicks();
        int idx = 0;
        for (const char* spec = pokea5_env; spec && *spec; idx++) {
            int ms = 0, off = 0, val = 0, dur = 0;
            int got = std::sscanf(spec, "%d:%d:%d:h%d", &ms, &off, &val, &dur);
            if (got >= 3 && !(s_pa_done & (1u << idx)) && t >= uint32_t(ms)) {
                mem_write16(0x080000u - uint32_t(off), uint16_t(val));
                if (t >= uint32_t(ms) + uint32_t(dur)) {
                    s_pa_done |= 1u << idx;
                    std::fprintf(stderr, "[poke] a5-%d = %04x done\n", off,
                                 uint16_t(val));
                }
            }
            spec = std::strchr(spec, ',');
            if (spec) spec++;
        }
    }

    // POP2_DUMP_STATE=frame[,frame...][:size] — dump the level-state blob
    // (*(a5-20500)) to /tmp/pop2_state_fN.bin at each listed frame.
    static const char* st_env = std::getenv("POP2_DUMP_STATE");
    if (st_env) {
        static const std::vector<int> st_frames = [] {
            std::vector<int> v;
            for (const char* p = st_env; *p && *p != ':';) {
                v.push_back(std::atoi(*p == 'f' ? p + 1 : p));
                while (*p && *p != ',' && *p != ':') p++;
                if (*p == ',') p++;
            }
            return v;
        }();
        static const int st_size = [] {
            const char* c = std::strchr(st_env, ':');
            return c ? std::atoi(c + 1) : 16384;
        }();
        if (std::find(st_frames.begin(), st_frames.end(), s_frame) !=
            st_frames.end()) {
            uint32_t st = mem_read32(0x080000u - 20500);
            char path[64];
            std::snprintf(path, sizeof path, "/tmp/pop2_state_f%d.bin", s_frame);
            std::FILE* f = st ? std::fopen(path, "wb") : nullptr;
            if (f) {
                for (int i = 0; i < st_size; i++) {
                    uint8_t b = mem_read8(st + uint32_t(i));
                    std::fwrite(&b, 1, 1, f);
                }
                std::fclose(f);
            }
            std::fprintf(stderr, "[state] blob=%06X dumped %d bytes -> %s\n",
                         st, st_size, f ? path : "(failed)");
            // companion dump: the full A5 globals window (a5-22266..a5)
            std::snprintf(path, sizeof path, "/tmp/pop2_a5_f%d.bin", s_frame);
            if (std::FILE* g = std::fopen(path, "wb")) {
                for (int i = -22266; i < 0; i++) {
                    uint8_t b = mem_read8(uint32_t(0x080000 + i));
                    std::fwrite(&b, 1, 1, g);
                }
                std::fclose(g);
            }
        }
    }
}

}  // namespace

void video_set_color(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < 0 || index > 255) return;
    s_pal[index] = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

const uint32_t* video_palette() { return s_pal; }

uint8_t video_match_color(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = int((s_pal[i] >> 16) & 0xFF) - r;
        int dg = int((s_pal[i] >> 8) & 0xFF) - g;
        int db = int(s_pal[i] & 0xFF) - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return uint8_t(best);
}

void video_pump() {
    if (g_interrupt_depth > 0) return;   // interrupt-time guest code must
                                         // not block on vsync or recurse
    if (s_state == State::Uninit)
        s_state = init_video() ? State::On : State::Off;
    if (s_state == State::Off) return;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            std::fprintf(stderr, "[video] window closed — exiting\n");
            std::exit(0);
        case SDL_CONTROLLERDEVICEADDED:
            if (!s_pad) {
                s_pad = SDL_GameControllerOpen(e.cdevice.which);
                if (s_pad)
                    std::fprintf(stderr, "[pad] connected: %s\n",
                                 SDL_GameControllerName(s_pad));
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (s_pad && e.cdevice.which == SDL_JoystickInstanceID(
                             SDL_GameControllerGetJoystick(s_pad))) {
                SDL_GameControllerClose(s_pad);
                s_pad = nullptr;
                std::fprintf(stderr, "[pad] disconnected\n");
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                // emulator hotkeys (not forwarded to the game)
                if (e.key.keysym.scancode == SDL_SCANCODE_F8) {
                    s_linear = !s_linear;
                    make_texture();
                    std::fprintf(stderr, "[video] filter: %s\n",
                                 s_linear ? "linear" : "nearest");
                    break;
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F9) {
                    s_integer = !s_integer;
                    SDL_RenderSetIntegerScale(s_ren,
                                              s_integer ? SDL_TRUE : SDL_FALSE);
                    std::fprintf(stderr, "[video] integer scale: %s\n",
                                 s_integer ? "on" : "off");
                    break;
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F11) {
                    s_fullscreen = !s_fullscreen;
                    SDL_SetWindowFullscreen(
                        s_win, s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                }
            }
            int vk = mac_vkey(e.key.keysym.scancode);
            if (vk < 0) break;
            if (e.type == SDL_KEYDOWN)
                s_keymap[vk >> 3] |= uint8_t(1u << (vk & 7));
            else
                s_keymap[vk >> 3] &= uint8_t(~(1u << (vk & 7)));
            if (e.key.repeat) {
                push_event(5, uint32_t(vk << 8) | mac_char(e.key.keysym));
            } else {
                push_event(e.type == SDL_KEYDOWN ? 3 : 4,
                           uint32_t(vk << 8) | mac_char(e.key.keysym));
            }
            break;
        }
        case SDL_MOUSEMOTION: {
            int w = 0, h = 0;
            SDL_GetWindowSize(s_win, &w, &h);
            s_mouse_h = int16_t(e.motion.x * kWidth / (w ? w : kWidth));
            s_mouse_v = int16_t(e.motion.y * kHeight / (h ? h : kHeight));
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                s_button = e.type == SDL_MOUSEBUTTONDOWN;
                push_event(s_button ? 1 : 2, 0);
            }
            break;
        default:
            break;
        }
    }

    pad_update();

    // POP2_AUTOCLICK=ms — synthesize a mouse click at screen center then.
    static const char* autoclick = std::getenv("POP2_AUTOCLICK");
    if (autoclick) {
        static int s_cphase = 0;
        uint32_t want = uint32_t(std::atoi(autoclick));
        uint32_t t = SDL_GetTicks();
        if (s_cphase == 0 && t >= want) {
            s_cphase = 1;
            s_button = true;
            push_event(1, 0);
            std::fprintf(stderr, "[video] autoclick down\n");
        } else if (s_cphase == 1 && t >= want + 250) {
            s_cphase = 2;
            s_button = false;
            push_event(2, 0);
        }
    }

    // POP2_TELEPORT=ms:room:col:row[:dir] — at time ms, move the live kid
    // block (a5-20690) to the given room/col/row and point the view room
    // global (a5-20420) there. Optional dir pins facing (0=right, -1=left).
    // Dev shortcut to reach distant rooms for testing.
    static const char* teleport = std::getenv("POP2_TELEPORT");
    // shared with the autokey "T<n>+ms" trigger below
    static int s_tp_idx = 0;            // current spec in the list
    static int s_tp_state = 0;          // 0=armed 1=poking 2=done
    if (teleport) {
        static uint32_t s_tp_t0 = 0;
        static int s_tp_room = 0;
        const char* spec = teleport;
        for (int i = 0; i < s_tp_idx && spec; i++) {
            spec = std::strchr(spec, ',');
            if (spec) spec++;
        }
        uint32_t t = SDL_GetTicks();
        static int s_tp_col = 0, s_tp_row = 0, s_tp_dir = 999;
        // K:room:col:row[:dir] fires once the kid stands STABLY (frame 15,
        // room published, ~0.75 s in a row) — immune to tick-start jitter
        // and to the inter-step frame-15 flickers of entry auto-walks
        static int s_tp_stand = 0;      // consecutive standing polls
        static bool s_tp_moved = false; // saw a non-15 frame since spec armed
        bool tp_due = false;
        if (spec && *spec && s_tp_state == 0) {
            if (spec[0] == 'K') {
                int kroom = int16_t(mem_read16(0x080000u - 20418));
                uint16_t kf = mem_read16(0x080000u - 20556);
                bool standing = kroom != 0 && kf == 15 &&
                                int16_t(mem_read16(0x080000u - 20544)) == kroom;
                // load replays freeze the globals at frame 15 for seconds;
                // demand an animation first so we fire on a REAL stand
                if (kf != 15 && kf != 0) s_tp_moved = true;
                s_tp_stand = standing ? s_tp_stand + 1 : 0;
                tp_due = s_tp_moved && s_tp_stand >= 45;
            } else {
                tp_due = t >= uint32_t(std::atoi(spec));
            }
        }
        if (spec && *spec && s_tp_state == 0 && tp_due) {
            int room = 0, col = 0, row = 0, dir = 999;
            int got = std::sscanf(spec[0] == 'K' ? spec + 1 : spec,
                                  spec[0] == 'K' ? ":%d:%d:%d:%d"
                                                 : "%*d:%d:%d:%d:%d",
                                  &room, &col, &row, &dir);
            if (got >= 3) {
                s_tp_room = room;
                s_tp_col = col;
                s_tp_row = row;
                s_tp_dir = (got == 4) ? dir : 999;
                s_tp_t0 = t;
                s_tp_state = 1;
                s_tp_stand = 0;     // next K-spec needs a fresh stable stand
                s_tp_moved = false; // ...preceded by fresh animation
                std::fprintf(stderr, "[video] teleport kid -> room%d (%d,%d)\n",
                             room, col, row);
            } else {
                s_tp_idx++;
            }
        }
        // The kid tick copies block->scratch->block and treats -20420 as a
        // one-tick crossing request, so a single write loses the race; keep
        // asserting block + request until the engine settles on them.
        static int s_tp_stable = 0;     // frames the engine kept our values
        static bool s_tp_skip = false;  // alternate write/verify frames
        if (s_tp_state == 1) {
            uint32_t kb = 0x080000u - 20690;
            uint16_t px = uint16_t(238 + s_tp_col * 51 + 25);
            uint16_t py = uint16_t(106 + s_tp_row * 120);
            bool held = mem_read16(kb + 4) == px && mem_read16(kb + 6) == py &&
                        mem_read16(kb + 22) == uint16_t(s_tp_room);
            bool cam_ok =
                mem_read16(0x080000u - 20418) == uint16_t(s_tp_room);
            // the kid's own tick re-publishing the room is the only proof
            // the transfer really took (camera alone can change without him)
            bool kid_ok =
                mem_read16(0x080000u - 20544) == uint16_t(s_tp_room);
            if (s_tp_skip) {
                // verify frame: did the engine keep our block?
                s_tp_stable = held ? s_tp_stable + 1 : 0;
                s_tp_skip = false;
            } else {
                mem_write16(kb + 4, px);
                mem_write16(kb + 6, py);
                mem_write16(kb + 12, uint16_t(s_tp_col));
                mem_write16(kb + 14, uint16_t(s_tp_row));
                mem_write16(kb + 22, uint16_t(s_tp_room));
                if (s_tp_dir != 999)
                    mem_write16(kb + 2, uint16_t(int16_t(s_tp_dir)));
                if (!cam_ok)
                    mem_write16(0x080000u - 20420, uint16_t(s_tp_room));
                // on multi-character levels the kid lives in a slot and
                // the opp block is rebuilt from it — move the slot too
                // (type field +32 == 10 marks the prince)
                for (int k = 0; k < 5; k++) {
                    uint32_t sb = 0x080000u - 20996 + uint32_t(k) * 62;
                    if (mem_read16(sb + 32) != 10) continue;
                    mem_write16(sb + 0, px);
                    mem_write16(sb + 2, py);
                    mem_write16(sb + 18, uint16_t(s_tp_room));
                    break;
                }
                s_tp_skip = true;
            }
            if ((s_tp_stable >= 2 && cam_ok && kid_ok) ||
                t > s_tp_t0 + 4000) {
                std::fprintf(stderr, "[video] teleport settled (stable=%d)\n",
                             s_tp_stable);
                s_tp_state = 0;
                s_tp_stable = 0;
                s_tp_skip = false;
                s_tp_idx++;
            }
        }
    }

    // POP2_AUTOKEY=ms[:vkey[:ch][:cmd]][:hMS][,more...] — synthesized key
    // presses: hex vkey (default 0x24 Return), hex char (default CR), "cmd"
    // holds the command modifier, "h<ms>" holds the key down that long
    // (default 500 ms). Comma-separated events play in order.
    static const char* autokey = std::getenv("POP2_AUTOKEY");
    if (autokey) {
        static int s_idx = 0;          // current event in the list
        static int s_phase = 0;        // 0=waiting 1=down 2=advance
        static bool s_cond_met = false;
        static uint32_t s_cond_t0 = 0;
        static uint32_t s_prev_done = 0;
        // "D" events are attempt separators: when the kid lies dead
        // (frame 185/206), fast-forward past the next D, release all
        // keys and start the following attempt fresh — no input leak
        // plays over the corpse/respawn. A D reached alive is a no-op.
        static bool s_dead_ff = false;
        uint16_t kfr = mem_read16(0x080000u - 20556);
        if (kfr == 185 || kfr == 206) {
            if (!s_dead_ff) {
                const char* sp = autokey;
                int j = 0;
                for (; j < s_idx && sp; j++) {
                    sp = std::strchr(sp, ',');
                    if (sp) sp++;
                }
                while (sp && *sp && *sp != 'D') {
                    sp = std::strchr(sp, ',');
                    if (sp) { sp++; j++; }
                }
                if (sp && *sp == 'D') j++;      // step past the separator
                s_idx = j;
                s_phase = 0;
                s_cond_met = false;
                s_prev_done = SDL_GetTicks();
                std::memset(s_keymap, 0, sizeof s_keymap);
                s_fake_shift = false;
                s_fake_cmd = false;
                s_dead_ff = true;
                std::fprintf(stderr, "[video] autokey death-skip -> event %d\n",
                             j);
            }
            // the corpse waits for a keypress to respawn — nudge with
            // Space about once a second until the kid is back
            static uint32_t s_dead_tap = 0;
            uint32_t tnow = SDL_GetTicks();
            if (tnow - s_dead_tap > 1000) {
                s_dead_tap = tnow;
                push_event(3, uint32_t(0x31 << 8) | 0x20u);
                push_event(4, uint32_t(0x31 << 8) | 0x20u);
            }
        } else if (kfr == 15) {
            s_dead_ff = false;
        }
        const char* spec = autokey;
        for (int i = 0; i < s_idx && spec; i++) {
            spec = std::strchr(spec, ',');
            if (spec) spec++;
        }
        if (spec && *spec) {
            if (spec[0] == 'D') {       // separator reached alive: no-op
                s_idx++;
                s_cond_met = false;
                s_prev_done = SDL_GetTicks();
                return;
            }
            // Trigger: plain ms, or R<room>.<col>.<row>[+delayms] — fires
            // <delay> ms after the kid block first matches (col/row may be
            // '*' = any). Conditional triggers keep the list strictly serial.
            uint32_t want = 0;
            bool conditional = (spec[0] == 'R');
            bool telcond = (spec[0] == 'T');
            bool relative = (spec[0] == '+');
            uint32_t delay = 0;
            if (telcond) {
                // T<n>+ms — fires <ms> after the n-th teleport spec settled
                int wn = std::atoi(spec + 1);
                const char* plus = std::strchr(spec, '+');
                const char* colon = std::strchr(spec, ':');
                if (plus && (!colon || plus < colon))
                    delay = uint32_t(std::atoi(plus + 1));
                if (!s_cond_met && s_tp_idx >= wn && s_tp_state == 0) {
                    s_cond_met = true;
                    s_cond_t0 = SDL_GetTicks();
                    std::fprintf(stderr, "[video] autokey cond T%d met"
                                 " (+%u ms)\n", wn, delay);
                }
            } else if (conditional) {
                const char* p = spec + 1;
                int wr = std::atoi(p);
                p = std::strchr(p, '.');
                int wc = (p && p[1] == '*') ? -1 : (p ? std::atoi(p + 1) : -1);
                p = p ? std::strchr(p + 1, '.') : nullptr;
                int ww = (p && p[1] == '*') ? -1 : (p ? std::atoi(p + 1) : -1);
                const char* plus = std::strchr(spec, '+');
                const char* colon = std::strchr(spec, ':');
                if (plus && (!colon || plus < colon)) delay = uint32_t(std::atoi(plus + 1));
                if (!s_cond_met) {
                    uint32_t kb = 0x080000u - 20690;
                    // room from the settled camera global, not the kid block:
                    // the teleport hammer writes the block itself, so reading
                    // it back here would fire the condition during the hammer
                    int kroom = int16_t(mem_read16(0x080000u - 20418));
                    int kcol = int16_t(mem_read16(kb + 12));
                    int krow = int16_t(mem_read16(kb + 14));
                    if (kroom == wr && (wc < 0 || kcol == wc) &&
                        (ww < 0 || krow == ww)) {
                        s_cond_met = true;
                        s_cond_t0 = SDL_GetTicks();
                        std::fprintf(stderr, "[video] autokey cond R%d.%d.%d"
                                     " met (+%u ms)\n", wr, wc, ww, delay);
                    }
                }
            } else if (relative) {
                delay = uint32_t(std::atoi(spec + 1));
            } else {
                want = uint32_t(std::atoi(spec));
            }
            int vk = 0x24, ch = 13;
            uint32_t hold = 500;
            bool cmd = false, shift = false, downonly = false, uponly = false;
            const char* end = std::strchr(spec, ',');
            size_t len = end ? size_t(end - spec) : std::strlen(spec);
            for (const char* c = std::strchr(spec, ':');
                 c && c < spec + len; c = std::strchr(c + 1, ':')) {
                if (std::strncmp(c, ":cmd", 4) == 0) cmd = true;
                else if (std::strncmp(c, ":shift", 6) == 0) shift = true;
                else if (std::strncmp(c, ":dn", 3) == 0) downonly = true;
                else if (std::strncmp(c, ":up", 3) == 0) uponly = true;
                else if (c[1] == 'h') hold = uint32_t(std::atoi(c + 2));
                else if (vk == 0x24 && ch == 13 && c == std::strchr(spec, ':'))
                    vk = int(std::strtol(c + 1, nullptr, 16));
                else ch = int(std::strtol(c + 1, nullptr, 16));
            }
            uint32_t t = SDL_GetTicks();
            static uint32_t s_down_t = 0;
            bool due = (conditional || telcond)
                                   ? (s_cond_met && t >= s_cond_t0 + delay)
                     : relative    ? (t >= s_prev_done + delay)
                                   : (t >= want);
            if (s_phase == 0 && due && uponly) {
                s_keymap[vk >> 3] &= uint8_t(~(1u << (vk & 7)));
                push_event(4, uint32_t(vk << 8) | uint32_t(ch));
                s_fake_cmd = false;
                if (vk == 0x38 || shift) s_fake_shift = false;
                if (shift) s_keymap[0x38 >> 3] &= uint8_t(~(1u << (0x38 & 7)));
                std::fprintf(stderr, "[video] autokey up-only vk=%02X\n", vk);
                s_idx++;
                s_cond_met = false;
                s_prev_done = t;
            } else if (s_phase == 0 && due) {
                s_phase = 1;
                s_down_t = t;
                s_fake_cmd = cmd;
                if (vk == 0x38 || shift) s_fake_shift = true;
                if (shift) s_keymap[0x38 >> 3] |= uint8_t(1u << (0x38 & 7));
                if (vk == 0xFE) {              // synthetic mouse button
                    s_button = true;
                    push_event(1, 0);
                } else {
                    s_keymap[vk >> 3] |= uint8_t(1u << (vk & 7));
                    push_event(3, uint32_t(vk << 8) | uint32_t(ch));
                }
                std::fprintf(stderr, "[video] autokey down vk=%02X ch=%02X%s"
                             " hold=%u%s\n", vk, ch, cmd ? " +cmd" : "", hold,
                             downonly ? " (dn-only)" : "");
                if (downonly) {
                    s_idx++;
                    s_phase = 0;
                    s_cond_met = false;
                    s_prev_done = t;
                }
            } else if (s_phase == 1 && t >= s_down_t + hold) {
                if (vk == 0xFE) {
                    s_button = false;
                    push_event(2, 0);
                } else {
                    s_keymap[vk >> 3] &= uint8_t(~(1u << (vk & 7)));
                    push_event(4, uint32_t(vk << 8) | uint32_t(ch));
                }
                s_fake_cmd = false;
                if (vk == 0x38 || shift) s_fake_shift = false;
                if (shift) s_keymap[0x38 >> 3] &= uint8_t(~(1u << (0x38 & 7)));
                s_idx++;
                s_phase = 0;
                s_cond_met = false;
                s_prev_done = t;
            }
        }
    }

    uint32_t now = SDL_GetTicks();
    if (now - s_last_present >= 16) {
        s_last_present = now;
        present();
#ifdef __EMSCRIPTEN__
        // Asyncify yield (~once per displayed frame): hand control back to the
        // browser so the canvas composites and timers/events run. video_pump()
        // is the universal pump — every event poll (WaitNextEvent/GetNextEvent)
        // and the Ticks busy-wait pacing loop reach it — so the tab can never
        // lock up regardless of which guest loop is spinning. The recompiled
        // 150k-line main loop is never restructured; it just unwinds here.
        emscripten_sleep(1);
#endif
    }
}

void video_get_keys(uint8_t out[16]) {
    video_pump();
    std::memcpy(out, s_keymap, 16);
}

bool video_next_event(uint16_t mask, MacEvent* out, bool remove) {
    video_pump();
    for (auto it = s_events.begin(); it != s_events.end(); ++it) {
        if (mask & (1u << it->what)) {
            *out = *it;
            if (remove) s_events.erase(it);
            return true;
        }
    }
    // Null event: a real Mac still fills the record, and `modifiers` carries
    // the LIVE modifier state — PoP2 polls held Shift this way (hang/grab
    // while falling have no key events to read).
    out->what = 0;
    out->message = 0;
    out->when = 0;
    out->where_h = s_mouse_h;
    out->where_v = s_mouse_v;
    out->modifiers = mac_modifiers();
    return false;
}

bool video_button() {
    video_pump();
    return s_button;
}

void video_get_mouse(int16_t* h, int16_t* v) {
    video_pump();
    *h = s_mouse_h;
    *v = s_mouse_v;
}

// ---- audio sink: SDL queued audio fed by the synth's double buffers ----
namespace {
SDL_AudioDeviceID s_adev = 0;
int s_arate = 0, s_abits = 0, s_achans = 0;
bool s_audio_dead = false;
}  // namespace

bool audio_queue(const uint8_t* data, uint32_t bytes, int rate, int bits,
                 int channels) {
    if (s_audio_dead || std::getenv("POP2_NO_AUDIO")) return false;
    if (s_adev && (rate != s_arate || bits != s_abits ||
                   channels != s_achans)) {
        SDL_CloseAudioDevice(s_adev);
        s_adev = 0;
    }
    if (!s_adev) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // may init before video
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            s_audio_dead = true;
            return false;
        }
        SDL_AudioSpec want{};
        want.freq = rate;
        want.format = bits == 16 ? AUDIO_S16MSB : AUDIO_U8;
        want.channels = uint8_t(channels);
        want.samples = 512;
        s_adev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (!s_adev) {
            std::fprintf(stderr, "[audio] open failed: %s\n", SDL_GetError());
            s_audio_dead = true;
            return false;
        }
        s_arate = rate; s_abits = bits; s_achans = channels;
        SDL_PauseAudioDevice(s_adev, 0);
        std::fprintf(stderr, "[audio] device open: %d Hz, %d-bit, %d ch\n",
                     rate, bits, channels);
    }
    SDL_QueueAudio(s_adev, data, bytes);
    static FILE* s_dump = [] {
        const char* p = std::getenv("POP2_DUMP_AUDIO");
        return p ? std::fopen(p, "wb") : nullptr;
    }();
    if (s_dump) std::fwrite(data, 1, bytes, s_dump);
    static int s_signal_logs = 0;
    if (s_signal_logs < 2 && bytes) {
        int peak = 0;
        for (uint32_t i = 0; i < bytes; i++) {
            int dev = int(data[i]) - 0x80;
            peak = std::max(peak, dev < 0 ? -dev : dev);
        }
        if (peak > 8) {
            s_signal_logs++;
            std::fprintf(stderr, "[audio] signal flowing, peak=%d\n", peak);
        }
    }
    return true;
}

uint32_t audio_queued_bytes() {
    return s_adev ? SDL_GetQueuedAudioSize(s_adev) : 0;
}

}  // namespace pop2
