// The game's clocks (QueryPerformanceCounter, timeGetTime) run behind the real ones by an offset. Loading a savestate
// sets the offset so the game's time continues from the moment the savestate was made: timers, frame deltas and
// script waits see no jump.
#pragma once
#include <cstdint>

namespace timewarp {

void InstallHooks();
int64_t Qpc();          // game's QueryPerformanceCounter now
uint32_t Ms();          // game's timeGetTime now
void Continue(int64_t qpc, uint32_t ms);   // make the game's clocks read these values now

}  // namespace timewarp
