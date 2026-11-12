#include "audio.h"
#include "common.h"
#include <objbase.h>

namespace audio {

// XAudio2 2.7 (DirectX SDK June 2010), created as a COM object.
static const GUID kClsidXAudio2 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID kClsidXAudio2Debug = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
enum { kStartEngine = 11, kStopEngine = 12, kGetPerformanceData = 14 };   // IXAudio2 vtable slots

struct PerformanceData {   // XAUDIO2_PERFORMANCE_DATA of XAudio2 2.7
    UINT64 audioCycles, totalCycles;
    UINT32 minCycles, maxCycles, memory, latency, glitches, activeSources, totalSources, activeSubmixes, resamplers,
        matrixMixes, xmaSources, xmaStreams;
};

static void* g_xaudio = nullptr;
static bool g_stopped = false;

static HRESULT(WINAPI* RealCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static HRESULT WINAPI Hook_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    HRESULT hr = RealCoCreateInstance(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && (IsEqualGUID(clsid, kClsidXAudio2) || IsEqualGUID(clsid, kClsidXAudio2Debug))) {
        g_xaudio = *out;
        Log("XAudio2 engine %p", g_xaudio);
    }
    return hr;
}

static void Call(int slot) {
    if (!g_xaudio) return;
    auto fn = (*(void***)g_xaudio)[slot];
    if (slot == kStartEngine) ((HRESULT(STDMETHODCALLTYPE*)(void*))fn)(g_xaudio);
    else ((void(STDMETHODCALLTYPE*)(void*))fn)(g_xaudio);
}

void InstallHooks() {
    RealCoCreateInstance = (decltype(RealCoCreateInstance))PatchIat(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", (void*)&Hook_CoCreateInstance);
    if (!RealCoCreateInstance) RealCoCreateInstance = &CoCreateInstance;
}

void Stop() {
    if (g_xaudio && !g_stopped) {
        Call(kStopEngine);
        g_stopped = true;
    }
}

void Report(const char* when) {
    if (!g_xaudio) return;
    PerformanceData d = {};
    ((void(STDMETHODCALLTYPE*)(void*, PerformanceData*))(*(void***)g_xaudio)[kGetPerformanceData])(g_xaudio, &d);
    Log("audio %s: %u of %u source voices playing, %u glitches so far, latency %u samples, engine %s", when,
        d.activeSources, d.totalSources, d.glitches, d.latency, d.audioCycles ? "running" : "idle");
}

void Start() {
    if (g_xaudio && g_stopped) {
        Call(kStartEngine);
        g_stopped = false;
    }
}

}  // namespace audio
