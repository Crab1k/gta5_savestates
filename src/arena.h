// Replacement for the Windows heap blocks GTA5.exe allocates (HeapAlloc & co.): one reserved region with a TLSF
// allocator whose control block lives inside the region, so restoring the region's pages restores the allocator.
// The game uses the Windows heap very little (its own allocators sit on VirtualAlloc'd pools), but the C runtime's
// data and a few game objects live there.
#pragma once
#include <cstddef>
#include <cstdint>

namespace arena {

bool   Init(size_t reserveBytes);
void*  Alloc(size_t size, bool zero);
bool   Free(void* p);                       // false: not an arena pointer
void*  Realloc(void* p, size_t size, bool inPlaceOnly, bool zero);
size_t Size(const void* p);                 // requested size of an arena block
bool   Contains(const void* p);
void   Resync();                            // after a load replaced the arena's pages (and maybe its extent)

uint8_t* Base();
size_t   Reserved();
size_t   LiveBytes();
uint32_t LiveBlocks();

}  // namespace arena
