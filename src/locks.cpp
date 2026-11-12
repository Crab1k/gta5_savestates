#include "locks.h"
#include <set>

namespace locks {

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::set<uint64_t>* g_list;

static void Add(void* cs) {
    AcquireSRWLockExclusive(&g_lock);
    g_list->insert((uint64_t)cs);
    ReleaseSRWLockExclusive(&g_lock);
}

static void (WINAPI* RealInit)(LPCRITICAL_SECTION) = &InitializeCriticalSection;
static BOOL (WINAPI* RealInitSpin)(LPCRITICAL_SECTION, DWORD) = &InitializeCriticalSectionAndSpinCount;
static BOOL (WINAPI* RealInitEx)(LPCRITICAL_SECTION, DWORD, DWORD);
static void (WINAPI* RealDelete)(LPCRITICAL_SECTION) = &DeleteCriticalSection;

static void WINAPI Hook_Init(LPCRITICAL_SECTION cs) {
    RealInit(cs);
    Add(cs);
}

static BOOL WINAPI Hook_InitSpin(LPCRITICAL_SECTION cs, DWORD spin) {
    BOOL ok = RealInitSpin(cs, spin);
    if (ok) Add(cs);
    return ok;
}

static BOOL WINAPI Hook_InitEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags) {
    BOOL ok = RealInitEx(cs, spin, flags);
    if (ok) Add(cs);
    return ok;
}

static void WINAPI Hook_Delete(LPCRITICAL_SECTION cs) {
    AcquireSRWLockExclusive(&g_lock);
    g_list->erase((uint64_t)cs);
    ReleaseSRWLockExclusive(&g_lock);
    RealDelete(cs);
}

void InstallHooks() {
    if (!g_list) g_list = new std::set<uint64_t>;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (void* p = PatchIat(exe, "kernel32.dll", "InitializeCriticalSection", (void*)&Hook_Init)) RealInit = (decltype(RealInit))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "InitializeCriticalSectionAndSpinCount", (void*)&Hook_InitSpin)) RealInitSpin = (decltype(RealInitSpin))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "InitializeCriticalSectionEx", (void*)&Hook_InitEx)) RealInitEx = (decltype(RealInitEx))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "DeleteCriticalSection", (void*)&Hook_Delete)) RealDelete = (decltype(RealDelete))p;
}

void* HookForProcAddress(const char* name) {
    if (!g_list) g_list = new std::set<uint64_t>;
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!strcmp(name, "InitializeCriticalSectionEx")) {
        if (!RealInitEx) RealInitEx = (decltype(RealInitEx))GetProcAddress(k, name);
        return RealInitEx ? (void*)&Hook_InitEx : nullptr;
    }
    if (!strcmp(name, "InitializeCriticalSectionAndSpinCount")) return (void*)&Hook_InitSpin;
    if (!strcmp(name, "InitializeCriticalSection")) return (void*)&Hook_Init;
    if (!strcmp(name, "DeleteCriticalSection")) return (void*)&Hook_Delete;
    return nullptr;
}

std::vector<uint64_t> List() {
    AcquireSRWLockShared(&g_lock);
    std::vector<uint64_t> v(g_list->begin(), g_list->end());
    ReleaseSRWLockShared(&g_lock);
    return v;
}

static bool Read(uint64_t a, RTL_CRITICAL_SECTION* out) {
    __try {
        *out = *(const RTL_CRITICAL_SECTION*)a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool Write(uint64_t a, const RTL_CRITICAL_SECTION& v) {
    __try {
        *(RTL_CRITICAL_SECTION*)a = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const PRTL_CRITICAL_SECTION_DEBUG kNoDebug = (PRTL_CRITICAL_SECTION_DEBUG)-1;

std::vector<std::pair<uint64_t, uint64_t>> DebugRecords() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (uint64_t a : List()) {
        RTL_CRITICAL_SECTION cs;
        if (Read(a, &cs) && cs.DebugInfo && cs.DebugInfo != kNoDebug) v.push_back({a, (uint64_t)cs.DebugInfo});
    }
    return v;
}

void Restore(const std::vector<uint64_t>& saved, bool foreign, DWORD (*liveTid)(DWORD),
             const std::vector<std::pair<uint64_t, uint64_t>>& liveDebug) {
    AcquireSRWLockExclusive(&g_lock);
    g_list->clear();
    g_list->insert(saved.begin(), saved.end());
    ReleaseSRWLockExclusive(&g_lock);
    if (!foreign) return;
    std::set<std::pair<uint64_t, uint64_t>> debug(liveDebug.begin(), liveDebug.end());
    int owned = 0, unknownOwner = 0, events = 0, records = 0, unreadable = 0;
    for (uint64_t a : saved) {
        RTL_CRITICAL_SECTION cs;
        if (!Read(a, &cs)) { unreadable++; continue; }
        if (cs.OwningThread) {
            DWORD now = liveTid((DWORD)(uintptr_t)cs.OwningThread);
            if (now) owned++; else unknownOwner++;
            cs.OwningThread = (HANDLE)(uintptr_t)now;
            if (!now) {   // owned by a thread this session has no counterpart of: free it
                cs.LockCount = -1;
                cs.RecursionCount = 0;
            }
        }
        if (cs.LockSemaphore) {
            cs.LockSemaphore = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            events++;
        }
        if (cs.DebugInfo && cs.DebugInfo != kNoDebug) {
            // Only this session's record for the same address is one ntdll knows.
            auto it = debug.lower_bound({a, 0});
            cs.DebugInfo = it != debug.end() && it->first == a ? (PRTL_CRITICAL_SECTION_DEBUG)it->second : kNoDebug;
            records++;
        }
        Write(a, cs);
    }
    Log("critical sections: %u restored; %d owners moved, %d owners unknown (freed), %d new events, %d debug records "
        "translated, %d unreadable", (unsigned)saved.size(), owned, unknownOwner, events, records, unreadable);
}

void Report(const char* why) {
    auto all = List();
    int owned = 0, events = 0, noDebug = 0, nullDebug = 0, debug = 0;
    for (uint64_t a : all) {
        RTL_CRITICAL_SECTION cs;
        if (!Read(a, &cs)) continue;
        if (cs.OwningThread) owned++;
        if (cs.LockSemaphore) events++;
        if (cs.DebugInfo == kNoDebug) noDebug++;
        else if (!cs.DebugInfo) nullDebug++;
        else debug++;
    }
    Log("critical sections (%s): %u; %d owned, %d with an event, debug records: %d none(-1), %d null, %d real", why,
        (unsigned)all.size(), owned, events, noDebug, nullDebug, debug);
}

}  // namespace locks
