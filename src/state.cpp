// File layout (all little-endian, packed), version 3:
//   Header
//   meta: modules and windows (foreign::Encode), the game's fiber/thread-local slot indices, its critical sections
//   per thread:  ThreadRec, CONTEXT, slot values, stack bytes [Rsp, StackBase), TLS block bytes
//   kernel objects: byte count, then kobj::Encode's records
//   per section: SectionRec, bytes
//   per region:  RegionRec, committed runs, page map (one byte per page: 0 never written, 1 zero, 2 data), data pages
//   outside pointers (foreign::EncodeTable) at Header.extOffset
// The GPU objects are in slotNN.gpu (d3d::Save) and the store under gpu\.
#include "state.h"
#include "addr.h"
#include "arena.h"
#include "audio.h"
#include "common.h"
#include "d3d.h"
#include "foreign.h"
#include "kobjects.h"
#include "locks.h"
#include "memory.h"
#include "store.h"
#include "threads.h"
#include "timewarp.h"
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace ss {

static std::string g_folder;
static uint64_t g_session = 0;   // creation time of this game process
static d3d::Capture* g_d3d[MAX_SLOTS];   // the Direct3D side of each slot made in this session
static std::set<uintptr_t> g_slotRegions[MAX_SLOTS];   // regions each slot's savestate has pages of
static std::set<uintptr_t> g_needed;                  // during a load: regions some savestate still needs
static bool g_forceRecreate = false;                  // lab: loads rebuild every object as for another session
static bool g_logLayout = false;                      // lab: log every region and thread stack at each save
static bool g_releaseFirst = true;                    // rebuild: let go of the live objects before (else after) it
static bool g_otherSessions = false;                  // lab: keep savestates for later game sessions (unfinished)

#pragma pack(push, 1)
struct Header {
    char magic[8];
    uint32_t version;
    uint32_t pid;
    uint64_t session;
    uint64_t exeBase;
    int64_t qpc;
    uint32_t ms;
    uint32_t threadCount;
    uint32_t kobjCount;
    uint32_t sectionCount;
    uint32_t regionCount;
    uint32_t nextOrdinal;   // the thread number CreateThread gave next
    uint64_t savedAt;       // FILETIME, local
    uint64_t pageBytes;     // bytes of region pages in the file
    uint64_t metaBytes;
    uint64_t extOffset;     // outside pointers
    uint64_t extBytes;
    uint64_t build;         // savestate.dll's link time and size: its own frames are on every saved stack
    uint64_t flags;         // kFor*
    uint64_t reserved[1];
};
struct ThreadRec {
    uint32_t tid;           // real thread id when saved
    uint32_t vtid;          // the id the game knows it by
    uint32_t ordinal;       // thread number (addr.h)
    uint32_t flsCount;
    uint32_t tlsCount;
    uint32_t pad;
    uint64_t stackBase;
    uint64_t stackBytes;
    uint64_t tlsBytes;
};
struct SectionRec {
    uint64_t addr;
    uint64_t size;
};
struct RegionRec {
    uint64_t base;
    uint64_t size;
    uint64_t pages;
    uint64_t dataPages;
    uint32_t runCount;
    uint32_t ownWatch;
};
#pragma pack(pop)

static const char kMagic[8] = {'G', 'T', 'A', '5', 'S', 'S', 'T', 0};

static uint64_t ThisBuild() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ThisBuild, &self);
    auto* nth = (IMAGE_NT_HEADERS*)((uint8_t*)self + ((IMAGE_DOS_HEADER*)self)->e_lfanew);
    return ((uint64_t)nth->FileHeader.TimeDateStamp << 32) | nth->OptionalHeader.SizeOfImage;
}
constexpr uint32_t kVersion = 3;
constexpr uint64_t kForOtherSessions = 1;   // outside pointers recorded (and GPU data requested): loadable later
constexpr size_t PAGE = 0x1000;

static std::string SlotPath(int slot) {
    char b[MAX_PATH];
    sprintf_s(b, "%s\\slot%02d.gss", g_folder.c_str(), slot + 1);
    return b;
}

// The slot's GPU objects as another session needs them (written once their contents are stored).
static std::string ManifestPath(int slot) {
    char b[MAX_PATH];
    sprintf_s(b, "%s\\slot%02d.gpu", g_folder.c_str(), slot + 1);
    return b;
}

void SetForceRecreate(bool on) { g_forceRecreate = on; }
void SetReleaseFirst(bool on) { g_releaseFirst = on; }
void SetOtherSessions(bool on) { g_otherSessions = on; }
// A rebuild from a manifest needs the GPU objects' contents stored at each save: only for another session or the lab.
static bool CaptureGpu() { return g_otherSessions || g_forceRecreate; }
static void (*g_notice)(const std::string&) = nullptr;
void SetNotice(void (*notice)(const std::string&)) { g_notice = notice; }
void SetLogLayout(bool on) { g_logLayout = on; }

// The store keeps GPU contents for the slots' manifests; what replaced savestates needed is garbage.
static DWORD WINAPI SweepStore(void*) {
    std::unordered_set<uint64_t> keep;
    for (int slot = 0; slot < MAX_SLOTS; slot++) d3d::References(ManifestPath(slot), &keep);
    store::Sweep(keep);
    return 0;
}

static DWORD WINAPI ClearStore(void*) {
    store::Clear();
    return 0;
}

// The game that made a savestate is still running (its process id with the same creation time).
static bool StillRunning(int slot) {
    Header h = {};
    DWORD got = 0;
    HANDLE f = CreateFileA(SlotPath(slot).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool read = ReadFile(f, &h, sizeof(h), &got, nullptr) && got >= 24 && !memcmp(h.magic, kMagic, 8);
    CloseHandle(f);
    if (!read || h.pid == GetCurrentProcessId()) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, h.pid);
    if (!p) return false;
    FILETIME c, e, k, u;
    bool same = GetProcessTimes(p, &c, &e, &k, &u) && (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == h.session;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return same && alive;
}

// Savestates last until the game is closed: the files an earlier session left are deleted (only ours: the slot files,
// their GPU data and unfinished .tmp files). Not those of a game that is still running.
static bool RemoveEarlier() {
    int removed = 0;
    bool kept = false;
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        if (StillRunning(slot)) {
            kept = true;
            continue;
        }
        for (const std::string& p : {SlotPath(slot), SlotPath(slot) + ".tmp", ManifestPath(slot), ManifestPath(slot) + ".tmp"})
            if (DeleteFileA(p.c_str())) removed++;
    }
    if (removed) Log("savestates of the earlier game session deleted (%d files)", removed);
    if (kept) Log("some savestates belong to a game that is still running: they stay");
    return !kept;
}

// One game at a time owns a savestates folder: a second copy started while the first runs (by mistake, say) must not
// delete the first one's savestates. Held for the life of the process.
static bool OwnFolder(const std::string& folder) {
    char full[MAX_PATH];
    if (!GetFullPathNameA(folder.c_str(), MAX_PATH, full, nullptr)) strcpy_s(full, folder.c_str());
    for (char* p = full; *p; p++) *p = (char)tolower((unsigned char)*p);
    char name[64];
    sprintf_s(name, "Local\\GTA5Savestates-%016llx", (unsigned long long)store::Hash(full, strlen(full)));
    HANDLE m = CreateMutexA(nullptr, FALSE, name);
    return m && GetLastError() != ERROR_ALREADY_EXISTS;
}

void Init(const std::string& folder) {
    g_folder = folder;
    CreateDirectoryA(folder.c_str(), nullptr);
    store::Init(folder, g_otherSessions ? store::Keep : CaptureGpu() ? store::Fresh : store::Off);
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    g_session = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
    if (!OwnFolder(folder)) {
        Log("another running game uses %s: its savestates are left alone", folder.c_str());
        return;
    }
    if (g_otherSessions) {
        CloseHandle(CreateThread(nullptr, 0, &SweepStore, nullptr, 0, nullptr));
    } else if (RemoveEarlier()) {
        // Their GPU contents too (the lab's rebuilds keep the folder and store their own in it).
        CloseHandle(CreateThread(nullptr, 0, CaptureGpu() ? &SweepStore : &ClearStore, nullptr, 0, nullptr));
    }
}

// ------------------------------------------------------------------ file I/O
// Collects a savestate in memory (64 MB chunks) while the game is stopped; the file is written after the game runs
// again, so the stop lasts only as long as a memory copy. Remembers where the game's memory went in the copy, for
// finding its outside pointers afterwards.
class Writer {
public:
    ~Writer() { for (Chunk& c : chunks_) VirtualFree(c.p, 0, MEM_RELEASE); }
    void Put(const void* p, size_t n) {
        auto* s = (const uint8_t*)p;
        while (n && ok_) {
            if (chunks_.empty() || chunks_.back().used == kChunk) {
                auto* m = (uint8_t*)VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (!m) {
                    // Out of RAM and page file: give everything back at once, so the rest of the save (still
                    // running while the game is stopped) has memory to finish and report it.
                    for (Chunk& c : chunks_) VirtualFree(c.p, 0, MEM_RELEASE);
                    chunks_.clear();
                    std::vector<Chunk>().swap(chunks_);
                    std::vector<Piece>().swap(pieces_);
                    ok_ = false;
                    outOfMemory_ = true;
                    return;
                }
                chunks_.push_back({m, 0});
            }
            Chunk& c = chunks_.back();
            size_t take = kChunk - c.used < n ? kChunk - c.used : n;
            memcpy(c.p + c.used, s, take);
            c.used += take;
            s += take;
            n -= take;
            total_ += take;
        }
    }
    // Game memory: also noted as a piece to scan.
    void PutMemory(const void* p, size_t n) {
        if (!ok_) return;
        pieces_.push_back({(uint64_t)p, total_, n});
        Put(p, n);
    }
    bool OutOfMemory() const { return outOfMemory_; }
    // Calls fn(game address, bytes, length) over the pieces, 8 bytes at a time where a piece crosses chunks.
    template <typename F>
    void ForEachPiece(F fn) const {
        for (const Piece& pc : pieces_) {
            uint64_t off = pc.offset, end = pc.offset + pc.n;
            while (off + 8 <= end) {
                size_t chunk = (size_t)(off / kChunk), within = (size_t)(off % kChunk);
                if (chunk >= chunks_.size()) return;
                uint64_t avail = kChunk - within < end - off ? kChunk - within : end - off;
                uint64_t whole = avail & ~(uint64_t)7;
                if (whole) {
                    fn(pc.addr + (off - pc.offset), chunks_[chunk].p + within, (size_t)whole);
                    off += whole;
                } else {   // 8 bytes split between two chunks
                    uint8_t b[8];
                    size_t first = (size_t)avail;
                    memcpy(b, chunks_[chunk].p + within, first);
                    if (chunk + 1 >= chunks_.size()) return;
                    memcpy(b + first, chunks_[chunk + 1].p, 8 - first);
                    fn(pc.addr + (off - pc.offset), b, 8);
                    off += 8;
                }
            }
        }
    }
    bool Ok() const { return ok_; }
    uint64_t Total() const { return total_; }
    uint8_t* Start() { return chunks_.empty() ? nullptr : chunks_[0].p; }   // for patching the header
    bool WriteTo(HANDLE f) {
        for (Chunk& c : chunks_) {
            DWORD done = 0;
            if (!WriteFile(f, c.p, (DWORD)c.used, &done, nullptr) || done != c.used) return false;
            VirtualFree(c.p, 0, MEM_RELEASE);
            c.p = nullptr;
        }
        chunks_.clear();
        return ok_;
    }

private:
    static constexpr size_t kChunk = 64u << 20;
    struct Chunk {
        uint8_t* p;
        size_t used;
    };
    struct Piece {
        uint64_t addr, offset, n;
    };
    std::vector<Chunk> chunks_;
    std::vector<Piece> pieces_;
    uint64_t total_ = 0;
    bool ok_ = true;
    bool outOfMemory_ = false;
};

class Reader {
public:
    explicit Reader(HANDLE f) : f_(f), buf_(4u << 20) {}
    bool Get(void* p, size_t n) {
        auto* d = (uint8_t*)p;
        while (n) {
            if (pos_ == len_) {
                if (n >= buf_.size()) return Raw(d, n);
                DWORD got = 0;
                if (!ReadFile(f_, buf_.data(), (DWORD)buf_.size(), &got, nullptr) || !got) return false;
                pos_ = 0;
                len_ = got;
            }
            size_t take = len_ - pos_ < n ? len_ - pos_ : n;
            memcpy(d, buf_.data() + pos_, take);
            pos_ += take;
            d += take;
            n -= take;
        }
        return true;
    }
    // Reads straight into the destination (game memory) once the buffer is used up.
    bool Raw(uint8_t* d, size_t n) {
        size_t take = len_ - pos_ < n ? len_ - pos_ : n;
        memcpy(d, buf_.data() + pos_, take);
        pos_ += take;
        d += take;
        n -= take;
        while (n) {
            DWORD chunk = n > (64u << 20) ? (64u << 20) : (DWORD)n, got = 0;
            if (!ReadFile(f_, d, chunk, &got, nullptr) || got != chunk) return false;
            d += chunk;
            n -= chunk;
        }
        return true;
    }
    bool Skip(uint64_t n) {
        size_t take = len_ - pos_ < n ? len_ - pos_ : (size_t)n;
        pos_ += take;
        n -= take;
        if (!n) return true;
        LARGE_INTEGER d;
        d.QuadPart = (LONGLONG)n;
        return SetFilePointerEx(f_, d, nullptr, FILE_CURRENT) != 0;
    }

private:
    HANDLE f_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0, len_ = 0;
};

// The meta block: lists of plain numbers around the foreign blob.
struct Meta {
    std::vector<foreign::Module> modules;
    std::vector<foreign::Window> windows;
    std::vector<DWORD> fls, tls;
    std::vector<uint64_t> critSecs;
};

static std::vector<uint8_t> EncodeMeta(const Meta& m) {
    std::vector<uint8_t> v;
    auto put = [&](const void* p, size_t n) { v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    std::vector<uint8_t> f = foreign::Encode(m.modules, m.windows);
    uint64_t n = f.size();
    put(&n, 8);
    put(f.data(), f.size());
    uint32_t k = (uint32_t)m.fls.size();
    put(&k, 4);
    put(m.fls.data(), k * 4);
    k = (uint32_t)m.tls.size();
    put(&k, 4);
    put(m.tls.data(), k * 4);
    n = m.critSecs.size();
    put(&n, 8);
    put(m.critSecs.data(), (size_t)n * 8);
    return v;
}

static bool DecodeMeta(const std::vector<uint8_t>& v, Meta* m) {
    const uint8_t* p = v.data();
    const uint8_t* end = p + v.size();
    auto get = [&](void* d, size_t n) {
        if ((size_t)(end - p) < n) return false;
        memcpy(d, p, n);
        p += n;
        return true;
    };
    uint64_t n = 0;
    uint32_t k = 0;
    if (!get(&n, 8) || n > (uint64_t)(end - p) || !foreign::Decode(p, (size_t)n, &m->modules, &m->windows)) return false;
    p += n;
    if (!get(&k, 4) || k > 4096) return false;
    m->fls.resize(k);
    if (!get(m->fls.data(), k * 4) || !get(&k, 4) || k > 4096) return false;
    m->tls.resize(k);
    if (!get(m->tls.data(), k * 4) || !get(&n, 8) || n > (uint64_t)(end - p) / 8) return false;
    m->critSecs.resize((size_t)n);
    return get(m->critSecs.data(), (size_t)n * 8);
}

// 0: page unreadable, 1: all zero, 2: data.
static int Classify(const uint8_t* page) {
    __try {
        auto* q = (const uint64_t*)page;
        uint64_t acc = 0;
        for (size_t i = 0; i < PAGE / 8; i += 8)
            acc |= q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7];
        return acc ? 2 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool ZeroPage(uint8_t* page) {
    __try {
        memset(page, 0, PAGE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------ save
// Captures no slot uses any more, waiting for a moment when the game is stopped.
static std::vector<d3d::Capture*> g_dropLater;

// Lets go of a capture's objects. Only while every game thread is parked (that may destroy objects the render thread
// uses): if the game can't be stopped now, the next save or load does it.
static void DropWhenParked(d3d::Capture* c) {
    if (!c) return;
    if (threads::ParkAll(5000, nullptr)) {
        d3d::Drop(c);
        threads::ReleaseAll();
    } else {
        g_dropLater.push_back(c);
    }
}

static void DropPending() {   // while parked
    for (d3d::Capture* c : g_dropLater) d3d::Drop(c);
    g_dropLater.clear();
}
static void WriteRegion(Writer& w, mem::Region* r, uint64_t* pageBytes) {
    mem::CollectWritten(r);
    std::vector<mem::Run> runs = mem::Committed(r);
    std::vector<uint8_t> map(r->pages, 0);
    uint64_t data = 0;
    for (size_t i = 0; i < r->pages; i++) {
        if (!r->Written(i)) continue;
        map[i] = (uint8_t)Classify(r->base + i * PAGE);
        if (map[i] == 2) data++;
    }
    RegionRec rr = {(uint64_t)r->base, r->size, r->pages, data, (uint32_t)runs.size(), r->ownWatch ? 1u : 0u};
    w.Put(&rr, sizeof(rr));
    w.Put(runs.data(), runs.size() * sizeof(mem::Run));
    w.Put(map.data(), map.size());
    for (size_t i = 0; i < r->pages;) {
        if (map[i] != 2) { i++; continue; }
        size_t j = i;
        while (j < r->pages && map[j] == 2 && j - i < 16384) j++;
        w.PutMemory(r->base + i * PAGE, (j - i) * PAGE);
        i = j;
    }
    *pageBytes += data * PAGE;
}

static bool SkipD3dObject(uint64_t p) { return d3d::Tracks(p); }
static std::string LabelTarget(uint64_t p) { return d3d::OtherOwner(p); }

static bool SaveImpl(int slot, std::string& msg) {
    const ULONGLONG t0 = GetTickCount64();
    const std::string path = SlotPath(slot), tmp = path + ".tmp";
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        msg = "cannot create " + tmp;
        return false;
    }

    audio::Stop();
    if (!threads::ParkAll(5000, &kobj::IoQuiet)) {
        audio::Start();
        CloseHandle(f);
        DeleteFileA(tmp.c_str());
        msg = "the game didn't stop in time (see savestate.log)";
        return false;
    }
    const ULONGLONG tParked = GetTickCount64();
    DropPending();
    mem::SetKeepFreed(true);   // from now on a savestate may refer to any region
    threads::SetKeep();         // and to any thread
    kobj::SetKeep();            // and to any open file

    DeleteFileA(ManifestPath(slot).c_str());   // belongs to the savestate this one replaces
    d3d::Capture* gpu = d3d::Save(CaptureGpu() ? ManifestPath(slot) : std::string(), g_d3d[slot]);
    std::vector<threads::Thread*> all;
    for (threads::Thread* t : threads::All())
        if (!t->zombie) all.push_back(t);   // ended for the game
    auto kobjs = kobj::Capture();
    auto sections = mem::DataSections();
    auto regions = mem::Regions();
    Meta meta;
    meta.modules = foreign::Modules();
    meta.windows = foreign::Windows();
    meta.fls = threads::FlsIndices();
    meta.tls = threads::TlsIndices();
    meta.critSecs = locks::List();
    std::vector<uint8_t> metaBlob = EncodeMeta(meta);

    Header h = {};
    memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.pid = GetCurrentProcessId();
    h.session = g_session;
    h.exeBase = g_exe.base;
    h.qpc = timewarp::Qpc();
    h.ms = timewarp::Ms();
    h.threadCount = (uint32_t)all.size();
    h.kobjCount = (uint32_t)kobjs.size();
    h.sectionCount = (uint32_t)sections.size();
    h.regionCount = (uint32_t)regions.size();
    h.nextOrdinal = threads::NextOrdinal();
    h.metaBytes = metaBlob.size();
    h.build = ThisBuild();
    h.flags = g_otherSessions ? kForOtherSessions : 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    FileTimeToLocalFileTime(&now, &now);
    h.savedAt = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;

    Writer w;
    w.Put(&h, sizeof(h));
    w.Put(metaBlob.data(), metaBlob.size());
    uint64_t stackTotal = 0;
    for (threads::Thread* t : all) {
        auto* sp = (uint8_t*)t->ctx.Rsp;
        uint8_t* tls = mem::TlsBlock(t->teb);
        ThreadRec tr = {t->tid, t->vtid, t->ordinal, (uint32_t)meta.fls.size(), (uint32_t)meta.tls.size(), 0,
                        (uint64_t)t->stackBase, (uint64_t)(t->stackBase - sp), tls ? (uint64_t)mem::TlsSize() : 0};
        w.Put(&tr, sizeof(tr));
        w.Put(&t->ctx, sizeof(CONTEXT));
        std::vector<void*> fls = t->fls, tlsv = t->tls;
        fls.resize(meta.fls.size());
        tlsv.resize(meta.tls.size());
        w.Put(fls.data(), fls.size() * 8);
        w.Put(tlsv.data(), tlsv.size() * 8);
        w.Put(sp, (size_t)tr.stackBytes);
        if (tls) w.Put(tls, (size_t)tr.tlsBytes);
        stackTotal += tr.stackBytes;
    }
    std::vector<uint8_t> kobjBlob = kobj::Encode(kobjs);
    uint64_t kobjBytes = kobjBlob.size();
    w.Put(&kobjBytes, 8);
    w.Put(kobjBlob.data(), kobjBlob.size());
    for (const mem::Section& s : sections) {
        SectionRec sr = {(uint64_t)s.p, s.size};
        w.Put(&sr, sizeof(sr));
        w.PutMemory(s.p, s.size);
    }
    if (g_logLayout) {
        for (mem::Region* r : regions) Log("layout region %p %llu KB", r->base, (unsigned long long)(r->size >> 10));
        for (threads::Thread* t : all) Log("layout thread %u stack %p..%p", t->ordinal, t->stackLow, t->stackBase);
        locks::Report("save");
    }
    std::set<uintptr_t> bases;
    for (mem::Region* r : regions) {
        WriteRegion(w, r, &h.pageBytes);
        bases.insert((uintptr_t)r->base);
    }

    threads::ReleaseAll();
    audio::Start();
    const ULONGLONG tReleased = GetTickCount64();

    // The outside pointers (for another session), found in the copy while the game runs again.
    {
        foreign::Table table;
        if (g_otherSessions) {
            foreign::Scanner sc(&SkipD3dObject, &LabelTarget);
            w.ForEachPiece([&](uint64_t addr, const uint8_t* p, size_t n) { sc.Scan(addr, p, n); });
            table = sc.Finish();
        }
        std::vector<uint8_t> ext = foreign::EncodeTable(table);
        h.extOffset = w.Total();
        h.extBytes = ext.size();
        w.Put(ext.data(), ext.size());
    }
    bool ok = w.Ok();
    const uint64_t fileBytes = w.Total();
    const ULONGLONG tScanned = GetTickCount64();

    // The header again, now with the totals, then the file.
    if (ok) memcpy(w.Start(), &h, sizeof(h));
    ok = ok && w.WriteTo(f);
    CloseHandle(f);
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        msg = w.OutOfMemory() ? "not enough memory (RAM and page file) to hold the savestate; close other programs or enlarge the page file"
                              : "writing " + path + " failed (disk full?)";
        DropWhenParked(gpu);
        return false;
    }
    g_slotRegions[slot] = bases;
    if (g_d3d[slot] || gpu) {
        // The slot's previous savestate no longer needs its objects. Releasing them may destroy them, which must
        // not happen while the render thread uses the device: park again for that.
        d3d::Capture* old = g_d3d[slot];
        g_d3d[slot] = gpu;
        DropWhenParked(old);
    }
    char b[200];
    sprintf_s(b, "Savestate %d saved (%.2f GB, game stopped %u ms)", slot + 1, fileBytes / 1073741824.0,
              (unsigned)(tReleased - tParked));
    msg = b;
    if (gpu && !d3d::MemoryWarning().empty()) msg += "\n" + d3d::MemoryWarning();
    Log("%s: %u threads (%llu KB of stacks), %u kernel objects, %u regions, %.1f MB of pages; park %u ms, outside "
        "pointers %u ms", msg.c_str(), (unsigned)all.size(), (unsigned long long)(stackTotal >> 10), (unsigned)kobjs.size(),
        (unsigned)regions.size(), h.pageBytes / 1048576.0, (unsigned)(tParked - t0), (unsigned)(tScanned - tReleased));
    return true;
}

// ------------------------------------------------------------------ load
struct SavedThread {
    ThreadRec rec;
    std::unique_ptr<CONTEXT, void (*)(CONTEXT*)> ctx{nullptr, [](CONTEXT* c) { _aligned_free(c); }};
    std::vector<uint64_t> fls, tls;
    std::vector<uint8_t> stack, tlsBlock;
    threads::Thread* live = nullptr;   // the thread that continues it
};

static uint64_t RemapRegion(foreign::Remap& rm, mem::Region* r) {
    mem::CollectWritten(r);
    uint64_t changed = 0;
    for (size_t i = 0; i < r->pages; i++) {
        if (!r->Written(i)) continue;
        __try {
            changed += rm.Run(r->base + i * PAGE, PAGE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return changed;
}

// The registers a parked thread continues with (not RSP: stacks stay where they were).
static uint64_t RemapContext(foreign::Remap& rm, CONTEXT* c) {
    DWORD64* regs[] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rbp, &c->Rsi, &c->Rdi, &c->R8, &c->R9,
                       &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15, &c->Rip};
    uint64_t changed = 0;
    for (DWORD64* r : regs) {
        uint64_t v = rm.Map(*r);
        if (v != *r) { *r = v; changed++; }
    }
    return changed;
}

static std::map<DWORD, DWORD> g_tidMap;   // during a load: saved real thread id -> live real thread id
static DWORD MapTid(DWORD old) {
    auto it = g_tidMap.find(old);
    return it == g_tidMap.end() ? 0 : it->second;
}

static bool LoadImpl(int slot, std::string& msg) {
    const ULONGLONG t0 = GetTickCount64();
    const std::string path = SlotPath(slot);
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        char b[64];
        sprintf_s(b, "Savestate %d is empty", slot + 1);
        msg = b;
        return false;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> closer(f, &CloseHandle);
    Reader rd(f);
    Header h;
    if (!rd.Get(&h, sizeof(h)) || memcmp(h.magic, kMagic, 8) != 0 || h.version != kVersion) {
        msg = "it was made by an older version of the savestate program";
        return false;
    }
    if (h.build != ThisBuild()) {
        msg = "it was made with another build of savestate.dll";
        return false;
    }
    const bool foreignSession = h.session != g_session || h.pid != GetCurrentProcessId();
    // After a reboot Windows puts GTA5.exe elsewhere: addresses in its image move with it (pointers to it are
    // rewritten with the other DLLs that moved, foreign::Plan).
    const int64_t exeDelta = (int64_t)(g_exe.base - h.exeBase);
    const uint64_t exeSize = g_exe.end - g_exe.base;
    auto moved = [&](uint64_t a) { return a >= h.exeBase && a < h.exeBase + exeSize ? a + exeDelta : a; };
    if (exeDelta && !foreignSession) {
        msg = "file is damaged";
        return false;
    }
    if (foreignSession && !(g_otherSessions && (h.flags & kForOtherSessions))) {
        msg = "it was made in an earlier game session (savestates last until the game is closed)";
        return false;
    }

    std::vector<uint8_t> metaBlob((size_t)h.metaBytes);
    Meta meta;
    if (h.metaBytes > (64u << 20) || !rd.Get(metaBlob.data(), metaBlob.size()) || !DecodeMeta(metaBlob, &meta)) {
        msg = "file is damaged";
        return false;
    }
    std::vector<SavedThread> saved(h.threadCount);
    unsigned maxOrdinal = 0;
    for (SavedThread& st : saved) {
        st.ctx.reset((CONTEXT*)_aligned_malloc(sizeof(CONTEXT), 16));
        if (!rd.Get(&st.rec, sizeof(st.rec)) || !rd.Get(st.ctx.get(), sizeof(CONTEXT)) || st.rec.flsCount > 4096 ||
            st.rec.tlsCount > 4096 || st.rec.stackBytes > addr::kStackSlot || st.rec.tlsBytes > (64u << 20)) {
            msg = "file is damaged";
            return false;
        }
        st.fls.resize(st.rec.flsCount);
        st.tls.resize(st.rec.tlsCount);
        st.stack.resize((size_t)st.rec.stackBytes);
        st.tlsBlock.resize((size_t)st.rec.tlsBytes);
        if (!rd.Get(st.fls.data(), st.fls.size() * 8) || !rd.Get(st.tls.data(), st.tls.size() * 8) ||
            !rd.Get(st.stack.data(), st.stack.size()) || !rd.Get(st.tlsBlock.data(), st.tlsBlock.size())) {
            msg = "file is damaged";
            return false;
        }
        if (st.rec.ordinal > maxOrdinal) maxOrdinal = st.rec.ordinal;
    }
    std::vector<kobj::Saved> kobjs;
    uint64_t kobjBytes = 0;
    std::vector<uint8_t> kobjBlob;
    if (!rd.Get(&kobjBytes, 8) || kobjBytes > (64u << 20)) { msg = "file is damaged"; return false; }
    kobjBlob.resize((size_t)kobjBytes);
    if (!rd.Get(kobjBlob.data(), kobjBlob.size()) || !kobj::Decode(kobjBlob.data(), kobjBlob.size(), &kobjs)) { msg = "file is damaged"; return false; }

    // Another session: the outside pointers, from the end of the file.
    foreign::Table ext;
    if (foreignSession) {
        HANDLE f2 = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        std::vector<uint8_t> blob(h.extBytes < (1ull << 30) ? (size_t)h.extBytes : 0);
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)h.extOffset;
        DWORD got = 0;
        bool ok = f2 != INVALID_HANDLE_VALUE && !blob.empty() && SetFilePointerEx(f2, at, nullptr, FILE_BEGIN) &&
                  ReadFile(f2, blob.data(), (DWORD)blob.size(), &got, nullptr) && got == blob.size() &&
                  foreign::DecodeTable(blob.data(), blob.size(), &ext);
        if (f2 != INVALID_HANDLE_VALUE) CloseHandle(f2);
        if (!ok) { msg = "file is damaged"; return false; }
    }

    // GPU objects: this session's pinned ones, or new ones from the manifest (another session, the lab's forced
    // rebuild, or a slot this session has no capture for).
    const bool rebuild = d3d::Ready() && (foreignSession || g_forceRecreate || !g_d3d[slot]);
    if (foreignSession && !d3d::Ready()) {
        msg = "wait until the game shows its first picture";
        return false;
    }
    if (rebuild && g_d3d[slot] && !d3d::Complete(g_d3d[slot])) {
        msg = "its GPU data is still being written; try again in a few seconds";
        return false;
    }
    if (rebuild && g_notice) {
        char b[160];
        sprintf_s(b, "Loading savestate %d%s: the game stops for a few seconds...", slot + 1,
                  foreignSession ? " from an earlier game session" : "");
        g_notice(b);
    }
    if (rebuild && !d3d::Prepare(ManifestPath(slot))) {
        msg = "its GPU data is missing (the game closed before it was written)";
        return false;
    }
    // Reading the stored contents takes most of a rebuild: done ahead, while the game runs, unless memory is short
    // (the file cache would only push the game out of RAM).
    MEMORYSTATUSEX mem = {sizeof(mem)};
    if (rebuild && GlobalMemoryStatusEx(&mem)) {
        Log("load: %llu MB of RAM free", (unsigned long long)(mem.ullAvailPhys >> 20));
        if (mem.ullAvailPhys > (4ull << 30)) d3d::WarmUp();
    }

    // Which live thread continues each saved one: the same thread in this session, else the one with its number.
    std::map<DWORD, threads::Thread*> byTid;
    std::map<unsigned, threads::Thread*> byOrdinal;
    for (threads::Thread* t : threads::All()) {
        byTid[t->tid] = t;
        if (t->ordinal < addr::kThreadSlots - 1) byOrdinal[t->ordinal] = t;
    }
    std::set<threads::Thread*> taken;
    std::vector<SavedThread*> toSpawn;
    for (SavedThread& st : saved) {
        threads::Thread* t = nullptr;
        if (!foreignSession && byTid.count(st.rec.tid)) t = byTid[st.rec.tid];
        else if (st.rec.ordinal < addr::kThreadSlots - 1 && byOrdinal.count(st.rec.ordinal)) t = byOrdinal[st.rec.ordinal];
        if (t && taken.count(t)) t = nullptr;
        if (!t && st.rec.ordinal >= addr::kThreadSlots - 1) {
            msg = "a game thread it needs has ended since it was made";
            Log("load: thread %u (number %u) of the savestate cannot be brought back", st.rec.tid, st.rec.ordinal);
            return false;
        }
        if (t && (uint64_t)t->stackBase != st.rec.stackBase) {
            msg = "a game thread's stack is somewhere else now (was the game started with GTA5-Savestates.exe?)";
            Log("load: thread number %u has its stack at %p, the savestate's at %p", st.rec.ordinal, t->stackBase, (void*)st.rec.stackBase);
            return false;
        }
        if (t) taken.insert(t);
        else toSpawn.push_back(&st);
        st.live = t;
    }
    if (foreignSession) foreign::LoadMissing(meta.modules);

    audio::Stop();
    if (!threads::ParkAll(5000, &kobj::IoQuiet)) {
        audio::Start();
        msg = "the game didn't stop in time (see savestate.log)";
        return false;
    }
    const ULONGLONG tParked = GetTickCount64();
    DropPending();
    for (SavedThread* st : toSpawn) {
        st->live = threads::Spawn(st->rec.ordinal);
        if (!st->live || (uint64_t)st->live->stackBase != st->rec.stackBase) {
            threads::ReleaseAll();
            audio::Start();
            msg = "a game thread it needs could not be started again (see savestate.log)";
            return false;
        }
        Log("load: started thread number %u again", st->rec.ordinal);
    }

    // What the restore replaces but this session's objects are known by: read it now.
    std::vector<uint64_t> liveSlots;
    std::vector<std::pair<uint64_t, uint64_t>> liveDebug;
    if (foreignSession) {
        for (foreign::Entry& e : ext.entries) e.slot = moved(e.slot);
        liveSlots = foreign::ReadSlots(ext);
        liveDebug = locks::DebugRecords();
    }

    // From here on the game's memory is being replaced: a read error leaves it half restored.
    bool ok = true;
    for (uint32_t i = 0; i < h.sectionCount && ok; i++) {
        SectionRec sr;
        ok = rd.Get(&sr, sizeof(sr));
        sr.addr = moved(sr.addr);
        ok = ok && g_exe.Contains((uintptr_t)sr.addr) && g_exe.Contains((uintptr_t)(sr.addr + sr.size - 1)) &&
             rd.Raw((uint8_t*)sr.addr, (size_t)sr.size);
    }
    std::set<uintptr_t> bases;
    uint64_t restoredPages = 0, zeroedPages = 0;
    unsigned madeRegions = 0, lostRegions = 0;
    for (uint32_t i = 0; i < h.regionCount && ok; i++) {
        RegionRec rr;
        if (!(ok = rd.Get(&rr, sizeof(rr)))) break;
        std::vector<mem::Run> runs(rr.runCount);
        std::vector<uint8_t> map((size_t)rr.pages);
        if (!(ok = rd.Get(runs.data(), runs.size() * sizeof(mem::Run)) && rd.Get(map.data(), map.size()))) break;
        bool made = false;
        mem::Region* r = rr.pages == (rr.size + PAGE - 1) / PAGE ? mem::Ensure(rr.base, rr.size, rr.ownWatch != 0, &made) : nullptr;
        if (!r) {
            lostRegions++;
            ok = rd.Skip(rr.dataPages * PAGE);
            continue;
        }
        if (made) madeRegions++;
        if (made || foreignSession) mem::Commit(r, runs);
        r->freedByGame = false;
        if (!made) mem::CollectWritten(r);
        for (size_t p = 0; p < rr.pages && ok;) {
            if (map[p] == 2) {
                size_t q = p;
                while (q < rr.pages && map[q] == 2) q++;
                ok = rd.Raw(r->base + p * PAGE, (q - p) * PAGE);
                restoredPages += q - p;
                p = q;
            } else {
                if (!made && r->Written(p) && ZeroPage(r->base + p * PAGE)) zeroedPages++;
                p++;
            }
        }
        if (made || foreignSession) mem::Protect(r, runs);
        bases.insert((uintptr_t)rr.base);
        addr::MoveNextPast(rr.base + rr.size);
    }
    if (!ok) Log("load: reading %s failed part way: the game's memory is inconsistent", path.c_str());
    if (lostRegions) Log("load: %u regions of the savestate could not be put back", lostRegions);
    arena::Resync();

    // Regions no savestate has pages of and the restored memory doesn't know: nothing refers to them any more.
    g_slotRegions[slot] = bases;
    g_needed.clear();
    for (const auto& s : g_slotRegions) g_needed.insert(s.begin(), s.end());
    mem::ReleaseUnless([](uintptr_t base) { return g_needed.count(base) != 0; });

    // Threads: the ids the game knows them by, and what Windows keeps for them outside the game's memory.
    std::set<DWORD> gameTids;
    g_tidMap.clear();
    for (SavedThread& st : saved) {
        st.live->vtid = st.rec.vtid;
        gameTids.insert(st.rec.vtid);
        g_tidMap[st.rec.tid] = st.live->tid;
        st.live->restoreFls.clear();
        st.live->restoreTls.clear();
        for (size_t k = 0; k < st.fls.size() && k < meta.fls.size(); k++) st.live->restoreFls.push_back({meta.fls[k], (void*)st.fls[k]});
        for (size_t k = 0; k < st.tls.size() && k < meta.tls.size(); k++) st.live->restoreTls.push_back({meta.tls[k], (void*)st.tls[k]});
        st.live->setSlots = true;
    }
    static DWORD s_spareTid = 0x7FFF0000;   // ids for live threads the restored game doesn't know
    std::set<threads::Thread*> continuing;
    for (SavedThread& st : saved) continuing.insert(st.live);
    for (threads::Thread* t : threads::All())
        if (!continuing.count(t) && gameTids.count(t->vtid)) t->vtid = s_spareTid--;
    threads::SetSlotIndices(meta.fls, meta.tls);
    threads::MoveNextOrdinalPast(h.nextOrdinal > 0 && h.nextOrdinal - 1 > maxOrdinal ? h.nextOrdinal - 1 : maxOrdinal);
    for (uint64_t& a : meta.critSecs) a = moved(a);
    locks::Restore(meta.critSecs, foreignSession, &MapTid, liveDebug);

    // Another session made its objects in the same order, so the object at a value is mostly the one the savestate
    // means (and other DLLs may share it): only objects unlike the saved ones are made new.
    threads::RunMutexOps(kobj::Restore(kobjs, g_forceRecreate, &threads::LiveTid, &threads::LiveHandle));
    for (const kobj::Saved& s : kobjs)
        if (s.kind == kobj::ThreadHandle) threads::SetGameHandle(s.h, (DWORD)s.pos);

    // GPU objects, and every pointer to something outside the game's memory that is elsewhere now.
    foreign::Remap remap;
    std::unordered_map<uint64_t, uint64_t> objMap;
    if (rebuild) {
        std::string why;
        if (g_releaseFirst) d3d::ReleaseGame();
        if (!d3d::Recreate(ManifestPath(slot), &objMap, &why)) Log("load: %s", why.c_str());
        for (auto& [from, to] : objMap) remap.Exact(from, to);
    } else {
        d3d::Load(g_d3d[slot]);
    }
    if (foreignSession) foreign::Plan(meta.modules, meta.windows, ext, liveSlots, &objMap, &remap);
    if (!remap.Empty()) {
        const ULONGLONG tr = GetTickCount64();
        uint64_t changed = 0;
        for (const mem::Section& s : mem::DataSections()) changed += remap.Run(s.p, s.size);
        for (mem::Region* r : mem::Regions()) changed += RemapRegion(remap, r);
        for (SavedThread& st : saved) {
            changed += remap.Run(st.stack.data(), st.stack.size() & ~(size_t)7);
            changed += remap.Run(st.tlsBlock.data(), st.tlsBlock.size() & ~(size_t)7);
            changed += RemapContext(remap, st.ctx.get());
            for (auto& [i, v] : st.live->restoreFls) v = (void*)remap.Map((uint64_t)v);
            for (auto& [i, v] : st.live->restoreTls) v = (void*)remap.Map((uint64_t)v);
        }
        Log("load: %llu pointers rewritten in %u ms", (unsigned long long)changed, (unsigned)(GetTickCount64() - tr));
    }
    if (rebuild) d3d::ReleaseUnreferenced(objMap);

    // Each thread's TLS block, and the way back to its own stack (see RunOnStack).
    for (SavedThread& st : saved) {
        if (uint8_t* tls = mem::TlsBlock(st.live->teb))
            memcpy(tls, st.tlsBlock.data(), st.tlsBlock.size() < mem::TlsSize() ? st.tlsBlock.size() : mem::TlsSize());
        if (st.stack.size() >= 0x10) memcpy(st.stack.data() + st.stack.size() - 0x10, threads::WayBack(st.live), 8);
    }

    std::vector<threads::Thread*> restore, exiting;
    for (SavedThread& st : saved) {
        threads::Thread* t = st.live;
        t->restoreStack = st.stack.data();
        t->restoreSize = st.stack.size();
        t->restoreCtx = st.ctx.get();
        restore.push_back(t);
    }
    for (threads::Thread* t : threads::All())
        if (!continuing.count(t)) exiting.push_back(t);
    // A savestate exists from now on (the loaded one): threads, regions and files are kept for it.
    mem::SetKeepFreed(true);
    threads::SetKeep();
    kobj::SetKeep();
    threads::RestoreAndWait(restore, exiting);
    timewarp::Continue(h.qpc, h.ms);
    threads::ReleaseAll();
    audio::Start();
    const ULONGLONG t1 = GetTickCount64();

    char b[200];
    sprintf_s(b, "Savestate %d loaded%s (game stopped %u ms)", slot + 1, foreignSession ? " from an earlier game session" : "",
              (unsigned)(t1 - tParked));
    msg = b;
    Log("%s: %llu pages restored, %llu zeroed, %u regions made, %u threads rewound (%u started), %u ended; park %u ms",
        msg.c_str(), (unsigned long long)restoredPages, (unsigned long long)zeroedPages, madeRegions,
        (unsigned)restore.size(), (unsigned)toSpawn.size(), (unsigned)exiting.size(), (unsigned)(tParked - t0));
    return ok;
}

// Memory can run out half way (a savestate needs about 1 GB of RAM while it is made): report it instead of letting the
// game end, and never leave the game stopped.
template <typename F>
static bool Guarded(F f, int slot, std::string& msg, const char* what) {
    try {
        return f(slot, msg);
    } catch (const std::bad_alloc&) {
        threads::ReleaseAll();
        audio::Start();
        msg = std::string("not enough memory (RAM and page file) to ") + what + " the savestate";
        Log("%s: out of memory", what);
        return false;
    }
}

bool Save(int slot, std::string& msg) {
    bool ok = Guarded(&SaveImpl, slot, msg, "make");
    if (!ok) DeleteFileA((SlotPath(slot) + ".tmp").c_str());
    return ok;
}

bool Load(int slot, std::string& msg) { return Guarded(&LoadImpl, slot, msg, "load"); }

std::string Describe(int slot) {
    char b[200];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(SlotPath(slot).c_str(), GetFileExInfoStandard, &a)) {
        sprintf_s(b, "Savestate slot %d (empty)", slot + 1);
        return b;
    }
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    Header h = {};
    HANDLE f = CreateFileA(SlotPath(slot).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD got = 0;
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, &h, sizeof(h), &got, nullptr);
        CloseHandle(f);
    }
    const bool current = got == sizeof(h) && !memcmp(h.magic, kMagic, 8) && h.version == kVersion;
    const bool thisSession = current && h.session == g_session && h.pid == GetCurrentProcessId();
    sprintf_s(b, "Savestate slot %d (saved %02d:%02d:%02d%s)", slot + 1, st.wHour, st.wMinute, st.wSecond,
              !current ? ", older version: can't be loaded" : thisSession ? "" : g_otherSessions && (h.flags & kForOtherSessions) ? ", earlier game session"
                                                                                  : ", earlier game session: can't be loaded");
    return b;
}

}  // namespace ss
