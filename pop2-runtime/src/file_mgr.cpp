// File Manager: Mac param-block + FSSpec file I/O over the host filesystem.
//
// Mac paths use ':' separators (":Data:DigiSnd.dat"); we resolve them
// case-insensitively under the game root directory. The FSSpec layer keeps a
// directory-ID registry: dirID 2 (fsRtDirID) = game root, subdirectories get
// stable ids on demand; the volume is named "PoP2" and any absolute path's
// volume component is accepted as an alias for the root. Data forks come from
// the files themselves, resource forks from unar's AppleDouble ".rsrc"
// siblings (0x52-byte header).
#include "pop2/mac.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Mirror the writable files in the game volume root (/data/pop2) to the
// IDBFS-backed /data/persist and flush to IndexedDB, so saved games survive a
// page reload. Called after a writable file is closed (i.e. a save was just
// written). pop2_persist_init() (main.cpp) restores them on the next boot.
EM_JS(void, pop2_persist_root, (), {
  // No-op where IndexedDB is unavailable (Node, private-browsing): /data/persist
  // was never mounted, so there is nothing to mirror to (see pop2_persist_init).
  var haveIDB = false;
  try { haveIDB = (typeof indexedDB !== 'undefined') && !!indexedDB; } catch (e) {}
  if (!haveIDB) return;
  // Debounced: a save burst (and the boot-time prefs writes) coalesce into one
  // mirror + syncfs ~0.8s after the last close, instead of one per close — that
  // repeated work (esp. copying the big read-only .rsrc fork) stalled startup.
  if (globalThis.__pop2syncTimer) clearTimeout(globalThis.__pop2syncTimer);
  globalThis.__pop2syncTimer = setTimeout(function () {
    try {
      var names = FS.readdir('/data/pop2');
      for (var i = 0; i < names.length; i++) {
        var f = names[i];
        if (f === '.' || f === '..' || /\.rsrc$/i.test(f)) continue;   // skip read-only resource forks
        var p = '/data/pop2/' + f;
        try { if (FS.isFile(FS.stat(p).mode)) FS.writeFile('/data/persist/' + f, FS.readFile(p)); } catch (e) {}
      }
    } catch (e) {}
    FS.syncfs(false, function () {});
  }, 800);
});
#endif

namespace fs = std::filesystem;

namespace pop2 {

namespace {

constexpr const char* kVolumeName = "PoP2";
constexpr int32_t kRootDirID = 2;       // fsRtDirID
constexpr int32_t kRootParID = 1;       // fsRtParID
constexpr uint32_t kAppleDoubleHdr = 0x52;

struct OpenFile {
    std::fstream f;
    int32_t base = 0;     // start of fork data within the host file
    int32_t pos = 0;
    int32_t size = 0;
    std::string name;     // leaf name (for PBGetFCBInfo)
    int32_t parID = 2;
    bool writable = false;
};

fs::path s_root;
std::map<int16_t, OpenFile> s_open;
int16_t s_next_ref = 100;

// directory-ID registry
std::map<int32_t, fs::path> s_dir_paths;
std::map<std::string, int32_t> s_dir_ids;   // canonical host path -> id
int32_t s_next_dir_id = 16;

// finder info overrides (FSpSetFInfo/FSpCreate), keyed by canonical host path
struct FInfo { uint32_t type, creator; };
std::map<std::string, FInfo> s_finfo;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string canon_key(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return lower((ec ? p : c).string());
}

// case-insensitive lookup of one child component in a host directory
std::optional<fs::path> ci_child(const fs::path& dir, const std::string& comp) {
    std::error_code ec;
    fs::path direct = dir / comp;
    if (fs::exists(direct, ec)) return direct;
    std::string want = lower(comp);
    for (auto& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == want) return e.path();
    return std::nullopt;
}

int32_t register_dir(const fs::path& p) {
    std::string key = canon_key(p);
    auto it = s_dir_ids.find(key);
    if (it != s_dir_ids.end()) return it->second;
    int32_t id = s_next_dir_id++;
    s_dir_ids[key] = id;
    s_dir_paths[id] = p;
    return id;
}

const fs::path* dir_path(int32_t dirID) {
    if (dirID == 0 || dirID == kRootDirID) {
        auto it = s_dir_paths.find(kRootDirID);
        return it == s_dir_paths.end() ? nullptr : &it->second;
    }
    auto it = s_dir_paths.find(dirID);
    return it == s_dir_paths.end() ? nullptr : &it->second;
}

std::vector<std::string> split_mac(const std::string& s) {
    std::vector<std::string> comps;
    std::string cur;
    for (char c : s) {
        if (c == ':') {
            if (!cur.empty()) comps.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) comps.push_back(cur);
    return comps;
}

int16_t open_host(const fs::path& host, bool resource_fork, int32_t parID) {
    fs::path target = host;
    int32_t base = 0;
    if (resource_fork) {
        fs::path rf = host;
        rf += ".rsrc";   // unar AppleDouble sibling
        if (!fs::exists(rf)) {
            std::fprintf(stderr, "[fs] OpenRF('%s'): no .rsrc sibling\n",
                         host.string().c_str());
            return fnfErr;
        }
        target = rf;
        base = kAppleDoubleHdr;
    }
    OpenFile of;
    of.f.open(target, std::ios::binary | std::ios::in | std::ios::out);
    of.writable = bool(of.f);
    if (!of.f) of.f.open(target, std::ios::binary | std::ios::in);
    if (!of.f) return fnfErr;
    of.f.seekg(0, std::ios::end);
    of.size = int32_t(of.f.tellg()) - base;
    if (of.size < 0) of.size = 0;
    of.base = base;
    of.name = host.filename().string();
    of.parID = parID;
    int16_t ref = s_next_ref++;
    s_open[ref] = std::move(of);
    std::fprintf(stderr, "[fs] Open('%s'%s) -> ref %d (%d bytes)\n",
                 host.filename().string().c_str(), resource_fork ? " rsrc" : "",
                 ref, s_open[ref].size);
    return ref;
}

}  // namespace

void fs_init(const std::string& game_root) {
    s_root = game_root;
    s_dir_paths[kRootDirID] = s_root;
    s_dir_ids[canon_key(s_root)] = kRootDirID;
}

// Resolve (vRefNum, dirID, mac_name) to a parent-dir id + leaf name per the
// IM:Files FSMakeFSSpec rules. We have a single volume, so vRefNum only
// matters as "default" (0) vs anything else — both map to the root unless
// dirID names a registered directory.
SpecInfo fs_make_spec(int16_t vRefNum, int32_t dirID, const std::string& mac_name) {
    (void)vRefNum;
    SpecInfo r;
    fs::path dir;
    std::vector<std::string> comps = split_mac(mac_name);
    bool absolute = !mac_name.empty() && mac_name[0] != ':' &&
                    mac_name.find(':') != std::string::npos;

    if (absolute || dirID == kRootParID) {
        // first component is a volume name; accept any as an alias of root
        dir = s_root;
        if (!comps.empty()) {
            if (comps.size() == 1) {
                // the spec names the volume (= root directory) itself
                r.err = noErr;
                r.parID = kRootParID;
                r.name = kVolumeName;
                r.host = s_root.string();
                r.exists = true;
                r.is_dir = true;
                r.dir_id = kRootDirID;
                return r;
            }
            comps.erase(comps.begin());
        }
    } else {
        const fs::path* dp = dir_path(dirID);
        if (!dp) { r.err = dirNFErr; return r; }
        dir = *dp;
    }

    if (comps.empty()) {
        // spec names the starting directory itself
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(dir, ec);
        if (ec) canon = dir;
        if (canon == fs::weakly_canonical(s_root, ec)) {
            r.parID = kRootParID;
            r.name = kVolumeName;
            r.dir_id = kRootDirID;
        } else {
            r.parID = register_dir(canon.parent_path());
            r.name = canon.filename().string();
            r.dir_id = register_dir(canon);
        }
        r.err = noErr;
        r.host = dir.string();
        r.exists = true;
        r.is_dir = true;
        return r;
    }

    for (size_t i = 0; i + 1 < comps.size(); i++) {
        auto next = ci_child(dir, comps[i]);
        if (!next || !fs::is_directory(*next)) {
            // tolerate doubled components ("PoP2:Data:Data:MIDISnd.dat"):
            // the game concatenates a configured prefix that already ends
            // with the directory it then names again
            if (lower(comps[i]) == lower(dir.filename().string())) continue;
            r.err = dirNFErr;
            return r;
        }
        dir = *next;
    }

    std::string leaf = comps.back();
    auto host = ci_child(dir, leaf);
    // historical convenience: bare names also match inside root's Data folder
    if (!host && canon_key(dir) == canon_key(s_root)) {
        if (auto data = ci_child(dir, "Data");
            data && fs::is_directory(*data)) {
            if (auto h2 = ci_child(*data, leaf)) { dir = *data; host = h2; }
        }
    }
    r.parID = register_dir(dir);
    if (host) {
        r.err = noErr;
        r.name = host->filename().string();
        r.host = host->string();
        r.exists = true;
        r.is_dir = fs::is_directory(*host);
        if (r.is_dir) r.dir_id = register_dir(*host);
    } else {
        r.err = fnfErr;   // spec is still valid for create
        r.name = leaf;
    }
    return r;
}

bool fs_dir_info(int32_t dirID, std::string* name, int32_t* parID) {
    if (dirID == kRootDirID || dirID == 0) {
        if (name) *name = kVolumeName;
        if (parID) *parID = kRootParID;
        return true;
    }
    auto it = s_dir_paths.find(dirID);
    if (it == s_dir_paths.end()) return false;
    if (name) *name = it->second.filename().string();
    if (parID) {
        fs::path parent = it->second.parent_path();
        *parID = canon_key(parent) == canon_key(s_root) ? kRootDirID
                                                        : register_dir(parent);
    }
    return true;
}

int16_t fs_open_at(int16_t vRefNum, int32_t dirID, const std::string& mac_name,
                   bool resource_fork) {
    SpecInfo s = fs_make_spec(vRefNum, dirID, mac_name);
    if (!s.exists || s.is_dir) {
        std::fprintf(stderr, "[fs] Open('%s' dir=%d): not found\n",
                     mac_name.c_str(), dirID);
        return s.err ? s.err : fnfErr;
    }
    return open_host(s.host, resource_fork, s.parID);
}

bool fs_fcb_info(int16_t refnum, std::string* name, int32_t* parID,
                 int32_t* eof, int32_t* pos) {
    auto it = s_open.find(refnum);
    if (it == s_open.end()) return false;
    if (name) *name = it->second.name;
    if (parID) *parID = it->second.parID;
    if (eof) *eof = it->second.size;
    if (pos) *pos = it->second.pos;
    return true;
}

int16_t fs_open(const std::string& mac_name, bool resource_fork) {
    return fs_open_at(0, 0, mac_name, resource_fork);
}

int16_t fs_create_file(int32_t parID, const std::string& name,
                       uint32_t creator, uint32_t type) {
    const fs::path* dp = dir_path(parID);
    if (!dp) return dirNFErr;
    if (ci_child(*dp, name)) return dupFNErr;
    std::ofstream f(*dp / name, std::ios::binary);
    if (!f) return ioErr;
    s_finfo[canon_key(*dp / name)] = {type, creator};
    std::fprintf(stderr, "[fs] Create('%s') in dir %d\n", name.c_str(), parID);
    return noErr;
}

int16_t fs_create_dir(int32_t parID, const std::string& name, int32_t* new_id) {
    const fs::path* dp = dir_path(parID);
    if (!dp) return dirNFErr;
    if (auto existing = ci_child(*dp, name)) {
        if (new_id && fs::is_directory(*existing))
            *new_id = register_dir(*existing);
        return dupFNErr;
    }
    std::error_code ec;
    if (!fs::create_directory(*dp / name, ec)) return ioErr;
    if (new_id) *new_id = register_dir(*dp / name);
    std::fprintf(stderr, "[fs] DirCreate('%s') in dir %d\n", name.c_str(), parID);
    return noErr;
}

int16_t fs_delete_spec(int32_t parID, const std::string& name) {
    const fs::path* dp = dir_path(parID);
    if (!dp) return dirNFErr;
    auto host = ci_child(*dp, name);
    if (!host) return fnfErr;
    if (fs::is_directory(*host)) return fBsyErr;   // only files
    std::error_code ec;
    fs::remove(*host, ec);
    std::fprintf(stderr, "[fs] Delete('%s') in dir %d\n", name.c_str(), parID);
    return ec ? ioErr : noErr;
}

void fs_set_finfo(const std::string& host, uint32_t type, uint32_t creator) {
    s_finfo[canon_key(host)] = {type, creator};
}

void fs_get_finfo(const std::string& host, uint32_t* type, uint32_t* creator) {
    auto it = s_finfo.find(canon_key(host));
    if (type) *type = it != s_finfo.end() ? it->second.type : 0x42494E41;     // 'BINA'
    if (creator) *creator = it != s_finfo.end() ? it->second.creator : 0x50524E63;  // 'PRNc'
}

int16_t fs_close(int16_t refnum) {
    auto it = s_open.find(refnum);
    if (it == s_open.end()) return fnOpnErr;
#ifdef __EMSCRIPTEN__
    const bool was_writable = it->second.writable;
#endif
    s_open.erase(it);   // closes the fstream — data is now flushed to MEMFS
#ifdef __EMSCRIPTEN__
    if (was_writable) ::pop2_persist_root();   // persist saved games to IndexedDB
#endif
    return noErr;
}

static int32_t seek_to(OpenFile& of, uint16_t mode, int32_t off) {
    switch (mode & 3) {
    case 1: of.pos = off; break;              // fsFromStart
    case 2: of.pos = of.size + off; break;    // fsFromLEOF
    case 3: of.pos = of.pos + off; break;     // fsFromMark
    default: break;                           // fsAtMark
    }
    if (of.pos < 0) { of.pos = 0; return posErr; }
    return noErr;
}

int32_t fs_read(int16_t refnum, uint32_t buf, uint32_t count,
                uint16_t pos_mode, int32_t pos_offset, uint32_t* act) {
    auto it = s_open.find(refnum);
    if (it == s_open.end()) return fnOpnErr;
    OpenFile& of = it->second;
    int32_t err = seek_to(of, pos_mode, pos_offset);
    if (err) return err;
    int32_t avail = of.size - of.pos;
    int32_t n = std::min<int32_t>(int32_t(count), std::max(avail, 0));
    if (n > 0) {
        of.f.clear();
        of.f.seekg(of.base + of.pos);
        of.f.read(reinterpret_cast<char*>(g_mem + (buf & ADDR_MASK)), n);
        of.pos += n;
    }
    if (act) *act = uint32_t(std::max(n, 0));
    return n == int32_t(count) ? noErr : eofErr;
}

int32_t fs_write(int16_t refnum, uint32_t buf, uint32_t count,
                 uint16_t pos_mode, int32_t pos_offset, uint32_t* act) {
    auto it = s_open.find(refnum);
    if (it == s_open.end()) {
        if (act) *act = count;   // pretend success (legacy prefs path)
        return noErr;
    }
    OpenFile& of = it->second;
    int32_t err = seek_to(of, pos_mode, pos_offset);
    if (err) return err;
    if (!of.writable) {
        if (act) *act = count;
        std::fprintf(stderr, "[fs] Write ref %d: read-only, dropped %u bytes\n",
                     refnum, count);
        return noErr;
    }
    of.f.clear();
    of.f.seekp(of.base + of.pos);
    of.f.write(reinterpret_cast<const char*>(g_mem + (buf & ADDR_MASK)),
               count);
    of.f.flush();
    if (!of.f) return ioErr;
    of.pos += int32_t(count);
    if (of.pos > of.size) of.size = of.pos;
    if (act) *act = count;
    std::fprintf(stderr, "[fs] Write ref %d: %u bytes @%d ('%s')\n", refnum,
                 count, of.pos - int32_t(count), of.name.c_str());
    return noErr;
}

int32_t fs_get_eof(int16_t refnum) {
    auto it = s_open.find(refnum);
    return it == s_open.end() ? -1 : it->second.size;
}

// A saved game is a plain file in the game root starting with the 'POP2' magic
// that "Save Game..." writes (Mac saves carry no extension).
static bool is_pop2_save(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char magic[4] = {};
    f.read(magic, 4);
    return bool(f) && std::memcmp(magic, "POP2", 4) == 0;
}

// The saved level is a big-endian uint16 at offset 0x42. Returns 1..14, or 0
// when unreadable or out of range (e.g. the oversized special-scene saves).
static int read_save_level(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    f.seekg(0x42);
    unsigned char b[2] = {};
    f.read(reinterpret_cast<char*>(b), 2);
    if (!f) return 0;
    int lvl = (int(b[0]) << 8) | int(b[1]);
    return (lvl >= 1 && lvl <= 14) ? lvl : 0;
}

// First saved-game file in the game root, or "". POP2_OPEN_SAVE=<leaf name>
// picks that save when several exist.
std::string fs_first_pop2_save() {
    const char* want = std::getenv("POP2_OPEN_SAVE");
    std::error_code ec;
    std::string first;
    for (auto& e : fs::directory_iterator(s_root, ec)) {
        if (!e.is_regular_file(ec) || !is_pop2_save(e.path())) continue;
        std::string leaf = e.path().filename().string();
        if (want && leaf == want) return leaf;
        if (first.empty()) first = leaf;
    }
    if (want && *want && !first.empty())
        std::fprintf(stderr, "[fs] POP2_OPEN_SAVE='%s' not found, using '%s'\n",
                     want, first.c_str());
    return first;
}

// ---- web save-manager ----
// A one-shot filename the Standard File trap consumes: SFPutFile saves under it,
// SFGetFile opens it. Set by pop2_save_slot / pop2_load_slot just before they
// inject the game's Save / Open menu command; cleared on the first SF call.
static std::string s_save_override;
void fs_set_save_override(const std::string& name) { s_save_override = name; }
std::string fs_take_save_override() {
    std::string n;
    n.swap(s_save_override);
    return n;
}

#ifdef __EMSCRIPTEN__
// Slot list for the web save-manager: one "level\tname" line per saved game in
// the volume root (level 0 = unknown). Read from JS via ccall; the buffer is
// valid until the next call. EMSCRIPTEN_KEEPALIVE keeps it exported.
extern "C" EMSCRIPTEN_KEEPALIVE const char* pop2_list_saves() {
    static std::string buf;
    buf.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(s_root, ec)) {
        if (e.is_regular_file(ec) && is_pop2_save(e.path())) {
            buf += std::to_string(read_save_level(e.path()));
            buf += '\t';
            buf += e.path().filename().string();
            buf += '\n';
        }
    }
    return buf.c_str();
}
#endif

int32_t fs_set_fpos(int16_t refnum, uint16_t pos_mode, int32_t pos_offset) {
    auto it = s_open.find(refnum);
    if (it == s_open.end()) return fnOpnErr;
    return seek_to(it->second, pos_mode, pos_offset);
}

int32_t fs_get_fpos(int16_t refnum) {
    auto it = s_open.find(refnum);
    return it == s_open.end() ? 0 : it->second.pos;
}

bool fs_stat_host(const std::string& host, bool* is_dir,
                  int32_t* data_len, int32_t* rsrc_len) {
    fs::path p = host;
    if (host.empty() || !fs::exists(p)) return false;
    bool dir = fs::is_directory(p);
    if (is_dir) *is_dir = dir;
    if (data_len) *data_len = dir ? 0 : int32_t(fs::file_size(p));
    if (rsrc_len) {
        *rsrc_len = 0;
        if (!dir) {
            fs::path rf = p;
            rf += ".rsrc";   // unar AppleDouble sibling
            if (fs::exists(rf)) {
                auto sz = int32_t(fs::file_size(rf));
                *rsrc_len = sz > int32_t(kAppleDoubleHdr)
                                ? sz - int32_t(kAppleDoubleHdr) : 0;
            }
        }
    }
    return true;
}

bool fs_stat(const std::string& mac_name, bool* is_dir,
             int32_t* data_len, int32_t* rsrc_len) {
    SpecInfo s = fs_make_spec(0, 0, mac_name);
    if (!s.exists) return false;
    return fs_stat_host(s.host, is_dir, data_len, rsrc_len);
}

}  // namespace pop2
