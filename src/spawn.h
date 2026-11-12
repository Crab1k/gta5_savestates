// The DLL's role inside PlayGTAV.exe (Project 127's DragonEmu starter): when it starts GTA5.exe, start it suspended
// and load the DLL into it at its entry point too. The DLL to load comes from the environment variable set by the
// launcher, which both processes inherit.
#pragma once
#include "inject.h"
#include <windows.h>
#include <cwchar>
#include <string>

namespace spawn {

inline const wchar_t* kDllVar = L"GTA5SS_DLL";

inline bool IsGame(const std::wstring& commandOrPath) {
    std::wstring s = commandOrPath;
    for (auto& ch : s) ch = towlower(ch);
    return s.find(L"gta5.exe") != std::wstring::npos;
}

// Called right after a CreateProcess that we forced to CREATE_SUSPENDED.
inline void AfterCreate(const PROCESS_INFORMATION& pi, DWORD callerFlags, const std::wstring& what) {
    wchar_t dll[MAX_PATH] = {};
    GetEnvironmentVariableW(kDllVar, dll, MAX_PATH);
    inject::Pending pending;
    std::wstring why;
    if (!dll[0] || !inject::AtEntryPoint(pi.hProcess, pi.hThread, dll, &pending, &why)) {
        std::wstring msg = L"Savestates can't be added to " + what + L": " + (dll[0] ? why : L"no DLL given") +
                           L".\nThe game starts without savestates.";
        MessageBoxW(nullptr, msg.c_str(), L"GTA V Savestates", MB_ICONWARNING);
    }
    if (!(callerFlags & CREATE_SUSPENDED)) ResumeThread(pi.hThread);
}

inline decltype(&CreateProcessA) RealCreateProcessA;
inline decltype(&CreateProcessW) RealCreateProcessW;

inline BOOL WINAPI HookCreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                      BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si,
                                      LPPROCESS_INFORMATION pi) {
    std::string target = std::string(app ? app : "") + " " + (cmd ? cmd : "");
    std::wstring wtarget(target.begin(), target.end());
    if (!IsGame(wtarget)) return RealCreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    if (!RealCreateProcessA(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, dir, si, pi)) return FALSE;
    AfterCreate(*pi, flags, wtarget);
    return TRUE;
}

inline BOOL WINAPI HookCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                      BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                                      LPPROCESS_INFORMATION pi) {
    std::wstring target = std::wstring(app ? app : L"") + L" " + (cmd ? cmd : L"");
    if (!IsGame(target)) return RealCreateProcessW(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    if (!RealCreateProcessW(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, dir, si, pi)) return FALSE;
    AfterCreate(*pi, flags, target);
    return TRUE;
}

// Patches the starter's own import table (PlayGTAV.exe imports CreateProcessA from kernel32).
inline void Install(void* (*patchIat)(HMODULE, const char*, const char*, void*)) {
    HMODULE exe = GetModuleHandleW(nullptr);
    RealCreateProcessA = (decltype(RealCreateProcessA))patchIat(exe, "kernel32.dll", "CreateProcessA", (void*)&HookCreateProcessA);
    RealCreateProcessW = (decltype(RealCreateProcessW))patchIat(exe, "kernel32.dll", "CreateProcessW", (void*)&HookCreateProcessW);
    if (!RealCreateProcessA) RealCreateProcessA = &CreateProcessA;
    if (!RealCreateProcessW) RealCreateProcessW = &CreateProcessW;
}

// True when this DLL was loaded into the game itself (GTA5.exe) rather than into a starter.
inline bool InGameProcess() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    return _wcsicmp(name, L"GTA5.exe") == 0;
}

}  // namespace spawn
