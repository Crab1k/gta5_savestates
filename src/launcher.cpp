// GTA5-Savestates.exe: starts GTA V with savestate.dll loaded, without changing any file of the game.
//
// With Project 127 (and the Rockstar version) the game is started by PlayGTAV.exe, which sets up the launcher
// emulation and then runs GTA5.exe. So the launcher starts PlayGTAV.exe suspended and loads savestate.dll into it at
// its entry point; there the DLL makes PlayGTAV start GTA5.exe suspended as well and loads itself into the game the
// same way, before the game's own code runs.
//
//   GTA5-Savestates.exe [path\to\GTA5.exe or game folder] [--dll path] [--wait] [game arguments...]
//   Without a path: [Launcher] Game= in savestate.ini, else the install folder Rockstar's installer recorded, else a
//   folder dialog (the choice is saved to the ini). --wait: stay until the game has exited.
#define WIN32_LEAN_AND_MEAN
#include "inject.h"
#include "spawn.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>

static const wchar_t* kTitle = L"GTA V Savestates";

static int Fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), kTitle, MB_ICONERROR);
    return 1;
}

static std::wstring Dir(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Accepts a game folder or a path to GTA5.exe; returns the folder if it holds GTA5.exe.
static std::wstring GameFolder(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/' || p.back() == L'"')) p.pop_back();
    if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0) p = Dir(p);
    return FileExists(p + L"\\GTA5.exe") ? p : L"";
}

static std::wstring InstalledFolder() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V", L"InstallFolder",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return GameFolder(buf);
    return L"";
}

static std::wstring AskForFolder() {
    BROWSEINFOW bi = {};
    bi.lpszTitle = L"Where is GTA V installed (the folder with GTA5.exe)?";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return L"";
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    return GameFolder(path);
}

// ------------------------------------------------------------------ Project 1.27's launch settings
// Its Launch button runs "start /affinity <mask> playgtav.exe -uilanguage <language> [-NoStutterFix] [-NoAudioFix]
// [-NoCrashFix]" (or the user's own arguments), and sets GTA5.exe to high priority 5 s after it started. The game is
// started the same way here, from the settings P127 keeps in the registry, or its defaults when it isn't installed.
// The affinity ("core fix") matters: GTA V 1.27 fails with ERR_SYS_THREAD on CPUs with many threads.
static std::wstring P127(const wchar_t* name, const wchar_t* def) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Project_127", name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return def;
}
static bool P127On(const wchar_t* name, bool def) { return _wcsicmp(P127(name, def ? L"True" : L"False").c_str(), L"True") == 0; }

// First n logical processors, n = the processor count kept between 4 and 16 (as P127 computes it).
static DWORD_PTR CoreFixMask() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n < 4) n = 4;
    if (n > 16) n = 16;
    return ((DWORD_PTR)1 << n) - 1;
}

struct Launch {
    std::wstring args;            // after the executable
    DWORD_PTR affinity = 0;       // 0: leave it
    bool highPriority = false;
};

static Launch FromP127() {
    Launch l;
    std::wstring lang = P127(L"LanguageSelected", L"English");
    if (_wcsicmp(lang.c_str(), L"English") == 0) lang = L"american";
    for (wchar_t& ch : lang) ch = towlower(ch);
    l.args = L"-uilanguage " + lang;
    if (!P127On(L"EnableStutterFix", true)) l.args += L" -NoStutterFix";
    if (!P127On(L"EnableAudioFix", true)) l.args += L" -NoAudioFix";
    if (!P127On(L"EnableCrashFix", true)) l.args += L" -NoCrashFix";
    if (P127On(L"EnableCoreFix", true)) l.affinity = CoreFixMask();
    l.highPriority = P127On(L"EnableAutoSetHighPriority", true);
    // The user's own line replaces all of it (it may carry its own "start /affinity <hex>" and "gta_p127.exe").
    std::wstring own = P127(L"OverWriteGTACommandLineArgs", L"");
    if (P127On(L"EnableOverWriteGTACommandLineArgs", false) && own.find_first_not_of(L" \t") != std::wstring::npos) {
        l.affinity = 0;
        std::wstring rest;
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW((L"x " + own).c_str(), &argc);
        for (int i = 1; argv && i < argc; i++) {
            std::wstring a = argv[i];
            if (_wcsicmp(a.c_str(), L"start") == 0 || _wcsicmp(a.c_str(), L"gta_p127.exe") == 0) continue;
            if (_wcsicmp(a.c_str(), L"/affinity") == 0 && i + 1 < argc) {
                l.affinity = (DWORD_PTR)wcstoull(argv[++i], nullptr, 16);
                continue;
            }
            rest += (rest.empty() ? L"" : L" ") + a;
        }
        if (argv) LocalFree(argv);
        l.args = rest;
    }
    return l;
}

static std::wstring Quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    for (wchar_t c : s) { if (c == L'"') r += L'\\'; r += c; }
    return r + L"\"";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring dir = Dir(self), ini = dir + L"\\savestate.ini";
    std::wstring dll = dir + L"\\savestate.dll";

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game, args;
    bool wait = false, argsGiven = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--wait") wait = true;
        else if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (game.empty() && !GameFolder(a).empty()) game = GameFolder(a);
        else { args += L" " + Quote(a); argsGiven = true; }
    }
    LocalFree(argv);
    if (!FileExists(dll)) return Fail(L"savestate.dll is missing:\n" + dll);

    if (game.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"Launcher", L"Game", L"", buf, MAX_PATH, ini.c_str());
        game = buf[0] ? GameFolder(buf) : L"";
    }
    if (game.empty()) game = InstalledFolder();
    if (game.empty()) {
        game = AskForFolder();
        if (game.empty()) return 1;
        WritePrivateProfileStringW(L"Launcher", L"Game", game.c_str(), ini.c_str());
    }
    // As Project 1.27 launches it; savestate.ini can override each part.
    Launch launch = FromP127();
    if (!argsGiven) {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(L"Launcher", L"Arguments", L"", buf, 512, ini.c_str());
        args = L" " + (buf[0] ? std::wstring(buf) : launch.args);
    }
    auto setting = [&](const wchar_t* key) {   // -1: empty or missing (as P127), else the number
        wchar_t buf[16] = {};
        GetPrivateProfileStringW(L"Launcher", key, L"", buf, 16, ini.c_str());
        return buf[0] ? _wtoi(buf) : -1;
    };
    const int coreFix = setting(L"CoreFix");
    if (coreFix == 0) launch.affinity = 0;
    if (coreFix == 1 && !launch.affinity) launch.affinity = CoreFixMask();
    const int high = setting(L"HighPriority");
    if (high >= 0) launch.highPriority = high != 0;

    // PlayGTAV.exe starts GTA5.exe (Project 127 / Rockstar); without it, start the game directly.
    const bool viaStarter = FileExists(game + L"\\PlayGTAV.exe");
    const std::wstring exe = game + (viaStarter ? L"\\PlayGTAV.exe" : L"\\GTA5.exe");
    SetEnvironmentVariableW(spawn::kDllVar, dll.c_str());
    SetEnvironmentVariableW(L"GTA5SS_HIGH_PRIORITY", launch.highPriority ? L"1" : nullptr);   // done by the DLL in GTA5.exe

    std::wstring cmd = Quote(exe) + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, game.c_str(), &si, &pi))
        return Fail(L"Cannot start " + exe + L" (error " + std::to_wstring(GetLastError()) + L").");
    // Before it runs: PlayGTAV.exe passes the affinity on to GTA5.exe.
    if (launch.affinity) SetProcessAffinityMask(pi.hProcess, launch.affinity);

    inject::Pending pending;
    std::wstring why;
    if (!inject::AtEntryPoint(pi.hProcess, pi.hThread, dll, &pending, &why)) {
        TerminateProcess(pi.hProcess, 1);
        return Fail(L"Cannot start " + exe + L" with savestates: " + why + L".");
    }
    ResumeThread(pi.hThread);
    DWORD error = 0;
    if (!inject::Wait(pending, 15000, &error))
        Fail(L"savestate.dll could not be loaded (error " + std::to_wstring(error) + L"). The game runs without savestates.");

    if (wait) {
        // PlayGTAV.exe stays until the game exits.
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
