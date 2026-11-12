// The game's threads: which they are, and stopping all of them at a consistent point.
//
// Every thread the game starts goes through GTA5.exe's CreateThread import, so the hook knows them all (plus the main
// thread). Every place where a game thread blocks goes through a hooked import too (WaitForSingleObject,
// WaitForMultipleObjects, Sleep, GetOverlappedResult). When a savestate is requested, each game thread stops ("parks")
// inside such a hook: its stack then holds only game frames above our hook, and its registers are captured at the
// park point. A savestate keeps every thread's stack and registers; loading one puts them back, so each thread
// continues exactly where it was when the savestate was made, even in the middle of a mission's script or a job.
#pragma once
#include "common.h"
#include <vector>

namespace threads {

struct alignas(16) Thread {
    CONTEXT ctx;                  // registers at the park point (RtlCaptureContext, needs 16-byte alignment)
    DWORD tid = 0;
    unsigned ordinal = 255;       // creation order among the game's threads (main: 0); picks its addresses (addr.h)
    DWORD vtid = 0;               // the thread id the game sees (the saved thread's, after a load from another session)
    HANDLE handle = nullptr;      // our own handle
    uint8_t* stackBase = nullptr; // TEB.NtTib.StackBase (top, exclusive)
    uint8_t* stackLow = nullptr;  // TEB.DeallocationStack (start of the stack's range)
    void* teb = nullptr;
    HANDLE wake = nullptr;        // auto-reset: the coordinator has a command for this parked thread
    volatile LONG parked = 0;
    volatile LONG cmd = 0;        // Cmd
    // A thread that ended (or that a loaded savestate doesn't know) while savestates exist: it stays parked with its
    // stack and id, so a savestate that needs it can bring it back. To the game it has ended.
    volatile LONG zombie = 0;
    HANDLE gameHandle = nullptr;  // the handle CreateThread gave the game
    HANDLE kick = nullptr;        // auto-reset: wakes the thread's current wait to look again (a waited-for thread ended)
    // set by the coordinator before cmd = Restore
    const uint8_t* restoreStack = nullptr;
    size_t restoreSize = 0;
    const CONTEXT* restoreCtx = nullptr;
    // The game's per-thread slot values (fiber-local and Tls* slots, in the order of FlsIndices/TlsIndices), recorded
    // by the thread at each park, and the (index, value) pairs a load puts back.
    std::vector<void*> fls, tls;
    std::vector<std::pair<DWORD, void*>> restoreFls, restoreTls;
    bool setSlots = false;
    volatile LONG restoring = 0;  // during a restore: 1 sent, 2 slots written, 3 about to jump; 0 again once landed
    // what the thread waits for right now (diagnostics)
    volatile HANDLE waitHandle = nullptr;
    volatile uintptr_t waitCaller = 0;
};

enum Cmd : LONG { None, Resume, Restore, Exit, RunOps };

// A change of mutex ownership, done by the thread itself (only an owner can release a mutex).
struct MutexOp {
    DWORD tid;
    HANDLE h;
    int release;   // times to release it
    int acquire;   // times to take it
};

void Init();          // registers the calling (main) thread
void SetKeep();       // a savestate exists: from now on threads that end become zombies
// The fiber-local and thread-local slot indices the game allocated (in order). A load sets the lists of the
// savestate: the restored memory uses its indices.
const std::vector<DWORD>& FlsIndices();
const std::vector<DWORD>& TlsIndices();
void SetSlotIndices(const std::vector<DWORD>& fls, const std::vector<DWORD>& tls);
void* HookForProcAddress(const char* name);   // FlsAlloc

// Thread numbers (addr.h): the next one CreateThread gives, and moving it past the numbers a load brings back.
unsigned NextOrdinal();
void MoveNextOrdinalPast(unsigned ordinal);
// A new game thread with number `ordinal` (one a savestate of another session needs and this session hasn't made),
// parked as a zombie with its stack at that number's slot, ready to be restored. Null if it could not start.
Thread* Spawn(unsigned ordinal);
// Where RunOnStack keeps the way back to the thread's own stack (the stack pointer of its machine frame).
inline uint64_t* WayBack(Thread* t) { return (uint64_t*)(t->stackBase - 0x10); }
// After a load: game thread handle `h` now stands for the thread the game knows as `gameTid`.
void SetGameHandle(HANDLE h, DWORD gameTid);
// Called from DllMain on the main thread: makes the game's entry point run on a stack in the session's address
// window (through the launcher's entry stub), like every other game thread.
void MoveMainThread();
void InstallHooks();  // CreateThread and the wait functions in GTA5.exe's import table

// Stops every game thread at a park point. False after timeoutMs (all are released again, the log says which threads
// did not stop). `quiet` is checked once all are parked; while it returns false they are released and tried again.
bool ParkAll(DWORD timeoutMs, bool (*quiet)(std::string* why));
void ReleaseAll();    // cmd = Resume for every parked thread

// Snapshot of the registry while everything is parked.
std::vector<Thread*> All();

// Load: every thread in `restore` jumps back to its saved stack and registers, every other parked thread exits.
// Returns once all restored threads stand at their saved park points (still parked).
void RestoreAndWait(const std::vector<Thread*>& restore, const std::vector<Thread*>& exiting);

// While all are parked: each thread releases, then takes, the mutexes listed for it.
void RunMutexOps(const std::vector<MutexOp>& ops);

// Thread ids as the game knows them (GetCurrentThreadId and GetThreadId are virtualised for it).
DWORD GameTid(DWORD realTid);           // real -> game id (unknown threads: unchanged)
DWORD GameTidOfHandle(HANDLE h);        // a game thread handle -> game id
DWORD LiveTid(DWORD gameTid);           // game id -> real id of the thread that has it now
HANDLE LiveHandle(DWORD gameTid);       // game id -> a real handle of that thread (ours, not to be closed); for a
                                        // thread no longer alive, the handle of a thread that has ended

bool IsGameThread();
unsigned Ordinal();   // the calling thread's creation number (255 when it isn't a game thread)

// Logs where every thread of the process is (instruction pointer and code addresses on its stack). For hangs.
void DumpAll(const char* why, std::string (*describe)(HANDLE));   // describe: what a handle is (kobj::Describe)

// The real wait functions, for code of ours that must not park.
extern DWORD(WINAPI* RealWaitForSingleObject)(HANDLE, DWORD);
extern void(WINAPI* RealSleep)(DWORD);

}  // namespace threads
