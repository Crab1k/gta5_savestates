#include "memory.h"
#include "addr.h"
#include "arena.h"
#include "threads.h"
#include <map>

namespace mem {

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::map<uintptr_t, Region*>* g_regions;   // by base
static volatile LONG g_keepFreed = 0;
static DWORD g_tlsIndex = 0;
static size_t g_tlsSize = 0;

static LPVOID(WINAPI* RealVirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD) = &VirtualAlloc;
static BOOL(WINAPI* RealVirtualFree)(LPVOID, SIZE_T, DWORD) = &VirtualFree;

static Region* AddRegion(void* base, size_t size, bool ownWatch) {
    auto* r = new Region();
    r->base = (uint8_t*)base;
    r->size = (size + 0xFFF) & ~(size_t)0xFFF;
    r->pages = r->size >> 12;
    r->ownWatch = ownWatch;
    r->written.assign((r->pages + 63) / 64, 0);
    AcquireSRWLockExclusive(&g_lock);
    (*g_regions)[(uintptr_t)base] = r;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static Region* Find(const void* p) {
    AcquireSRWLockShared(&g_lock);
    Region* found = nullptr;
    auto it = g_regions->upper_bound((uintptr_t)p);
    if (it != g_regions->begin()) {
        --it;
        if ((uintptr_t)p < (uintptr_t)it->second->base + it->second->size) found = it->second;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

// ------------------------------------------------------------------ VirtualAlloc / VirtualFree of GTA5.exe
static LPVOID WINAPI Hook_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if (!(type & MEM_RESERVE)) return RealVirtualAlloc(addr, size, type, protect);
    // Regions the game itself asks write watch for are watched by the game: we save all their committed pages.
    bool ownWatch = !(type & (MEM_WRITE_WATCH | MEM_LARGE_PAGES | MEM_PHYSICAL));
    // At the address this thread's number gives it (addr.h), the same in every session.
    LPVOID p = addr ? nullptr : addr::Allocate(threads::Ordinal(), size, ownWatch ? type | MEM_WRITE_WATCH : type, protect, (void*(__stdcall*)(void*, size_t, unsigned long, unsigned long))RealVirtualAlloc);
    if (!p) p = RealVirtualAlloc(addr, size, ownWatch ? type | MEM_WRITE_WATCH : type, protect);
    if (!p && ownWatch) {
        ownWatch = false;
        p = RealVirtualAlloc(addr, size, type, protect);
    }
    if (p) {
        AddRegion(p, size, ownWatch);
        if (size >= (16u << 20)) {
            void* frames[8] = {};
            USHORT n = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
            std::string from;
            for (USHORT i = 0; i < n; i++) from += " < " + Where((uintptr_t)frames[i]);
            Log("game region %p: %llu MB%s, from%s", p, (unsigned long long)(size >> 20), ownWatch ? "" : " (no write watch)", from.c_str());
        }
    }
    return p;
}

// Zeroes the committed pages of [p, p+n) (what a decommit followed by a commit would give the game).
static void ZeroCommitted(uint8_t* p, size_t n) {
    uint8_t* end = p + n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        uint8_t* runEnd = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (runEnd > end) runEnd = end;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) memset(p, 0, runEnd - p);
        p = runEnd;
    }
}

static BOOL WINAPI Hook_VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    Region* r = Find(addr);
    if (!r) return RealVirtualFree(addr, size, type);
    if (g_keepFreed) {
        // Savestates may hold memory that points into this region: keep it, as the game would find it after a load.
        if (type & MEM_RELEASE) {
            r->freedByGame = true;
            Log("game released region %p (%llu MB): kept for savestates", r->base, (unsigned long long)(r->size >> 20));
        } else if (type & MEM_DECOMMIT) {
            ZeroCommitted((uint8_t*)addr, size);
        }
        return TRUE;
    }
    BOOL ok = RealVirtualFree(addr, size, type);
    if (ok && (type & MEM_RELEASE)) {
        AcquireSRWLockExclusive(&g_lock);
        g_regions->erase((uintptr_t)r->base);
        ReleaseSRWLockExclusive(&g_lock);
        delete r;
    }
    return ok;
}

void SetKeepFreed(bool keep) { InterlockedExchange(&g_keepFreed, keep ? 1 : 0); }

void ReleaseUnless(bool (*needed)(uintptr_t base)) {
    std::vector<Region*> drop;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_regions->begin(); it != g_regions->end();) {
        Region* r = it->second;
        if (r->base != arena::Base() && !needed((uintptr_t)r->base)) {
            drop.push_back(r);
            it = g_regions->erase(it);
        } else {
            ++it;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    size_t bytes = 0;
    for (Region* r : drop) {
        bytes += r->size;
        RealVirtualFree(r->base, 0, MEM_RELEASE);
        delete r;
    }
    if (!drop.empty()) Log("released %u regions (%llu MB) that no savestate and no restored memory uses", (unsigned)drop.size(), (unsigned long long)(bytes >> 20));
}

// ------------------------------------------------------------------ Windows heap of GTA5.exe -> arena
static LPVOID(WINAPI* RealHeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL(WINAPI* RealHeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID(WINAPI* RealHeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T(WINAPI* RealHeapSize)(HANDLE, DWORD, LPCVOID);

static LPVOID WINAPI Hook_HeapAlloc(HANDLE h, DWORD flags, SIZE_T size) {
    if (void* p = arena::Alloc(size, (flags & HEAP_ZERO_MEMORY) != 0)) return p;
    Log("arena full: %llu-byte block went to the Windows heap (it won't be in savestates)", (unsigned long long)size);
    return RealHeapAlloc(h, flags, size);
}

static BOOL WINAPI Hook_HeapFree(HANDLE h, DWORD flags, LPVOID p) {
    if (!p) return TRUE;
    if (arena::Free(p)) return TRUE;
    return RealHeapFree(h, flags, p);
}

static LPVOID WINAPI Hook_HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T size) {
    const bool inPlace = (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0;
    const bool zero = (flags & HEAP_ZERO_MEMORY) != 0;
    if (arena::Contains(p)) {
        if (void* q = arena::Realloc(p, size, inPlace, zero)) return q;
        return nullptr;
    }
    if (!p) return Hook_HeapAlloc(h, flags, size);
    if (inPlace) return RealHeapReAlloc(h, flags, p, size);
    // A block from before the hook: move it into the arena.
    SIZE_T old = RealHeapSize(h, 0, p);
    void* q = arena::Alloc(size, false);
    if (!q) return RealHeapReAlloc(h, flags, p, size);
    memcpy(q, p, old < size ? old : size);
    if (zero && size > old) memset((uint8_t*)q + old, 0, size - old);
    RealHeapFree(h, 0, p);
    return q;
}

static SIZE_T WINAPI Hook_HeapSize(HANDLE h, DWORD flags, LPCVOID p) {
    if (arena::Contains(p)) return arena::Size(p);
    return RealHeapSize(h, flags, p);
}

// ------------------------------------------------------------------ setup
void Init(size_t arenaReserve) {
    g_regions = new std::map<uintptr_t, Region*>;
    if (arena::Init(arenaReserve)) AddRegion(arena::Base(), arena::Reserved(), true);
    else Log("cannot reserve %llu MB for the arena", (unsigned long long)(arenaReserve >> 20));

    auto* base = (uint8_t*)g_exe.base;
    auto* nth = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& td = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (td.VirtualAddress) {
        auto* tls = (IMAGE_TLS_DIRECTORY64*)(base + td.VirtualAddress);
        g_tlsIndex = *(DWORD*)tls->AddressOfIndex;
        g_tlsSize = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;
    }
}

void InstallHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (void* p = PatchIat(exe, "kernel32.dll", "VirtualAlloc", (void*)&Hook_VirtualAlloc)) RealVirtualAlloc = (decltype(RealVirtualAlloc))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "VirtualFree", (void*)&Hook_VirtualFree)) RealVirtualFree = (decltype(RealVirtualFree))p;
    RealHeapAlloc = (decltype(RealHeapAlloc))PatchIat(exe, "kernel32.dll", "HeapAlloc", (void*)&Hook_HeapAlloc);
    RealHeapFree = (decltype(RealHeapFree))PatchIat(exe, "kernel32.dll", "HeapFree", (void*)&Hook_HeapFree);
    RealHeapReAlloc = (decltype(RealHeapReAlloc))PatchIat(exe, "kernel32.dll", "HeapReAlloc", (void*)&Hook_HeapReAlloc);
    RealHeapSize = (decltype(RealHeapSize))PatchIat(exe, "kernel32.dll", "HeapSize", (void*)&Hook_HeapSize);
    if (!RealHeapAlloc) RealHeapAlloc = &HeapAlloc;
    if (!RealHeapFree) RealHeapFree = &HeapFree;
    if (!RealHeapReAlloc) RealHeapReAlloc = &HeapReAlloc;
    if (!RealHeapSize) RealHeapSize = &HeapSize;
}

std::vector<Region*> Regions() {
    std::vector<Region*> v;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_regions) v.push_back(kv.second);
    ReleaseSRWLockShared(&g_lock);
    return v;
}

void CollectWritten(Region* r) {
    if (!r->ownWatch) {
        // No write watch: every committed page counts as written.
        for (uint8_t* p = r->base; p < r->base + r->size;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            uint8_t* end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            if (end > r->base + r->size) end = r->base + r->size;
            if (mbi.State == MEM_COMMIT)
                for (uint8_t* q = p; q < end; q += 0x1000) {
                    size_t i = (size_t)(q - r->base) >> 12;
                    r->written[i >> 6] |= 1ull << (i & 63);
                }
            p = end;
        }
        return;
    }
    static std::vector<void*> addrs(1 << 16);
    for (;;) {
        ULONG_PTR count = addrs.size();
        DWORD gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, r->base, r->size, addrs.data(), &count, &gran) != 0) {
            Log("GetWriteWatch failed for %p (error %u)", r->base, GetLastError());
            return;
        }
        for (ULONG_PTR i = 0; i < count; i++) {
            size_t page = (size_t)((uint8_t*)addrs[i] - r->base) >> 12;
            r->written[page >> 6] |= 1ull << (page & 63);
        }
        if (count < addrs.size()) return;
    }
}

std::vector<Run> Committed(const Region* r) {
    std::vector<Run> runs;
    for (uint8_t* p = r->base; p < r->base + r->size;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        uint8_t* end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (end > r->base + r->size) end = r->base + r->size;
        if (mbi.State == MEM_COMMIT)
            runs.push_back({(uint32_t)((p - r->base) >> 12), (uint32_t)((end - p) >> 12), mbi.Protect, 0});
        p = end;
    }
    return runs;
}

Region* Ensure(uint64_t base, uint64_t size, bool ownWatch, bool* made) {
    *made = false;
    size = (size + 0xFFF) & ~(uint64_t)0xFFF;
    std::vector<Region*> inTheWay;
    Region* same = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (auto& [b, r] : *g_regions) {
        if (b == base && r->size == size) same = r;
        else if (b < base + size && b + r->size > base) inTheWay.push_back(r);
    }
    ReleaseSRWLockShared(&g_lock);
    if (same) return same;
    for (Region* r : inTheWay) {
        if (r->base == arena::Base()) return nullptr;
        Log("load: region %p (%llu MB) is in the way of the savestate's region %p: released", r->base,
            (unsigned long long)(r->size >> 20), (void*)base);
        AcquireSRWLockExclusive(&g_lock);
        g_regions->erase((uintptr_t)r->base);
        ReleaseSRWLockExclusive(&g_lock);
        RealVirtualFree(r->base, 0, MEM_RELEASE);
        delete r;
    }
    void* p = RealVirtualAlloc((void*)base, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p != (void*)base) {
        if (p) RealVirtualFree(p, 0, MEM_RELEASE);
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQuery((void*)base, &mbi, sizeof(mbi));
        Log("load: cannot reserve the savestate's region %p (%llu MB); something else is there (allocation %p, %s)",
            (void*)base, (unsigned long long)(size >> 20), mbi.AllocationBase,
            mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : mbi.State == MEM_FREE ? "free" : "private");
        return nullptr;
    }
    *made = true;
    Region* r = AddRegion(p, (size_t)size, true);
    r->ownWatch = ownWatch;   // regions the game watches itself: all their committed pages count (CollectWritten)
    return r;
}

void Commit(Region* r, const std::vector<Run>& runs) {
    for (const Run& run : runs) {
        if ((uint64_t)run.first + run.pages > r->pages) continue;
        if (!RealVirtualAlloc(r->base + ((size_t)run.first << 12), (size_t)run.pages << 12, MEM_COMMIT, PAGE_READWRITE))
            Log("load: cannot commit %u pages at %p (error %u)", run.pages, r->base + ((size_t)run.first << 12), GetLastError());
    }
}

void Protect(Region* r, const std::vector<Run>& runs) {
    for (const Run& run : runs) {
        DWORD protect = run.protect & ~(DWORD)(PAGE_NOCACHE | PAGE_WRITECOMBINE);
        if (protect == PAGE_READWRITE || (uint64_t)run.first + run.pages > r->pages) continue;
        DWORD old;
        VirtualProtect(r->base + ((size_t)run.first << 12), (size_t)run.pages << 12, protect, &old);
    }
}

std::vector<Section> DataSections() {
    std::vector<Section> v;
    auto* base = (uint8_t*)g_exe.base;
    auto* nth = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nth);
    for (int i = 0; i < nth->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (!memcmp(sec->Name, ".tls", 4)) continue;   // the template; each thread's copy is saved with the thread
        Section s;
        s.p = base + sec->VirtualAddress;
        s.size = sec->Misc.VirtualSize;
        memcpy(s.name, sec->Name, 8);
        s.name[8] = 0;
        v.push_back(s);
    }
    return v;
}

uint8_t* TlsBlock(void* teb) {
    if (!g_tlsSize) return nullptr;
    auto** slots = *(uint8_t***)((uint8_t*)teb + 0x58);   // TEB.ThreadLocalStoragePointer
    return slots ? slots[g_tlsIndex] : nullptr;
}

size_t TlsSize() { return g_tlsSize; }

}  // namespace mem
