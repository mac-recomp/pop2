// Memory Manager: handles + pointers in the guest heap.
//
// Blocks never move (no compaction — 16 MB of guest space vs ~2 MB of data),
// so HLock/HUnlock/MoveHHi are state-only. Master pointers live in guest
// memory (MP_BASE..MP_END) because the game dereferences handles directly;
// freed master pointers are reused. The heap is a first-fit free list with
// coalescing — the attract loop alone reloads shape sets every few seconds,
// so a bump allocator runs the 10 MB heap dry in minutes.
#include "pop2/mac.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

namespace pop2 {

namespace {

struct Block {
    uint32_t size = 0;
    uint8_t state = 0;   // bit7 locked, bit6 purgeable, bit5 resource
    bool pinned = false; // data outside the heap (recompiled blob segment)
};

uint32_t s_next_mp = MP_BASE;
std::vector<uint32_t> s_free_mps;
std::unordered_map<uint32_t, Block> s_handles;  // handle (MP addr) -> block
std::unordered_map<uint32_t, uint32_t> s_ptrs;  // ptr -> alloc size
std::unordered_map<uint32_t, uint32_t> s_by_data;  // data addr -> handle
std::map<uint32_t, uint32_t> s_free;            // addr -> span size (merged)

uint32_t aligned_size(uint32_t size) { return (size + 3) & ~3u; }

void free_span(uint32_t addr, uint32_t size) {
    if (!size) return;
    auto next = s_free.lower_bound(addr);
    // double-free detector: the span must not overlap existing free spans
    if (next != s_free.end() && addr + size > next->first)
        std::fprintf(stderr, "[mm] DOUBLE FREE %06X+%u overlaps free %06X+%u\n",
                     addr, size, next->first, next->second);
    if (next != s_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second > addr)
            std::fprintf(stderr,
                         "[mm] DOUBLE FREE %06X+%u inside free %06X+%u\n",
                         addr, size, prev->first, prev->second);
    }
    if (next != s_free.begin()) {                // merge with predecessor
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr) {
            addr = prev->first;
            size += prev->second;
            s_free.erase(prev);
        }
    }
    if (next != s_free.end() && addr + size == next->first) {  // and successor
        size += next->second;
        s_free.erase(next);
    }
    s_free[addr] = size;
}

uint32_t heap_alloc(uint32_t size) {
    uint32_t need = aligned_size(size ? size : 4);
    for (auto it = s_free.begin(); it != s_free.end(); ++it) {  // first fit
        if (it->second < need) continue;
        uint32_t addr = it->first;
        uint32_t left = it->second - need;
        s_free.erase(it);
        if (left) s_free[addr + need] = left;
        return addr;
    }
    uint32_t total = 0, largest = 0;
    for (auto& [a, sz] : s_free) { total += sz; largest = std::max(largest, sz); }
    std::fprintf(stderr, "[mm] ALLOC FAIL size=%u (free total=%u largest=%u)\n",
                 size, total, largest);
    return 0;
}

}  // namespace

void mm_init() {
    s_free.clear();
    s_free[HEAP_BASE] = HEAP_END - HEAP_BASE;
}

uint32_t mm_new_handle(uint32_t size, bool clear) {
    uint32_t data = heap_alloc(size);
    if (!data) return 0;
    uint32_t h;
    if (!s_free_mps.empty()) {
        h = s_free_mps.back();
        s_free_mps.pop_back();
    } else {
        if (s_next_mp + 4 > MP_END) fatal("out of master pointers");
        h = s_next_mp;
        s_next_mp += 4;
    }
    mem_write32(h, data);
    s_handles[h] = {size, 0};
    s_by_data[data] = h;
    if (clear) std::memset(g_mem + data, 0, size);
    return h;
}

// Handle whose data lives at a fixed address outside the heap — used to make
// the game unpack the MDRV driver blob right onto its recompiled image base,
// so guest code/data addresses match the native seg-25 functions.
uint32_t mm_new_handle_at(uint32_t addr, uint32_t size) {
    uint32_t h;
    if (!s_free_mps.empty()) {
        h = s_free_mps.back();
        s_free_mps.pop_back();
    } else {
        if (s_next_mp + 4 > MP_END) fatal("out of master pointers");
        h = s_next_mp;
        s_next_mp += 4;
    }
    mem_write32(h, addr);
    s_handles[h] = {size, 0, true};
    s_by_data[addr] = h;
    return h;
}

void mm_dispose_handle(uint32_t h) {
    auto it = s_handles.find(h);
    if (it == s_handles.end()) return;
    uint32_t data = mem_read32(h);
    s_by_data.erase(data);
    if (!it->second.pinned)
        free_span(data, aligned_size(it->second.size ? it->second.size : 4));
    mem_write32(h, 0);
    s_handles.erase(it);
    s_free_mps.push_back(h);
}

int32_t mm_get_handle_size(uint32_t h) {
    auto it = s_handles.find(h);
    return it == s_handles.end() ? nilHandleErr : int32_t(it->second.size);
}

bool mm_set_handle_size(uint32_t h, uint32_t size) {
    auto it = s_handles.find(h);
    if (it == s_handles.end()) return false;
    if (it->second.pinned) {                    // fixed image: shrink only
        if (size > it->second.size) return false;
        it->second.size = size;
        return true;
    }
    uint32_t old_data = mem_read32(h);
    uint32_t old_size = it->second.size;
    uint32_t old_alloc = aligned_size(old_size ? old_size : 4);
    uint32_t new_alloc = aligned_size(size ? size : 4);
    if (new_alloc <= old_alloc) {               // shrink in place, free tail
        if (new_alloc < old_alloc)
            free_span(old_data + new_alloc, old_alloc - new_alloc);
        it->second.size = size;
        return true;
    }
    // try to grow into an adjacent free span
    auto nb = s_free.find(old_data + old_alloc);
    if (nb != s_free.end() && old_alloc + nb->second >= new_alloc) {
        uint32_t take = new_alloc - old_alloc;
        uint32_t addr = nb->first, span = nb->second;
        s_free.erase(nb);
        if (span > take) s_free[addr + take] = span - take;
        it->second.size = size;
        return true;
    }
    uint32_t data = heap_alloc(size);
    if (!data) return false;
    std::memcpy(g_mem + data, g_mem + old_data, old_size);
    s_by_data.erase(old_data);
    free_span(old_data, old_alloc);
    s_by_data[data] = h;
    mem_write32(h, data);
    it->second.size = size;
    return true;
}

uint32_t mm_new_ptr(uint32_t size, bool clear) {
    uint32_t p = heap_alloc(size);
    if (!p) return 0;
    s_ptrs[p] = size;
    if (clear) std::memset(g_mem + p, 0, size);
    return p;
}

void mm_dispose_ptr(uint32_t p) {
    auto it = s_ptrs.find(p);
    if (it == s_ptrs.end()) return;
    free_span(p, aligned_size(it->second ? it->second : 4));
    s_ptrs.erase(it);
}

int32_t mm_get_ptr_size(uint32_t p) {
    auto it = s_ptrs.find(p);
    return it == s_ptrs.end() ? memWZErr : int32_t(it->second);
}

uint32_t mm_recover_handle(uint32_t dataPtr) {
    auto it = s_by_data.find(dataPtr);
    return it == s_by_data.end() ? 0 : it->second;
}

uint32_t mm_free_mem() {
    uint32_t total = 0;
    for (const auto& [addr, size] : s_free) total += size;
    return total;
}

uint8_t mm_hget_state(uint32_t h) {
    auto it = s_handles.find(h);
    return it == s_handles.end() ? 0 : it->second.state;
}

void mm_hset_state(uint32_t h, uint8_t st) {
    auto it = s_handles.find(h);
    if (it != s_handles.end()) it->second.state = st;
}

}  // namespace pop2
