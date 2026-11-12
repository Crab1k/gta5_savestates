#include "common.h"
#include <cstdio>

Image g_exe;

namespace nt {
QueryInformationThread_t QueryInformationThread;
QueryEvent_t QueryEvent;
QuerySemaphore_t QuerySemaphore;
QueryMutant_t QueryMutant;

void Init() {
    HMODULE m = GetModuleHandleA("ntdll.dll");
    QueryInformationThread = (QueryInformationThread_t)GetProcAddress(m, "NtQueryInformationThread");
    QueryEvent = (QueryEvent_t)GetProcAddress(m, "NtQueryEvent");
    QuerySemaphore = (QuerySemaphore_t)GetProcAddress(m, "NtQuerySemaphore");
    QueryMutant = (QueryMutant_t)GetProcAddress(m, "NtQueryMutant");
}
}  // namespace nt

std::string Where(uintptr_t p) {
    char b[160];
    if (g_exe.Contains(p)) {
        sprintf_s(b, "GTA5.exe+%llX", (unsigned long long)(p - g_exe.base));
        return b;
    }
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    else
        m = nullptr;
    const char* s = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    sprintf_s(b, "%s+%llX", s, (unsigned long long)(p - (uintptr_t)m));
    return b;
}

void Unprotect(void* p, size_t n) {
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
}

void* PatchIat(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = (uint8_t*)mod;
    auto* nth = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        for (int i = 0; iat[i].u1.Function; i++) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            void* old = (void*)iat[i].u1.Function;
            Unprotect(&iat[i].u1.Function, sizeof(void*));
            iat[i].u1.Function = (ULONGLONG)hook;
            return old;
        }
    }
    Log("import %s!%s not found", dll, func);
    return nullptr;
}
