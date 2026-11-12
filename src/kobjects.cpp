#include "kobjects.h"
#include "threads.h"
#include <algorithm>
#include <cstdio>
#include <intrin.h>
#include <map>
#include <unordered_map>

namespace kobj {

struct Obj {
    Saved d;                     // description (h, kind, a/b/c, path); state fields unused here
    bool closedByGame = false;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<HANDLE, Obj>* g_objs;
static std::map<OVERLAPPED*, HANDLE>* g_pending;   // overlapped disk I/O the game started that may still be in flight
static volatile LONG g_keep = 0;                  // savestates exist: hold back closes

static BOOL(WINAPI* RealCloseHandle)(HANDLE) = &CloseHandle;

// ------------------------------------------------------------------ the handle block
constexpr uintptr_t kBlockLo = 0x40000, kBlockHi = 0x60000;
static CRITICAL_SECTION g_placeLock;
static std::vector<HANDLE>* g_free;   // block values holding a dummy event, lowest last (handed out first)
static bool g_blockReady = false;

static bool InBlock(HANDLE h) { return (uintptr_t)h >= kBlockLo && (uintptr_t)h < kBlockHi; }

void Init() {
    InitializeCriticalSection(&g_placeLock);
    g_objs = new std::unordered_map<HANDLE, Obj>;
    g_pending = new std::map<OVERLAPPED*, HANDLE>;
    g_free = new std::vector<HANDLE>;
    // Handle values are given out from the bottom of the process's handle table: fill it with dummy events up to
    // the end of the block, keep the ones inside the block and give the lower ones back.
    std::vector<HANDLE> below;
    for (;;) {
        HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!h) break;
        if ((uintptr_t)h < kBlockLo) below.push_back(h);
        else if ((uintptr_t)h < kBlockHi) g_free->push_back(h);
        else { RealCloseHandle(h); break; }
    }
    for (HANDLE h : below) RealCloseHandle(h);
    std::sort(g_free->begin(), g_free->end(), [](HANDLE a, HANDLE b) { return a > b; });
    g_blockReady = g_free->size() > 1000;
    Log("handle block: %u values reserved (%u below given back)", (unsigned)g_free->size(), (unsigned)below.size());
}

// Moves `obj` to value `v` (which must hold one of our dummies): close the dummy and duplicate at once; Windows hands
// out the entry it has just freed. Returns false if another thread got the value in between.
static bool PutAt(HANDLE v, HANDLE obj) {
    for (int attempt = 0; attempt < 8; attempt++) {
        RealCloseHandle(v);
        HANDLE got = nullptr;
        DuplicateHandle(GetCurrentProcess(), obj, GetCurrentProcess(), &got, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (got == v) return true;
        // Somewhere else: give it back and get v back with a dummy, then try again.
        std::vector<HANDLE> misses;
        if (got) misses.push_back(got);
        bool regained = false;
        for (int i = 0; i < 64 && !regained; i++) {
            HANDLE d = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (d == v) regained = true;
            else misses.push_back(d);
        }
        for (HANDLE m : misses) RealCloseHandle(m);
        if (!regained) return false;
    }
    return false;
}

// Puts a dummy back at a block value that the game no longer uses, and returns the value to the free list.
static void Recycle(HANDLE v) {
    EnterCriticalSection(&g_placeLock);
    RealCloseHandle(v);
    HANDLE d = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d == v) g_free->push_back(v);
    else RealCloseHandle(d);   // someone else took v: it is lost to the block
    LeaveCriticalSection(&g_placeLock);
}

static HANDLE PlaceValue(HANDLE real) {
    if (!real || real == INVALID_HANDLE_VALUE || !g_blockReady) return real;
    EnterCriticalSection(&g_placeLock);
    HANDLE placed = real;
    while (!g_free->empty()) {
        HANDLE v = g_free->back();
        g_free->pop_back();
        if (PutAt(v, real)) {
            RealCloseHandle(real);
            placed = v;
            break;
        }
    }
    LeaveCriticalSection(&g_placeLock);
    if (placed == real) Log("handle block full or contended: %p stays where it is", real);
    return placed;
}

// Replaces whatever is at block value v by obj (for loads). v must hold a dummy or a game object: a value the block
// lost to another thread (see Recycle) holds someone else's handle, which stays.
static bool Install(HANDLE v, HANDLE obj) {
    EnterCriticalSection(&g_placeLock);
    auto it = std::find(g_free->begin(), g_free->end(), v);
    bool ours = it != g_free->end();
    if (ours) g_free->erase(it);
    if (!ours) {
        AcquireSRWLockShared(&g_lock);
        ours = g_objs->count(v) != 0;
        ReleaseSRWLockShared(&g_lock);
    }
    bool ok = ours && PutAt(v, obj);
    LeaveCriticalSection(&g_placeLock);
    if (!ours) Log("load: handle value %p belongs to someone else now; the savestate's object can't go there", v);
    return ok;
}

HANDLE Ours(HANDLE h) {
    if (!g_blockReady || !InBlock(h)) return h;
    // A block value the block lost (see Recycle): give our object a value outside the block, and the values it had
    // back to the block as dummies.
    EnterCriticalSection(&g_placeLock);
    std::vector<HANDLE> inside;
    HANDLE out = h;
    for (int i = 0; i < 16 && InBlock(out); i++) {
        inside.push_back(out);
        HANDLE d = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) break;
        out = d;
    }
    for (HANDLE v : inside) {
        if (v == out) continue;
        RealCloseHandle(v);
        HANDLE d = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (d == v) g_free->push_back(v);
        else RealCloseHandle(d);
    }
    LeaveCriticalSection(&g_placeLock);
    return out;
}

// ------------------------------------------------------------------ creation hooks
static volatile LONG g_logCreates = 0;   // lab: log this many creations (handle values in creation order)
void LogCreates(int n) { InterlockedExchange(&g_logCreates, n); }

static HANDLE Track(HANDLE real, const Saved& d) {
    if (!real || real == INVALID_HANDLE_VALUE) return real;
    HANDLE h = PlaceValue(real);
    if (g_logCreates > 0 && InterlockedDecrement(&g_logCreates) >= 0) Log("handle %p kind %u", h, d.kind);
    AcquireSRWLockExclusive(&g_lock);
    Obj& o = (*g_objs)[h];
    o.d = d;
    o.d.h = h;
    o.closedByGame = false;
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

static Saved Desc(Kind k, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, std::wstring path = L"") {
    Saved d;
    d.kind = k;
    d.a = a;
    d.b = b;
    d.c = c;
    d.path = std::move(path);
    return d;
}

static std::wstring Wide(const char* s) {
    std::wstring w;
    if (!s) return w;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    w.resize(n > 0 ? n - 1 : 0);
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

void SetKeep() { InterlockedExchange(&g_keep, 1); }

HANDLE Place(HANDLE real, Kind k) { return Track(real, Desc(k)); }

static HANDLE(WINAPI* RealCreateEventA)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
static HANDLE WINAPI Hook_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL init, LPCSTR name) {
    return Track(RealCreateEventA(sa, manual, init, name), Desc(Event, manual ? 1 : 0));
}

static HANDLE(WINAPI* RealCreateSemaphoreA)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
static HANDLE WINAPI Hook_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name) {
    return Track(RealCreateSemaphoreA(sa, init, max, name), Desc(Semaphore, (uint32_t)max));
}

static HANDLE WINAPI Hook_CreateSemaphoreExW(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCWSTR name, DWORD flags, DWORD access) {
    return Track(CreateSemaphoreExW(sa, init, max, name, flags, access | SEMAPHORE_ALL_ACCESS), Desc(Semaphore, (uint32_t)max));
}

static HANDLE(WINAPI* RealCreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    return Track(RealCreateMutexA(sa, own, name), Desc(Mutex));
}

static bool ReadShared(DWORD access, DWORD share) {
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    return !(access & writes) && (share & FILE_SHARE_READ);
}

static HANDLE(WINAPI* RealCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI Hook_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    return Track(RealCreateFileA(p, a, s, sa, c, f, t), Desc(File, a, s, f, Wide(p)));
}

static HANDLE(WINAPI* RealCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    return Track(RealCreateFileW(p, a, s, sa, c, f, t), Desc(File, a, s, f, p ? p : L""));
}

static HANDLE(WINAPI* RealFindFirstChangeNotificationA)(LPCSTR, BOOL, DWORD);
static HANDLE WINAPI Hook_FindFirstChangeNotificationA(LPCSTR p, BOOL subtree, DWORD filter) {
    return Track(RealFindFirstChangeNotificationA(p, subtree, filter), Desc(ChangeNotify, subtree ? 1 : 0, filter, 0, Wide(p)));
}

// Events, semaphores, mutexes and thread handles stay open: a savestate may refer to them (and a closed handle's
// value would be reused for something else). Once savestates exist, so do files opened read-only and shareable: the
// game closes and reopens archives when a story save is loaded from the menu. Files it writes (save files) close
// for real, so they can be opened again.
static bool Close(HANDLE h, BOOL(WINAPI* realClose)(HANDLE)) {
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_objs->find(h);
    bool tracked = it != g_objs->end(), keep = false;
    if (tracked) {
        if (it->second.d.kind != File || (g_keep && ReadShared(it->second.d.a, it->second.d.b))) keep = it->second.closedByGame = true;
        else g_objs->erase(it);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (keep) return true;
    if (tracked && InBlock(h)) {
        Recycle(h);   // closes it and keeps the value ours
        return true;
    }
    return realClose(h) != FALSE;
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) { return Close(h, RealCloseHandle); }

static BOOL(WINAPI* RealFindCloseChangeNotification)(HANDLE);
static BOOL WINAPI Hook_FindCloseChangeNotification(HANDLE h) { return Close(h, RealFindCloseChangeNotification); }

// Only reads and writes of disk files must be finished before a savestate. Pipes (the launcher emulation talks to the
// game through one) keep a read pending all the time, and that read stays pending across a savestate anyway.
static void NotePending(HANDLE f, LPOVERLAPPED ov, BOOL ok) {
    if (!ov || ok || GetLastError() != ERROR_IO_PENDING) return;
    if (GetFileType(f) != FILE_TYPE_DISK) {
        SetLastError(ERROR_IO_PENDING);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    (*g_pending)[ov] = f;
    ReleaseSRWLockExclusive(&g_lock);
    SetLastError(ERROR_IO_PENDING);
}

static BOOL(WINAPI* RealReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI Hook_ReadFile(HANDLE f, LPVOID b, DWORD n, LPDWORD r, LPOVERLAPPED ov) {
    BOOL ok = RealReadFile(f, b, n, r, ov);
    NotePending(f, ov, ok);
    return ok;
}

static BOOL(WINAPI* RealWriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI Hook_WriteFile(HANDLE f, LPCVOID b, DWORD n, LPDWORD w, LPOVERLAPPED ov) {
    BOOL ok = RealWriteFile(f, b, n, w, ov);
    NotePending(f, ov, ok);
    return ok;
}

void InstallHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
#define HOOK(name) Real##name = (decltype(Real##name))PatchIat(exe, "kernel32.dll", #name, (void*)&Hook_##name); \
    if (!Real##name) Real##name = &name
    HOOK(CreateEventA);
    HOOK(CreateSemaphoreA);
    HOOK(CreateMutexA);
    HOOK(CreateFileA);
    HOOK(CreateFileW);
    HOOK(FindFirstChangeNotificationA);
    HOOK(FindCloseChangeNotification);
    HOOK(CloseHandle);
    HOOK(ReadFile);
    HOOK(WriteFile);
#undef HOOK
}

void* HookForProcAddress(const char* name) {
    if (!strcmp(name, "CreateSemaphoreExW")) return (void*)&Hook_CreateSemaphoreExW;
    return nullptr;
}

bool IoQuiet(std::string* why) {
    AcquireSRWLockExclusive(&g_lock);
    int busy = 0;
    HANDLE example = nullptr;
    for (auto it = g_pending->begin(); it != g_pending->end();) {
        if (it->first->Internal == (ULONG_PTR)STATUS_PENDING) { busy++; example = it->second; ++it; }
        else it = g_pending->erase(it);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (busy && why) {
        char path[MAX_PATH] = "?";
        GetFinalPathNameByHandleA(example, path, MAX_PATH, 0);
        *why = std::to_string(busy) + " file reads in flight (" + path + ")";
    }
    return busy == 0;
}

// ------------------------------------------------------------------ states
struct EventInfo { int type; LONG state; };
struct SemaphoreInfo { LONG count, max; };
struct MutantInfo { LONG count; BOOLEAN ownedByCaller, abandoned; };
struct ClientId { HANDLE process, thread; };

// Owner thread and recursion count of a mutex (0, 0 when it is free).
static bool MutexState(HANDLE h, DWORD* owner, LONG* recursion) {
    MutantInfo mi = {};
    if (nt::QueryMutant(h, 0, &mi, sizeof(mi), nullptr) < 0) return false;
    *owner = 0;
    *recursion = mi.count > 0 ? 0 : 1 - mi.count;
    if (!*recursion) return true;
    ClientId ci = {};
    if (nt::QueryMutant(h, 1, &ci, sizeof(ci), nullptr) < 0) return false;   // MutantOwnerInformation
    *owner = (DWORD)(uintptr_t)ci.thread;
    return true;
}

std::string Describe(HANDLE h) {
    char b[160];
    EventInfo ei = {};
    SemaphoreInfo si = {};
    AcquireSRWLockShared(&g_lock);
    auto it = g_objs->find(h);
    const char* tracked = it == g_objs->end() ? "untracked " : (it->second.closedByGame ? "closed-by-game " : "");
    ReleaseSRWLockShared(&g_lock);
    if (nt::QueryEvent(h, 0, &ei, sizeof(ei), nullptr) >= 0)
        sprintf_s(b, "%s%s event %p (%s)", tracked, ei.type ? "auto" : "manual", h, ei.state ? "set" : "not set");
    else if (nt::QuerySemaphore(h, 0, &si, sizeof(si), nullptr) >= 0)
        sprintf_s(b, "%ssemaphore %p (count %ld of %ld)", tracked, h, si.count, si.max);
    else if (DWORD owner = 0, rec = 0; MutexState(h, &owner, (LONG*)&rec))
        sprintf_s(b, "%smutex %p (owner thread %u, %u times)", tracked, h, owner, rec);
    else
        sprintf_s(b, "%shandle %p", tracked, h);
    return b;
}

std::vector<Saved> Capture() {
    std::vector<Saved> out;
    AcquireSRWLockShared(&g_lock);
    out.reserve(g_objs->size());
    for (auto& [h, o] : *g_objs) {
        if (o.closedByGame && o.d.kind != File) continue;   // the game can't use it any more
        Saved s = o.d;
        s.state = 0;
        s.pos = -1;
        if (s.kind == Event) {
            EventInfo ei = {};
            if (nt::QueryEvent(h, 0, &ei, sizeof(ei), nullptr) >= 0) s.state = ei.state;
        } else if (s.kind == Semaphore) {
            SemaphoreInfo si = {};
            if (nt::QuerySemaphore(h, 0, &si, sizeof(si), nullptr) >= 0) s.state = si.count;
        } else if (s.kind == Mutex) {
            DWORD owner = 0;
            LONG rec = 0;
            if (MutexState(h, &owner, &rec)) {
                s.state = rec;
                s.pos = threads::GameTid(owner);   // as the game knows the owner
            }
        } else if (s.kind == ThreadHandle) {
            s.pos = threads::GameTidOfHandle(h);
        } else if (s.kind == File && !(s.c & FILE_FLAG_OVERLAPPED) && !o.closedByGame) {
            LARGE_INTEGER pos = {}, zero = {};
            if (SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) s.pos = pos.QuadPart;
        }
        out.push_back(s);
    }
    ReleaseSRWLockShared(&g_lock);
    return out;
}

// ------------------------------------------------------------------ file encoding
std::vector<uint8_t> Encode(const std::vector<Saved>& saved) {
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    for (const Saved& s : saved) {
        uint64_t h = (uint64_t)s.h;
        uint32_t len = (uint32_t)s.path.size();
        put(&h, 8); put(&s.kind, 4); put(&s.a, 4); put(&s.b, 4); put(&s.c, 4); put(&s.state, 4); put(&s.pos, 8); put(&len, 4);
        put(s.path.data(), len * 2);
    }
    return out;
}

bool Decode(const uint8_t* p, size_t n, std::vector<Saved>* out) {
    const uint8_t* end = p + n;
    auto get = [&](void* d, size_t k) {
        if ((size_t)(end - p) < k) return false;
        memcpy(d, p, k);
        p += k;
        return true;
    };
    while (p < end) {
        Saved s;
        uint64_t h = 0;
        uint32_t len = 0;
        if (!get(&h, 8) || !get(&s.kind, 4) || !get(&s.a, 4) || !get(&s.b, 4) || !get(&s.c, 4) || !get(&s.state, 4) ||
            !get(&s.pos, 8) || !get(&len, 4))
            return false;
        s.h = (HANDLE)h;
        s.path.resize(len);
        if (len && !get(&s.path[0], len * 2)) return false;
        out->push_back(std::move(s));
    }
    return true;
}

// ------------------------------------------------------------------ restore
static bool SameObject(const Saved& live, const Saved& want) {
    return live.kind == want.kind && live.a == want.a && live.b == want.b && live.c == want.c &&
           _wcsicmp(live.path.c_str(), want.path.c_str()) == 0;
}

// A new object like the saved one (not yet at its value).
static HANDLE Make(const Saved& s) {
    switch (s.kind) {
    case Event: return CreateEventW(nullptr, s.a != 0, FALSE, nullptr);
    case Semaphore: return CreateSemaphoreW(nullptr, 0, s.a ? (LONG)s.a : 0x7FFFFFFF, nullptr);
    case Mutex: return CreateMutexW(nullptr, FALSE, nullptr);
    case File: {
        HANDLE f = RealCreateFileW(s.path.c_str(), s.a, s.b, nullptr, OPEN_EXISTING, s.c, nullptr);
        return f == INVALID_HANDLE_VALUE ? nullptr : f;
    }
    case ChangeNotify: {
        HANDLE h = FindFirstChangeNotificationW(s.path.c_str(), s.a != 0, s.b);
        return h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
    default: return nullptr;
    }
}

std::vector<threads::MutexOp> Restore(const std::vector<Saved>& saved, bool recreate, DWORD (*liveTid)(DWORD),
                                      HANDLE (*threadHandle)(DWORD)) {
    std::vector<threads::MutexOp> ops;
    int events = 0, semaphores = 0, files = 0, mutexes = 0, made = 0, failed = 0;
    for (const Saved& s : saved) {
        // The object at this value: the live one if it is like the saved one, else a new one.
        AcquireSRWLockShared(&g_lock);
        auto it = g_objs->find(s.h);
        bool same = it != g_objs->end() && SameObject(it->second.d, s);
        ReleaseSRWLockShared(&g_lock);
        if (s.kind == ThreadHandle) {
            HANDLE th = threadHandle((DWORD)s.pos);
            HANDLE dup = nullptr;
            same = same && !recreate && GetThreadId(s.h) == GetThreadId(th);
            if (!same && th && DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                if (Install(s.h, dup)) made++; else failed++;
                RealCloseHandle(dup);
            }
        } else if (!same || recreate) {
            HANDLE obj = Make(s);
            if (!obj && s.kind == File) {
                // Can't be opened again (a pipe to the launcher has one end): the live session's own handle to the
                // same file, under the savestate's value.
                AcquireSRWLockShared(&g_lock);
                for (auto& [h, o] : *g_objs)
                    if (h != s.h && !o.closedByGame && SameObject(o.d, s) &&
                        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &obj, 0, FALSE, DUPLICATE_SAME_ACCESS))
                        break;
                ReleaseSRWLockShared(&g_lock);
            }
            if (obj && Install(s.h, obj)) made++;
            else {
                failed++;
                Log("load: could not recreate %s handle %p (kind %u, %ls)", s.kind == File ? "file" : "kernel", s.h, s.kind, s.path.c_str());
            }
            if (obj) RealCloseHandle(obj);
        }
        AcquireSRWLockExclusive(&g_lock);
        Obj& o = (*g_objs)[s.h];
        o.d = s;
        o.d.state = 0;
        o.d.pos = -1;
        o.closedByGame = false;   // in use again in the restored memory
        ReleaseSRWLockExclusive(&g_lock);

        if (s.kind == Event) {
            EventInfo ei = {};
            if (nt::QueryEvent(s.h, 0, &ei, sizeof(ei), nullptr) < 0) continue;
            if (ei.state != s.state) {
                if (s.state) SetEvent(s.h); else ResetEvent(s.h);
                events++;
            }
        } else if (s.kind == Semaphore) {
            SemaphoreInfo si = {};
            if (nt::QuerySemaphore(s.h, 0, &si, sizeof(si), nullptr) < 0) continue;
            if (si.count < s.state) ReleaseSemaphore(s.h, s.state - si.count, nullptr);
            for (LONG i = s.state; i < si.count; i++) threads::RealWaitForSingleObject(s.h, 0);
            if (si.count != s.state) semaphores++;
        } else if (s.kind == Mutex) {
            DWORD owner = 0;
            LONG rec = 0;
            if (!MutexState(s.h, &owner, &rec)) continue;
            DWORD want = s.state ? liveTid((DWORD)s.pos) : 0;
            if (owner == want && rec == s.state) continue;
            if (rec) ops.push_back({owner, s.h, (int)rec, 0});         // its current owner lets go
            if (s.state) ops.push_back({want, s.h, 0, (int)s.state});  // the thread that continues the saved owner takes it
            mutexes++;
        } else if (s.kind == File && s.pos >= 0) {
            LARGE_INTEGER pos;
            pos.QuadPart = s.pos;
            SetFilePointerEx(s.h, pos, nullptr, FILE_BEGIN);
            files++;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_pending->clear();
    ReleaseSRWLockExclusive(&g_lock);
    Log("kernel objects: %d events, %d semaphores, %d mutexes changed, %d file positions set; %d objects made, %d failed",
        events, semaphores, mutexes, files, made, failed);
    return ops;
}

}  // namespace kobj
