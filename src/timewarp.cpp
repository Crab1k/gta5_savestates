#include "timewarp.h"
#include "common.h"
#include <mmsystem.h>

namespace timewarp {

static volatile int64_t g_qpcOffset = 0;
static volatile LONG g_msOffset = 0;
static BOOL(WINAPI* RealQpc)(LARGE_INTEGER*) = &QueryPerformanceCounter;
static DWORD(WINAPI* RealTimeGetTime)();

static BOOL WINAPI Hook_QueryPerformanceCounter(LARGE_INTEGER* out) {
    BOOL ok = RealQpc(out);
    out->QuadPart -= g_qpcOffset;
    return ok;
}

static DWORD WINAPI Hook_timeGetTime() { return RealTimeGetTime() - (DWORD)g_msOffset; }

void InstallHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (void* p = PatchIat(exe, "kernel32.dll", "QueryPerformanceCounter", (void*)&Hook_QueryPerformanceCounter))
        RealQpc = (decltype(RealQpc))p;
    RealTimeGetTime = (decltype(RealTimeGetTime))PatchIat(exe, "winmm.dll", "timeGetTime", (void*)&Hook_timeGetTime);
    if (!RealTimeGetTime) RealTimeGetTime = (decltype(RealTimeGetTime))GetProcAddress(LoadLibraryA("winmm.dll"), "timeGetTime");
}

int64_t Qpc() {
    LARGE_INTEGER t;
    RealQpc(&t);
    return t.QuadPart - g_qpcOffset;
}

uint32_t Ms() { return RealTimeGetTime() - (DWORD)g_msOffset; }

void Continue(int64_t qpc, uint32_t ms) {
    LARGE_INTEGER t;
    RealQpc(&t);
    InterlockedExchange64(&g_qpcOffset, t.QuadPart - qpc);
    InterlockedExchange(&g_msOffset, (LONG)(RealTimeGetTime() - ms));
}

}  // namespace timewarp
