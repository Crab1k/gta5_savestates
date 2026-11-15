// Direct3D 11 objects live outside the game's memory, so a savestate keeps them alive and puts their reference counts
// and the device context's pipeline state back on load.
//
// Every object the game creates through the device (textures, buffers, views, shaders, states, ...) gets a small
// "sentinel" attached as private data: the runtime releases it when the object is destroyed, so we always know which
// objects exist. A savestate pins every live object (one reference of ours) and records how many references the game
// held. A load sets each object's count back to what the game held then (objects the restored memory doesn't know are
// released) and re-applies the pipeline state the immediate context had.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
struct ID3D11DeviceContext;

namespace d3d {

struct Capture;
struct ContextState;

// While one exists on a thread, objects that thread creates are ours (the overlay's), not tracked as the game's.
struct OwnObjects {
    OwnObjects();
    ~OwnObjects();
};

// The immediate context's whole pipeline state, held with references, and set back when this goes away.
class SavedPipeline {
public:
    SavedPipeline();
    ~SavedPipeline();
private:
    ContextState* s_;
};

void* HookForProcAddress(const char* name);   // D3D11CreateDevice / D3D11CreateDeviceAndSwapChain
void DeclineAmdExtensions(bool on);           // tell the game AMD's driver extensions don't exist (before the device)
bool Ready();
ID3D11DeviceContext* Context();
bool Tracks(uint64_t p);   // a game object we know (rebuilt from the manifest in another session)
std::string OtherOwner(uint64_t p);   // "GPU object of x.dll" for an object another DLL made, else ""

// Save, Load, Recreate and ReleaseUnreferenced run while every game thread is parked.
// Save pins every object and starts copying contents; once all are stored, FinishManifests writes `manifestPath`
// (what another session needs to rebuild the objects). An empty manifestPath: no contents, no manifest.
Capture* Save(const std::string& manifestPath, const Capture* replacing);   // replacing: the slot's previous one
std::string MemoryWarning();   // after Save: a note for the user when video memory runs short, else empty
// The swap chain makes its buffers anew when the game switches between fullscreen and a window or changes resolution;
// right before, savestates let go of their views of the old buffers, and those made before can't be loaded any more
// (d3d.cpp says why).
unsigned SwapEpoch();                          // changes with each such switch
bool FromEarlierBuffers(const Capture* c);     // made before the last switch
bool Loadable(const Capture* c, std::string* why);   // same session, while the game is stopped: else `why` says why
void Load(const Capture* c);                  // same session: the pinned objects themselves
void Drop(Capture* c);                        // the savestate is gone: unpin its objects
bool Complete(const Capture* c);              // its manifest is written

// Another session (or the lab's forced rebuild): makes new objects from a manifest, sets their reference counts and
// the pipeline state, and returns old -> new object pointers (device, context, swap chain and back buffer included)
// for rewriting the restored memory.
bool Recreate(const std::string& manifestPath, std::unordered_map<uint64_t, uint64_t>* map, std::string* why);
// Before the game is stopped for a rebuild: reads the manifest (Recreate uses it). False if it is missing or damaged.
bool Prepare(const std::string& manifestPath);
// Then, while the game still runs and if memory allows: reads every stored content it needs once (several at a time),
// so the rebuild finds them in the file cache.
void WarmUp();
// Right before a rebuild: lets go of the game's references to the live objects (the restored game doesn't know them),
// so the old and the rebuilt objects don't need memory at the same time. Objects savestates pin stay.
void ReleaseGame();
// The stored contents and shader code a manifest needs (false if it is missing or damaged).
bool References(const std::string& manifestPath, std::unordered_set<uint64_t>* out);
void ReleaseUnreferenced(const std::unordered_map<uint64_t, uint64_t>& map);

void Pump();             // present hook: a batch of content read-backs
void FinishManifests();  // worker thread: writes the manifests whose contents are all stored

void Stats();                                 // lab: objects and resource bytes by kind

}  // namespace d3d
