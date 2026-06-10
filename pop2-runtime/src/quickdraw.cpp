// Software QuickDraw: rasterizes text and rect primitives straight into the
// port's pixels (window framebuffer or GWorld PixMap). The game has no FONT
// resources — system fonts lived in the System file — so text uses a built-in
// 8x16 console font (font8x16.inc, see tools/psf2inc.py).
//
// Both GrafPort and CGrafPort share the field offsets we touch: pnLoc @48,
// txMode @72, fgColor @80, bkColor @84. fgColor/bkColor hold a resolved
// palette index (set by Fore/BackColor traps), not a classic color code.
#include "pop2/mac.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace pop2 {

#include "font8x16.inc"

static const bool s_trace = std::getenv("POP2_TRACE_TEXT") != nullptr;

static const int kAscent = 12;
static const int kDescent = 4;
static const int kAdvance = 8;

int qd_char_width() { return kAdvance; }
int qd_ascent() { return kAscent; }
int qd_descent() { return kDescent; }

namespace {

struct Rect16 {
    int16_t t, l, b, r;
    bool empty() const { return b <= t || r <= l; }
};

Rect16 read_rect(uint32_t p) {
    return {int16_t(mem_read16(p)), int16_t(mem_read16(p + 2)),
            int16_t(mem_read16(p + 4)), int16_t(mem_read16(p + 6))};
}

Rect16 sect(Rect16 a, Rect16 b) {
    return {std::max(a.t, b.t), std::max(a.l, b.l),
            std::min(a.b, b.b), std::min(a.r, b.r)};
}

// Pixel target of a port: pixels, rowBytes, bounds (local->pixel offset),
// plus the port's effective clip in local coordinates.
struct Target {
    uint32_t base = 0;
    uint32_t row = 0;
    Rect16 bounds{};    // portBits/PixMap bounds
    Rect16 clip{};      // bounds ∩ portRect ∩ clipRgn ∩ visRgn (local coords)
    bool ok = false;
};

Target resolve(uint32_t port) {
    Target t;
    if (!port) return t;
    uint16_t ver = mem_read16(port + 6);
    if ((ver & 0xC000) == 0xC000) {            // CGrafPort: PixMap handle @+2
        uint32_t pm = mem_read32(mem_read32(port + 2));
        t.base = mem_read32(pm);
        t.row = mem_read16(pm + 4) & 0x3FFF;
        t.bounds = read_rect(pm + 6);
    } else {                                    // GrafPort: BitMap @+2
        t.base = mem_read32(port + 2);
        t.row = ver;                            // rowBytes (no flag bits)
        t.bounds = read_rect(port + 8);
    }
    if (!t.base || !t.row) return t;
    t.clip = sect(t.bounds, read_rect(port + 16));      // portRect
    for (uint32_t rgn_off : {28u, 24u}) {               // clipRgn, visRgn
        uint32_t rgn = mem_read32(port + rgn_off);
        uint32_t rp = rgn ? mem_read32(rgn) : 0;
        if (rp) t.clip = sect(t.clip, read_rect(rp + 2));
    }
    t.ok = true;
    return t;
}

uint8_t* pixel_ptr(const Target& t, int16_t h, int16_t v) {
    return g_mem + t.base + uint32_t(v - t.bounds.t) * t.row +
           uint32_t(h - t.bounds.l);
}

uint8_t fg_pixel(uint32_t port) { return uint8_t(mem_read32(port + 80)); }
uint8_t bk_pixel(uint32_t port) { return uint8_t(mem_read32(port + 84)); }

void fill(const Target& t, Rect16 r, uint8_t pix) {
    r = sect(r, t.clip);
    if (r.empty()) return;
    for (int16_t v = r.t; v < r.b; v++)
        std::memset(pixel_ptr(t, r.l, v), pix, size_t(r.r - r.l));
}

void invert(const Target& t, Rect16 r) {
    r = sect(r, t.clip);
    if (r.empty()) return;
    for (int16_t v = r.t; v < r.b; v++) {
        uint8_t* p = pixel_ptr(t, r.l, v);
        for (int16_t h = r.l; h < r.r; h++, p++) *p = uint8_t(~*p);
    }
}

// Draw one glyph with its top-left at (h, v). txMode srcCopy (0) also paints
// the background bits; everything else behaves as srcOr (transparent bg).
void glyph(const Target& t, int16_t h, int16_t v, uint8_t ch,
           uint8_t fg, uint8_t bk, bool copy_mode) {
    // The game's own fonts (NFNT 23331/24878) draw the typographic open
    // quote in the '#' slot and the close quote in '"'; story TEXT data
    // encodes quotes that way. Our ASCII font has no curly quotes.
    if (ch == '#') ch = '"';
    const unsigned char* rows =
        (ch >= 32 && ch < 127) ? kFont[ch - 32] : kFont[0];
    for (int y = 0; y < kFontHeight; y++) {
        int16_t pv = int16_t(v + y);
        if (pv < t.clip.t || pv >= t.clip.b) continue;
        uint8_t bits = rows[y];
        for (int x = 0; x < 8; x++) {
            int16_t ph = int16_t(h + x);
            if (ph < t.clip.l || ph >= t.clip.r) continue;
            bool on = bits & (0x80u >> x);
            if (on)
                *pixel_ptr(t, ph, pv) = fg;
            else if (copy_mode)
                *pixel_ptr(t, ph, pv) = bk;
        }
    }
}

}  // namespace

// Draw len bytes at the pen position (baseline), advance the pen.
void qd_draw_text(uint32_t port, uint32_t textp, uint32_t len) {
    Target t = resolve(port);
    if (!t.ok) return;
    int16_t pen_v = int16_t(mem_read16(port + 48));
    int16_t pen_h = int16_t(mem_read16(port + 50));
    uint8_t fg = fg_pixel(port), bk = bk_pixel(port);
    bool copy = mem_read16(port + 72) == 0;
    if (s_trace) {
        std::string s;
        for (uint32_t i = 0; i < len; i++) s.push_back(char(mem_read8(textp + i)));
        std::fprintf(stderr,
                     "[text] '%s' pen=(%d,%d) fg=%u bk=%u mode=%u size=%u "
                     "port=%06X base=%06X\n",
                     s.c_str(), pen_h, pen_v, fg, bk, mem_read16(port + 72),
                     mem_read16(port + 74), port, t.base);
    }
    for (uint32_t i = 0; i < len; i++) {
        glyph(t, pen_h, int16_t(pen_v - kAscent), mem_read8(textp + i),
              fg, bk, copy);
        pen_h = int16_t(pen_h + kAdvance);
    }
    mem_write16(port + 50, uint16_t(pen_h));
}

// TETextBox: erase the box, then draw lines split on CR, justified.
// just: 0 left, 1 center, -1 right.
void qd_text_box(uint32_t port, uint32_t textp, uint32_t len,
                 uint32_t rectp, int16_t just) {
    Target t = resolve(port);
    if (!t.ok) return;
    Rect16 box = read_rect(rectp);
    fill(t, box, bk_pixel(port));
    Target boxed = t;
    boxed.clip = sect(t.clip, box);
    uint8_t fg = fg_pixel(port), bk = bk_pixel(port);
    int16_t v = int16_t(box.t + kAscent);
    uint32_t line_start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i < len && mem_read8(textp + i) != 0x0D) continue;
        uint32_t n = i - line_start;
        int16_t w = int16_t(n * kAdvance);
        int16_t h = box.l;
        if (just == 1) h = int16_t(box.l + (box.r - box.l - w) / 2);
        else if (just < 0) h = int16_t(box.r - w);
        for (uint32_t k = 0; k < n; k++)
            glyph(boxed, int16_t(h + int16_t(k * kAdvance)),
                  int16_t(v - kAscent), mem_read8(textp + line_start + k),
                  fg, bk, false);
        v = int16_t(v + kFontHeight);
        line_start = i + 1;
    }
}

// verb: 0 frame, 1 paint (fg), 2 erase (bk), 3 invert, 4 fill with pattern
void qd_rect_op(uint32_t port, uint32_t rectp, int verb, uint32_t patp) {
    Target t = resolve(port);
    if (!t.ok) return;
    Rect16 r = read_rect(rectp);
    if (s_trace)
        std::fprintf(stderr,
                     "[rect] verb=%d (%d,%d,%d,%d) fg=%u bk=%u port=%06X\n",
                     verb, r.t, r.l, r.b, r.r, fg_pixel(port), bk_pixel(port),
                     port);
    switch (verb) {
    case 0: {  // frame: four 1px edges
        fill(t, {r.t, r.l, int16_t(r.t + 1), r.r}, fg_pixel(port));
        fill(t, {int16_t(r.b - 1), r.l, r.b, r.r}, fg_pixel(port));
        fill(t, {r.t, r.l, r.b, int16_t(r.l + 1)}, fg_pixel(port));
        fill(t, {r.t, int16_t(r.r - 1), r.b, r.r}, fg_pixel(port));
        return;
    }
    case 1: fill(t, r, fg_pixel(port)); return;
    case 2: fill(t, r, bk_pixel(port)); return;
    case 3: invert(t, r); return;
    case 4: {  // approximate the pattern: any ink -> fg, blank -> bk
        bool any = false;
        for (int i = 0; i < 8 && !any; i++) any = mem_read8(patp + i) != 0;
        fill(t, r, any ? fg_pixel(port) : bk_pixel(port));
        return;
    }
    }
}

// PaintRgn for regions that carry span data (rgnSize > 10, our format built
// by BitMapToRegion in traps.cpp: count.w at +10, then {v, h1, h2} triplets).
// Plain rectangular regions (rgnSize == 10) fill the bounding box.
void qd_paint_rgn(uint32_t port, uint32_t rgnp) {
    Target t = resolve(port);
    if (!t.ok) return;
    uint8_t fg = fg_pixel(port);
    if (mem_read16(rgnp) <= 10) { fill(t, read_rect(rgnp + 2), fg); return; }
    uint16_t n = mem_read16(rgnp + 10);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sp = rgnp + 12 + i * 6;
        int16_t v = int16_t(mem_read16(sp));
        fill(t, {v, int16_t(mem_read16(sp + 2)), int16_t(v + 1),
                 int16_t(mem_read16(sp + 4))}, fg);
    }
}

// ---- debug: dump offscreen GWorld buffers alongside POP2_DUMP_FB ----
namespace {
struct GWorldInfo { uint32_t base, row; int w, h; };
std::vector<GWorldInfo> s_gworlds;
}  // namespace

void qd_register_gworld(uint32_t base, uint32_t row, int w, int h) {
    s_gworlds.push_back({base, row, w, h});
}

void qd_unregister_gworld(uint32_t base) {
    for (auto it = s_gworlds.begin(); it != s_gworlds.end(); ++it)
        if (it->base == base) { s_gworlds.erase(it); return; }
}

void qd_dump_gworlds(const char* prefix) {
    // POP2_DUMP_RAW=1: identity grayscale instead of the live palette —
    // tells "art missing" apart from "palette faded to black"
    static const bool raw = std::getenv("POP2_DUMP_RAW") != nullptr;
    static uint32_t gray[256];
    if (raw && !gray[1])
        for (int i = 0; i < 256; i++)
            gray[i] = 0xFF000000u | uint32_t(i) << 16 | uint32_t(i) << 8 |
                      uint32_t(i);
    const uint32_t* pal = raw ? gray : video_palette();
    for (size_t n = 0; n < s_gworlds.size(); n++) {
        const GWorldInfo& g = s_gworlds[n];
        char path[256];
        std::snprintf(path, sizeof path, "%s_gw%zu_%06X.ppm", prefix, n,
                      g.base);
        FILE* f = std::fopen(path, "wb");
        if (!f) continue;
        std::fprintf(f, "P6\n%d %d\n255\n", g.w, g.h);
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                uint32_t c = pal[g_mem[g.base + uint32_t(y) * g.row +
                                       uint32_t(x)]];
                uint8_t rgb[3] = {uint8_t(c >> 16), uint8_t(c >> 8),
                                  uint8_t(c)};
                std::fwrite(rgb, 1, 3, f);
            }
        std::fclose(f);
        std::fprintf(stderr, "[qd] gworld dumped to %s\n", path);
    }
}

// 1px Bresenham line from the pen to (h,v); updates the pen.
void qd_line_to(uint32_t port, int16_t h, int16_t v) {
    Target t = resolve(port);
    int16_t y = int16_t(mem_read16(port + 48));
    int16_t x = int16_t(mem_read16(port + 50));
    mem_write16(port + 48, uint16_t(v));
    mem_write16(port + 50, uint16_t(h));
    if (!t.ok) return;
    uint8_t fg = fg_pixel(port);
    int dx = std::abs(h - x), dy = -std::abs(v - y);
    int sx = x < h ? 1 : -1, sy = y < v ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (x >= t.clip.l && x < t.clip.r && y >= t.clip.t && y < t.clip.b)
            *pixel_ptr(t, x, y) = fg;
        if (x == h && y == v) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x = int16_t(x + sx); }
        if (e2 <= dx) { err += dx; y = int16_t(y + sy); }
    }
}

}  // namespace pop2
