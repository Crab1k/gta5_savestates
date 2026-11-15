// Shared helpers of savestate.dll.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>

#define SAVESTATE_VERSION "1.0.2"

void Log(const char* fmt, ...);

// The game image (GTA5.exe) in memory.
struct Image {
    uintptr_t base = 0, end = 0;
    bool Contains(uintptr_t p) const { return p >= base && p < end; }
};
extern Image g_exe;

// "GTA5.exe+1234" / "d3d11.dll+5678" for log lines.
std::string Where(uintptr_t p);

void Unprotect(void* p, size_t n);
void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook);   // returns the previous target

// Low-level ntdll entry points not in the SDK import libraries.
namespace nt {
using QueryInformationThread_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using QueryEvent_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using QuerySemaphore_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using QueryMutant_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
extern QueryInformationThread_t QueryInformationThread;
extern QueryEvent_t QueryEvent;
extern QuerySemaphore_t QuerySemaphore;
extern QueryMutant_t QueryMutant;
void Init();
}
