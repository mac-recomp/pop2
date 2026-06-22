// Mac OS types, error codes, guest memory layout, Pascal-stack helpers.
#pragma once
#include "pop2/cpu.h"
#include <string>

namespace pop2 {

// ---- guest layout ----
inline constexpr uint32_t LOWMEM_TOP   = 0x003000;
inline constexpr uint32_t STACK_TOP    = 0x070000;
inline constexpr uint32_t A5_BASE      = 0x080000;
// Scratch EvQEl returned by PPostEvent: lives in the unused gap between the A5
// world and the first CODE segment (0x200000). The caller fills it in; we never
// read it back (the event itself is delivered through the runtime event queue).
inline constexpr uint32_t EVQEL_SCRATCH = 0x1F0000;
inline constexpr uint32_t MP_BASE      = 0x3F0000;  // master pointers
inline constexpr uint32_t MP_END       = 0x400000;
inline constexpr uint32_t HEAP_BASE    = 0x400000;
inline constexpr uint32_t HEAP_END     = 0xE00000;
// GetTrapAddress returns executable stub addresses in this range;
// call_virtual/jump_virtual decode the trap number back out and dispatch.
inline constexpr uint32_t TRAP_STUB_BASE = 0xE00000;
inline constexpr uint32_t TRAP_STUB_END  = 0xE10000;
inline uint32_t trap_stub_addr(uint16_t trap) {
    return TRAP_STUB_BASE | (uint32_t(trap & 0xFFF) << 4);
}
inline constexpr uint32_t FB_BASE      = 0xF00000;  // framebuffer
// Guest screen geometry — PoP2's native size (12" Macintosh RGB display).
// The framebuffer stride, the screen PixMap rowBytes, and every screen-bounds
// rect MUST equal these: drawing strides by the port's rowBytes while present()
// reads by SCREEN_W, so any mismatch shears the image. 8bpp => rowBytes==SCREEN_W.
inline constexpr int SCREEN_W = 512;
inline constexpr int SCREEN_H = 384;

// ---- error codes ----
inline constexpr int16_t noErr        = 0;
inline constexpr int16_t memFullErr   = -108;
inline constexpr int16_t nilHandleErr = -109;
inline constexpr int16_t memWZErr     = -111;
inline constexpr int16_t resNotFound  = -192;
inline constexpr int16_t resFNotFound = -193;
inline constexpr int16_t nsvErr       = -35;
inline constexpr int16_t ioErr        = -36;
inline constexpr int16_t fnOpnErr     = -38;
inline constexpr int16_t eofErr       = -39;
inline constexpr int16_t posErr       = -40;
inline constexpr int16_t fnfErr       = -43;
inline constexpr int16_t fBsyErr      = -47;
inline constexpr int16_t dupFNErr     = -48;
inline constexpr int16_t paramErr     = -50;
inline constexpr int16_t dirNFErr     = -120;

// ---- pascal-convention stack helpers (args pushed L->R, callee pops) ----
inline uint16_t arg_w(Cpu& cpu) { return pop16(cpu); }
inline uint32_t arg_l(Cpu& cpu) { return pop32(cpu); }
// after all args are popped the result slot is at SP
inline void ret_w(Cpu& cpu, uint16_t v) { mem_write16(cpu.a[7], v); }
inline void ret_l(Cpu& cpu, uint32_t v) { mem_write32(cpu.a[7], v); }

// read a Str255 / Pascal string from guest memory
inline std::string read_pstr(uint32_t addr) {
    uint8_t len = mem_read8(addr);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; i++) s.push_back(char(mem_read8(addr + 1 + i)));
    return s;
}
inline void write_pstr(uint32_t addr, const std::string& s) {
    uint8_t len = uint8_t(s.size() > 255 ? 255 : s.size());
    mem_write8(addr, len);
    for (int i = 0; i < len; i++) mem_write8(addr + 1 + i, uint8_t(s[i]));
}

// ---- memory manager ----
void mm_init();
uint32_t mm_new_handle(uint32_t size, bool clear);   // 0 on failure
uint32_t mm_new_handle_at(uint32_t addr, uint32_t size);  // pinned data block
void mm_dispose_handle(uint32_t h);
int32_t mm_get_handle_size(uint32_t h);              // <0 = error
bool mm_set_handle_size(uint32_t h, uint32_t size);
uint32_t mm_new_ptr(uint32_t size, bool clear);
void mm_dispose_ptr(uint32_t p);
int32_t mm_get_ptr_size(uint32_t p);
uint32_t mm_recover_handle(uint32_t dataPtr);
uint32_t mm_free_mem();
uint8_t mm_hget_state(uint32_t h);
void mm_hset_state(uint32_t h, uint8_t st);

// ---- resource manager ----
void rm_init(const std::string& app_dir);            // extracted/app
int16_t rm_open_res_file(const std::string& name);   // returns refnum or <0
void rm_create_res_file(const std::string& name);
std::string rm_first_saved_game();
void rm_close_res_file(int16_t refnum);
void rm_use_res_file(int16_t refnum);
int16_t rm_cur_res_file();
uint32_t rm_get_resource(uint32_t type, int16_t id, bool one_deep);
uint32_t rm_get_named_resource(uint32_t type, const std::string& name);
void rm_add_resource(uint32_t h, uint32_t type, int16_t id,
                     const std::string& name);
void rm_changed_resource(uint32_t h);
void rm_remove_resource(uint32_t h);
void rm_release_resource(uint32_t h);
void rm_detach_resource(uint32_t h);
int16_t rm_home_res_file(uint32_t h);
int32_t rm_size_rsrc(uint32_t h);
int16_t rm_res_error();
void rm_set_res_error(int16_t err);
int16_t rm_count_resources(uint32_t type, bool one_deep);
uint32_t rm_get_ind_resource(uint32_t type, int16_t index, bool one_deep);
void rm_get_res_info(uint32_t h, int16_t* id, uint32_t* type, std::string* name);

std::string type_to_str(uint32_t type);

// ---- video/input (SDL2; lazily initialized, POP2_NO_VIDEO=1 disables) ----
struct MacEvent {
    uint16_t what = 0;        // 1 mDown 2 mUp 3 kDown 4 kUp 5 autoKey 6 update
    uint32_t message = 0;
    uint32_t when = 0;
    int16_t where_v = 0, where_h = 0;
    uint16_t modifiers = 0;
};
void video_pump();                       // poll input + present ~60Hz
void video_set_color(int index, uint8_t r, uint8_t g, uint8_t b);
uint8_t video_match_color(uint8_t r, uint8_t g, uint8_t b);  // nearest index
void video_get_keys(uint8_t out[16]);
bool video_next_event(uint16_t mask, MacEvent* out, bool remove = true);
bool video_button();
void video_get_mouse(int16_t* h, int16_t* v);
void video_post_event(uint16_t what, uint32_t message);  // PostEvent/PPostEvent sink

extern int g_interrupt_depth;   // nonzero inside guest interrupt callbacks

// ---- audio sink (video.cpp; SDL queue) ----
// Returns false when no audio device is available (headless).
bool audio_queue(const uint8_t* data, uint32_t bytes, int rate, int bits,
                 int channels);
uint32_t audio_queued_bytes();
// doubleback pump telemetry (traps.cpp): total fires / frames rendered
extern uint64_t g_dbl_fires, g_dbl_frames;

// ---- software QuickDraw (quickdraw.cpp) ----
// Ports keep a resolved palette index in fgColor/bkColor (+80/+84).
void qd_draw_text(uint32_t port, uint32_t textp, uint32_t len);
void qd_text_box(uint32_t port, uint32_t textp, uint32_t len,
                 uint32_t rectp, int16_t just);
void qd_rect_op(uint32_t port, uint32_t rectp, int verb, uint32_t patp = 0);
void qd_paint_rgn(uint32_t port, uint32_t rgnp);
void qd_line_to(uint32_t port, int16_t h, int16_t v);
int qd_char_width();
int qd_ascent();
int qd_descent();
// debug: GWorld registry so frame dumps can include offscreen buffers
void qd_register_gworld(uint32_t base, uint32_t row, int w, int h);
void qd_unregister_gworld(uint32_t base);
void qd_dump_gworlds(const char* prefix);
const uint32_t* video_palette();         // 256 ARGB entries

// ---- file manager (host filesystem behind Mac File Manager param blocks) ----
void fs_init(const std::string& game_root);
int16_t fs_open(const std::string& mac_name, bool resource_fork);  // refnum or <0
int16_t fs_open_at(int16_t vRefNum, int32_t dirID, const std::string& mac_name,
                   bool resource_fork);
int16_t fs_close(int16_t refnum);
int32_t fs_read(int16_t refnum, uint32_t buf, uint32_t count,
                uint16_t pos_mode, int32_t pos_offset, uint32_t* act);
int32_t fs_write(int16_t refnum, uint32_t buf, uint32_t count,
                 uint16_t pos_mode, int32_t pos_offset, uint32_t* act);
int32_t fs_get_eof(int16_t refnum);
std::string fs_first_pop2_save();
// Web save-manager: a one-shot filename the Standard File trap consumes
// (SFPutFile = name to save under, SFGetFile = file to open), set just before
// the Save / Open menu command is injected.
void fs_set_save_override(const std::string& name);
std::string fs_take_save_override();
int32_t fs_set_fpos(int16_t refnum, uint16_t pos_mode, int32_t pos_offset);
int32_t fs_get_fpos(int16_t refnum);
bool fs_fcb_info(int16_t refnum, std::string* name, int32_t* parID,
                 int32_t* eof, int32_t* pos);
// true if found; fills is_dir + data/resource fork sizes
bool fs_stat(const std::string& mac_name, bool* is_dir,
             int32_t* data_len, int32_t* rsrc_len);
bool fs_stat_host(const std::string& host, bool* is_dir,
                  int32_t* data_len, int32_t* rsrc_len);

// ---- FSSpec layer: directory-ID registry over the host tree ----
// dirID 2 = game root (volume "PoP2", parID 1); other dirs get ids on demand.
struct SpecInfo {
    int16_t err = noErr;     // noErr, or fnfErr (spec valid but absent), or
                             // nsvErr/dirNFErr (path invalid)
    int32_t parID = 0;       // parent directory id (valid unless err is fatal)
    std::string name;        // leaf name, host case when it exists
    std::string host;        // host path when exists
    bool exists = false;
    bool is_dir = false;
    int32_t dir_id = 0;      // when is_dir: the directory's own id
};
SpecInfo fs_make_spec(int16_t vRefNum, int32_t dirID, const std::string& mac_name);
bool fs_dir_info(int32_t dirID, std::string* name, int32_t* parID);
int16_t fs_create_file(int32_t parID, const std::string& name,
                       uint32_t creator, uint32_t type);
int16_t fs_create_dir(int32_t parID, const std::string& name, int32_t* new_id);
int16_t fs_delete_spec(int32_t parID, const std::string& name);
void fs_set_finfo(const std::string& host, uint32_t type, uint32_t creator);
void fs_get_finfo(const std::string& host, uint32_t* type, uint32_t* creator);

}  // namespace pop2
