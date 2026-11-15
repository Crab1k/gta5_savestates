// Savestate files: making one from the stopped game, and putting one back.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss {

constexpr int MAX_SLOTS = 10;

// Deletes the savestates an earlier game session left (unless SetOtherSessions): call the setters first.
void Init(const std::string& folder);
void SetOtherSessions(bool on);   // lab: keep savestates for later game sessions (unfinished: some loads crash the driver)
void SetForceRecreate(bool on);   // lab: every load rebuilds kernel and GPU objects as for another session
void SetReleaseFirst(bool on);    // lab: a rebuild lets go of the live GPU objects before it (else after it)
void SetLogLayout(bool on);       // lab: log the memory layout at each save
bool Save(int slot, std::string& msg);
// After the game switched between fullscreen and a window or changed resolution: deletes the savestates made before,
// which can't be loaded any more (`dropped`: their slots).
bool DropStale(std::vector<int>* dropped);
bool Load(int slot, std::string& msg);
// Called before a load that stops the game for a while (another session's savestate), while the game still runs.
void SetNotice(void (*notice)(const std::string& text));
std::string Describe(int slot);

}  // namespace ss
