// Resource Manager over the extracted/ dump tree.
//
// Each "open resource file" is a directory produced by dump_resources.py:
//   <dir>/<TYPE>/<id>[_<name>].bin
// Search order matches classic Mac: current file first, then earlier-opened
// files, app file last.
#include "pop2/mac.h"

#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace fs = std::filesystem;

namespace pop2 {

namespace {

struct ResKey {
    uint32_t type;
    int16_t id;
    bool operator<(const ResKey& o) const {
        return type != o.type ? type < o.type : id < o.id;
    }
};

struct LoadedRes {
    uint32_t handle;
    int16_t refnum;
    uint32_t type;
    int16_t id;
    std::string name;
};

struct ResFile {
    int16_t refnum;
    fs::path dir;
    bool open = true;
};

std::vector<ResFile> s_files;          // index 0 = app file
int16_t s_current = 2;
int16_t s_next_refnum = 3;
int16_t s_res_error = 0;
fs::path s_data_root;                  // extracted/data
// deque: stable element addresses (s_by_handle keeps pointers into it)
std::map<ResKey, std::deque<LoadedRes>> s_loaded;    // may exist per refnum
std::map<uint32_t, LoadedRes*> s_by_handle;

std::string sanitize_type(uint32_t type) {
    std::string out;
    for (int i = 3; i >= 0; i--) {
        uint8_t b = uint8_t(type >> (i * 8));
        char c = char(b);
        if (isalnum(uint8_t(c)) || c == '-' || c == '.') out.push_back(c);
        else if (c == ' ') out.push_back('_');
        else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", b);
            out += buf;
        }
    }
    return out;
}

ResFile* file_by_refnum(int16_t refnum) {
    for (auto& f : s_files)
        if (f.refnum == refnum && f.open) return &f;
    return nullptr;
}

// search chain: current file, then files opened before it, app last
std::vector<ResFile*> search_chain(bool one_deep) {
    std::vector<ResFile*> chain;
    ResFile* cur = file_by_refnum(s_current);
    if (cur) chain.push_back(cur);
    if (!one_deep) {
        for (auto it = s_files.rbegin(); it != s_files.rend(); ++it)
            if (it->open && &*it != cur) chain.push_back(&*it);
    }
    return chain;
}

std::optional<fs::path> find_res_file(const ResFile& f, uint32_t type, int16_t id) {
    fs::path tdir = f.dir / sanitize_type(type);
    if (!fs::exists(tdir)) return std::nullopt;
    std::string exact = std::to_string(id) + ".bin";
    std::string prefix = std::to_string(id) + "_";
    for (auto& e : fs::directory_iterator(tdir)) {
        std::string n = e.path().filename().string();
        if (n == exact || (n.rfind(prefix, 0) == 0 && n.size() > 4 &&
                           n.compare(n.size() - 4, 4, ".bin") == 0))
            return e.path();
    }
    return std::nullopt;
}

uint32_t load_file_as_handle(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    auto size = size_t(f.tellg());
    f.seekg(0);
    uint32_t h = mm_new_handle(uint32_t(size), false);
    if (!h) return 0;
    f.read(reinterpret_cast<char*>(g_mem + mem_read32(h)), std::streamsize(size));
    return h;
}

std::string res_name_from_path(const fs::path& p, int16_t id) {
    std::string stem = p.stem().string();
    std::string prefix = std::to_string(id) + "_";
    if (stem.rfind(prefix, 0) == 0) return stem.substr(prefix.size());
    return "";
}

}  // namespace

void rm_init(const std::string& app_dir) {
    fs::path app(app_dir);
    s_files.push_back({2, app});
    s_data_root = app.parent_path() / "data";
    s_current = 2;
}

void rm_create_res_file(const std::string& name) {
    std::string stem = name;
    auto colon = stem.rfind(':');
    if (colon != std::string::npos) stem = stem.substr(colon + 1);
    auto dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    if (stem.empty()) { s_res_error = paramErr; return; }
    std::error_code ec;
    fs::create_directories(s_data_root / stem, ec);
    std::fprintf(stderr, "[rm] CreateResFile('%s') -> %s\n", name.c_str(),
                 (s_data_root / stem).string().c_str());
    s_res_error = ec ? ioErr : 0;
}

// first user save dir (one that contains a DSLV resource and is not the
// shipped Prince2 data file)
std::string rm_first_saved_game() {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(s_data_root, ec)) {
        if (!e.is_directory()) continue;
        std::string stem = e.path().filename().string();
        if (stem == "Prince2") continue;
        if (fs::exists(e.path() / "DSLV")) return stem;
    }
    if (fs::exists(s_data_root / "Prince2" / "DSLV")) return "Prince2";
    return "";
}

int16_t rm_open_res_file(const std::string& name) {
    // map ":Data:Prince2.opt" / "Birdhead.rsrc" -> extracted/data/<stem>/
    std::string stem = name;
    auto colon = stem.rfind(':');
    if (colon != std::string::npos) stem = stem.substr(colon + 1);
    auto dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    fs::path dir = s_data_root / stem;
    if (!fs::exists(dir)) {
        std::fprintf(stderr, "[rm] OpenResFile('%s'): no dump dir %s\n",
                     name.c_str(), dir.string().c_str());
        s_res_error = resFNotFound;
        return -1;
    }
    // already open?
    for (auto& f : s_files)
        if (f.open && f.dir == dir) { s_current = f.refnum; return f.refnum; }
    int16_t refnum = s_next_refnum++;
    s_files.push_back({refnum, dir});
    s_current = refnum;
    s_res_error = 0;
    std::fprintf(stderr, "[rm] OpenResFile('%s') -> refnum %d\n", name.c_str(), refnum);
    return refnum;
}

void rm_close_res_file(int16_t refnum) {
    if (ResFile* f = file_by_refnum(refnum)) {
        f->open = false;
        // release this file's resources
        for (auto& [key, vec] : s_loaded)
            for (auto& lr : vec)
                if (lr.refnum == refnum && lr.handle) {
                    s_by_handle.erase(lr.handle);
                    mm_dispose_handle(lr.handle);
                    lr.handle = 0;
                }
        if (s_current == refnum) s_current = 2;
    }
}

void rm_use_res_file(int16_t refnum) {
    if (file_by_refnum(refnum)) s_current = refnum;
    else s_res_error = resFNotFound;
}

int16_t rm_cur_res_file() { return s_current; }

uint32_t rm_get_resource(uint32_t type, int16_t id, bool one_deep) {
    if (std::getenv("POP2_TRACE_RES2")) {
        static int s_n = 0;
        if (s_n < 4000 && ++s_n)
            std::fprintf(stderr, "[rm] get %s %d\n",
                         type_to_str(type).c_str(), id);
    }
    ResKey key{type, id};
    auto& vec = s_loaded[key];
    for (ResFile* f : search_chain(one_deep)) {
        for (auto& lr : vec)
            if (lr.refnum == f->refnum && lr.handle) {
                // the game may DisposeHandle resources behind our back; a
                // stale cache hit would hand out reused memory — reload
                if (mm_get_handle_size(lr.handle) < 0 ||
                    !(mm_hget_state(lr.handle) & 0x20)) {
                    s_by_handle.erase(lr.handle);
                    lr.handle = 0;
                    continue;
                }
                s_res_error = 0;
                return lr.handle;
            }
        if (auto p = find_res_file(*f, type, id)) {
            uint32_t h = load_file_as_handle(*p);
            if (!h) { s_res_error = memFullErr; return 0; }
            mm_hset_state(h, mm_hget_state(h) | 0x20);  // resource bit
            vec.push_back({h, f->refnum, type, id, res_name_from_path(*p, id)});
            s_by_handle[h] = &vec.back();
            s_res_error = 0;
            if (type == 0x53484150 && std::getenv("POP2_TRACE_RES")) {
                uint32_t d = mem_read32(h);
                std::fprintf(stderr, "[rm] SHAP %d <- %s h=%06X p=%06X "
                             "head=%02X%02X%02X%02X %02X%02X%02X%02X\n",
                             id, p->string().c_str(), h, d,
                             mem_read8(d), mem_read8(d + 1), mem_read8(d + 2),
                             mem_read8(d + 3), mem_read8(d + 4),
                             mem_read8(d + 5), mem_read8(d + 6),
                             mem_read8(d + 7));
            }
            return h;
        }
    }
    s_res_error = resNotFound;
    return 0;
}

uint32_t rm_get_named_resource(uint32_t type, const std::string& name) {
    for (ResFile* f : search_chain(false)) {
        fs::path tdir = f->dir / sanitize_type(type);
        if (!fs::exists(tdir)) continue;
        for (auto& e : fs::directory_iterator(tdir)) {
            std::string stem = e.path().stem().string();
            auto us = stem.find('_');
            if (us == std::string::npos) continue;
            if (stem.substr(us + 1) == name) {
                int16_t id = int16_t(std::stoi(stem.substr(0, us)));
                return rm_get_resource(type, id, false);
            }
        }
    }
    s_res_error = resNotFound;
    return 0;
}

// persist a resource's current handle contents into its dump file
static bool write_res_file(const fs::path& dir, uint32_t type, int16_t id,
                           const std::string& name, uint32_t h) {
    int32_t size = mm_get_handle_size(h);
    if (size < 0) return false;
    fs::path tdir = dir / sanitize_type(type);
    std::error_code ec;
    fs::create_directories(tdir, ec);
    fs::path p = tdir / (name.empty()
                             ? std::to_string(id) + ".bin"
                             : std::to_string(id) + "_" + name + ".bin");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(g_mem + mem_read32(h)), size);
    std::fprintf(stderr, "[rm] wrote %s %d -> %s (%d bytes)\n",
                 type_to_str(type).c_str(), id, p.string().c_str(), size);
    return true;
}

void rm_add_resource(uint32_t h, uint32_t type, int16_t id,
                     const std::string& name) {
    ResFile* f = file_by_refnum(s_current);
    if (!f || !h) { s_res_error = resNotFound; return; }
    // the dump tree may hold an older copy under a name suffix: drop it
    if (auto p = find_res_file(*f, type, id)) fs::remove(*p);
    if (!write_res_file(f->dir, type, id, name, h)) {
        s_res_error = ioErr;
        return;
    }
    ResKey key{type, id};
    auto& vec = s_loaded[key];
    mm_hset_state(h, mm_hget_state(h) | 0x20);
    vec.push_back({h, f->refnum, type, id, name});
    s_by_handle[h] = &vec.back();
    s_res_error = 0;
}

void rm_changed_resource(uint32_t h) {
    auto it = s_by_handle.find(h);
    if (it == s_by_handle.end()) { s_res_error = resNotFound; return; }
    LoadedRes* lr = it->second;
    ResFile* f = file_by_refnum(lr->refnum);
    if (!f) { s_res_error = resNotFound; return; }
    if (auto p = find_res_file(*f, lr->type, lr->id)) fs::remove(*p);
    s_res_error = write_res_file(f->dir, lr->type, lr->id, lr->name,
                                 lr->handle)
                      ? 0
                      : ioErr;
}

void rm_remove_resource(uint32_t h) {
    auto it = s_by_handle.find(h);
    if (it == s_by_handle.end()) { s_res_error = resNotFound; return; }
    LoadedRes* lr = it->second;
    if (ResFile* f = file_by_refnum(lr->refnum))
        if (auto p = find_res_file(*f, lr->type, lr->id)) fs::remove(*p);
    lr->handle = 0;   // detached: the game still owns the handle
    s_by_handle.erase(it);
    s_res_error = 0;
}

void rm_release_resource(uint32_t h) {
    auto it = s_by_handle.find(h);
    if (it == s_by_handle.end()) { s_res_error = resNotFound; return; }
    it->second->handle = 0;
    s_by_handle.erase(it);
    mm_dispose_handle(h);
    s_res_error = 0;
}

void rm_detach_resource(uint32_t h) {
    auto it = s_by_handle.find(h);
    if (it == s_by_handle.end()) { s_res_error = resNotFound; return; }
    it->second->handle = 0;   // forget without disposing
    s_by_handle.erase(it);
    s_res_error = 0;
}

int16_t rm_home_res_file(uint32_t h) {
    auto it = s_by_handle.find(h);
    return it == s_by_handle.end() ? (s_res_error = resNotFound, int16_t(-1))
                                   : it->second->refnum;
}

int32_t rm_size_rsrc(uint32_t h) { return mm_get_handle_size(h); }

int16_t rm_res_error() { return s_res_error; }
void rm_set_res_error(int16_t err) { s_res_error = err; }

int16_t rm_count_resources(uint32_t type, bool one_deep) {
    int16_t n = 0;
    for (ResFile* f : search_chain(one_deep)) {
        fs::path tdir = f->dir / sanitize_type(type);
        if (!fs::exists(tdir)) continue;
        for ([[maybe_unused]] auto& e : fs::directory_iterator(tdir)) n++;
    }
    return n;
}

uint32_t rm_get_ind_resource(uint32_t type, int16_t index, bool one_deep) {
    // 1-based index across the search chain, per-file sorted by id
    if (index < 1) { s_res_error = resNotFound; return 0; }
    for (ResFile* f : search_chain(one_deep)) {
        fs::path tdir = f->dir / sanitize_type(type);
        if (!fs::exists(tdir)) continue;
        std::vector<int> ids;
        for (auto& e : fs::directory_iterator(tdir)) {
            std::string stem = e.path().stem().string();
            ids.push_back(std::stoi(stem));   // up to first non-digit
        }
        std::sort(ids.begin(), ids.end());
        if (index <= int16_t(ids.size())) {
            int16_t save = s_current;
            s_current = f->refnum;
            uint32_t h = rm_get_resource(type, int16_t(ids[index - 1]), true);
            s_current = save;
            return h;
        }
        index -= int16_t(ids.size());
    }
    s_res_error = resNotFound;
    return 0;
}

void rm_get_res_info(uint32_t h, int16_t* id, uint32_t* type, std::string* name) {
    auto it = s_by_handle.find(h);
    if (it == s_by_handle.end()) { s_res_error = resNotFound; return; }
    if (id) *id = it->second->id;
    if (type) *type = it->second->type;
    if (name) *name = it->second->name;
}

std::string type_to_str(uint32_t type) {
    std::string s;
    for (int i = 3; i >= 0; i--) {
        char c = char(type >> (i * 8));
        s.push_back(isprint(uint8_t(c)) ? c : '?');
    }
    return s;
}

}  // namespace pop2
