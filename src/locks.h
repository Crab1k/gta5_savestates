// The game's critical sections. A critical section in the game's memory names its owner by real thread id and may
// hold an event handle (for waiters) and a pointer to ntdll's debug record. Within one session a savestate's values
// stay right; a savestate of another session needs them translated: the owner to the live thread that continues the
// saved one, a fresh event, and no debug record ntdll doesn't know.
#pragma once
#include "common.h"
#include <vector>

namespace locks {

void InstallHooks();                          // InitializeCriticalSection* / DeleteCriticalSection of GTA5.exe
void* HookForProcAddress(const char* name);   // the same, when the game looks them up itself

std::vector<uint64_t> List();                 // addresses of the game's critical sections now
// After a load put back the memory of a savestate whose critical sections are `saved`: they are the game's now.
// With `foreign` (another session) their contents are translated; liveTid maps a saved owner's real thread id to
// the real id of the live thread that continues it (0: unknown). `liveDebug` holds this session's debug records by
// address, read before the memory was replaced.
void Restore(const std::vector<uint64_t>& saved, bool foreign, DWORD (*liveTid)(DWORD savedRealTid),
             const std::vector<std::pair<uint64_t, uint64_t>>& liveDebug);
std::vector<std::pair<uint64_t, uint64_t>> DebugRecords();   // (address, DebugInfo) of the live list
void Report(const char* why);                 // lab: owners, events and debug records of the list

}  // namespace locks
