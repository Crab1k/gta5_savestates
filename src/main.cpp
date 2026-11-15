// savestate.dll: full savestates for GTA V 1.27 (loaded by GTA5-Savestates.exe).
//   F9 save to the current slot, F12 load it, F10/F11 previous/next slot (savestate.ini to change keys).
// Inside PlayGTAV.exe (the starter) the same DLL only makes sure it is loaded into GTA5.exe too (spawn.h).
#include "addr.h"
#include "audio.h"
#include "census.h"
#include "common.h"
#include "d3d.h"
#include "foreign.h"
#include "kobjects.h"
#include "locks.h"
#include "memory.h"
#include "overlay.h"
#include "spawn.h"
#include "state.h"
#include "threads.h"
#include "timewarp.h"
#include <cctype>
#include <cstdarg>
#include <intrin.h>
#include <cstdio>

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static char g_selfDir[MAX_PATH], g_iniPath[MAX_PATH];

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static int IniInt(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_iniPath); }

// ------------------------------------------------------------------ how the game ends (diagnostics)
static std::string CallerTrail(int skip) {
    void* frames[16] = {};
    USHORT n = RtlCaptureStackBackTrace(skip, 16, frames, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; i++) s += " < " + Where((uintptr_t)frames[i]);
    return s;
}

static void(WINAPI* RealExitProcess)(UINT) = &ExitProcess;
static BOOL(WINAPI* RealTerminateProcess)(HANDLE, UINT) = &TerminateProcess;

static void WINAPI Hook_ExitProcess(UINT code) {
    Log("the game calls ExitProcess(%u) from%s", code, CallerTrail(1).c_str());
    RealExitProcess(code);
}

static BOOL WINAPI Hook_TerminateProcess(HANDLE p, UINT code) {
    if (GetProcessId(p) == GetCurrentProcessId()) Log("the game calls TerminateProcess(%u) from%s", code, CallerTrail(1).c_str());
    return RealTerminateProcess(p, code);
}

// ------------------------------------------------------------------ functions the game looks up itself
static FARPROC(WINAPI* RealGetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI Hook_GetProcAddress(HMODULE m, LPCSTR name) {
    if ((uintptr_t)name > 0xFFFF)
    {
        if (void* hook = kobj::HookForProcAddress(name)) return (FARPROC)hook;
        if (void* hook = d3d::HookForProcAddress(name)) return (FARPROC)hook;
        if (void* hook = threads::HookForProcAddress(name)) return (FARPROC)hook;
        if (void* hook = locks::HookForProcAddress(name)) return (FARPROC)hook;
        if (void* hook = foreign::HookForProcAddress(name)) return (FARPROC)hook;
        if (!strcmp(name, "ExitProcess")) return (FARPROC)&Hook_ExitProcess;
        if (!strcmp(name, "TerminateProcess")) return (FARPROC)&Hook_TerminateProcess;
    }
    return RealGetProcAddress(m, name);
}

// ------------------------------------------------------------------ feedback
static HWND g_window = nullptr;
static wchar_t g_title[256];

static BOOL CALLBACK FindGameWindow(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"grcWindow")) {
        g_window = w;
        GetWindowTextW(w, g_title, 256);
        return FALSE;
    }
    return TRUE;
}

static bool Focused() {
    if (!g_window) EnumWindows(&FindGameWindow, 0);
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static ULONGLONG g_titleUntil = 0;

// The window belongs to a game thread: never wait for it (it may be stopped or hung).
static void SetTitle(const wchar_t* text) {
    DWORD_PTR r;
    SendMessageTimeoutW(g_window, WM_SETTEXT, 0, (LPARAM)text, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &r);
}

// The game's window title carries the message (windowed mode); a sound tells success or failure in any mode.
static bool g_showMessages = true;
static bool g_census = false;   // [Lab] Census=1: log what the game memory points at after each save

static void Say(const std::string& s, bool ok) {
    Log("%s", s.c_str());
    // A second line is a warning: long enough to read.
    if (g_showMessages) overlay::Show(s, s.find('\n') != std::string::npos ? 8000 : 2500);
    if (g_window) {
        std::string one = s;
        for (char& ch : one) if (ch == '\n') ch = ' ';
        std::wstring w(one.begin(), one.end());
        SetTitle((std::wstring(g_title) + L"  -  " + w).c_str());
        g_titleUntil = GetTickCount64() + 4000;
    }
    MessageBeep(ok ? MB_OK : MB_ICONHAND);
}

// Before a long stop: the message must be on screen before the game stops drawing.
static void Notice(const std::string& s) {
    Log("%s", s.c_str());
    if (!g_showMessages) return;
    overlay::Show(s, 60000);
    overlay::WaitFrames(2, 500);
}

// ------------------------------------------------------------------ hotkeys
static int g_keySave, g_keyLoad, g_keyPrev, g_keyNext, g_slot = 0;
static bool g_down[256];

// Key names savestate.ini may use (besides F1-F24, A-Z, 0-9 and NumPad0-NumPad9), as README.md lists them.
struct KeyName {
    const char* name;
    int vk;
};
static const KeyName kKeyNames[] = {
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL}, {"Backspace", VK_BACK}, {"Tab", VK_TAB}, {"Enter", VK_RETURN},
    {"Space", VK_SPACE}, {"NumPadAdd", VK_ADD}, {"NumPadSubtract", VK_SUBTRACT}, {"NumPadMultiply", VK_MULTIPLY},
    {"NumPadDivide", VK_DIVIDE}, {"NumPadDecimal", VK_DECIMAL}, {"Minus", VK_OEM_MINUS}, {"Equals", VK_OEM_PLUS},
    {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Semicolon", VK_OEM_1}, {"Slash", VK_OEM_2},
    {"Backquote", VK_OEM_3}, {"LeftBracket", VK_OEM_4}, {"Backslash", VK_OEM_5}, {"RightBracket", VK_OEM_6},
    {"Quote", VK_OEM_7}, {"MouseMiddle", VK_MBUTTON}, {"Mouse4", VK_XBUTTON1}, {"Mouse5", VK_XBUTTON2},
};

// "F9", "numpad5", "X", "Mouse4" ... or a virtual-key code ("120", "0x78"); 0 if it is none of them.
static int KeyFromName(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(0, 1);
    if (s.empty()) return 0;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);   // A-Z, 0-9
    auto number = [](const std::string& t, int base) {
        char* end = nullptr;
        long v = strtol(t.c_str(), &end, base);
        return end && !*end && v > 0 && v < 256 ? (int)v : 0;
    };
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return number(s.substr(2), 16);
    if (isdigit((unsigned char)s[0])) return number(s, 10);
    if ((s[0] == 'F' || s[0] == 'f') && s.size() <= 3 && isdigit((unsigned char)s[1])) {
        int n = number(s.substr(1), 10);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    if (s.size() == 7 && _strnicmp(s.c_str(), "NumPad", 6) == 0 && isdigit((unsigned char)s[6])) return VK_NUMPAD0 + (s[6] - '0');
    for (const KeyName& k : kKeyNames)
        if (_stricmp(s.c_str(), k.name) == 0) return k.vk;
    return 0;
}

static std::string KeyLabel(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NumPad" + std::to_string(vk - VK_NUMPAD0);
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk) return k.name;
    return "key code " + std::to_string(vk);
}

// A key setting of savestate.ini; empty or missing: the default. A name it doesn't know: the default, and the log says so.
static int IniKey(const char* setting, int def) {
    char buf[64] = {};
    GetPrivateProfileStringA("Savestate", setting, "", buf, sizeof(buf), g_iniPath);
    if (!buf[0]) return def;
    int vk = KeyFromName(buf);
    if (!vk) {
        Log("savestate.ini: %s=%s is not a key this program knows (see README.md); %s is used", setting, buf, KeyLabel(def).c_str());
        return def;
    }
    return vk;
}

static bool Pressed(int vk) {
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = d && !g_down[vk & 0xFF];
    g_down[vk & 0xFF] = d;
    return edge;
}

// After a load, a game that stops answering its window is logged once (where every thread is), for diagnosis.
static ULONGLONG g_watchUntil = 0;
static void WatchForHang() {
    static ULONGLONG hungSince = 0;
    if (!g_watchUntil || !g_window) return;
    ULONGLONG now = GetTickCount64();
    if (now > g_watchUntil) { g_watchUntil = 0; hungSince = 0; return; }
    if (!IsHungAppWindow(g_window)) { hungSince = 0; return; }
    if (!hungSince) hungSince = now;
    if (now - hungSince > 3000) {
        threads::DumpAll("the game stopped answering after a load", &kobj::Describe);
        g_watchUntil = 0;
        hungSince = 0;
    }
}

static ULONGLONG g_audioCheckAt = 0;

static DWORD WINAPI Worker(void*) {
    // As Project 1.27 does (the launcher passes its setting): GTA5.exe at high priority, 5 s after the start.
    wchar_t high[4] = {};
    ULONGLONG raiseAt = GetEnvironmentVariableW(L"GTA5SS_HIGH_PRIORITY", high, 4) && high[0] == L'1' ? GetTickCount64() + 5000 : 0;
    unsigned swapEpoch = d3d::SwapEpoch();
    for (;;) {
        Sleep(15);
        // The game switched between fullscreen and a window or changed resolution: the savestates made before go.
        const unsigned epoch = d3d::SwapEpoch();
        std::vector<int> lost;
        if (epoch != swapEpoch && ss::DropStale(&lost)) {
            swapEpoch = epoch;
            if (!lost.empty()) {
                std::string list;
                for (size_t i = 0; i < lost.size(); i++)
                    list += (i ? (i + 1 == lost.size() ? " and " : ", ") : "") + std::to_string(lost[i] + 1);
                Say((lost.size() > 1 ? "Savestates " : "Savestate ") + list +
                        " deleted: savestates don't last through a switch between fullscreen and a window or a change of resolution",
                    false);
            }
        }
        if (raiseAt && GetTickCount64() > raiseAt) {
            raiseAt = 0;
            Log("high priority: %s", SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) ? "set" : "could not be set");
        }
        WatchForHang();
        d3d::FinishManifests();
        if (g_audioCheckAt && GetTickCount64() > g_audioCheckAt) {
            g_audioCheckAt = 0;
            audio::Report("3 s after the load");
        }
        if (g_titleUntil && GetTickCount64() > g_titleUntil) {
            g_titleUntil = 0;
            if (g_window) SetTitle(g_title);
        }
        bool focused = Focused();
        bool save = Pressed(g_keySave), load = Pressed(g_keyLoad), prev = Pressed(g_keyPrev), next = Pressed(g_keyNext);
        if (!focused) continue;
        std::string msg;
        if (save) {
            audio::Report("before the save");
            bool ok = ss::Save(g_slot, msg);
            Say(ok ? msg : "Cannot save: " + msg, ok);
            if (ok && g_census) {
                census::Run("after a save");
                d3d::Stats();
            }
        } else if (load) {
            bool ok = ss::Load(g_slot, msg);
            Say(ok ? msg : "Cannot load: " + msg, ok);
            if (ok) {
                g_watchUntil = GetTickCount64() + 120000;
                g_audioCheckAt = GetTickCount64() + 3000;
            }
        } else if (prev || next) {
            g_slot = (g_slot + (next ? 1 : ss::MAX_SLOTS - 1)) % ss::MAX_SLOTS;
            Say(ss::Describe(g_slot), true);
        }
    }
}

// ------------------------------------------------------------------ crash log
// Logs the first access violations with where they happened and the code addresses on the stack; never handles them.
static HMODULE g_self;

static bool CodeBytes(const uint8_t* ip, char* out, size_t n) {
    __try {
        for (int i = 0; i < 48; i++) sprintf_s(out + i * 3, n - i * 3, "%02X ", ip[i]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Logs the first few serious exceptions (and C++ exceptions) anywhere in the process: where, and the code addresses
// on the stack. The game's own handlers still get them.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0, cxx = 0;
    auto* er = ep->ExceptionRecord;
    const DWORD code = er->ExceptionCode;
    const bool isCxx = code == 0xE06D7363;
    if ((code & 0xF0000000) != 0xC0000000 && !isCxx) return EXCEPTION_CONTINUE_SEARCH;
    // Our own page probes fault on purpose (and handle it).
    HMODULE m = nullptr;
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &m) && m == g_self)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(isCxx ? &cxx : &count) > (isCxx ? 3 : 8)) return EXCEPTION_CONTINUE_SEARCH;
    auto* cx = ep->ContextRecord;
    std::string trail;
    int found = 0;
    for (auto* sp = (uintptr_t*)cx->Rsp; found < 16 && sp < (uintptr_t*)cx->Rsp + 4096; sp++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = *sp;
        if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            continue;
        trail += " " + Where(v);
        found++;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION)
        Log("CRASH (first chance) access violation at %s, %s %p, thread %u | stack:%s", Where((uintptr_t)er->ExceptionAddress).c_str(),
            er->ExceptionInformation[0] ? "write" : "read", (void*)er->ExceptionInformation[1], GetCurrentThreadId(), trail.c_str());
    else
        Log("%s (first chance) %08X at %s, thread %u | stack:%s", isCxx ? "C++ exception" : "CRASH", code,
            Where((uintptr_t)er->ExceptionAddress).c_str(), GetCurrentThreadId(), trail.c_str());
    Log("  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX rsp=%llX r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX",
        cx->Rax, cx->Rbx, cx->Rcx, cx->Rdx, cx->Rsi, cx->Rdi, cx->Rbp, cx->Rsp, cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15);
    char bytes[200] = "";
    if (!isCxx && CodeBytes((const uint8_t*)er->ExceptionAddress - 16, bytes, sizeof(bytes))) Log("  code at ip-16: %s", bytes);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ entry
static void Init(HMODULE self) {
    g_self = self;
    InitializeCriticalSection(&g_logCs);
    GetModuleFileNameA(self, g_selfDir, MAX_PATH);
    *strrchr(g_selfDir, '\\') = 0;
    char logPath[MAX_PATH];
    sprintf_s(logPath, "%s\\savestate.log", g_selfDir);
    char prevPath[MAX_PATH];
    sprintf_s(prevPath, "%s\\savestate.prev.log", g_selfDir);
    MoveFileExA(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);   // the previous game session's, kept for comparison
    g_log = _fsopen(logPath, "w", _SH_DENYNO);
    sprintf_s(g_iniPath, "%s\\savestate.ini", g_selfDir);

    HMODULE exe = GetModuleHandleW(nullptr);
    auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_exe.base = (uintptr_t)exe;
    g_exe.end = g_exe.base + nth->OptionalHeader.SizeOfImage;
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    Log("savestate.dll %s in %s (base %p, timestamp %08X)", SAVESTATE_VERSION, exePath, exe, nth->FileHeader.TimeDateStamp);
    {
        // The system, for reports from other PCs.
        RTL_OSVERSIONINFOW v = {sizeof(v)};
        if (auto get = (LONG(WINAPI*)(RTL_OSVERSIONINFOW*))GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) get(&v);
        MEMORYSTATUSEX ms = {sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        DWORD_PTR mask = 0, sysMask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &mask, &sysMask);
        Log("system: Windows %lu.%lu build %lu, %lu CPU threads (%u groups), game limited to %d of them (mask %llX), "
            "RAM %llu MB (%llu free), commit limit %llu MB (%llu free)",
            v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
            (unsigned)GetActiveProcessorGroupCount(), (int)__popcnt64(mask), (unsigned long long)mask,
            (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20),
            (unsigned long long)(ms.ullTotalPageFile >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20));
    }
    AddVectoredExceptionHandler(1, &CrashFilter);

    nt::Init();
    kobj::LogCreates(IniInt("Lab", "LogHandles", 0));
    char folder[MAX_PATH] = {};
    GetPrivateProfileStringA("Savestate", "Folder", "", folder, MAX_PATH, g_iniPath);
    if (!folder[0]) sprintf_s(folder, "%s\\savestates", g_selfDir);
    kobj::Init();
    threads::Init();
    threads::MoveMainThread();
    mem::Init((size_t)IniInt("Savestate", "ArenaMB", 512) << 20);
    mem::InstallHooks();
    threads::InstallHooks();
    kobj::InstallHooks();
    locks::InstallHooks();
    foreign::InstallHooks();
    if (void* p = PatchIat(exe, "kernel32.dll", "ExitProcess", (void*)&Hook_ExitProcess)) RealExitProcess = (decltype(RealExitProcess))p;
    if (void* p = PatchIat(exe, "kernel32.dll", "TerminateProcess", (void*)&Hook_TerminateProcess)) RealTerminateProcess = (decltype(RealTerminateProcess))p;
    timewarp::InstallHooks();
    audio::InstallHooks();
    RealGetProcAddress = (decltype(RealGetProcAddress))PatchIat(exe, "kernel32.dll", "GetProcAddress", (void*)&Hook_GetProcAddress);
    if (!RealGetProcAddress) RealGetProcAddress = &GetProcAddress;

    g_keySave = IniKey("SaveKey", VK_F9);
    g_keyLoad = IniKey("LoadKey", VK_F12);
    g_keyPrev = IniKey("PrevSlotKey", VK_F10);
    g_keyNext = IniKey("NextSlotKey", VK_F11);
    Log("keys: save %s, load %s, previous slot %s, next slot %s", KeyLabel(g_keySave).c_str(), KeyLabel(g_keyLoad).c_str(),
        KeyLabel(g_keyPrev).c_str(), KeyLabel(g_keyNext).c_str());
    {
        const int keys[] = {g_keySave, g_keyLoad, g_keyPrev, g_keyNext};
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                if (keys[i] == keys[j]) Log("savestate.ini: %s is set for two actions; it does only one of them", KeyLabel(keys[i]).c_str());
    }
    g_showMessages = IniInt("Savestate", "ShowMessages", 1) != 0;
    g_census = IniInt("Lab", "Census", 0) != 0;

    // Savestates last until the game is closed; the lab can keep them for later sessions (unfinished).
    const bool otherSessions = IniInt("Lab", "OtherSessions", 0) != 0;
    ss::SetOtherSessions(otherSessions);
    d3d::DeclineAmdExtensions(otherSessions);
    ss::SetForceRecreate(IniInt("Lab", "ForceRecreate", 0) != 0);
    ss::SetReleaseFirst(IniInt("Lab", "ReleaseFirst", 1) != 0);
    ss::SetNotice(&Notice);
    ss::SetLogLayout(IniInt("Lab", "LogLayout", 0) != 0);
    ss::Init(folder);
    CloseHandle(CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr));
    Log("ready: savestates in %s (main thread %u)", folder, GetCurrentThreadId());
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (spawn::InGameProcess()) Init(h);
        else spawn::Install(&PatchIat);
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        Log("process ending, on thread %u:%s", GetCurrentThreadId(), CallerTrail(1).c_str());
    }
    return TRUE;
}
