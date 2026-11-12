#include "store.h"
#include "common.h"
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace store {

static std::string g_dir;
static std::mutex g_lock;
static std::unordered_set<uint64_t>* g_known;            // on disk or queued
static std::deque<std::pair<uint64_t, std::vector<uint8_t>>>* g_queue;
static HANDLE g_work, g_idle;
static volatile LONG g_inFlight = 0;
static Mode g_mode = Off;

static std::string PathOf(uint64_t h) {
    char b[MAX_PATH];
    sprintf_s(b, "%s\\%02x\\%016llx.bin", g_dir.c_str(), (unsigned)(h >> 56), (unsigned long long)h);
    return b;
}

static DWORD WINAPI Writer(void*) {
    for (;;) {
        WaitForSingleObject(g_work, INFINITE);
        for (;;) {
            std::pair<uint64_t, std::vector<uint8_t>> item;
            {
                std::lock_guard<std::mutex> l(*&g_lock);
                if (g_queue->empty()) break;
                item = std::move(g_queue->front());
                g_queue->pop_front();
            }
            std::string path = PathOf(item.first), tmp = path + ".tmp";
            char dir[MAX_PATH];
            sprintf_s(dir, "%s\\%02x", g_dir.c_str(), (unsigned)(item.first >> 56));
            CreateDirectoryA(dir, nullptr);
            HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD done = 0;
            bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, item.second.data(), (DWORD)item.second.size(), &done, nullptr) &&
                      done == item.second.size();
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileA(tmp.c_str());
                Log("store: writing %s failed", path.c_str());
                std::lock_guard<std::mutex> l(g_lock);
                g_known->erase(item.first);
            }
            InterlockedDecrement(&g_inFlight);
        }
        if (!g_inFlight) SetEvent(g_idle);
    }
}

void Init(const std::string& folder, Mode mode) {
    g_dir = folder + "\\gpu";
    g_mode = mode;
    g_known = new std::unordered_set<uint64_t>;
    g_queue = new std::deque<std::pair<uint64_t, std::vector<uint8_t>>>;
    g_work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (mode == Off) return;
    CreateDirectoryA(folder.c_str(), nullptr);
    CreateDirectoryA(g_dir.c_str(), nullptr);
    CloseHandle(CreateThread(nullptr, 0, &Writer, nullptr, 0, nullptr));
}

// 64-bit hash, 8 bytes per step (xxHash64-style mixing).
uint64_t Hash(const void* p, size_t n) {
    const uint64_t k1 = 0x9E3779B185EBCA87ull, k2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t h = 0x27D4EB2F165667C5ull ^ (n * k1);
    auto* q = (const uint8_t*)p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, q + i, 8);
        h ^= _rotl64(v * k2, 31) * k1;
        h = _rotl64(h, 27) * k1 + 0x85EBCA77C2B2AE63ull;
    }
    for (; i < n; i++) h = _rotl64(h ^ (q[i] * 0x165667B19E3779F9ull), 11) * k1;
    h ^= h >> 33;
    h *= k2;
    h ^= h >> 29;
    return h ? h : 1;   // 0 means "no content"
}

// The disk check and the note are one step under the lock, so Sweep can't delete a file between them.
bool Has(uint64_t h) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_known->count(h)) return true;
    if (g_mode != Keep || GetFileAttributesA(PathOf(h).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    g_known->insert(h);
    return true;
}

void Put(uint64_t h, std::vector<uint8_t>&& data) {
    if (g_mode == Off || Has(h)) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_known->insert(h).second) return;
        g_queue->push_back({h, std::move(data)});
    }
    InterlockedIncrement(&g_inFlight);
    ResetEvent(g_idle);
    SetEvent(g_work);
}

bool Get(uint64_t h, std::vector<uint8_t>* out) {
    if (g_mode == Off) return false;
    {
        // Still queued: take it from memory.
        std::lock_guard<std::mutex> l(g_lock);
        for (auto& item : *g_queue)
            if (item.first == h) { *out = item.second; return true; }
    }
    HANDLE f = CreateFileA(PathOf(h).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    out->resize((size_t)size.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(f, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size();
    CloseHandle(f);
    return ok;
}

void Flush() { WaitForSingleObject(g_idle, 60000); }
size_t Pending() { return (size_t)g_inFlight; }

}  // namespace store

namespace store {

void Sweep(const std::unordered_set<uint64_t>& keep) {
    const ULONGLONG t0 = GetTickCount64();
    uint64_t removed = 0, bytes = 0, kept = 0;
    for (int d = 0; d < 256; d++) {
        char pattern[MAX_PATH];
        sprintf_s(pattern, "%s\\%02x\\*.bin", g_dir.c_str(), d);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pattern, &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            uint64_t h = _strtoui64(fd.cFileName, nullptr, 16);
            if (keep.count(h)) { kept++; continue; }
            // Checked and deleted under the lock: a Put of the same data then writes it again.
            std::lock_guard<std::mutex> l(g_lock);
            if (g_known->count(h)) { kept++; continue; }   // stored or queued in this session
            char path[MAX_PATH];
            sprintf_s(path, "%s\\%02x\\%s", g_dir.c_str(), d, fd.cFileName);
            if (DeleteFileA(path)) {
                removed++;
                bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    Log("store: removed %llu contents no savestate needs (%.0f MB), %llu kept, in %u ms", (unsigned long long)removed,
        bytes / 1048576.0, (unsigned long long)kept, (unsigned)(GetTickCount64() - t0));
}

}  // namespace store

namespace store {

void Clear() {
    const ULONGLONG t0 = GetTickCount64();
    uint64_t removed = 0, bytes = 0;
    for (int d = 0; d < 256; d++) {
        char dir[MAX_PATH], pattern[MAX_PATH];
        sprintf_s(dir, "%s\\%02x", g_dir.c_str(), d);
        sprintf_s(pattern, "%s\\*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pattern, &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            const char* ext = strrchr(fd.cFileName, '.');
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ext || (_stricmp(ext, ".bin") && _stricmp(ext, ".tmp"))) continue;
            char path[MAX_PATH];
            sprintf_s(path, "%s\\%s", dir, fd.cFileName);
            if (DeleteFileA(path)) {
                removed++;
                bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
        RemoveDirectoryA(dir);   // only if empty
    }
    RemoveDirectoryA(g_dir.c_str());
    if (removed) Log("store: deleted the GPU data of the earlier game session's savestates (%llu files, %.0f MB) in %u ms",
                     (unsigned long long)removed, bytes / 1048576.0, (unsigned)(GetTickCount64() - t0));
}

}  // namespace store