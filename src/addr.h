// Where the game's memory goes, so a savestate can be loaded in a later game session.
//
// Every session uses the same fixed layout high in the address space (nothing else lives there):
//  - each game thread gets a sub-window, chosen by the order in which the game created it, for the memory regions it
//    reserves; the main thread is number 0;
//  - each game thread's stack and thread-local block sit at fixed slots by the same number;
//  - our heap arena sits at one fixed address.
// The game starts up the same way every time, so a later session rebuilds the same memory at the same addresses.
// Pointers the game keeps and pointers other DLLs keep into the game's memory (callbacks) then stay valid when a
// savestate of an earlier session is loaded.
#pragma once
#include <cstddef>
#include <cstdint>

namespace addr {

constexpr uintptr_t kRegionsBase = 0x200000000000;   // 32 TB: per thread number, 64 GB each
constexpr uintptr_t kRegionsPerThread = 0x1000000000;
constexpr unsigned kThreadSlots = 256;               // thread numbers 0..254; 255: threads we don't know yet
constexpr uintptr_t kStacksBase = 0x300000000000;    // 48 TB: 16 MB per thread number
constexpr uintptr_t kStackSlot = 0x1000000;
constexpr uintptr_t kTlsBase = 0x300100000000;       // 64 KB per thread number
constexpr uintptr_t kTlsSlot = 0x10000;
constexpr uintptr_t kArenaBase = 0x300200000000;

// Reserves (with MEM_COMMIT also commits) `size` bytes at the next free address of thread number `thread`'s window.
void* Allocate(unsigned thread, size_t size, unsigned long type, unsigned long protect,
               void* (__stdcall* realAlloc)(void*, size_t, unsigned long, unsigned long) = nullptr);
// After a load from another session: the windows' next free addresses must lie past everything restored.
void MoveNextPast(uintptr_t end);

inline void* StackSlot(unsigned thread) { return (void*)(kStacksBase + (uintptr_t)thread * kStackSlot); }
inline void* TlsSlot(unsigned thread) { return (void*)(kTlsBase + (uintptr_t)thread * kTlsSlot); }

}  // namespace addr
