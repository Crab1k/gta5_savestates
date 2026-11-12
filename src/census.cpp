// Lab diagnostic: what does the game's memory point at outside itself? Scans every saved byte (data sections and the
// written pages of the game's regions) for 64-bit values that point into this process and sorts them by target:
// the game's own memory, module images, thread stacks, and other memory (objects that other DLLs allocated for the
// game, recognised by the vtable they start with). This is the list of things a savestate from another game
// session has to rebuild or remap.
#include "census.h"
#include "common.h"
#include "memory.h"
#include "threads.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace census {

struct Span {
    uintptr_t lo, hi;
    int kind;              // 0 game region, 1 GTA5.exe, 2 other image, 3 game thread stack, 4 other private, 5 mapped
    uintptr_t allocBase;
    std::string name;
};

static bool ReadQword(uintptr_t p, uintptr_t* out) {
    __try {
        *out = *(const uintptr_t*)p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string ModuleName(uintptr_t p) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* s = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    return s;
}

void Run(const char* why) {
    const ULONGLONG t0 = GetTickCount64();
    std::vector<Span> spans;
    // Process memory map.
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0x10000; VirtualQuery((void*)p, &mbi, sizeof(mbi)); p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State == MEM_COMMIT) {
            Span s = {(uintptr_t)mbi.BaseAddress, (uintptr_t)mbi.BaseAddress + mbi.RegionSize, 4, (uintptr_t)mbi.AllocationBase, ""};
            if (mbi.Type == MEM_IMAGE) {
                s.kind = g_exe.Contains(s.lo) ? 1 : 2;
                s.name = ModuleName(s.lo);
            } else if (mbi.Type == MEM_MAPPED) {
                s.kind = 5;
            }
            spans.push_back(s);
        }
        if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < p) break;
    }
    auto regions = mem::Regions();
    auto inRegion = [&](uintptr_t v) {
        for (mem::Region* r : regions)
            if (v >= (uintptr_t)r->base && v < (uintptr_t)r->base + r->size) return true;
        return false;
    };
    std::vector<std::pair<uintptr_t, uintptr_t>> stacks;
    for (threads::Thread* t : threads::All()) {
        auto* dealloc = *(uint8_t**)((uint8_t*)t->teb + 0x1478);   // TEB.DeallocationStack
        stacks.push_back({(uintptr_t)dealloc, (uintptr_t)t->stackBase});
    }
    for (Span& s : spans) {
        if (s.kind == 4 || s.kind == 5) {
            if (inRegion(s.lo)) s.kind = 0;
            for (auto& st : stacks)
                if (s.lo >= st.first && s.lo < st.second) s.kind = 3;
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });
    auto find = [&](uintptr_t v) -> const Span* {
        auto it = std::upper_bound(spans.begin(), spans.end(), v, [](uintptr_t x, const Span& s) { return x < s.lo; });
        if (it == spans.begin()) return nullptr;
        --it;
        return v < it->hi ? &*it : nullptr;
    };

    uint64_t counts[6] = {}, scanned = 0;
    std::map<std::string, uint64_t> images;
    std::map<std::string, uint64_t> objects;     // "vtable owner" -> pointers to such objects
    std::map<uintptr_t, uint64_t> privateBases;  // other private allocations by base
    std::map<uintptr_t, int> distinct;
    auto scan = [&](const uint8_t* p, size_t n) {
        auto* q = (const uintptr_t*)p;
        for (size_t i = 0; i < n / 8; i++) {
            uintptr_t v = q[i];
            if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;
            const Span* s = find(v);
            if (!s) continue;
            counts[s->kind]++;
            if (s->kind == 2) images[s->name]++;
            else if (s->kind == 4 || s->kind == 5) {
                privateBases[s->allocBase]++;
                if (distinct.size() < 2000000) distinct[v]++;
            }
        }
        scanned += n;
    };
    for (const mem::Section& s : mem::DataSections()) scan(s.p, s.size);
    for (mem::Region* r : regions)
        for (size_t i = 0; i < r->pages; i++) {
            if (!r->Written(i)) continue;
            uintptr_t probe;
            if (!ReadQword((uintptr_t)r->base + i * 0x1000, &probe)) continue;
            scan(r->base + i * 0x1000, 0x1000);
        }
    // Objects: what the pointed-at memory starts with.
    for (auto& [v, n] : distinct) {
        uintptr_t first = 0;
        std::string cls = "(no vtable)";
        if (ReadQword(v, &first)) {
            const Span* s = find(first);
            if (s && (s->kind == 1 || s->kind == 2)) {
                char b[160];
                sprintf_s(b, "%s vtable %s", s->kind == 1 ? "GTA5.exe" : s->name.c_str(), Where(first).c_str());
                cls = b;
            }
        }
        objects[cls] += n;
    }

    Log("census (%s): %.0f MB scanned in %u ms; pointers into: game memory %llu, GTA5.exe %llu, other modules %llu, "
        "game stacks %llu, other private memory %llu (%u distinct targets), mapped %llu",
        why, scanned / 1048576.0, (unsigned)(GetTickCount64() - t0), counts[0], counts[1], counts[2], counts[3], counts[4],
        (unsigned)distinct.size(), counts[5]);
    std::vector<std::pair<uint64_t, std::string>> sorted;
    for (auto& [k, n] : images) sorted.push_back({n, k});
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i = 0; i < sorted.size() && i < 25; i++) Log("  module %-28s %llu", sorted[i].second.c_str(), sorted[i].first);
    sorted.clear();
    for (auto& [k, n] : objects) sorted.push_back({n, k});
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i = 0; i < sorted.size() && i < 60; i++) Log("  object %-60s %llu", sorted[i].second.c_str(), sorted[i].first);
    std::vector<std::pair<uint64_t, uintptr_t>> bases;
    for (auto& [k, n] : privateBases) bases.push_back({n, k});
    std::sort(bases.rbegin(), bases.rend());
    for (size_t i = 0; i < bases.size() && i < 25; i++) {
        MEMORY_BASIC_INFORMATION a;
        size_t size = 0;
        for (uintptr_t q = bases[i].second; VirtualQuery((void*)q, &a, sizeof(a)) && (uintptr_t)a.AllocationBase == bases[i].second; q += a.RegionSize)
            size += a.RegionSize;
        Log("  private allocation %p (%llu KB) %llu pointers", (void*)bases[i].second, (unsigned long long)(size >> 10), bases[i].first);
    }
}

}  // namespace census
