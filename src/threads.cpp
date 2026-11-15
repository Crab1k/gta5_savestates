#include "threads.h"
#include "addr.h"
#include "inject.h"
#include "kobjects.h"
#include "memory.h"
#include "timewarp.h"
#include <intrin.h>
#include <malloc.h>
#include <tlhelp32.h>
#include <algorithm>
#include <map>

namespace threads {

DWORD(WINAPI* RealWaitForSingleObject)(HANDLE, DWORD) = &WaitForSingleObject;
void(WINAPI* RealSleep)(DWORD) = &Sleep;
static DWORD(WINAPI* RealWaitForMultipleObjects)(DWORD, const HANDLE*, BOOL, DWORD) = &WaitForMultipleObjects;
static BOOL(WINAPI* RealGetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL) = &GetOverlappedResult;
static HANDLE(WINAPI* RealCreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

static DWORD g_tls = TLS_OUT_OF_INDEXES;
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::map<DWORD, Thread*>* g_threads;
static HANDLE g_parkEvent;                 // manual reset, set while a park is requested
static volatile LONG g_parkRequested = 0;
static volatile LONG g_landed = 0;         // threads that passed their capture point (see Park)
static volatile LONG g_opsDone = 0;        // threads that finished their mutex operations
static std::map<DWORD, std::vector<MutexOp>>* g_ops;   // per thread, during RunMutexOps
static volatile LONG g_opsPhase = 0;       // 0: releases, 1: acquires
static volatile LONG g_keep = 0;           // savestates exist: ended threads become zombies
static volatile LONG g_zombies = 0;
static std::map<HANDLE, Thread*>* g_byHandle;   // game thread handles

static inline Thread* Self() { return (Thread*)TlsGetValue(g_tls); }

extern "C" uint64_t RunOnStack(void* top, uint64_t (*fn)(void*, void*), void* a, void* b);

// ------------------------------------------------------------------ stacks in the session window
// TEB fields of the stack (x64).
static void** TebStackBase(void* teb) { return (void**)((uint8_t*)teb + 0x08); }
static void** TebStackLimit(void* teb) { return (void**)((uint8_t*)teb + 0x10); }
static void** TebDeallocationStack(void* teb) { return (void**)((uint8_t*)teb + 0x1478); }
static void** TebTlsSlot(void* teb) {   // ThreadLocalStoragePointer[GTA5.exe's index]
    auto** slots = *(void***)((uint8_t*)teb + 0x58);
    uint8_t* block = mem::TlsBlock(teb);
    if (!slots || !block) return nullptr;
    for (int i = 0; i < 64; i++)
        if (slots[i] == block) return &slots[i];
    return nullptr;
}

// The calling thread's own GTA5.exe thread-local block and the fixed one it uses instead (OnWindowStack).
static thread_local void* t_ownTls = nullptr;
static thread_local void* t_slotTls = nullptr;

// Gives the thread its own thread-local block back (with the current contents) and frees the fixed one: Windows
// frees the block in the table when the thread ends, and ours is no heap block. The table may have been replaced
// since the swap (Windows grows every thread's table when a DLL with thread-local data is loaded), so the slot is
// looked up in the current one.
static void GiveBackTls() {
    if (!t_slotTls) return;
    void** slot = TebTlsSlot(NtCurrentTeb());
    if (slot && *slot == t_slotTls) {
        memcpy(t_ownTls, t_slotTls, mem::TlsSize());
        *slot = t_ownTls;
        VirtualFree(t_slotTls, 0, MEM_RELEASE);
    }
    t_slotTls = nullptr;
}

// Moves the calling thread's stack and GTA5.exe thread-local block to the fixed slots of thread number `ordinal` for
// the duration of fn, then puts the thread's own ones back (Windows frees them when the thread ends).
static uint64_t OnWindowStack(unsigned ordinal, size_t size, uint64_t (*fn)(void*, void*), void* a, void* b) {
    void* teb = NtCurrentTeb();
    auto* stack = ordinal < addr::kThreadSlots - 1 && size <= addr::kStackSlot
                      ? (uint8_t*)VirtualAlloc(addr::StackSlot(ordinal), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
                      : nullptr;
    if (!stack) {
        Log("thread %u: its stack slot is taken; it keeps its own stack", ordinal);
        return fn(a, b);
    }
    void** tlsSlot = TebTlsSlot(teb);
    void* oldTls = tlsSlot ? *tlsSlot : nullptr;
    if (tlsSlot) {
        size_t n = (mem::TlsSize() + 0xFFF) & ~(size_t)0xFFF;
        if (void* block = VirtualAlloc(addr::TlsSlot(ordinal), n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            memcpy(block, oldTls, mem::TlsSize());
            *tlsSlot = block;
            t_ownTls = oldTls;
            t_slotTls = block;
        }
    }
    void* oldBase = *TebStackBase(teb);
    void* oldLimit = *TebStackLimit(teb);
    void* oldDealloc = *TebDeallocationStack(teb);
    *TebDeallocationStack(teb) = stack;
    *TebStackLimit(teb) = stack;
    *TebStackBase(teb) = stack + size;
    uint64_t r = RunOnStack(stack + size, fn, a, b);
    *TebStackBase(teb) = oldBase;
    *TebStackLimit(teb) = oldLimit;
    *TebDeallocationStack(teb) = oldDealloc;
    GiveBackTls();
    // The thread is ending: its slots are free for a thread of the same number that a load may start again.
    VirtualFree(stack, 0, MEM_RELEASE);
    return r;
}

constexpr size_t kThreadStack = 1u << 20;
bool IsGameThread() { return Self() != nullptr; }
unsigned Ordinal() {
    Thread* t = Self();
    return t ? t->ordinal : addr::kThreadSlots - 1;
}
static volatile LONG g_nextOrdinal = 0;   // the main thread is 0
unsigned NextOrdinal() { return (unsigned)g_nextOrdinal + 1; }
void MoveNextOrdinalPast(unsigned ordinal) {
    for (LONG cur = g_nextOrdinal; cur < (LONG)ordinal; cur = g_nextOrdinal)
        if (InterlockedCompareExchange(&g_nextOrdinal, (LONG)ordinal, cur) == cur) break;
}

// The game's clock in ms. Deadlines of interrupted waits use it: it continues from the savestate's time on a load
// (also one from another session, where the real clock has nothing to do with the saved deadlines).
static LONGLONG g_qpcPerMs = 1;
static inline ULONGLONG Now() { return (ULONGLONG)(timewarp::Qpc() / g_qpcPerMs); }

// ------------------------------------------------------------------ the game's FLS / TLS slots
static std::vector<DWORD> g_flsIndices, g_tlsIndices;
static SRWLOCK g_slotLock = SRWLOCK_INIT;
const std::vector<DWORD>& FlsIndices() { return g_flsIndices; }
const std::vector<DWORD>& TlsIndices() { return g_tlsIndices; }

static DWORD(WINAPI* RealFlsAlloc)(PFLS_CALLBACK_FUNCTION);
static DWORD WINAPI Hook_FlsAlloc(PFLS_CALLBACK_FUNCTION cb) {
    DWORD i = RealFlsAlloc(cb);
    if (i != FLS_OUT_OF_INDEXES) {
        AcquireSRWLockExclusive(&g_slotLock);
        g_flsIndices.push_back(i);
        ReleaseSRWLockExclusive(&g_slotLock);
    }
    return i;
}

static DWORD(WINAPI* RealTlsAlloc)() = &TlsAlloc;
static DWORD WINAPI Hook_TlsAlloc() {
    DWORD i = RealTlsAlloc();
    if (i != TLS_OUT_OF_INDEXES) {
        AcquireSRWLockExclusive(&g_slotLock);
        g_tlsIndices.push_back(i);
        ReleaseSRWLockExclusive(&g_slotLock);
    }
    return i;
}

static void WINAPI Hook_ExitThread(DWORD code);

void* HookForProcAddress(const char* name) {
    if (!strcmp(name, "ExitThread")) return (void*)&Hook_ExitThread;
    if (!strcmp(name, "FlsAlloc")) {
        RealFlsAlloc = (decltype(RealFlsAlloc))GetProcAddress(GetModuleHandleA("kernel32.dll"), "FlsAlloc");
        return RealFlsAlloc ? (void*)&Hook_FlsAlloc : nullptr;
    }
    return nullptr;
}

// Called by the thread itself.
static void ReadSlots(Thread* t) {
    t->fls.clear();
    t->tls.clear();
    for (DWORD i : g_flsIndices) t->fls.push_back(FlsGetValue(i));
    for (DWORD i : g_tlsIndices) t->tls.push_back(TlsGetValue(i));
}

static void WriteSlots(Thread* t) {
    for (auto& [i, v] : t->restoreFls) FlsSetValue(i, v);
    for (auto& [i, v] : t->restoreTls) TlsSetValue(i, v);
}

void SetSlotIndices(const std::vector<DWORD>& fls, const std::vector<DWORD>& tls) {
    AcquireSRWLockExclusive(&g_slotLock);
    if (fls != g_flsIndices || tls != g_tlsIndices) Log("thread slots: the savestate's fiber/thread-local indices differ from this session's");
    g_flsIndices = fls;
    g_tlsIndices = tls;
    ReleaseSRWLockExclusive(&g_slotLock);
}

static void Register(Thread* t) {
    auto* tib = (NT_TIB*)NtCurrentTeb();
    t->tid = GetCurrentThreadId();
    t->vtid = t->tid;
    t->teb = tib;
    t->stackBase = (uint8_t*)tib->StackBase;
    t->stackLow = (uint8_t*)*TebDeallocationStack(tib);
    t->wake = kobj::Ours(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    t->kick = kobj::Ours(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t->handle = kobj::Ours(t->handle);
    TlsSetValue(g_tls, t);
    AcquireSRWLockExclusive(&g_lock);
    (*g_threads)[t->tid] = t;
    ReleaseSRWLockExclusive(&g_lock);
}

static void Unregister(Thread* t) {
    AcquireSRWLockExclusive(&g_lock);
    g_threads->erase(t->tid);
    ReleaseSRWLockExclusive(&g_lock);
    TlsSetValue(g_tls, nullptr);
    // The Thread object stays allocated: a savestate may still describe this thread.
}

// ------------------------------------------------------------------ parking
[[noreturn]] __declspec(noinline) static void CopyAndRestore(uint8_t* dst, const uint8_t* src, size_t n, const CONTEXT* ctx) {
    memcpy(dst, src, n);
    RtlRestoreContext((CONTEXT*)ctx, nullptr);
    __fastfail(FAST_FAIL_INVALID_ARG);
}

// Puts the saved stack bytes back and continues at the saved park point. The saved stack may reach deeper than this
// frame, so first move our own frame well below it; everything above is about to be replaced anyway.
[[noreturn]] __declspec(noinline) static void JumpToSaved(Thread* t) {
    if (t->setSlots) {
        WriteSlots(t);   // the savestate's slot values
        t->setSlots = false;
    }
    InterlockedExchange(&t->restoring, 2);
    uint8_t* dst = t->stackBase - t->restoreSize;
    uint8_t* here = (uint8_t*)_AddressOfReturnAddress();
    uint8_t* floor = dst - 0x8000;
    size_t need = here > floor ? (size_t)(here - floor) : 0;
    volatile uint8_t* pad = (uint8_t*)_alloca(need + 64);
    pad[0] = 0;
    InterlockedExchange(&t->restoring, 3);
    CopyAndRestore(dst, t->restoreStack, t->restoreSize, t->restoreCtx);
}

// Holds a game thread until the coordinator says what to do. The context captured here is the thread's saved state:
// a load resumes the thread right after RtlCaptureContext, on the stack as it was at that moment. Returns the
// thread's record: after a load from another session, the frames up to here are the saved session's, and so is any
// Thread pointer in them; callers use the returned one.
__declspec(noinline) static Thread* Park(Thread* t) {
    ReadSlots(t);
    RtlCaptureContext(&t->ctx);
    t = Self();
    InterlockedExchange(&t->restoring, 0);
    InterlockedIncrement(&g_landed);
    InterlockedExchange(&t->parked, 1);
    for (;;) {
        RealWaitForSingleObject(t->wake, INFINITE);
        LONG c = InterlockedExchange(&t->cmd, None);
        if (c == Resume && !t->zombie) break;
        if (c == Restore) JumpToSaved(t);
        if (c == RunOps) {
            for (const MutexOp& op : (*g_ops)[t->tid]) {
                if (g_opsPhase == 0)
                    for (int i = 0; i < op.release; i++) ReleaseMutex(op.h);
                else
                    for (int i = 0; i < op.acquire; i++) RealWaitForSingleObject(op.h, 0);
            }
            InterlockedIncrement(&g_opsDone);
        }
        if (c == Exit) {
            Unregister(t);
            InterlockedExchange(&t->parked, 0);
            ExitThread(0);
        }
    }
    InterlockedExchange(&t->parked, 0);
    return t;
}

// True when h is the handle of a game thread that has ended for the game (a zombie): waiting on it succeeds.
static bool Ended(HANDLE h) {
    if (!g_zombies) return false;
    AcquireSRWLockShared(&g_lock);
    auto it = g_byHandle->find(h);
    bool ended = it != g_byHandle->end() && it->second->zombie;
    ReleaseSRWLockShared(&g_lock);
    return ended;
}

static void BecomeZombie(Thread* t) {
    InterlockedExchange(&t->zombie, 1);
    InterlockedIncrement(&g_zombies);
    // Threads already waiting for this one to end must look again.
    for (Thread* w : All())
        if (w->waitHandle == t->gameHandle) SetEvent(w->kick);
}

static inline DWORD Remaining(ULONGLONG deadline) {
    ULONGLONG now = Now();
    return now >= deadline ? 0 : (DWORD)(deadline - now);
}

static DWORD WINAPI Hook_WaitForSingleObject(HANDLE h, DWORD ms) {
    Thread* t = Self();
    if (!t) return RealWaitForSingleObject(h, ms);
    const ULONGLONG deadline = ms == INFINITE ? 0 : Now() + ms;
    t->waitCaller = (uintptr_t)_ReturnAddress();
    for (;;) {
        if (g_parkRequested) t = Park(t);
        if (Ended(h)) return WAIT_OBJECT_0;
        DWORD left = ms == INFINITE ? INFINITE : Remaining(deadline);
        HANDLE hs[3] = {h, g_parkEvent, t->kick};
        t->waitHandle = h;
        DWORD r = RealWaitForMultipleObjects(3, hs, FALSE, left);
        t->waitHandle = nullptr;
        if (r == WAIT_OBJECT_0 + 1 || r == WAIT_OBJECT_0 + 2) continue;   // park requested / look again
        if (r == WAIT_FAILED) return RealWaitForSingleObject(h, left);
        if (g_parkRequested) t = Park(t);       // hold the result until the savestate is done
        return r;
    }
}

static DWORD WINAPI Hook_WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    Thread* t = Self();
    if (!t || n == 0 || n >= MAXIMUM_WAIT_OBJECTS - 1) return RealWaitForMultipleObjects(n, hs, all, ms);
    const ULONGLONG deadline = ms == INFINITE ? 0 : Now() + ms;
    t->waitCaller = (uintptr_t)_ReturnAddress();
    for (;;) {
        if (g_parkRequested) t = Park(t);
        DWORD left = ms == INFINITE ? INFINITE : Remaining(deadline);
        DWORD r;
        t->waitHandle = hs[0];
        struct Clear { ~Clear() { if (Thread* s = Self()) s->waitHandle = nullptr; } } clear;
        if (!all) {
            for (DWORD i = 0; i < n; i++)
                if (Ended(hs[i])) return WAIT_OBJECT_0 + i;
            HANDLE buf[MAXIMUM_WAIT_OBJECTS];
            memcpy(buf, hs, n * sizeof(HANDLE));
            buf[n] = g_parkEvent;
            buf[n + 1] = t->kick;
            r = RealWaitForMultipleObjects(n + 2, buf, FALSE, left);
            if (r == WAIT_OBJECT_0 + n || r == WAIT_OBJECT_0 + n + 1) continue;
            if (r == WAIT_FAILED) return RealWaitForMultipleObjects(n, hs, all, left);
        } else {
            // Waiting for all handles can't include the park event: wait in short slices instead. Ended threads
            // count as signalled.
            HANDLE buf[MAXIMUM_WAIT_OBJECTS];
            DWORD m = 0;
            for (DWORD i = 0; i < n; i++)
                if (!Ended(hs[i])) buf[m++] = hs[i];
            if (!m) return WAIT_OBJECT_0;
            DWORD slice = left == INFINITE || left > 20 ? 20 : left;
            r = RealWaitForMultipleObjects(m, buf, TRUE, slice);
            if (r == WAIT_TIMEOUT && (ms == INFINITE || Remaining(deadline) > 0)) continue;
        }
        if (g_parkRequested) t = Park(t);
        return r;
    }
}

static void WINAPI Hook_Sleep(DWORD ms) {
    Thread* t = Self();
    if (!t) { RealSleep(ms); return; }
    if (g_parkRequested) t = Park(t);
    if (ms == 0) { RealSleep(0); return; }
    const ULONGLONG deadline = ms == INFINITE ? 0 : Now() + ms;
    for (;;) {
        DWORD left = ms == INFINITE ? INFINITE : Remaining(deadline);
        if (left == 0) return;
        if (RealWaitForSingleObject(g_parkEvent, left) != WAIT_OBJECT_0) return;
        if (g_parkRequested) t = Park(t);
    }
}

static BOOL WINAPI Hook_GetOverlappedResult(HANDLE f, LPOVERLAPPED ov, LPDWORD n, BOOL wait) {
    Thread* t = Self();
    if (!t || !wait) return RealGetOverlappedResult(f, ov, n, wait);
    // Wait for completion through the parking wait, then let Windows report the result.
    while (ov->Internal == (ULONG_PTR)STATUS_PENDING) {
        HANDLE ev = (HANDLE)((uintptr_t)ov->hEvent & ~(uintptr_t)1);
        Hook_WaitForSingleObject(ev ? ev : f, INFINITE);
    }
    return RealGetOverlappedResult(f, ov, n, FALSE);
}

// ------------------------------------------------------------------ thread creation
struct StartInfo {
    LPTHREAD_START_ROUTINE fn;
    void* arg;
    Thread* t;
    size_t stack;   // the stack the thread runs on (OnWindowStack)
};

static uint64_t RunGameThread(void* p, void*) {
    auto* si = (StartInfo*)p;
    LPTHREAD_START_ROUTINE fn = si->fn;
    void* arg = si->arg;
    Thread* t = si->t;
    delete si;
    Register(t);
    DWORD r = fn(arg);
    t = Self();   // (this frame may come from a savestate of another session)
    if (g_keep) {
        // Savestates may need this thread again: it doesn't end, it waits (parked) for a load to bring it back.
        BecomeZombie(t);
        for (;;) t = Park(t);
    }
    Unregister(t);
    return r;
}

// A game thread can also end by calling ExitThread itself (the C runtime's _endthreadex does). Like returning from its
// function (RunGameThread), it becomes a zombie while savestates exist; else it gives its fixed thread-local block
// back first. (Windows then frees the stack in the TEB, the fixed one; the thread's own stack stays reserved.)
static void(WINAPI* RealExitThread)(DWORD) = &ExitThread;
static void WINAPI Hook_ExitThread(DWORD code) {
    if (Thread* t = Self()) {
        if (g_keep) {
            BecomeZombie(t);
            for (;;) t = Park(t);
        }
        Unregister(t);
        GiveBackTls();
    }
    RealExitThread(code);
}

static DWORD WINAPI Trampoline(void* p) {
    unsigned ordinal = ((StartInfo*)p)->t->ordinal;
    size_t stack = ((StartInfo*)p)->stack;
    return (DWORD)OnWindowStack(ordinal, stack, &RunGameThread, p, nullptr);
}

// A thread a load will restore: it waits as a zombie until then. Restored, it runs the saved thread's frames (which
// start in RunGameThread) and ends through this thread's own way back.
static uint64_t RunSpawned(void* p, void*) {
    Thread* t = (Thread*)p;
    Register(t);
    InterlockedExchange(&t->zombie, 1);
    InterlockedIncrement(&g_zombies);
    for (;;) t = Park(t);
}

static DWORD WINAPI SpawnStart(void* p) {
    return (DWORD)OnWindowStack(((Thread*)p)->ordinal, kThreadStack, &RunSpawned, p, nullptr);
}

Thread* Spawn(unsigned ordinal) {
    auto* t = new Thread();
    t->ordinal = ordinal;
    HANDLE h = RealCreateThread(nullptr, 0, &SpawnStart, t, 0, nullptr);
    if (!h) return nullptr;
    CloseHandle(h);
    for (ULONGLONG start = GetTickCount64(); !t->parked; RealSleep(1))
        if (GetTickCount64() - start > 3000) {
            Log("thread %u: started for the savestate, but it didn't get ready", ordinal);
            return nullptr;
        }
    return t;
}

static DWORD WINAPI EndAtOnce(void*) { return 0; }

// What a game thread handle becomes when the restored game knows a thread that no longer exists: one that has ended.
static HANDLE EndedHandle() {
    static HANDLE h = nullptr;
    if (!h) {
        h = RealCreateThread(nullptr, 0, &EndAtOnce, nullptr, 0, nullptr);
        if (h) RealWaitForSingleObject(h = kobj::Ours(h), 5000);
    }
    return h;
}

static HANDLE WINAPI Hook_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                       DWORD flags, LPDWORD tidOut) {
    // As much stack as the game asks for (0: the exe's default, 1 MB): at least 1 MB, at most the thread's slot.
    size_t size = ((size_t)stack + 0xFFFF) & ~(size_t)0xFFFF;
    if (size < kThreadStack) size = kThreadStack;
    if (size > addr::kStackSlot) size = addr::kStackSlot;
    auto* si = new StartInfo{fn, arg, new Thread(), size};
    Thread* t = si->t;
    t->ordinal = (unsigned)InterlockedIncrement(&g_nextOrdinal);
    if (t->ordinal >= addr::kThreadSlots - 1) t->ordinal = addr::kThreadSlots - 1;
    HANDLE h = RealCreateThread(sa, stack, &Trampoline, si, flags, tidOut);
    DWORD error = h ? 0 : GetLastError();
    Log("thread %u: started at %s, %llu KB of stack asked%s, %llu KB given%s", t->ordinal, Where((uintptr_t)fn).c_str(),
        (unsigned long long)(stack >> 10), (flags & STACK_SIZE_PARAM_IS_A_RESERVATION) ? " (reserve)" : "",
        (unsigned long long)(size >> 10), h ? "" : "; CreateThread FAILED");
    if (!h) {
        MEMORYSTATUSEX ms = {sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        Log("  error %u; RAM free %llu MB, commit free %llu of %llu MB", error, (unsigned long long)(ms.ullAvailPhys >> 20),
            (unsigned long long)(ms.ullAvailPageFile >> 20), (unsigned long long)(ms.ullTotalPageFile >> 20));
        SetLastError(error);
        delete t;
        delete si;
        return h;
    }
    h = kobj::Place(h, kobj::ThreadHandle);   // a fixed value; its close is held back like an event's
    t->gameHandle = h;
    AcquireSRWLockExclusive(&g_lock);
    (*g_byHandle)[h] = t;
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

// ------------------------------------------------------------------ thread ids as the game sees them
static DWORD(WINAPI* RealGetCurrentThreadId)() = &GetCurrentThreadId;
static DWORD(WINAPI* RealGetThreadId)(HANDLE) = &GetThreadId;

static DWORD WINAPI Hook_GetCurrentThreadId() {
    Thread* t = Self();
    return t ? t->vtid : RealGetCurrentThreadId();
}

static Thread* ByHandle(HANDLE h) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_byHandle->find(h);
    Thread* t = it != g_byHandle->end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_lock);
    return t;
}

static DWORD WINAPI Hook_GetThreadId(HANDLE h) {
    Thread* t = ByHandle(h);
    return t && t->tid ? t->vtid : RealGetThreadId(h);
}

DWORD GameTid(DWORD realTid) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_threads->find(realTid);
    DWORD v = it != g_threads->end() ? it->second->vtid : realTid;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

DWORD GameTidOfHandle(HANDLE h) {
    Thread* t = ByHandle(h);
    return t && t->tid ? t->vtid : RealGetThreadId(h);
}

static Thread* ByGameTid(DWORD vtid) {
    AcquireSRWLockShared(&g_lock);
    Thread* found = nullptr;
    for (auto& kv : *g_threads)
        if (kv.second->vtid == vtid) found = kv.second;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

DWORD LiveTid(DWORD gameTid) {
    Thread* t = ByGameTid(gameTid);
    return t ? t->tid : gameTid;
}

HANDLE LiveHandle(DWORD gameTid) {
    Thread* t = ByGameTid(gameTid);
    return t ? t->handle : EndedHandle();
}

void SetGameHandle(HANDLE h, DWORD gameTid) {
    Thread* t = ByGameTid(gameTid);
    AcquireSRWLockExclusive(&g_lock);
    if (t) {
        (*g_byHandle)[h] = t;
        t->gameHandle = h;
    } else {
        g_byHandle->erase(h);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ------------------------------------------------------------------ coordinator side
void Init() {
    g_tls = TlsAlloc();
    g_threads = new std::map<DWORD, Thread*>;
    g_byHandle = new std::map<HANDLE, Thread*>;
    g_parkEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerMs = f.QuadPart >= 1000 ? f.QuadPart / 1000 : 1;
    auto* main = new Thread();
    main->ordinal = 0;
    Register(main);
}

void SetKeep() { InterlockedExchange(&g_keep, 1); }

// ------------------------------------------------------------------ the main thread
static uint64_t RunEntry(void* entry, void* peb) {
    // Now on the window stack: the registry must describe it.
    Thread* t = Self();
    auto* tib = (NT_TIB*)NtCurrentTeb();
    t->stackBase = (uint8_t*)tib->StackBase;
    t->stackLow = (uint8_t*)*TebDeallocationStack(tib);
    return ((uint64_t(*)(void*))entry)(peb);
}

static uint64_t MainOnWindowStack(void* peb, void* entry) {
    auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)g_exe.base + ((IMAGE_DOS_HEADER*)g_exe.base)->e_lfanew);
    size_t size = (size_t)nth->OptionalHeader.SizeOfStackReserve;
    if (size < (4u << 20)) size = 4u << 20;
    return OnWindowStack(0, size, &RunEntry, entry, peb);
}

void MoveMainThread() {
    // The launcher's stub called LoadLibraryW, so its return address is on this stack: find the stub page and point
    // its redirect slot at us.
    auto* tib = (NT_TIB*)NtCurrentTeb();
    for (auto* sp = (uintptr_t*)_AddressOfReturnAddress(); sp < (uintptr_t*)tib->StackBase; sp++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)*sp, &mbi, sizeof(mbi)) || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_EXECUTE_READWRITE)
            continue;
        auto* page = (uint8_t*)mbi.AllocationBase;
        if (memcmp(page, inject::kStubSignature, sizeof(inject::kStubSignature)) != 0) continue;
        *(uint64_t*)(page + inject::kRedirect) = (uint64_t)&MainOnWindowStack;
        return;
    }
    Log("main thread: launcher stub not found, it keeps its own stack (savestates of this session can't be loaded later)");
}

void InstallHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    RealCreateThread = (decltype(RealCreateThread))PatchIat(exe, "kernel32.dll", "CreateThread", (void*)&Hook_CreateThread);
    if (void* p = PatchIat(exe, "kernel32.dll", "WaitForSingleObject", (void*)&Hook_WaitForSingleObject)) RealWaitForSingleObject = (decltype(RealWaitForSingleObject))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "WaitForMultipleObjects", (void*)&Hook_WaitForMultipleObjects)) RealWaitForMultipleObjects = (decltype(RealWaitForMultipleObjects))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "Sleep", (void*)&Hook_Sleep)) RealSleep = (decltype(RealSleep))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "GetOverlappedResult", (void*)&Hook_GetOverlappedResult)) RealGetOverlappedResult = (decltype(RealGetOverlappedResult))p;
    if (!RealCreateThread) RealCreateThread = &CreateThread;
    if (void* p = PatchIat(exe, "kernel32.dll", "GetCurrentThreadId", (void*)&Hook_GetCurrentThreadId)) RealGetCurrentThreadId = (decltype(RealGetCurrentThreadId))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "TlsAlloc", (void*)&Hook_TlsAlloc)) RealTlsAlloc = (decltype(RealTlsAlloc))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "ExitThread", (void*)&Hook_ExitThread)) RealExitThread = (decltype(RealExitThread))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "GetThreadId", (void*)&Hook_GetThreadId)) RealGetThreadId = (decltype(RealGetThreadId))p;
}

std::vector<Thread*> All() {
    std::vector<Thread*> v;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_threads) v.push_back(kv.second);
    ReleaseSRWLockShared(&g_lock);
    return v;
}

// Logs where a thread that didn't park is running, from its instruction pointer and code addresses on its stack.
// The addresses are collected while it is suspended and named only after it runs again (naming allocates and takes
// the loader lock, which the suspended thread may hold).
static void DescribeStraggler(Thread* t) {
    if (SuspendThread(t->handle) == (DWORD)-1) return;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    uintptr_t addrs[13];
    int n = 0;
    if (GetThreadContext(t->handle, &c)) {
        addrs[n++] = c.Rip;
        for (auto* sp = (uintptr_t*)c.Rsp; n < 13 && (uint8_t*)sp < t->stackBase && sp < (uintptr_t*)c.Rsp + 8192; sp++) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t v = *sp;
            if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
                !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                continue;
            addrs[n++] = v;
        }
    }
    ResumeThread(t->handle);
    std::string trail;
    for (int i = 0; i < n; i++) trail += (i ? " < " : "") + Where(addrs[i]);
    Log("  thread %u did not park: %s", t->tid, trail.c_str());
}

void DumpAll(const char* why, std::string (*describe)(HANDLE)) {
    Log("thread dump (%s):", why);
    std::map<DWORD, Thread*> game;
    for (Thread* t : All()) game[t->tid] = t;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        // Collect raw addresses while the thread is suspended; name them only after resuming it (naming takes the
        // loader lock, which the suspended thread might hold).
        uintptr_t found[24];
        int n = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &c)) {
                found[n++] = c.Rip;
                for (auto* sp = (uintptr_t*)c.Rsp; n < 24 && sp < (uintptr_t*)c.Rsp + 8192; sp++) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(sp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
                    uintptr_t v = *sp;
                    if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
                        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                        continue;
                    found[n++] = v;
                }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
        std::string trail;
        for (int i = 0; i < n; i++) trail += (i ? " < " : "") + Where(found[i]);
        auto it = game.find(te.th32ThreadID);
        std::string wait;
        if (it != game.end() && it->second->waitHandle)
            wait = " waits on " + describe(it->second->waitHandle) + " from " + Where(it->second->waitCaller);
        Log("  %5u %s%s%s | %s", te.th32ThreadID, it != game.end() ? "game" : "    ",
            it != game.end() && it->second->parked ? " parked" : "", wait.c_str(), trail.c_str());
    }
    CloseHandle(snap);
}

void ReleaseAll() {
    ResetEvent(g_parkEvent);
    InterlockedExchange(&g_parkRequested, 0);
    for (Thread* t : All()) {
        if (!t->parked) continue;
        InterlockedExchange(&t->cmd, Resume);
        SetEvent(t->wake);
    }
}

bool ParkAll(DWORD timeoutMs, bool (*quiet)(std::string* why)) {
    const ULONGLONG start = GetTickCount64();
    std::string lastWhy;
    for (int attempt = 1;; attempt++) {
        InterlockedExchange(&g_parkRequested, 1);
        SetEvent(g_parkEvent);
        for (;;) {
            auto all = All();
            size_t parked = std::count_if(all.begin(), all.end(), [](Thread* t) { return t->parked != 0; });
            if (parked == all.size()) break;
            if (GetTickCount64() - start > timeoutMs) {
                Log("park: %u of %u threads parked after %u ms", (unsigned)parked, (unsigned)all.size(), timeoutMs);
                for (Thread* t : all)
                    if (!t->parked) DescribeStraggler(t);
                ReleaseAll();
                return false;
            }
            Sleep(0);
        }
        std::string why;
        if (!quiet || quiet(&why)) {
            if (attempt > 1) Log("park: quiet after %d attempts (%u ms)", attempt, (unsigned)(GetTickCount64() - start));
            return true;
        }
        lastWhy = why;
        ReleaseAll();
        if (GetTickCount64() - start > timeoutMs) {
            Log("park: never quiet within %u ms (%s)", timeoutMs, lastWhy.c_str());
            return false;
        }
        Sleep(1);
    }
}

void RunMutexOps(const std::vector<MutexOp>& ops) {
    if (ops.empty()) return;
    std::map<DWORD, std::vector<MutexOp>> byThread;
    for (const MutexOp& op : ops) byThread[op.tid].push_back(op);
    g_ops = &byThread;
    std::map<DWORD, Thread*> live;
    for (Thread* t : All()) live[t->tid] = t;
    for (LONG phase = 0; phase < 2; phase++) {
        InterlockedExchange(&g_opsPhase, phase);
        InterlockedExchange(&g_opsDone, 0);
        LONG sent = 0;
        for (auto& kv : byThread) {
            auto it = live.find(kv.first);
            if (it == live.end()) continue;
            InterlockedExchange(&it->second->cmd, RunOps);
            SetEvent(it->second->wake);
            sent++;
        }
        const ULONGLONG start = GetTickCount64();
        while (g_opsDone < sent && GetTickCount64() - start < 3000) Sleep(0);
        if (g_opsDone < sent) Log("mutex ops: only %d of %d threads answered", (int)g_opsDone, (int)sent);
    }
    g_ops = nullptr;
}

void RestoreAndWait(const std::vector<Thread*>& restore, const std::vector<Thread*>& exiting) {
    // Threads the restored game doesn't know: they stay parked as zombies (another savestate may need them).
    for (Thread* t : exiting)
        if (!t->zombie) BecomeZombie(t);
    for (Thread* t : restore)
        if (t->zombie) {
            InterlockedExchange(&t->zombie, 0);
            InterlockedDecrement(&g_zombies);
        }
    InterlockedExchange(&g_landed, 0);
    for (Thread* t : restore) {
        InterlockedExchange(&t->restoring, 1);
        InterlockedExchange(&t->cmd, Restore);
        SetEvent(t->wake);
    }
    const ULONGLONG start = GetTickCount64();
    while (g_landed < (LONG)restore.size()) {
        if (GetTickCount64() - start > 5000) {
            Log("restore: only %d of %u threads landed", (int)g_landed, (unsigned)restore.size());
            for (Thread* t : restore)
                if (t->restoring) {
                    Log("  thread number %u (%u) stopped at restore stage %d", t->ordinal, t->tid, (int)t->restoring);
                    DescribeStraggler(t);
                }
            break;
        }
        Sleep(0);
    }
}

}  // namespace threads
