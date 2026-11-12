#include "foreign.h"
#include "addr.h"
#include <psapi.h>
#include <algorithm>
#include <map>
#include <set>

namespace foreign {

// ------------------------------------------------------------------ what the process looks like
std::vector<Module> Modules() {
    std::vector<Module> v;
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return v;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi = {};
        wchar_t path[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) || !GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        v.push_back({path, (uint64_t)mi.lpBaseOfDll, (uint64_t)mi.SizeOfImage});
    }
    return v;
}

static BOOL CALLBACK AddWindow(HWND w, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[128];
    if (pid == GetCurrentProcessId() && GetClassNameW(w, cls, 128)) ((std::vector<Window>*)p)->push_back({(uint64_t)w, cls});
    return TRUE;
}

std::vector<Window> Windows() {
    std::vector<Window> v;
    EnumWindows(&AddWindow, (LPARAM)&v);
    return v;
}

static std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

static std::string FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring f = Lower(slash == std::wstring::npos ? path : path.substr(slash + 1));
    std::string s;
    for (wchar_t c : f) s += c < 128 ? (char)c : '?';
    return s;
}

static bool IsGame(uint64_t v) {
    return (v >= addr::kRegionsBase && v < addr::kArenaBase + 0x100000000ull) || g_exe.Contains((uintptr_t)v);
}

static bool ReadQword(uint64_t p, uint64_t* out) {
    __try {
        *out = *(const uint64_t*)p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The committed memory of the process, sorted: which values point at DLL images and which at other DLLs' memory.
class MemoryMap {
public:
    enum Kind : uint8_t { Game, Image, Outside };
    MemoryMap() {
        mods_ = Modules();
        std::sort(mods_.begin(), mods_.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        MEMORY_BASIC_INFORMATION mbi;
        for (uint64_t p = 0x10000; p < 0x7FFFFFFF0000ull && VirtualQuery((void*)p, &mbi, sizeof(mbi));) {
            uint64_t lo = (uint64_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT) {
                Kind k = mbi.Type == MEM_IMAGE ? Image : IsGame(lo) ? Game : Outside;
                if (!spans_.empty() && spans_.back().hi == lo && spans_.back().kind == k) spans_.back().hi = hi;
                else spans_.push_back({lo, hi, k});
            }
            if (hi <= p) break;
            p = hi;
        }
    }
    // Outside or Image for a committed address there, else Game (game memory, or nothing).
    Kind KindOf(uint64_t v) const {
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull || IsGame(v)) return Game;
        auto it = std::upper_bound(spans_.begin(), spans_.end(), v, [](uint64_t x, const Span& s) { return x < s.lo; });
        if (it == spans_.begin()) return Game;
        --it;
        return v < it->hi ? it->kind : Game;
    }
    // "module.dll+rva" of the vtable an object starts with, "" if it doesn't start with one.
    std::string ClassOf(uint64_t v) const {
        uint64_t first = 0;
        if (!ReadQword(v, &first) || KindOf(first) != Image) return "";
        auto it = std::upper_bound(mods_.begin(), mods_.end(), first, [](uint64_t x, const Module& m) { return x < m.base; });
        if (it == mods_.begin()) return "";
        --it;
        if (first >= it->base + it->size) return "";
        char b[32];
        sprintf_s(b, "+%llX", (unsigned long long)(first - it->base));
        return FileName(it->path) + b;
    }

private:
    struct Span {
        uint64_t lo, hi;
        Kind kind;
    };
    std::vector<Span> spans_;
    std::vector<Module> mods_;
};

// ------------------------------------------------------------------ scanning a savestate
struct Scanner::Impl {
    bool (*skip)(uint64_t);
    std::string (*label)(uint64_t) = nullptr;
    MemoryMap map;
    Table table;
    std::unordered_map<uint64_t, uint32_t> classOf;   // value -> class index (+1; 0 = skipped)
    std::unordered_map<std::string, uint32_t> classIndex{{"", 0}};
    std::map<uint64_t, size_t> plainBases;   // allocation -> values without a vtable pointing into it (diagnostics)
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> plainSamples;   // allocation -> (slot, value), a few
};

Scanner::Scanner(bool (*skip)(uint64_t), std::string (*label)(uint64_t)) : m_(new Impl()) {
    m_->skip = skip;
    m_->label = label;
}
Scanner::~Scanner() { delete m_; }

void Scanner::Scan(uint64_t addr, const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull || IsGame(v)) continue;
        auto it = m_->classOf.find(v);
        if (it == m_->classOf.end()) {
            uint32_t c = 0;
            if (m_->map.KindOf(v) == MemoryMap::Outside && !(m_->skip && m_->skip(v))) {
                std::string name = m_->map.ClassOf(v);
                std::string extra = m_->label ? m_->label(v) : std::string();
                if (!extra.empty()) name += " [" + extra + "]";
                MEMORY_BASIC_INFORMATION mbi;
                if (name.empty() && VirtualQuery((void*)v, &mbi, sizeof(mbi))) m_->plainBases[(uint64_t)mbi.AllocationBase]++;
                auto ci = m_->classIndex.find(name);
                if (ci == m_->classIndex.end()) {
                    ci = m_->classIndex.emplace(name, (uint32_t)m_->table.classes.size()).first;
                    m_->table.classes.push_back(name);
                }
                c = ci->second + 1;
            }
            it = m_->classOf.emplace(v, c).first;
        }
        if (it->second) {
            m_->table.entries.push_back({addr + i, v, it->second - 1, 0});
            if (it->second == 1) {   // no vtable: keep a few examples per allocation
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery((void*)v, &mbi, sizeof(mbi))) {
                    auto& s = m_->plainSamples[(uint64_t)mbi.AllocationBase];
                    if (s.size() < 3) s.push_back({addr + i, v});
                }
            }
        }
    }
}

// The module whose writable data holds a heap handle (its C runtime's heap), for diagnostics.
static std::string HeapOwnerModule(HANDLE heap) {
    for (const Module& m : Modules()) {
        auto* base = (uint8_t*)m.base;
        auto* nth = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nth);
        for (int i = 0; i < nth->FileHeader.NumberOfSections; i++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            auto* q = (const uint64_t*)(base + sec->VirtualAddress);
            for (size_t k = 0; k < sec->Misc.VirtualSize / 8; k++) {
                uint64_t v = 0;
                if (ReadQword((uint64_t)&q[k], &v) && v == (uint64_t)heap) return FileName(m.path);
            }
        }
    }
    return "?";
}

// For each allocation in ases: the Windows heap it belongs to ("heap of x.dll"), if any.
static std::map<uint64_t, std::string> HeapsOf(const std::set<uint64_t>& bases) {
    std::map<uint64_t, std::string> out;
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    for (DWORD h = 0; h < n && h < 64 && out.size() < bases.size(); h++) {
        if (!HeapLock(heaps[h])) continue;
        PROCESS_HEAP_ENTRY e = {};
        while (HeapWalk(heaps[h], &e)) {
            if (!(e.wFlags & PROCESS_HEAP_REGION) && e.cbData < 0x10000) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(e.lpData, &mbi, sizeof(mbi))) continue;
            if (bases.count((uint64_t)mbi.AllocationBase) && !out.count((uint64_t)mbi.AllocationBase))
                out[(uint64_t)mbi.AllocationBase] = std::string("heap ") + std::to_string(h) + (h == 0 ? " (process heap)" : "");
        }
        HeapUnlock(heaps[h]);
        std::string owner;
        for (auto& [b, name] : out)
            if (name.rfind(std::string("heap ") + std::to_string(h), 0) == 0 && name.find(" of ") == std::string::npos) {
                if (owner.empty()) owner = h == 0 ? "the process" : HeapOwnerModule(heaps[h]);
                name += " of " + owner;
            }
    }
    return out;
}

Table Scanner::Finish() {    // Where values without a vtable lead (the largest few allocations), and whether that is a Windows heap.
    std::vector<std::pair<size_t, uint64_t>> top;
    for (auto& [base, n] : m_->plainBases) top.push_back({n, base});
    std::sort(top.rbegin(), top.rend());
    std::set<uint64_t> topBases;
    for (size_t i = 0; i < top.size() && i < 8; i++) topBases.insert(top[i].second);
    std::map<uint64_t, std::string> heapOf = HeapsOf(topBases);
    for (size_t i = 0; i < top.size() && i < 8; i++) {
        uint64_t base = top[i].second, size = 0;
        MEMORY_BASIC_INFORMATION mbi = {};
        for (uint64_t p = base; VirtualQuery((void*)p, &mbi, sizeof(mbi)) && (uint64_t)mbi.AllocationBase == base; p += mbi.RegionSize) size += mbi.RegionSize;
        auto ho = heapOf.find(base);
        Log("outside plain memory: allocation %p (%llu KB, %s, %s) has %u of the values", (void*)base,
            (unsigned long long)(size >> 10), mbi.Type == MEM_MAPPED ? "mapped" : mbi.Type == MEM_IMAGE ? "image" : "private",
            ho != heapOf.end() ? ho->second.c_str() : "no Windows heap", (unsigned)top[i].first);
        for (auto& [slot, v] : m_->plainSamples[base]) {
            uint64_t q[2] = {};
            ReadQword(v, &q[0]);
            ReadQword(v + 8, &q[1]);
            Log("  e.g. %s holds %p, which holds %s, %s", Where(slot).c_str(), (void*)v, Where(q[0]).c_str(), Where(q[1]).c_str());
        }
    }
    Log("outside pointers: %u slots, %u distinct targets, %u kinds of objects", (unsigned)m_->table.entries.size(),
        (unsigned)std::count_if(m_->classOf.begin(), m_->classOf.end(), [](auto& kv) { return kv.second != 0; }),
        (unsigned)m_->table.classes.size() - 1);
    return std::move(m_->table);
}

// ------------------------------------------------------------------ file encoding
namespace {
struct Out {
    std::vector<uint8_t> v;
    void Put(const void* p, size_t n) { v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void U32(uint32_t x) { Put(&x, 4); }
    void U64(uint64_t x) { Put(&x, 8); }
    void WStr(const std::wstring& s) { U32((uint32_t)s.size()); Put(s.data(), s.size() * 2); }
    void Str(const std::string& s) { U32((uint32_t)s.size()); Put(s.data(), s.size()); }
};
struct In {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    void Get(void* d, size_t n) {
        if ((size_t)(end - p) < n) { ok = false; memset(d, 0, n); return; }
        memcpy(d, p, n);
        p += n;
    }
    uint32_t U32() { uint32_t x; Get(&x, 4); return x; }
    uint64_t U64() { uint64_t x; Get(&x, 8); return x; }
    std::wstring WStr() {
        uint32_t n = U32();
        if (!ok || (size_t)(end - p) < (size_t)n * 2) { ok = false; return {}; }
        std::wstring s((const wchar_t*)p, n);
        p += (size_t)n * 2;
        return s;
    }
    std::string Str() {
        uint32_t n = U32();
        if (!ok || (size_t)(end - p) < n) { ok = false; return {}; }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
};
}  // namespace

std::vector<uint8_t> Encode(const std::vector<Module>& modules, const std::vector<Window>& windows) {
    Out o;
    o.U32((uint32_t)modules.size());
    for (const Module& m : modules) { o.U64(m.base); o.U64(m.size); o.WStr(m.path); }
    o.U32((uint32_t)windows.size());
    for (const Window& w : windows) { o.U64(w.hwnd); o.WStr(w.cls); }
    return o.v;
}

bool Decode(const uint8_t* p, size_t n, std::vector<Module>* modules, std::vector<Window>* windows) {
    In in{p, p + n};
    modules->resize(in.U32() & 0xFFFF);
    for (Module& m : *modules) { m.base = in.U64(); m.size = in.U64(); m.path = in.WStr(); }
    windows->resize(in.U32() & 0xFFFF);
    for (Window& w : *windows) { w.hwnd = in.U64(); w.cls = in.WStr(); }
    return in.ok;
}

std::vector<uint8_t> EncodeTable(const Table& t) {
    Out o;
    o.U32((uint32_t)t.classes.size());
    for (const std::string& c : t.classes) o.Str(c);
    o.U64(t.entries.size());
    o.Put(t.entries.data(), t.entries.size() * sizeof(Entry));
    return o.v;
}

bool DecodeTable(const uint8_t* p, size_t n, Table* t) {
    In in{p, p + n};
    t->classes.resize(in.U32() & 0xFFFFF);
    for (std::string& c : t->classes) c = in.Str();
    uint64_t count = in.U64();
    if (!in.ok || count > (in.end - in.p) / sizeof(Entry)) return false;
    t->entries.resize((size_t)count);
    in.Get(t->entries.data(), (size_t)count * sizeof(Entry));
    for (const Entry& e : t->entries)
        if (e.cls >= t->classes.size()) return false;
    return in.ok && !t->classes.empty();
}

// ------------------------------------------------------------------ remapping
Remap::Remap() : filter_((1u << 22) / 64, 0) {}

void Remap::Exact(uint64_t from, uint64_t to) {
    if (from == to) return;
    exact_[from] = to;
    uint32_t h = Hash(from);
    filter_[h >> 6] |= 1ull << (h & 63);
    if (from < elo_) elo_ = from;
    if (from > ehi_) ehi_ = from;
}

void Remap::Range(uint64_t lo, uint64_t hi, int64_t delta) {
    if (!delta || hi <= lo) return;
    ranges_.push_back({lo, hi, delta});
    if (lo < rlo_) rlo_ = lo;
    if (hi > rhi_) rhi_ = hi;
}

uint64_t Remap::Map(uint64_t v) const {
    if (v >= elo_ && v <= ehi_) {
        uint32_t h = Hash(v);
        if ((filter_[h >> 6] >> (h & 63)) & 1) {
            auto it = exact_.find(v);
            if (it != exact_.end()) return it->second;
        }
    }
    if (v >= rlo_ && v < rhi_)
        for (const R& r : ranges_)
            if (v >= r.lo && v < r.hi) return v + r.delta;
    return v;
}

uint64_t Remap::Run(uint8_t* p, size_t n) {
    uint64_t changed = 0;
    auto* q = (uint64_t*)p;
    for (size_t i = 0; i < n / 8; i++) {
        uint64_t v = q[i];
        if ((v < elo_ || v > ehi_) && (v < rlo_ || v >= rhi_)) continue;
        uint64_t w = Map(v);
        if (w != v) {
            q[i] = w;
            changed++;
        }
    }
    return changed;
}

// ------------------------------------------------------------------ planning a load from another session
// A saved class without its label ("d3d11.dll+1D6970 [GPU object of x.dll]" -> "d3d11.dll+1D6970").
static std::string BaseClass(const std::string& c) {
    size_t p = c.find(" [");
    return p == std::string::npos ? c : c.substr(0, p);
}

static bool UnderFolder(const std::wstring& path, const std::wstring& folder) {
    return !folder.empty() && Lower(path).compare(0, folder.size(), Lower(folder)) == 0;
}

void LoadMissing(const std::vector<Module>& saved) {
    wchar_t exe[MAX_PATH], win[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring gameDir = exe;
    gameDir = gameDir.substr(0, gameDir.find_last_of(L'\\') + 1);
    GetWindowsDirectoryW(win, MAX_PATH);
    for (const Module& m : saved) {
        if (GetModuleHandleW(m.path.c_str())) continue;
        // Only the game's and Windows' own DLLs: others (overlays, tools) are no part of the game.
        if (!UnderFolder(m.path, gameDir) && !UnderFolder(m.path, win)) {
            Log("load: %ls was loaded when the savestate was made, not now (left out)", m.path.c_str());
            continue;
        }
        HMODULE h = LoadLibraryW(m.path.c_str());
        Log("load: %ls wasn't loaded in this session: %s", m.path.c_str(), h ? "loaded it" : "cannot load it");
    }
}

std::vector<uint64_t> ReadSlots(const Table& t) {
    std::vector<uint64_t> live(t.entries.size(), 0);
    for (size_t i = 0; i < t.entries.size(); i++)
        if (!ReadQword(t.entries[i].slot, &live[i])) live[i] = 0;
    return live;
}

void Plan(const std::vector<Module>& modules, const std::vector<Window>& windows, const Table& t,
          const std::vector<uint64_t>& live, const std::unordered_map<uint64_t, uint64_t>* objects, Remap* out) {
    // DLL images that moved.
    std::map<std::wstring, Module> now;
    for (Module& m : Modules()) now[Lower(m.path)] = m;
    int moved = 0, missing = 0;
    for (const Module& m : modules) {
        auto it = now.find(Lower(m.path));
        if (it == now.end()) { missing++; continue; }
        if (it->second.base != m.base) {
            out->Range(m.base, m.base + m.size, (int64_t)(it->second.base - m.base));
            Log("load: %ls is at %p now (was at %p)", m.path.c_str(), (void*)it->second.base, (void*)m.base);
            moved++;
        }
    }

    // Windows, by class (only classes with one window then and now).
    std::map<std::wstring, std::vector<uint64_t>> then, live2;
    for (const Window& w : windows) then[w.cls].push_back(w.hwnd);
    for (const Window& w : Windows()) live2[w.cls].push_back(w.hwnd);
    int windowsMapped = 0;
    for (auto& [cls, hs] : then) {
        auto it = live2.find(cls);
        if (hs.size() == 1 && it != live2.end() && it->second.size() == 1) {
            out->Exact(hs[0], it->second[0]);
            windowsMapped++;
        }
    }

    // Other interfaces of rebuilt GPU objects (a texture's DXGI surface, say): the game keeps pointers into the
    // object, a fixed offset past the pointer it was given. d3d11.dll is the same in both sessions, so the rebuilt
    // object has the interface at the same offset.
    size_t interior = 0;
    if (objects && !objects->empty()) {
        std::vector<std::pair<uint64_t, uint64_t>> sorted(objects->begin(), objects->end());
        std::sort(sorted.begin(), sorted.end());
        for (const Entry& e : t.entries) {
            const std::string& cls = t.classes[e.cls];
            if (out->Has(e.value) || (cls.rfind("d3d11.dll+", 0) != 0 && cls.rfind("dxgi.dll+", 0) != 0) || cls.find(" [") != std::string::npos)
                continue;
            auto it = std::upper_bound(sorted.begin(), sorted.end(), std::make_pair(e.value, ~0ull));
            if (it == sorted.begin()) continue;
            --it;
            if (e.value - it->first >= 0x1000) continue;
            out->Exact(e.value, it->second + (e.value - it->first));
            interior++;
        }
    }
    // Outside objects: the live value in the same slots, if it is the same kind of object.
    MemoryMap map;
    std::unordered_map<uint64_t, std::string> liveClass;
    auto classOfLive = [&](uint64_t v) -> const std::string* {
        if (map.KindOf(v) != MemoryMap::Outside) return nullptr;
        auto it = liveClass.find(v);
        if (it == liveClass.end()) it = liveClass.emplace(v, map.ClassOf(v)).first;
        return &it->second;
    };
    struct Group {
        uint32_t cls = 0;
        size_t slots = 0;
        std::map<uint64_t, size_t> votes;
    };
    std::unordered_map<uint64_t, Group> groups;
    std::map<std::string, std::set<uint64_t>> liveByClass;
    for (size_t i = 0; i < t.entries.size(); i++) {
        const Entry& e = t.entries[i];
        Group& g = groups[e.value];
        g.cls = e.cls;
        g.slots++;
        const std::string* lc = live[i] ? classOfLive(live[i]) : nullptr;
        if (!lc || *lc != BaseClass(t.classes[e.cls])) continue;
        g.votes[live[i]]++;
        if (!lc->empty()) liveByClass[*lc].insert(live[i]);
    }
    std::map<std::string, std::vector<uint64_t>> unmappedByClass;
    std::map<std::string, size_t> unmappedSlots;
    size_t mappedObjects = 0, mappedPlain = 0, sameAddress = 0;
    for (auto& [old, g] : groups) {
        if (out->Has(old)) continue;
        const std::string& cls = t.classes[g.cls];
        uint64_t best = 0;
        size_t bestVotes = 0, total = 0;
        for (auto& [v, n] : g.votes) {
            total += n;
            if (n > bestVotes) { best = v; bestVotes = n; }
        }
        // Objects with a vtable: the most common live counterpart. Plain memory: only an unanimous one.
        bool ok = bestVotes && (!cls.empty() || bestVotes == total);
        if (ok) {
            if (best == old) sameAddress++;
            out->Exact(old, best);
            (cls.empty() ? mappedPlain : mappedObjects)++;
        } else {
            unmappedByClass[cls].push_back(old);
            unmappedSlots[cls] += g.slots;
        }
    }
    // Where unmapped values sit (the first few), for the lab.
    int shown = 0;
    for (size_t i = 0; i < t.entries.size() && shown < 12; i++) {
        const Entry& e = t.entries[i];
        if (out->Has(e.value)) continue;
        Log("  unmapped %p in slot %s (%p): live value there %p (%s)", (void*)e.value, Where(e.slot).c_str(), (void*)e.slot,
            (void*)live[i], live[i] ? (map.KindOf(live[i]) == MemoryMap::Outside ? map.ClassOf(live[i]).c_str() : "not outside") : "unreadable");
        shown++;
    }
    // A kind of object the savestate and this session each have exactly one of: they are the same.
    for (auto it = unmappedByClass.begin(); it != unmappedByClass.end();) {
        auto lb = liveByClass.find(BaseClass(it->first));
        if (!it->first.empty() && it->second.size() == 1 && lb != liveByClass.end() && lb->second.size() == 1) {
            out->Exact(it->second[0], *lb->second.begin());
            mappedObjects++;
            unmappedSlots.erase(it->first);
            it = unmappedByClass.erase(it);
        } else {
            ++it;
        }
    }
    size_t unmapped = 0;
    for (auto& [cls, v] : unmappedByClass) unmapped += v.size();
    Log("load (another session): %u pointers into rebuilt GPU objects (other interfaces)", (unsigned)interior);
    Log("load (another session): %d DLLs moved, %d not loaded; %d windows; outside objects: %llu mapped, %llu plain "
        "blocks mapped (%llu at the same address), %u values left as they were",
        moved, missing, windowsMapped, (unsigned long long)mappedObjects, (unsigned long long)mappedPlain,
        (unsigned long long)sameAddress, (unsigned)unmapped);
    std::vector<std::pair<size_t, std::string>> worst;
    for (auto& [cls, n] : unmappedSlots) worst.push_back({n, cls});
    std::sort(worst.rbegin(), worst.rend());
    for (size_t i = 0; i < worst.size() && i < 20; i++)
        Log("  not mapped: %-48s %u values in %u slots", worst[i].second.empty() ? "(plain memory)" : worst[i].second.c_str(),
            (unsigned)unmappedByClass[worst[i].second].size(), (unsigned)worst[i].first);
}

}  // namespace foreign

// ------------------------------------------------------------------ pointers the game scrambles
// The C runtime in GTA5.exe keeps function pointers scrambled with EncodePointer, whose secret is random in every
// process: the restored memory of another session would decode to garbage. The game gets a scrambling that is the
// same in every session instead.
namespace foreign {

static const uint64_t kKey = 0x5D3A9C1E7B2F4086ull;
static PVOID WINAPI Hook_EncodePointer(PVOID p) { return (PVOID)_rotr64((uint64_t)p ^ kKey, 17); }
static PVOID WINAPI Hook_DecodePointer(PVOID p) { return (PVOID)(_rotl64((uint64_t)p, 17) ^ kKey); }

void InstallHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    PatchIat(exe, "kernel32.dll", "EncodePointer", (void*)&Hook_EncodePointer);
    PatchIat(exe, "kernel32.dll", "DecodePointer", (void*)&Hook_DecodePointer);
}

void* HookForProcAddress(const char* name) {
    if (!strcmp(name, "EncodePointer")) return (void*)&Hook_EncodePointer;
    if (!strcmp(name, "DecodePointer")) return (void*)&Hook_DecodePointer;
    return nullptr;
}

}  // namespace foreign
