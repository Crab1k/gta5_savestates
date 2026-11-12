// Can a handle be put at a chosen value? Reserve a block of handle values with dummy events, then free one value and
// duplicate an object right away: Windows hands out the entry it just freed.
#include <windows.h>
#include <cstdio>
#include <vector>

int main() {
    const uintptr_t lo = 0x40000, hi = 0x48000;   // the block we want to own
    std::vector<HANDLE> below, block;
    for (;;) {
        HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!h) { printf("CreateEvent failed\n"); return 1; }
        if ((uintptr_t)h < lo) below.push_back(h);
        else if ((uintptr_t)h < hi) block.push_back(h);
        else { CloseHandle(h); break; }
    }
    printf("reserved %zu values in [%llX, %llX), %zu dummies below\n", block.size(), (unsigned long long)lo,
           (unsigned long long)hi, below.size());
    for (HANDLE h : below) CloseHandle(h);   // give the low values back

    int ok = 0, tries = 0, maxRetry = 0;
    for (int i = 0; i < 2000; i++) {
        HANDLE target = block[(i * 37) % block.size()];
        HANDLE obj = CreateSemaphoreW(nullptr, 0, 10, nullptr);   // the object to move (any value)
        int attempt = 0;
        HANDLE got = nullptr;
        std::vector<HANDLE> misses;
        for (; attempt < 50; attempt++) {
            CloseHandle(target);
            DuplicateHandle(GetCurrentProcess(), obj, GetCurrentProcess(), &got, 0, FALSE, DUPLICATE_SAME_ACCESS);
            if (got == target) break;
            misses.push_back(got);   // landed elsewhere: keep it out of the way and try again
            HANDLE d = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // re-fill the target if someone took it
            if (d != target) misses.push_back(d);
        }
        tries += attempt + 1;
        if (attempt > maxRetry) maxRetry = attempt;
        if (got == target) ok++;
        for (HANDLE m : misses) CloseHandle(m);
        CloseHandle(obj);
        // put a dummy back so the block stays ours
        CloseHandle(got);
        HANDLE d = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (d != target) { printf("dummy refill went to %p instead of %p\n", d, target); }
    }
    printf("placed %d of 2000 at the chosen value, %d attempts in all, worst %d retries\n", ok, tries, maxRetry);
    return 0;
}
