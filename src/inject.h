// Loading a DLL into a process that was created suspended, before its exe's entry point runs (x64).
//
// The first thread of a new process starts in RtlUserThreadStart with RCX = the exe's entry point and RDX = its PEB,
// and calls RCX(RDX) once the Windows loader has loaded the static imports. Pointing RCX at a small stub makes that call
// load our DLL first and then jump to the real entry point with the same argument and stack. So the DLL is in place
// before the game's C runtime starts, and no file of the game is touched.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace inject {

// Stub page layout: code at +0, results at +0x80 (HMODULE), +0x88 (GetLastError), +0x90 (done flag), +0x98 an
// optional replacement for the entry point (set by the DLL while it loads: called with RCX = PEB, RDX = the real entry
// point), path at +0x100.
enum : uint32_t { kModule = 0x80, kError = 0x88, kDone = 0x90, kRedirect = 0x98, kPath = 0x100 };
constexpr uint8_t kStubSignature[] = {0x51, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB9};

struct Pending {
    HANDLE process = nullptr;
    uint8_t* page = nullptr;
};

// Prepares `thread` (the suspended first thread of `process`) to load `dll` before the entry point. On failure `why`
// says what went wrong and the process is left as it was.
inline bool AtEntryPoint(HANDLE process, HANDLE thread, const std::wstring& dll, Pending* out, std::wstring* why) {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &ctx)) { *why = L"cannot read the start-up state"; return false; }
    uint64_t base = 0;
    uint32_t lfanew = 0, entryRva = 0;
    SIZE_T n = 0;
    ReadProcessMemory(process, (void*)(ctx.Rdx + 0x10), &base, 8, &n);   // PEB.ImageBaseAddress
    ReadProcessMemory(process, (void*)(base + 0x3C), &lfanew, 4, &n);
    ReadProcessMemory(process, (void*)(base + lfanew + 0x28), &entryRva, 4, &n);
    const uint64_t entry = base + entryRva;
    if (!base || ctx.Rcx != entry) { *why = L"unexpected start-up state (not a 64-bit program?)"; return false; }

    auto* page = (uint8_t*)VirtualAllocEx(process, nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page) { *why = L"cannot allocate memory in the process"; return false; }
    const uint64_t p = (uint64_t)page;
    // kernel32 is at the same address in every 64-bit process of this boot.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const uint64_t loadLibrary = (uint64_t)GetProcAddress(k32, "LoadLibraryW");
    const uint64_t getLastError = (uint64_t)GetProcAddress(k32, "GetLastError");

    uint8_t code[128];
    int c = 0;
    auto b = [&](std::initializer_list<uint8_t> v) { for (uint8_t x : v) code[c++] = x; };
    auto q = [&](uint64_t v) { memcpy(code + c, &v, 8); c += 8; };
    b({0x51});                          // push rcx              (the PEB argument; also aligns the stack)
    b({0x48, 0x83, 0xEC, 0x20});        // sub rsp, 20h          (shadow space)
    b({0x48, 0xB9}); q(p + kPath);      // mov rcx, path
    b({0x48, 0xB8}); q(loadLibrary);    // mov rax, LoadLibraryW
    b({0xFF, 0xD0});                    // call rax
    b({0x48, 0xBA}); q(p + kModule);    // mov rdx, &module
    b({0x48, 0x89, 0x02});              // mov [rdx], rax
    b({0x48, 0xB8}); q(getLastError);   // mov rax, GetLastError
    b({0xFF, 0xD0});                    // call rax
    b({0x48, 0xBA}); q(p + kError);     // mov rdx, &error
    b({0x89, 0x02});                    // mov [rdx], eax
    b({0x48, 0xBA}); q(p + kDone);      // mov rdx, &done
    b({0xC7, 0x02, 1, 0, 0, 0});        // mov dword [rdx], 1
    b({0x48, 0x83, 0xC4, 0x20});        // add rsp, 20h
    b({0x59});                          // pop rcx
    b({0x48, 0xBA}); q(entry);          // mov rdx, entry point
    b({0x48, 0xA1}); q(p + kRedirect);  // mov rax, [redirect]
    b({0x48, 0x85, 0xC0});              // test rax, rax
    b({0x75, 0x03});                    // jnz +3
    b({0x48, 0x89, 0xD0});              // mov rax, rdx
    b({0xFF, 0xE0});                    // jmp rax

    bool ok = WriteProcessMemory(process, page, code, c, &n) &&
              WriteProcessMemory(process, page + kPath, dll.c_str(), (dll.size() + 1) * 2, &n);
    FlushInstructionCache(process, page, c);
    ctx.Rcx = p;
    if (!ok || !SetThreadContext(thread, &ctx)) {
        VirtualFreeEx(process, page, 0, MEM_RELEASE);
        *why = L"cannot prepare the process";
        return false;
    }
    out->process = process;
    out->page = page;
    return true;
}

// After the thread was resumed: waits until the stub ran. Returns false (with the Windows error) if the DLL didn't load.
inline bool Wait(const Pending& pending, DWORD timeoutMs, DWORD* error) {
    uint64_t state[3] = {};   // module, error, done
    SIZE_T n = 0;
    for (DWORD waited = 0; waited < timeoutMs; waited += 20) {
        if (WaitForSingleObject(pending.process, 20) == WAIT_OBJECT_0) break;
        ReadProcessMemory(pending.process, pending.page + kModule, state, sizeof(state), &n);
        if (state[2]) break;
    }
    *error = (DWORD)state[1];
    return state[2] && state[0];
}

}  // namespace inject
