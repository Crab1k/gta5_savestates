// Pointers from the game's memory to things outside it, which another game session has at other addresses:
//  - DLL images, when Windows put a DLL elsewhere (after a reboot, or a rebuilt savestate.dll);
//  - objects other DLLs made for the game (XAudio2, DirectSound, DirectInput, DXGI, Social Club, ...);
//  - the game's windows.
// A savestate records where its outside pointers are (their "slots") and what they point at. Another session made the
// same objects at start-up and keeps its own pointers to them in the same slots (the game's start-up is the same
// every time, and so is the address layout, addr.h): the live value in a slot, read before the load replaces the
// memory, says what the saved value stands for now.
#pragma once
#include "common.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace foreign {

struct Module {
    std::wstring path;
    uint64_t base = 0, size = 0;
};
std::vector<Module> Modules();

struct Window {
    uint64_t hwnd = 0;
    std::wstring cls;
};
std::vector<Window> Windows();   // top-level windows of this process

struct Entry {
    uint64_t slot = 0, value = 0;
    uint32_t cls = 0;            // index into Table::classes; 0: "" (no vtable)
    uint32_t pad = 0;
};
struct Table {
    std::vector<std::string> classes{""};   // "module.dll+rva" of the object's vtable
    std::vector<Entry> entries;
};

// Finds the outside pointers in a savestate's memory, given piece by piece (game address, bytes, length).
class Scanner {
public:
    // skip: pointers handled elsewhere (Direct3D objects); label: a name for a target that says more than its vtable.
    explicit Scanner(bool (*skip)(uint64_t value), std::string (*label)(uint64_t value) = nullptr);
    ~Scanner();
    void Scan(uint64_t addr, const uint8_t* p, size_t n);
    Table Finish();
private:
    struct Impl;
    Impl* m_;
};

std::vector<uint8_t> Encode(const std::vector<Module>& modules, const std::vector<Window>& windows);
bool Decode(const uint8_t* p, size_t n, std::vector<Module>* modules, std::vector<Window>* windows);
std::vector<uint8_t> EncodeTable(const Table& t);
bool DecodeTable(const uint8_t* p, size_t n, Table* t);

// Old -> new values, applied to every aligned 64-bit value of the restored memory.
class Remap {
public:
    Remap();
    void Exact(uint64_t from, uint64_t to);
    void Range(uint64_t lo, uint64_t hi, int64_t delta);   // [lo, hi) moves by delta
    bool Has(uint64_t from) const { return exact_.count(from) != 0; }
    bool Empty() const { return exact_.empty() && ranges_.empty(); }
    uint64_t Map(uint64_t v) const;
    uint64_t Run(uint8_t* p, size_t n);   // returns how many values changed
private:
    static uint32_t Hash(uint64_t v) { return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 42); }
    std::unordered_map<uint64_t, uint64_t> exact_;
    std::vector<uint64_t> filter_;
    uint64_t elo_ = ~0ull, ehi_ = 0;
    struct R {
        uint64_t lo, hi;
        int64_t delta;
    };
    std::vector<R> ranges_;
    uint64_t rlo_ = ~0ull, rhi_ = 0;
};

// EncodePointer / DecodePointer of GTA5.exe: the same scrambling in every session.
void InstallHooks();
void* HookForProcAddress(const char* name);

// Loads DLLs the savestate's session had and this one doesn't (before the game is stopped).
void LoadMissing(const std::vector<Module>& saved);
// The live values in the table's slots: read before the memory is replaced.
std::vector<uint64_t> ReadSlots(const Table& t);
// Adds the moves of DLL images (by path), windows (by class), pointers into rebuilt GPU objects (`objects`: old ->
// new object, may be null) and outside objects (by slot) to `out`. Values `out` already maps stay as they are.
void Plan(const std::vector<Module>& modules, const std::vector<Window>& windows, const Table& t,
          const std::vector<uint64_t>& live, const std::unordered_map<uint64_t, uint64_t>* objects, Remap* out);

}  // namespace foreign
