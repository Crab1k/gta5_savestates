// recon.asi: logs what GTA5.exe allocates, which threads it starts, where they wait and where the main loop is.
// Throwaway measurement tool for planning the savestate design; never shipped.
#include <windows.h>
#include <objbase.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../src/spawn.h"

static FILE* g_log;
static CRITICAL_SECTION g_logCs;
static uintptr_t g_lo, g_hi;
static DWORD g_mainTid;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static bool InGame(uintptr_t p) { return p >= g_lo && p < g_hi; }
static uint32_t Rva(uintptr_t p) { return InGame(p) ? (uint32_t)(p - g_lo) : 0xFFFFFFFF; }

static std::string Where(uintptr_t p) {
    char b[160];
    if (InGame(p)) { sprintf_s(b, "+%X", (unsigned)(p - g_lo)); return b; }
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* s = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    sprintf_s(b, "%s+%llX", s, (unsigned long long)(p - (uintptr_t)m));
    return b;
}

static void Unprotect(void* p, size_t n) {
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
}

static void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        for (int i = 0; iat[i].u1.Function; i++) {
            if (!imp->OriginalFirstThunk || IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            void* old = (void*)iat[i].u1.Function;
            Unprotect(&iat[i].u1.Function, 8);
            iat[i].u1.Function = (ULONGLONG)hook;
            return old;
        }
    }
    Log("IAT: %s!%s not found", dll, func);
    return nullptr;
}

// ------------------------------------------------------------------ counters keyed by (function, thread, caller)
static SRWLOCK g_statLock = SRWLOCK_INIT;
struct Key {
    const char* fn;
    DWORD tid;
    uint32_t rva;
    bool operator<(const Key& o) const {
        if (fn != o.fn) return fn < o.fn;
        if (tid != o.tid) return tid < o.tid;
        return rva < o.rva;
    }
};
static std::map<Key, uint64_t>* g_stats;
static void Count(const char* fn, uintptr_t ret) {
    Key k{fn, GetCurrentThreadId(), Rva(ret)};
    AcquireSRWLockExclusive(&g_statLock);
    (*g_stats)[k]++;
    ReleaseSRWLockExclusive(&g_statLock);
}

// ------------------------------------------------------------------ threads
struct ThreadInfo {
    DWORD tid;
    std::string name;
    std::string start;
    std::string creator;
};
static SRWLOCK g_threadLock = SRWLOCK_INIT;
static std::map<DWORD, ThreadInfo>* g_threads;

static void NameThread(DWORD tid, const char* name) {
    AcquireSRWLockExclusive(&g_threadLock);
    (*g_threads)[tid].tid = tid;
    (*g_threads)[tid].name = name;
    ReleaseSRWLockExclusive(&g_threadLock);
}

static void CopyName(char* out, size_t n, const char* in) {
    __try { strncpy_s(out, n, in, _TRUNCATE); } __except (1) {}
}

static LONG WINAPI Veh(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode == 0x406D1388 && er->NumberParameters >= 3 && er->ExceptionInformation[0] == 0x1000) {
        DWORD tid = (DWORD)er->ExceptionInformation[2];
        if (tid == 0xFFFFFFFF) tid = GetCurrentThreadId();
        char name[64] = {};
        CopyName(name, sizeof(name), (const char*)er->ExceptionInformation[1]);
        Log("thread name %u = '%s' (raised at %s)", tid, name, Where((uintptr_t)er->ExceptionAddress).c_str());
        NameThread(tid, name);
    } else if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        static std::atomic<int> n{0};
        if (n++ < 5)
            Log("AV at %s, %s %p", Where((uintptr_t)er->ExceptionAddress).c_str(),
                er->ExceptionInformation[0] ? "write" : "read", (void*)er->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static HANDLE(WINAPI* RealCreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static HANDLE WINAPI Hook_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                       DWORD flags, LPDWORD tidOut) {
    DWORD tid = 0;
    HANDLE h = RealCreateThread(sa, stack, fn, arg, flags, &tid);
    if (tidOut) *tidOut = tid;
    std::string start = Where((uintptr_t)fn), creator = Where((uintptr_t)_ReturnAddress());
    Log("CreateThread tid=%u start=%s arg=%p stack=%llu flags=%X from %s", tid, start.c_str(), arg,
        (unsigned long long)stack, flags, creator.c_str());
    AcquireSRWLockExclusive(&g_threadLock);
    auto& t = (*g_threads)[tid];
    t.tid = tid;
    t.start = start;
    t.creator = creator;
    ReleaseSRWLockExclusive(&g_threadLock);
    return h;
}

// ------------------------------------------------------------------ memory
static LPVOID(WINAPI* RealVirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static BOOL(WINAPI* RealVirtualFree)(LPVOID, SIZE_T, DWORD);
static std::atomic<int64_t> g_vaCommitted{0};
static std::atomic<uint64_t> g_vaCalls{0};

static LPVOID WINAPI Hook_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    LPVOID p = RealVirtualAlloc(addr, size, type, protect);
    g_vaCalls++;
    if (type & MEM_COMMIT) g_vaCommitted += (int64_t)size;
    static std::atomic<int> logged{0};
    if (size >= (1 << 20) || logged++ < 400)
        Log("VirtualAlloc(%p, %llu KB, type=%X, prot=%X) = %p from %s", addr, (unsigned long long)(size >> 10), type,
            protect, p, Where((uintptr_t)_ReturnAddress()).c_str());
    Count("VirtualAlloc", (uintptr_t)_ReturnAddress());
    return p;
}

static BOOL WINAPI Hook_VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    static std::atomic<int> logged{0};
    if (logged++ < 200)
        Log("VirtualFree(%p, %llu KB, type=%X) from %s", addr, (unsigned long long)(size >> 10), type,
            Where((uintptr_t)_ReturnAddress()).c_str());
    Count("VirtualFree", (uintptr_t)_ReturnAddress());
    return RealVirtualFree(addr, size, type);
}

static LPVOID(WINAPI* RealHeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL(WINAPI* RealHeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID(WINAPI* RealHeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T(WINAPI* RealHeapSize)(HANDLE, DWORD, LPCVOID);
static HANDLE(WINAPI* RealGetProcessHeap)();
static std::atomic<int64_t> g_heapLive{0}, g_heapPeak{0};
static std::atomic<uint64_t> g_heapAllocs{0}, g_heapFrees{0}, g_heapBig{0};

static void NoteHeap(int64_t delta) {
    int64_t now = (g_heapLive += delta);
    int64_t peak = g_heapPeak.load();
    while (now > peak && !g_heapPeak.compare_exchange_weak(peak, now)) {}
}

static LPVOID WINAPI Hook_HeapAlloc(HANDLE h, DWORD f, SIZE_T n) {
    LPVOID p = RealHeapAlloc(h, f, n);
    g_heapAllocs++;
    if (p) NoteHeap((int64_t)n);
    if (n >= (4 << 20)) {
        g_heapBig++;
        Log("HeapAlloc %llu KB from %s", (unsigned long long)(n >> 10), Where((uintptr_t)_ReturnAddress()).c_str());
    }
    return p;
}

static BOOL WINAPI Hook_HeapFree(HANDLE h, DWORD f, LPVOID p) {
    if (p) {
        g_heapFrees++;
        NoteHeap(-(int64_t)RealHeapSize(h, 0, p));
    }
    return RealHeapFree(h, f, p);
}

static LPVOID WINAPI Hook_HeapReAlloc(HANDLE h, DWORD f, LPVOID p, SIZE_T n) {
    SIZE_T old = p ? RealHeapSize(h, 0, p) : 0;
    LPVOID q = RealHeapReAlloc(h, f, p, n);
    if (q) NoteHeap((int64_t)n - (int64_t)old);
    return q;
}

static HANDLE WINAPI Hook_GetProcessHeap() {
    Count("GetProcessHeap", (uintptr_t)_ReturnAddress());
    return RealGetProcessHeap();
}

static HLOCAL(WINAPI* RealLocalAlloc)(UINT, SIZE_T);
static HLOCAL WINAPI Hook_LocalAlloc(UINT f, SIZE_T n) {
    Count("LocalAlloc", (uintptr_t)_ReturnAddress());
    return RealLocalAlloc(f, n);
}

// ------------------------------------------------------------------ waits, sync objects, files, loop
static void(WINAPI* RealSleep)(DWORD);
static void WINAPI Hook_Sleep(DWORD ms) {
    Count(ms == 0 ? "Sleep(0)" : "Sleep", (uintptr_t)_ReturnAddress());
    RealSleep(ms);
}

static DWORD(WINAPI* RealWFSO)(HANDLE, DWORD);
static DWORD WINAPI Hook_WFSO(HANDLE h, DWORD ms) {
    Count(ms == INFINITE ? "WaitForSingleObject(INF)" : "WaitForSingleObject", (uintptr_t)_ReturnAddress());
    return RealWFSO(h, ms);
}

static DWORD(WINAPI* RealWFMO)(DWORD, const HANDLE*, BOOL, DWORD);
static DWORD WINAPI Hook_WFMO(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    Count(all ? "WaitForMultipleObjects(all)" : "WaitForMultipleObjects", (uintptr_t)_ReturnAddress());
    return RealWFMO(n, hs, all, ms);
}

static BOOL(WINAPI* RealGetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL WINAPI Hook_GetOverlappedResult(HANDLE f, LPOVERLAPPED o, LPDWORD n, BOOL wait) {
    Count(wait ? "GetOverlappedResult(wait)" : "GetOverlappedResult", (uintptr_t)_ReturnAddress());
    return RealGetOverlappedResult(f, o, n, wait);
}

static BOOL(WINAPI* RealReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI Hook_ReadFile(HANDLE f, LPVOID b, DWORD n, LPDWORD r, LPOVERLAPPED o) {
    Count(o ? "ReadFile(overlapped)" : "ReadFile", (uintptr_t)_ReturnAddress());
    return RealReadFile(f, b, n, r, o);
}

static HANDLE(WINAPI* RealCreateEventA)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
static HANDLE WINAPI Hook_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init, LPCSTR name) {
    Count(manual ? "CreateEventA(manual)" : "CreateEventA(auto)", (uintptr_t)_ReturnAddress());
    return RealCreateEventA(sa, manual, init, name);
}

static HANDLE(WINAPI* RealCreateSemaphoreA)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
static HANDLE WINAPI Hook_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name) {
    Count("CreateSemaphoreA", (uintptr_t)_ReturnAddress());
    return RealCreateSemaphoreA(sa, init, max, name);
}

static HANDLE(WINAPI* RealCreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    Count("CreateMutexA", (uintptr_t)_ReturnAddress());
    return RealCreateMutexA(sa, own, name);
}

static HANDLE(WINAPI* RealCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    HANDLE h = RealCreateFileW(p, a, s, sa, c, f, t);
    static std::atomic<int> logged{0};
    if (logged++ < 300) Log("CreateFileW '%ls' access=%X flags=%X = %p from %s", p, a, f, h, Where((uintptr_t)_ReturnAddress()).c_str());
    return h;
}

static HANDLE(WINAPI* RealCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI Hook_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    HANDLE h = RealCreateFileA(p, a, s, sa, c, f, t);
    static std::atomic<int> logged{0};
    if (logged++ < 300) Log("CreateFileA '%s' access=%X flags=%X = %p from %s", p, a, f, h, Where((uintptr_t)_ReturnAddress()).c_str());
    return h;
}

static BOOL(WINAPI* RealPeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI Hook_PeekMessageW(LPMSG m, HWND w, UINT lo, UINT hi, UINT r) {
    Count("PeekMessageW", (uintptr_t)_ReturnAddress());
    return RealPeekMessageW(m, w, lo, hi, r);
}

static HMODULE(WINAPI* RealLoadLibraryA)(LPCSTR);
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR n) {
    HMODULE m = RealLoadLibraryA(n);
    Log("LoadLibraryA('%s') = %p from %s", n, m, Where((uintptr_t)_ReturnAddress()).c_str());
    return m;
}

static HMODULE(WINAPI* RealLoadLibraryW)(LPCWSTR);
static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR n) {
    HMODULE m = RealLoadLibraryW(n);
    Log("LoadLibraryW('%ls') = %p from %s", n, m, Where((uintptr_t)_ReturnAddress()).c_str());
    return m;
}

static HMODULE(WINAPI* RealLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) {
    HMODULE m = RealLoadLibraryExW(n, f, fl);
    Log("LoadLibraryExW('%ls', %X) = %p from %s", n, fl, m, Where((uintptr_t)_ReturnAddress()).c_str());
    return m;
}

static FARPROC(WINAPI* RealGetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI Hook_GetProcAddress(HMODULE m, LPCSTR n) {
    FARPROC p = RealGetProcAddress(m, n);
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::string, int>* seen = new std::map<std::string, int>;
    if ((uintptr_t)n > 0xFFFF) {
        char mn[MAX_PATH] = "?";
        GetModuleFileNameA(m, mn, MAX_PATH);
        std::string k = std::string(strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn) + "!" + n;
        AcquireSRWLockExclusive(&lock);
        bool first = (*seen)[k]++ == 0;
        ReleaseSRWLockExclusive(&lock);
        if (first) Log("GetProcAddress %s = %p from %s", k.c_str(), p, Where((uintptr_t)_ReturnAddress()).c_str());
    }
    return p;
}

static HRESULT(WINAPI* RealCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static HRESULT WINAPI Hook_CoCreateInstance(REFCLSID c, LPUNKNOWN o, DWORD ctx, REFIID i, LPVOID* out) {
    HRESULT hr = RealCoCreateInstance(c, o, ctx, i, out);
    Log("CoCreateInstance {%08lX-%04X-%04X-...} iid {%08lX-...} = %08lX from %s", c.Data1, c.Data2, c.Data3, i.Data1,
        hr, Where((uintptr_t)_ReturnAddress()).c_str());
    return hr;
}

static DWORD(WINAPI* RealTlsAlloc)();
static DWORD WINAPI Hook_TlsAlloc() {
    DWORD i = RealTlsAlloc();
    Log("TlsAlloc = %u from %s", i, Where((uintptr_t)_ReturnAddress()).c_str());
    return i;
}

static UINT(WINAPI* RealGetWriteWatch)(DWORD, PVOID, SIZE_T, PVOID*, ULONG_PTR*, LPDWORD);
static UINT WINAPI Hook_GetWriteWatch(DWORD f, PVOID b, SIZE_T s, PVOID* a, ULONG_PTR* c, LPDWORD g) {
    Count("GetWriteWatch", (uintptr_t)_ReturnAddress());
    return RealGetWriteWatch(f, b, s, a, c, g);
}

// ------------------------------------------------------------------ periodic report
typedef LONG(NTAPI* NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);

static void ReportThreads() {
    auto query = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        void* start = nullptr;
        struct { LONG status; void* teb; void* pid; void* tid; ULONG_PTR affinity; LONG prio, basePrio; } tbi = {};
        if (th) {
            query(th, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr);
            query(th, 0 /*ThreadBasicInformation*/, &tbi, 48, nullptr);
            CloseHandle(th);
        }
        size_t used = 0, reserved = 0;
        if (tbi.teb) {
            auto* tib = (NT_TIB*)tbi.teb;
            void* dealloc = *(void**)((uint8_t*)tbi.teb + 0x1478);   // TEB.DeallocationStack
            used = (uintptr_t)tib->StackBase - (uintptr_t)tib->StackLimit;
            reserved = (uintptr_t)tib->StackBase - (uintptr_t)dealloc;
        }
        std::string name;
        AcquireSRWLockShared(&g_threadLock);
        auto it = g_threads->find(te.th32ThreadID);
        if (it != g_threads->end()) name = it->second.name;
        ReleaseSRWLockShared(&g_threadLock);
        Log("  thread %5u %-28s start %-32s stack committed %4llu KB / %5llu KB%s", te.th32ThreadID, name.c_str(),
            Where((uintptr_t)start).c_str(), (unsigned long long)(used >> 10), (unsigned long long)(reserved >> 10),
            te.th32ThreadID == g_mainTid ? " (main)" : "");
    }
    CloseHandle(snap);
}

static void ReportMemory() {
    struct Bucket { uint64_t bytes = 0; int regions = 0; };
    std::map<std::string, Bucket> by;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    uint64_t total = 0;
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
            std::string k = std::string(mbi.Type == MEM_PRIVATE ? "private" : "mapped") + (writable ? " rw" : " ro");
            // Name big allocations by size class so the game's heaps stand out.
            by[k].bytes += mbi.RegionSize;
            by[k].regions++;
            total += mbi.RegionSize;
        }
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (p < (uintptr_t)mbi.BaseAddress) break;
    }
    for (auto& [k, b] : by) Log("  mem %-12s %8llu MB in %d regions", k.c_str(), (unsigned long long)(b.bytes >> 20), b.regions);

    // Largest private allocations (by allocation base), to find the rage heaps.
    std::map<uintptr_t, uint64_t> allocs;
    p = 0;
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) allocs[(uintptr_t)mbi.AllocationBase] += mbi.RegionSize;
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (p < (uintptr_t)mbi.BaseAddress) break;
    }
    std::vector<std::pair<uint64_t, uintptr_t>> big;
    for (auto& [b, n] : allocs) big.push_back({n, b});
    std::sort(big.rbegin(), big.rend());
    for (size_t i = 0; i < big.size() && i < 25; i++) {
        MEMORY_BASIC_INFORMATION a;
        VirtualQuery((void*)big[i].second, &a, sizeof(a));
        size_t reserve = 0;
        for (uintptr_t q = big[i].second;; ) {
            MEMORY_BASIC_INFORMATION r;
            if (!VirtualQuery((void*)q, &r, sizeof(r)) || r.AllocationBase != (void*)big[i].second) break;
            reserve += r.RegionSize;
            q += r.RegionSize;
        }
        Log("  alloc %p committed %6llu MB of %6llu MB reserved", (void*)big[i].second,
            (unsigned long long)(big[i].first >> 20), (unsigned long long)(reserve >> 20));
    }
}

static void ReportStats() {
    std::vector<std::pair<uint64_t, Key>> rows;
    AcquireSRWLockExclusive(&g_statLock);
    for (auto& [k, n] : *g_stats) rows.push_back({n, k});
    g_stats->clear();
    ReleaseSRWLockExclusive(&g_statLock);
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
        int c = strcmp(a.second.fn, b.second.fn);
        if (c) return c < 0;
        return a.first > b.first;
    });
    for (auto& [n, k] : rows)
        Log("  %-28s tid %5u from +%-8X x %llu", k.fn, k.tid, k.rva, (unsigned long long)n);
}

static DWORD WINAPI Reporter(void*) {
    for (int round = 1;; round++) {
        Sleep(round == 1 ? 15000 : 30000);
        Log("==== report %d: heap live %lld MB (peak %lld MB), allocs %llu, frees %llu, >=4MB %llu; VirtualAlloc calls %llu, committed %lld MB",
            round, (long long)(g_heapLive >> 20), (long long)(g_heapPeak >> 20), (unsigned long long)g_heapAllocs,
            (unsigned long long)g_heapFrees, (unsigned long long)g_heapBig, (unsigned long long)g_vaCalls,
            (long long)(g_vaCommitted >> 20));
        ReportStats();
        ReportThreads();
        ReportMemory();
    }
}

// ------------------------------------------------------------------ entry
#define HOOK(dll, name, hook, real) real = (decltype(real))PatchIat(exe, dll, name, (void*)&hook)

static void Init(HMODULE self) {
    InitializeCriticalSection(&g_logCs);
    g_log = _fsopen("E:\\Claude Projects\\gta_5_savestates\\lab\\recon.log", "w", _SH_DENYNO);
    g_stats = new std::map<Key, uint64_t>;
    g_threads = new std::map<DWORD, ThreadInfo>;
    g_mainTid = GetCurrentThreadId();
    HMODULE exe = GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_lo = (uintptr_t)exe;
    g_hi = g_lo + nt->OptionalHeader.SizeOfImage;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    Log("recon: %s at %p, main thread %u, command line: %s", path, exe, g_mainTid, GetCommandLineA());
    AddVectoredExceptionHandler(1, &Veh);

    HOOK("KERNEL32.dll", "VirtualAlloc", Hook_VirtualAlloc, RealVirtualAlloc);
    HOOK("KERNEL32.dll", "VirtualFree", Hook_VirtualFree, RealVirtualFree);
    HOOK("KERNEL32.dll", "HeapAlloc", Hook_HeapAlloc, RealHeapAlloc);
    HOOK("KERNEL32.dll", "HeapFree", Hook_HeapFree, RealHeapFree);
    HOOK("KERNEL32.dll", "HeapReAlloc", Hook_HeapReAlloc, RealHeapReAlloc);
    RealHeapSize = (decltype(RealHeapSize))GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSize");
    HOOK("KERNEL32.dll", "GetProcessHeap", Hook_GetProcessHeap, RealGetProcessHeap);
    HOOK("KERNEL32.dll", "LocalAlloc", Hook_LocalAlloc, RealLocalAlloc);
    HOOK("KERNEL32.dll", "CreateThread", Hook_CreateThread, RealCreateThread);
    HOOK("KERNEL32.dll", "Sleep", Hook_Sleep, RealSleep);
    HOOK("KERNEL32.dll", "WaitForSingleObject", Hook_WFSO, RealWFSO);
    HOOK("KERNEL32.dll", "WaitForMultipleObjects", Hook_WFMO, RealWFMO);
    HOOK("KERNEL32.dll", "GetOverlappedResult", Hook_GetOverlappedResult, RealGetOverlappedResult);
    HOOK("KERNEL32.dll", "ReadFile", Hook_ReadFile, RealReadFile);
    HOOK("KERNEL32.dll", "CreateEventA", Hook_CreateEventA, RealCreateEventA);
    HOOK("KERNEL32.dll", "CreateSemaphoreA", Hook_CreateSemaphoreA, RealCreateSemaphoreA);
    HOOK("KERNEL32.dll", "CreateMutexA", Hook_CreateMutexA, RealCreateMutexA);
    HOOK("KERNEL32.dll", "CreateFileW", Hook_CreateFileW, RealCreateFileW);
    HOOK("KERNEL32.dll", "CreateFileA", Hook_CreateFileA, RealCreateFileA);
    HOOK("KERNEL32.dll", "LoadLibraryA", Hook_LoadLibraryA, RealLoadLibraryA);
    HOOK("KERNEL32.dll", "LoadLibraryW", Hook_LoadLibraryW, RealLoadLibraryW);
    HOOK("KERNEL32.dll", "LoadLibraryExW", Hook_LoadLibraryExW, RealLoadLibraryExW);
    HOOK("KERNEL32.dll", "GetProcAddress", Hook_GetProcAddress, RealGetProcAddress);
    HOOK("KERNEL32.dll", "TlsAlloc", Hook_TlsAlloc, RealTlsAlloc);
    HOOK("KERNEL32.dll", "GetWriteWatch", Hook_GetWriteWatch, RealGetWriteWatch);
    HOOK("USER32.dll", "PeekMessageW", Hook_PeekMessageW, RealPeekMessageW);
    HOOK("ole32.dll", "CoCreateInstance", Hook_CoCreateInstance, RealCoCreateInstance);
    Log("hooks installed");
    CloseHandle(CreateThread(nullptr, 0, &Reporter, nullptr, 0, nullptr));
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (!spawn::InGameProcess()) spawn::Install(&PatchIat);
        else Init(h);
    }
    return TRUE;
}
