#include "addr.h"
#include "common.h"

namespace addr {

static volatile LONG64 g_next[kThreadSlots];   // next free address in each thread's window (0: window start)

void* Allocate(unsigned thread, size_t size, unsigned long type, unsigned long protect,
               void* (__stdcall* realAlloc)(void*, size_t, unsigned long, unsigned long)) {
    if (thread >= kThreadSlots) thread = kThreadSlots - 1;
    const uintptr_t start = kRegionsBase + thread * kRegionsPerThread, end = start + kRegionsPerThread;
    const size_t span = (size + 0xFFFF) & ~(size_t)0xFFFF;
    InterlockedCompareExchange64(&g_next[thread], (LONG64)start, 0);
    for (int attempt = 0; attempt < 64; attempt++) {
        uintptr_t at = (uintptr_t)InterlockedExchangeAdd64(&g_next[thread], (LONG64)(span + 0x10000));   // 64 KB gap
        if (at + span > end) {
            Log("address window of thread %u is full", thread);
            return nullptr;
        }
        void* p = realAlloc ? realAlloc((void*)at, size, type | MEM_RESERVE, protect)
                            : VirtualAlloc((void*)at, size, type | MEM_RESERVE, protect);
        if (p) return p;   // else something sits there (a region restored from a savestate): try further on
    }
    return nullptr;
}

void MoveNextPast(uintptr_t end) {
    if (end < kRegionsBase || end >= kRegionsBase + kThreadSlots * kRegionsPerThread) return;
    unsigned thread = (unsigned)((end - kRegionsBase) / kRegionsPerThread);
    LONG64 want = (LONG64)((end + 0x1FFFF) & ~(uintptr_t)0xFFFF);
    for (LONG64 cur = g_next[thread]; cur < want; cur = g_next[thread])
        if (InterlockedCompareExchange64(&g_next[thread], want, cur) == cur) break;
}

}  // namespace addr
