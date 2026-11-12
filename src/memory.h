// The game's memory: what a savestate copies.
//  - GTA5.exe's writable sections (.data and friends): every global of the game.
//  - Every region GTA5.exe reserves with VirtualAlloc: rage's heaps and pools (a few GB, reserved at start-up).
//  - The arena that replaces the Windows heap blocks GTA5.exe allocates.
// Regions are reserved with write watch, so only pages the game has ever written need to be saved.
#pragma once
#include "common.h"
#include <vector>

namespace mem {

struct Region {
    uint8_t* base = nullptr;
    size_t size = 0;
    size_t pages = 0;
    bool ownWatch = false;      // write watch added by us: we may read and reset it
    bool freedByGame = false;   // the game released it while savestates existed; kept for them
    std::vector<uint64_t> written;   // pages written at any time since the region was reserved
    bool Written(size_t page) const { return (written[page >> 6] >> (page & 63)) & 1; }
};

struct Section {
    uint8_t* p;
    size_t size;
    char name[9];
};

void Init(size_t arenaReserve);
void InstallHooks();
void SetKeepFreed(bool keep);    // true once a savestate exists: released regions are kept for it
// After a load: releases every region (but the arena) for which needed(base) is false. Only safe for regions that
// neither the restored memory nor any savestate refers to.
void ReleaseUnless(bool (*needed)(uintptr_t base));

std::vector<Region*> Regions();
void CollectWritten(Region* r);  // adds the pages written since the last call to r->written

// Committed page runs of a region, as a savestate records them.
struct Run {
    uint32_t first, pages, protect, pad;
};
std::vector<Run> Committed(const Region* r);
// A load: the region a savestate has at [base, base+size). Reuses the live one if it is exactly that; otherwise
// releases live regions in the way and reserves it there. Null if the address range can't be had.
Region* Ensure(uint64_t base, uint64_t size, bool ownWatch, bool* made);
void Commit(Region* r, const std::vector<Run>& runs);    // commits the runs read-write (keeps committed pages as they are)
void Protect(Region* r, const std::vector<Run>& runs);   // then gives them their saved protection
std::vector<Section> DataSections();

// The game's implicit thread-local storage block of a thread (GTA5.exe's __declspec(thread) data).
uint8_t* TlsBlock(void* teb);
size_t TlsSize();

}  // namespace mem
