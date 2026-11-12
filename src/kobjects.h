// Kernel objects GTA5.exe creates that are part of its state: events and semaphores (their signal state), mutexes
// (their owner), files (their position), thread handles, and the overlapped reads in flight.
//
// Handle values: the game's handles are put at fixed values in a block reserved at start-up (handle numbers 0x40000
// up), handed out in creation order. So a savestate's handle values mean the same in every game session, and loading
// one in another session only has to put equivalent objects at those values. They are real handles: the game and
// any DLL it passes them to use them as usual.
//
// A handle the game closes is kept open while savestates exist (they may hold memory that still uses it); the game
// sees the close succeed.
#pragma once
#include "common.h"
#include "threads.h"
#include <string>
#include <vector>

namespace kobj {

enum Kind : uint32_t { Event = 1, Semaphore = 2, Mutex = 3, File = 4, ThreadHandle = 5, ChangeNotify = 6 };

// A game kernel object as a savestate keeps it: enough to make an equivalent one in another session, and its state.
struct Saved {
    HANDLE h = nullptr;
    uint32_t kind = 0;
    uint32_t a = 0, b = 0, c = 0;   // event: manual reset; semaphore: maximum; file: access, share, flags;
                                    // change notification: subtree, filter
    int32_t state = 0;              // event: signalled; semaphore: count; mutex: owner's recursion count (0: free)
    int64_t pos = -1;               // file: position (non-overlapped files); mutex: owner thread; thread handle: thread
    std::wstring path;              // file, change notification
};

void Init();                                  // reserves the handle block; before any game code runs
void InstallHooks();
void* HookForProcAddress(const char* name);   // replacements for functions the game resolves with GetProcAddress
HANDLE Place(HANDLE real, Kind k);            // puts a handle created elsewhere (threads) into the block
HANDLE Ours(HANDLE h);                        // a handle of ours that must stay out of the block (a load may write there)
void SetKeep();                               // a savestate exists: hold back closes of read-only files too
void LogCreates(int n);                       // lab: log the first n handles the game creates

std::vector<Saved> Capture();                 // while all game threads are parked
std::vector<uint8_t> Encode(const std::vector<Saved>& saved);
bool Decode(const uint8_t* p, size_t n, std::vector<Saved>* out);

// Load: makes every saved handle value refer to an object like the saved one (a new one when the live object there
// differs, or always with `recreate`), puts the saved states back, and returns what each thread must do to give the
// mutexes their saved owners (threads::RunMutexOps). liveTid maps a saved thread id to the live thread that continues
// it; threadHandle gives a real handle of that live thread.
std::vector<threads::MutexOp> Restore(const std::vector<Saved>& saved, bool recreate, DWORD (*liveTid)(DWORD savedTid),
                                      HANDLE (*threadHandle)(DWORD savedTid));

bool IoQuiet(std::string* why);               // no overlapped read/write of the game in flight
std::string Describe(HANDLE h);               // "auto event 0000000000040010 (not set)" etc., for diagnostics

}  // namespace kobj
