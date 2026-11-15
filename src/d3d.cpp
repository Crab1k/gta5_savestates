#include "d3d.h"
#include "common.h"
#include "overlay.h"
#include "store.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <algorithm>
#include <deque>
#include <memory>
#include <type_traits>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace d3d {

// ------------------------------------------------------------------ what we know about each game object
enum Kind : uint8_t {
    K_None, K_Buffer, K_Tex1D, K_Tex2D, K_Tex3D, K_SRV, K_UAV, K_RTV, K_DSV, K_Layout, K_VS, K_GS, K_GSSO, K_PS, K_HS,
    K_DS, K_CS, K_Linkage, K_Blend, K_DSS, K_RS, K_Sampler, K_Query, K_Predicate, K_Counter
};
enum Flag : uint8_t {
    F_Desc = 1,        // view created with a description (else null)
    F_Immutable = 2,   // resource contents fixed at creation: read back once, after a save
    F_Target = 4,      // render target / depth buffer: redrawn by the game, contents not kept
    F_Snapshot = 8,    // other default/staging resource: contents copied at each save
};

struct Tracked {
    uint32_t pins = 0;                   // savestates holding one reference each
    uint8_t kind = K_None, flags = 0;
    std::vector<uint8_t> desc;           // creation description
    ID3D11DeviceChild* parent = nullptr; // views: their resource
    uint64_t code = 0;                   // shaders, layouts: bytecode in the store
    uint64_t content = 0;                // immutable resources: contents in the store (0 = not read back yet)
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D11DeviceChild*, Tracked>* g_objs;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static volatile LONG g_created = 0, g_destroyed = 0;

// ------------------------------------------------------------------ the swap chain's buffers
// A savestate pins the game's views of the swap chain's buffers like every other object: GTA makes new views of the same
// buffers when it comes back to fullscreen after Alt+Tab, and a savestate from before must still find its own. But the
// swap chain makes its buffers anew (ResizeBuffers: another resolution, fullscreen or a window) only once nothing refers
// to them, and a pinned view would make that fail, which ends the game. So right before ResizeBuffers the savestates let
// go of those views (UnpinBufferViews), and the savestates made before can't be loaded any more: they are deleted
// (ss::DropStale). Rewriting a savestate's pointers to the old buffers and views was tried: the load works, but at the
// next switch GTA no longer lets go of its views (it keeps more about its back buffer than those pointers).
static volatile LONG g_swapEpoch = 0;   // changes each time savestates let go of their views of the buffers

// The swap chain's buffers now (as the game got them since they were last made), for the log: the game's views of them
// are logged as they come and go.
static volatile LONG64 g_current[16];
static bool CurrentBuffer(uint64_t p) {
    if (!p) return false;
    for (auto& b : g_current)
        if ((uint64_t)b == p) return true;
    return false;
}
static void NoteBuffer(uint64_t p) {
    if (!p || CurrentBuffer(p)) return;
    for (auto& b : g_current)
        if (!b && InterlockedCompareExchange64(&b, (LONG64)p, 0) == 0) return;
}
static void ForgetBuffers() {
    for (auto& b : g_current) InterlockedExchange64(&b, 0);
}
static bool BufferView(uint8_t kind, ID3D11DeviceChild* parent) {
    return kind >= K_SRV && kind <= K_DSV && CurrentBuffer((uint64_t)parent);
}

struct SwapInfo {
    UINT width = 0, height = 0, count = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    BOOL fullscreen = FALSE;
    std::vector<uint64_t> buffers;   // GetBuffer(i), 0 where not accessible; no reference kept
};

static SwapInfo CurrentSwap(bool withBuffers) {
    SwapInfo s;
    DXGI_SWAP_CHAIN_DESC d;
    if (!g_swap || FAILED(g_swap->GetDesc(&d))) return s;
    s.width = d.BufferDesc.Width;
    s.height = d.BufferDesc.Height;
    s.count = d.BufferCount;
    s.format = d.BufferDesc.Format;
    if (FAILED(g_swap->GetFullscreenState(&s.fullscreen, nullptr))) s.fullscreen = !d.Windowed;
    for (UINT i = 0; withBuffers && i < (d.BufferCount ? d.BufferCount : 1) && i < 16; i++) {
        ID3D11Texture2D* b = nullptr;
        s.buffers.push_back(SUCCEEDED(g_swap->GetBuffer(i, __uuidof(ID3D11Texture2D), (void**)&b)) && b ? (uint64_t)b : 0);
        NoteBuffer(s.buffers.back());
        if (b) b->Release();
    }
    return s;
}

static std::string ModeName(const SwapInfo& s) {
    char b[80];
    sprintf_s(b, "%s at %ux%u", s.fullscreen ? "fullscreen" : "a window", s.width, s.height);
    return b;
}

// {6E2C1F0A-5B3D-4A8E-9C47-2D1F5A6B7C80}
static const GUID kSentinelGuid = {0x6e2c1f0a, 0x5b3d, 0x4a8e, {0x9c, 0x47, 0x2d, 0x1f, 0x5a, 0x6b, 0x7c, 0x80}};

struct Sentinel final : IUnknown {
    volatile LONG refs = 1;
    ID3D11DeviceChild* obj;
    explicit Sentinel(ID3D11DeviceChild* o) : obj(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown)) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) {
            // The runtime let go of its private data: the object is being destroyed.
            AcquireSRWLockExclusive(&g_lock);
            auto it = g_objs->find(obj);
            const bool bufferView = it != g_objs->end() && BufferView(it->second.kind, it->second.parent);
            if (it != g_objs->end()) g_objs->erase(it);
            ReleaseSRWLockExclusive(&g_lock);
            if (bufferView) Log("d3d: view %p of the back buffer destroyed (thread %u)", obj, GetCurrentThreadId());
            InterlockedIncrement(&g_destroyed);
            delete this;
        }
        return (ULONG)r;
    }
};

static thread_local int t_own = 0;
static thread_local int t_forGame = 0;   // Recreate: what we create stands in for the game's objects
OwnObjects::OwnObjects() { t_own++; }
OwnObjects::~OwnObjects() { t_own--; }

// Only what GTA5.exe itself creates is the game's: objects other DLLs make on its device (the Social Club overlay,
// GPU libraries) belong to those DLLs, which a load doesn't rewind.
static SRWLOCK g_otherLock = SRWLOCK_INIT;
static std::map<HMODULE, int> g_otherCreators;   // objects other DLLs created, by module (diagnostics)
static std::unordered_map<ID3D11DeviceChild*, HMODULE> g_otherObjs;   // those alive now, and their creator

// Like Sentinel, for an object another DLL made: forgets it when it is destroyed.
struct OtherSentinel final : IUnknown {
    volatile LONG refs = 1;
    ID3D11DeviceChild* obj;
    explicit OtherSentinel(ID3D11DeviceChild* o) : obj(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown)) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) {
            AcquireSRWLockExclusive(&g_otherLock);
            g_otherObjs.erase(obj);
            ReleaseSRWLockExclusive(&g_otherLock);
            delete this;
        }
        return (ULONG)r;
    }
};

std::string OtherOwner(uint64_t p) {
    HMODULE m = nullptr;
    AcquireSRWLockShared(&g_otherLock);
    auto it = g_otherObjs.find((ID3D11DeviceChild*)p);
    if (it != g_otherObjs.end()) m = it->second;
    bool found = it != g_otherObjs.end();
    ReleaseSRWLockShared(&g_otherLock);
    if (!found) return "";
    char name[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, name, MAX_PATH);
    const char* s = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    return std::string("GPU object of ") + s;
}

static void Track(void* caller, ID3D11DeviceChild* obj, Tracked&& info) {
    if (!obj || t_own) return;
    if (!g_exe.Contains((uintptr_t)caller) && !t_forGame) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)caller, &m);
        // State objects are shared: one the game made stays the game's.
        AcquireSRWLockShared(&g_lock);
        bool games = g_objs->count(obj) != 0;
        ReleaseSRWLockShared(&g_lock);
        if (games) return;
        AcquireSRWLockExclusive(&g_otherLock);
        g_otherCreators[m]++;
        bool fresh = g_otherObjs.emplace(obj, m).second;
        ReleaseSRWLockExclusive(&g_otherLock);
        if (fresh) {
            auto* s = new OtherSentinel(obj);
            obj->SetPrivateDataInterface(kSentinelGuid, s);
            s->Release();
        }
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    bool known = !g_objs->emplace(obj, std::move(info)).second;   // state objects are shared: the same one comes back
    ReleaseSRWLockExclusive(&g_lock);
    if (known) return;
    auto* s = new Sentinel(obj);
    obj->SetPrivateDataInterface(kSentinelGuid, s);
    s->Release();   // the object holds the only reference now
    InterlockedIncrement(&g_created);
}

template <typename T>
static std::vector<uint8_t> Bytes(const T* p) {
    return p ? std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + sizeof(T)) : std::vector<uint8_t>();
}

static Tracked Resource(Kind k, std::vector<uint8_t> desc, UINT usage, UINT bind) {
    Tracked t;
    t.kind = k;
    t.desc = std::move(desc);
    if (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) t.flags = F_Target;
    else if (usage == D3D11_USAGE_IMMUTABLE) t.flags = F_Immutable;
    else if (usage == D3D11_USAGE_DEFAULT || usage == D3D11_USAGE_STAGING) t.flags = F_Snapshot;
    return t;
}

static Tracked Plain(Kind k, std::vector<uint8_t> desc = {}) {
    Tracked t;
    t.kind = k;
    t.desc = std::move(desc);
    return t;
}

static uint64_t KeepCode(const void* code, SIZE_T len) {
    if (!code || !len) return 0;
    uint64_t h = store::Hash(code, len);
    store::Put(h, std::vector<uint8_t>((const uint8_t*)code, (const uint8_t*)code + len));
    return h;
}

// Input layout and stream-output declarations carry name pointers: encode them with the names inline.
static void PutU32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4); }
static void PutStr(std::vector<uint8_t>& v, const char* s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    PutU32(v, n);
    v.insert(v.end(), s, s + n);
}
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t U32() { uint32_t x = 0; if (end - p >= 4) memcpy(&x, p, 4); p += 4; return x; }
    std::string Str() { uint32_t n = U32(); std::string s((const char*)p, (const char*)p + (end - p >= n ? n : 0)); p += n; return s; }
};

static std::vector<uint8_t> EncodeLayout(const D3D11_INPUT_ELEMENT_DESC* e, UINT n) {
    std::vector<uint8_t> v;
    PutU32(v, n);
    for (UINT i = 0; i < n; i++) {
        PutStr(v, e[i].SemanticName);
        PutU32(v, e[i].SemanticIndex); PutU32(v, e[i].Format); PutU32(v, e[i].InputSlot);
        PutU32(v, e[i].AlignedByteOffset); PutU32(v, e[i].InputSlotClass); PutU32(v, e[i].InstanceDataStepRate);
    }
    return v;
}

static std::vector<uint8_t> EncodeSO(const D3D11_SO_DECLARATION_ENTRY* e, UINT ne, const UINT* strides, UINT ns, UINT rs) {
    std::vector<uint8_t> v;
    PutU32(v, ne);
    for (UINT i = 0; i < ne; i++) {
        PutU32(v, e[i].Stream);
        PutStr(v, e[i].SemanticName);
        PutU32(v, e[i].SemanticIndex); PutU32(v, e[i].StartComponent); PutU32(v, e[i].ComponentCount); PutU32(v, e[i].OutputSlot);
    }
    PutU32(v, ns);
    for (UINT i = 0; i < ns; i++) PutU32(v, strides[i]);
    PutU32(v, rs);
    return v;
}

// ------------------------------------------------------------------ device hooks
template <typename T>
static T Patch(void** vtbl, int index, T hook) {
    T old = (T)vtbl[index];
    if (old == hook) return nullptr;
    Unprotect(&vtbl[index], sizeof(void*));
    vtbl[index] = (void*)hook;
    return old;
}

using Dev = ID3D11Device;
#define REAL(Name, Params) static HRESULT(STDMETHODCALLTYPE* Real##Name) Params;

REAL(CreateBuffer, (Dev*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**))
static HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(Dev* d, const D3D11_BUFFER_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Buffer** o) {
    HRESULT hr = RealCreateBuffer(d, a, b, o);
    if (SUCCEEDED(hr) && o && *o) Track(_ReturnAddress(), *o, Resource(K_Buffer, Bytes(a), a->Usage, a->BindFlags));
    return hr;
}

#define TEXTURE(N, Desc)                                                                                              \
    REAL(CreateTexture##N##D, (Dev*, const Desc*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture##N##D**))              \
    static HRESULT STDMETHODCALLTYPE Hook_CreateTexture##N##D(Dev* d, const Desc* a, const D3D11_SUBRESOURCE_DATA* b, \
                                                              ID3D11Texture##N##D** o) {                              \
        HRESULT hr = RealCreateTexture##N##D(d, a, b, o);                                                            \
        if (SUCCEEDED(hr) && o && *o) Track(_ReturnAddress(), *o, Resource(K_Tex##N##D, Bytes(a), a->Usage, a->BindFlags));           \
        return hr;                                                                                                   \
    }
TEXTURE(1, D3D11_TEXTURE1D_DESC)
TEXTURE(2, D3D11_TEXTURE2D_DESC)
TEXTURE(3, D3D11_TEXTURE3D_DESC)
#undef TEXTURE

// The log: the game made a view of a swap chain buffer.
static void NoteView(void* caller, ID3D11DeviceChild* o, ID3D11Resource* r, uint8_t kind) {
    if (t_own || !g_exe.Contains((uintptr_t)caller) || !CurrentBuffer((uint64_t)r)) return;
    ULONG refs = o->AddRef() - 1;
    o->Release();
    Log("d3d: view %p (kind %u) of the back buffer %p made, %u references (thread %u)", o, kind, r, (unsigned)refs,
        GetCurrentThreadId());
}

#define VIEW(Name, K, Desc, Out)                                                                                    \
    REAL(Create##Name, (Dev*, ID3D11Resource*, const Desc*, Out**))                                                \
    static HRESULT STDMETHODCALLTYPE Hook_Create##Name(Dev* d, ID3D11Resource* r, const Desc* a, Out** o) {        \
        HRESULT hr = RealCreate##Name(d, r, a, o);                                                                 \
        if (SUCCEEDED(hr) && o && *o) {                                                                            \
            Tracked t = Plain(K, Bytes(a));                                                                        \
            t.flags = a ? F_Desc : 0;                                                                              \
            t.parent = r;                                                                                          \
            Track(_ReturnAddress(), *o, std::move(t));                                                                               \
            NoteView(_ReturnAddress(), *o, r, K);                                                                  \
        }                                                                                                          \
        return hr;                                                                                                 \
    }
VIEW(ShaderResourceView, K_SRV, D3D11_SHADER_RESOURCE_VIEW_DESC, ID3D11ShaderResourceView)
VIEW(UnorderedAccessView, K_UAV, D3D11_UNORDERED_ACCESS_VIEW_DESC, ID3D11UnorderedAccessView)
VIEW(RenderTargetView, K_RTV, D3D11_RENDER_TARGET_VIEW_DESC, ID3D11RenderTargetView)
VIEW(DepthStencilView, K_DSV, D3D11_DEPTH_STENCIL_VIEW_DESC, ID3D11DepthStencilView)
#undef VIEW

REAL(CreateInputLayout, (Dev*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**))
static HRESULT STDMETHODCALLTYPE Hook_CreateInputLayout(Dev* d, const D3D11_INPUT_ELEMENT_DESC* a, UINT n, const void* c, SIZE_T l, ID3D11InputLayout** o) {
    HRESULT hr = RealCreateInputLayout(d, a, n, c, l, o);
    if (SUCCEEDED(hr) && o && *o && !t_own) {
        Tracked t = Plain(K_Layout, EncodeLayout(a, n));
        t.code = KeepCode(c, l);
        Track(_ReturnAddress(), *o, std::move(t));
    }
    return hr;
}

#define SHADER(Name, K)                                                                                             \
    REAL(Create##Name, (Dev*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11##Name**))                          \
    static HRESULT STDMETHODCALLTYPE Hook_Create##Name(Dev* d, const void* c, SIZE_T l, ID3D11ClassLinkage* k,     \
                                                       ID3D11##Name** o) {                                         \
        HRESULT hr = RealCreate##Name(d, c, l, k, o);                                                              \
        if (SUCCEEDED(hr) && o && *o && !t_own) {                                                                  \
            Tracked t = Plain(K);                                                                                  \
            t.code = KeepCode(c, l);                                                                               \
            t.parent = k;                                                                                          \
            Track(_ReturnAddress(), *o, std::move(t));                                                                               \
        }                                                                                                          \
        return hr;                                                                                                 \
    }
SHADER(VertexShader, K_VS)
SHADER(GeometryShader, K_GS)
SHADER(PixelShader, K_PS)
SHADER(HullShader, K_HS)
SHADER(DomainShader, K_DS)
SHADER(ComputeShader, K_CS)
#undef SHADER

REAL(CreateGeometryShaderWithStreamOutput, (Dev*, const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**))
static HRESULT STDMETHODCALLTYPE Hook_CreateGeometryShaderWithStreamOutput(Dev* d, const void* c, SIZE_T l,
        const D3D11_SO_DECLARATION_ENTRY* e, UINT ne, const UINT* s, UINT ns, UINT rs, ID3D11ClassLinkage* k, ID3D11GeometryShader** o) {
    HRESULT hr = RealCreateGeometryShaderWithStreamOutput(d, c, l, e, ne, s, ns, rs, k, o);
    if (SUCCEEDED(hr) && o && *o && !t_own) {
        Tracked t = Plain(K_GSSO, EncodeSO(e, ne, s, ns, rs));
        t.code = KeepCode(c, l);
        t.parent = k;
        Track(_ReturnAddress(), *o, std::move(t));
    }
    return hr;
}

REAL(CreateClassLinkage, (Dev*, ID3D11ClassLinkage**))
static HRESULT STDMETHODCALLTYPE Hook_CreateClassLinkage(Dev* d, ID3D11ClassLinkage** o) {
    HRESULT hr = RealCreateClassLinkage(d, o);
    if (SUCCEEDED(hr) && o && *o) Track(_ReturnAddress(), *o, Plain(K_Linkage));
    return hr;
}

#define STATE(Name, K, Desc, Out)                                                                                   \
    REAL(Create##Name, (Dev*, const Desc*, Out**))                                                                 \
    static HRESULT STDMETHODCALLTYPE Hook_Create##Name(Dev* d, const Desc* a, Out** o) {                          \
        HRESULT hr = RealCreate##Name(d, a, o);                                                                    \
        if (SUCCEEDED(hr) && o && *o) Track(_ReturnAddress(), *o, Plain(K, Bytes(a)));                                               \
        return hr;                                                                                                 \
    }
STATE(BlendState, K_Blend, D3D11_BLEND_DESC, ID3D11BlendState)
STATE(DepthStencilState, K_DSS, D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState)
STATE(RasterizerState, K_RS, D3D11_RASTERIZER_DESC, ID3D11RasterizerState)
STATE(SamplerState, K_Sampler, D3D11_SAMPLER_DESC, ID3D11SamplerState)
STATE(Query, K_Query, D3D11_QUERY_DESC, ID3D11Query)
STATE(Predicate, K_Predicate, D3D11_QUERY_DESC, ID3D11Predicate)
STATE(Counter, K_Counter, D3D11_COUNTER_DESC, ID3D11Counter)
#undef STATE
#undef REAL

static volatile LONG g_deferred = 0;
static HRESULT(STDMETHODCALLTYPE* RealCreateDeferredContext)(Dev*, UINT, ID3D11DeviceContext**);
static HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(Dev* d, UINT f, ID3D11DeviceContext** o) {
    HRESULT hr = RealCreateDeferredContext(d, f, o);
    if (SUCCEEDED(hr)) Log("d3d: deferred context %p created (%d so far)", *o, (int)InterlockedIncrement(&g_deferred));
    return hr;
}

static void HookDevice(ID3D11Device* dev) {
    void** v = *(void***)dev;
#define P(Name, Idx) if (auto old = Patch(v, Idx, &Hook_##Name)) Real##Name = old
    P(CreateBuffer, 3); P(CreateTexture1D, 4); P(CreateTexture2D, 5); P(CreateTexture3D, 6);
    P(CreateShaderResourceView, 7); P(CreateUnorderedAccessView, 8); P(CreateRenderTargetView, 9);
    P(CreateDepthStencilView, 10); P(CreateInputLayout, 11); P(CreateVertexShader, 12); P(CreateGeometryShader, 13);
    P(CreateGeometryShaderWithStreamOutput, 14); P(CreatePixelShader, 15); P(CreateHullShader, 16);
    P(CreateDomainShader, 17); P(CreateComputeShader, 18); P(CreateClassLinkage, 19); P(CreateBlendState, 20);
    P(CreateDepthStencilState, 21); P(CreateRasterizerState, 22); P(CreateSamplerState, 23); P(CreateQuery, 24);
    P(CreatePredicate, 25); P(CreateCounter, 26); P(CreateDeferredContext, 27);
#undef P
}

static bool IsView(uint8_t kind) { return kind >= K_SRV && kind <= K_DSV; }

// Is `p` (any Direct3D object) a view of one of `buffers`?
static bool ViewOfBuffer(IUnknown* p, const std::unordered_map<uint64_t, uint32_t>& buffers) {
    if (buffers.empty()) return false;
    ID3D11View* v = nullptr;
    if (FAILED(p->QueryInterface(__uuidof(ID3D11View), (void**)&v)) || !v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    const bool yes = r && buffers.count((uint64_t)r) != 0;
    if (r) r->Release();
    v->Release();
    return yes;
}

// The game's views of the swap chain's buffers that are alive now, with their references (the log).
static int LogBufferViews(const char* when) {
    std::vector<std::pair<ID3D11DeviceChild*, uint32_t>> views;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_objs)
        if (BufferView(kv.second.kind, kv.second.parent)) views.push_back({kv.first, kv.second.pins});
    ReleaseSRWLockShared(&g_lock);
    for (auto& [o, pins] : views) {
        ULONG refs = o->AddRef() - 1;
        o->Release();
        Log("d3d:   %s: view %p of the back buffer alive, %u references, %u of them savestates'", when, o, (unsigned)refs, pins);
    }
    return (int)views.size();
}

static HRESULT(STDMETHODCALLTYPE* RealGetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
static int UnpinBufferViews(IDXGISwapChain* sc);   // after Capture

static HRESULT(STDMETHODCALLTYPE* RealResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    if (sc != g_swap) return RealResizeBuffers(sc, n, w, h, f, flags);
    Log("d3d: the game makes the swap chain buffers anew (%ux%u, thread %u)", w, h, GetCurrentThreadId());
    const int unpinned = UnpinBufferViews(sc);
    LogBufferViews("before");
    HRESULT hr = RealResizeBuffers(sc, n, w, h, f, flags);
    if (SUCCEEDED(hr)) ForgetBuffers();
    if (SUCCEEDED(hr) || unpinned) InterlockedIncrement(&g_swapEpoch);
    Log("d3d: swap chain buffers made anew, %s: %s (%08X)", ModeName(CurrentSwap(false)).c_str(),
        SUCCEEDED(hr) ? "done" : "failed", (unsigned)hr);
    return hr;
}

static HRESULT(STDMETHODCALLTYPE* RealSetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
static HRESULT STDMETHODCALLTYPE Hook_SetFullscreenState(IDXGISwapChain* sc, BOOL on, IDXGIOutput* out) {
    HRESULT hr = RealSetFullscreenState(sc, on, out);
    if (sc == g_swap)
        Log("d3d: the game switches to %s: %s (%08X, thread %u)", on ? "fullscreen" : "a window", SUCCEEDED(hr) ? "done" : "failed",
            (unsigned)hr, GetCurrentThreadId());
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetBuffer(IDXGISwapChain* sc, UINT i, REFIID iid, void** out) {
    HRESULT hr = RealGetBuffer(sc, i, iid, out);
    if (sc == g_swap && SUCCEEDED(hr) && out && *out && g_exe.Contains((uintptr_t)_ReturnAddress())) {
        if (!CurrentBuffer((uint64_t)*out)) Log("d3d: the game got back buffer %u: %p (thread %u)", i, *out, GetCurrentThreadId());
        NoteBuffer((uint64_t)*out);
    }
    return hr;
}

static decltype(&D3D11CreateDeviceAndSwapChain) RealCreateDeviceAndSwapChain;
static decltype(&D3D11CreateDevice) RealCreateDevice;

static HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f,
        const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc,
        ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    HRESULT hr = RealCreateDeviceAndSwapChain(a, t, s, f, fl, nfl, sdk, scd, sc, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        HookDevice(*dev);
        g_dev = *dev;
        g_swap = sc ? *sc : nullptr;
        (*dev)->GetImmediateContext(&g_ctx);   // keeps one reference for our lifetime
        if (g_swap) {
            void** sv = *(void***)g_swap;
            if (auto old = Patch(sv, 13, &Hook_ResizeBuffers)) RealResizeBuffers = old;
            if (auto old = Patch(sv, 10, &Hook_SetFullscreenState)) RealSetFullscreenState = old;
            if (auto old = Patch(sv, 9, &Hook_GetBuffer)) RealGetBuffer = old;
            overlay::Install(g_swap, *dev);
        }
        Log("d3d: device %p, immediate context %p, swap chain %p (%ux%u)", *dev, g_ctx, g_swap,
            scd ? scd->BufferDesc.Width : 0, scd ? scd->BufferDesc.Height : 0);
    }
    return hr;
}

static HRESULT WINAPI Hook_D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f,
        const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    HRESULT hr = RealCreateDevice(a, t, s, f, fl, nfl, sdk, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) HookDevice(*dev);
    return hr;
}

// AMD's private driver extensions (AmdDxExtCreate11, exported by the AMD driver): objects made through them live only
// inside the driver, so a savestate can neither keep nor rebuild them, and after a load from another session the
// game would call into the earlier session's. The game is told they don't exist, as on other GPUs; they only serve
// AMD-specific features such as multi-GPU. Only for savestates of later sessions (a lab setting): within a session the
// driver keeps its objects, and a load leaves them as they are.
static bool g_declineAmd = false;
void DeclineAmdExtensions(bool on) { g_declineAmd = on; }

static HRESULT __cdecl NoAmdExtensions(void* device, void** ext) {
    static volatile LONG logged = 0;
    if (!InterlockedExchange(&logged, 1)) Log("d3d: the game asked for AMD's driver extensions; declined (savestates can't keep them)");
    if (ext) *ext = nullptr;
    return E_NOINTERFACE;
}

void* HookForProcAddress(const char* name) {
    if (g_declineAmd && (!strcmp(name, "AmdDxExtCreate11") || !strcmp(name, "AmdDxExtCreate"))) return (void*)&NoAmdExtensions;
    if (!g_objs) g_objs = new std::unordered_map<ID3D11DeviceChild*, Tracked>;
    HMODULE m = GetModuleHandleA("d3d11.dll");
    if (!m) return nullptr;
    if (!strcmp(name, "D3D11CreateDeviceAndSwapChain")) {
        RealCreateDeviceAndSwapChain = (decltype(RealCreateDeviceAndSwapChain))GetProcAddress(m, name);
        return RealCreateDeviceAndSwapChain ? (void*)&Hook_D3D11CreateDeviceAndSwapChain : nullptr;
    }
    if (!strcmp(name, "D3D11CreateDevice")) {
        RealCreateDevice = (decltype(RealCreateDevice))GetProcAddress(m, name);
        return RealCreateDevice ? (void*)&Hook_D3D11CreateDevice : nullptr;
    }
    return nullptr;
}

bool Ready() { return g_ctx != nullptr; }

bool Tracks(uint64_t p) {
    AcquireSRWLockShared(&g_lock);
    bool known = g_objs && g_objs->count((ID3D11DeviceChild*)p) != 0;
    ReleaseSRWLockShared(&g_lock);
    return known;
}
ID3D11DeviceContext* Context() { return g_ctx; }

// ------------------------------------------------------------------ pipeline state of the immediate context
// Raw pointers only: every bound object is pinned by the same capture (or held in its `bound` list), so it stays alive
// (views of the back buffer only until it is made anew, after which the capture can't be loaded: UnpinBufferViews).
struct StageState {
    IUnknown* shader = nullptr;
    ID3D11Buffer* cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* smp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
};

struct ContextState {
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {}, offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    StageState vs, hs, ds, gs, ps, cs;
    ID3D11UnorderedAccessView* csUav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ID3D11RasterizerState* rs = nullptr;
    UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11UnorderedAccessView* omUav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
};

// Calls fn on every object pointer in the state (for remapping a state from another session).
template <typename F>
static void ForEachPointer(ContextState& s, F fn) {
    auto one = [&](auto*& p) { p = (std::remove_reference_t<decltype(p)>)fn((void*)p); };
    auto all = [&](auto& a) { for (auto*& p : a) one(p); };
    auto stage = [&](StageState& st) { one(st.shader); all(st.cb); all(st.srv); all(st.smp); };
    one(s.il); all(s.vb); one(s.ib);
    stage(s.vs); stage(s.hs); stage(s.ds); stage(s.gs); stage(s.ps); stage(s.cs);
    all(s.csUav); one(s.rs); all(s.rtv); one(s.dsv); all(s.omUav); one(s.blend); one(s.dss); all(s.so); one(s.predicate);
}

// Get* returns references. A savestate's capture drops them at once (the objects are pinned anyway); the overlay
// keeps them while it draws, so nothing it unbinds can be destroyed under it.
static thread_local bool t_keepRefs = false;
template <typename T, size_t N>
static void UnrefAll(T* (&a)[N]) { if (!t_keepRefs) for (T* p : a) if (p) p->Release(); }
template <typename T>
static void Unref(T* p) { if (p && !t_keepRefs) p->Release(); }
template <typename T, size_t N>
static void DropAll(T* (&a)[N]) { for (T* p : a) if (p) p->Release(); }
template <typename T>
static void Drop(T* p) { if (p) p->Release(); }

#define GET_STAGE(X, Type, st)                                                                            \
    {                                                                                                     \
        ID3D11##Type* sh = nullptr;                                                                       \
        c->X##GetShader(&sh, nullptr, nullptr);                                                           \
        st.shader = sh;                                                                                   \
        Unref(sh);                                                                                        \
        c->X##GetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, st.cb);            \
        c->X##GetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, st.srv);                \
        c->X##GetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, st.smp);                              \
        UnrefAll(st.cb); UnrefAll(st.srv); UnrefAll(st.smp);                                              \
    }

static void GetState(ID3D11DeviceContext* c, ContextState& s) {
    c->IAGetInputLayout(&s.il); Unref(s.il);
    c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, s.vb, s.strides, s.offsets); UnrefAll(s.vb);
    c->IAGetIndexBuffer(&s.ib, &s.ibFormat, &s.ibOffset); Unref(s.ib);
    c->IAGetPrimitiveTopology(&s.topology);
    GET_STAGE(VS, VertexShader, s.vs) GET_STAGE(HS, HullShader, s.hs) GET_STAGE(DS, DomainShader, s.ds)
    GET_STAGE(GS, GeometryShader, s.gs) GET_STAGE(PS, PixelShader, s.ps) GET_STAGE(CS, ComputeShader, s.cs)
    c->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, s.csUav); UnrefAll(s.csUav);
    c->RSGetState(&s.rs); Unref(s.rs);
    c->RSGetViewports(&s.numViewports, s.viewports);
    c->RSGetScissorRects(&s.numScissors, s.scissors);
    c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, s.omUav);
    UnrefAll(s.rtv); Unref(s.dsv); UnrefAll(s.omUav);
    c->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask); Unref(s.blend);
    c->OMGetDepthStencilState(&s.dss, &s.stencilRef); Unref(s.dss);
    c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, s.so); UnrefAll(s.so);
    c->GetPredication(&s.predicate, &s.predicateValue); Unref(s.predicate);
}
#undef GET_STAGE

#define SET_STAGE(X, Type, st)                                                              \
    c->X##SetShader((ID3D11##Type*)st.shader, nullptr, 0);                                  \
    c->X##SetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, st.cb);  \
    c->X##SetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, st.srv);      \
    c->X##SetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, st.smp);

static void SetState(ID3D11DeviceContext* c, const ContextState& s) {
    c->ClearState();
    c->IASetInputLayout(s.il);
    c->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, s.vb, s.strides, s.offsets);
    c->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset);
    c->IASetPrimitiveTopology(s.topology);
    SET_STAGE(VS, VertexShader, s.vs) SET_STAGE(HS, HullShader, s.hs) SET_STAGE(DS, DomainShader, s.ds)
    SET_STAGE(GS, GeometryShader, s.gs) SET_STAGE(PS, PixelShader, s.ps) SET_STAGE(CS, ComputeShader, s.cs)
    UINT keep[D3D11_PS_CS_UAV_REGISTER_COUNT];
    for (UINT& k : keep) k = (UINT)-1;
    c->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, s.csUav, keep);
    c->RSSetState(s.rs);
    c->RSSetViewports(s.numViewports, s.viewports);
    c->RSSetScissorRects(s.numScissors, s.scissors);
    UINT numRtv = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) if (s.rtv[i]) numRtv = i + 1;
    UINT firstUav = D3D11_PS_CS_UAV_REGISTER_COUNT;
    for (UINT i = numRtv; i < D3D11_PS_CS_UAV_REGISTER_COUNT; i++) if (s.omUav[i]) { firstUav = i; break; }
    if (firstUav < D3D11_PS_CS_UAV_REGISTER_COUNT)
        c->OMSetRenderTargetsAndUnorderedAccessViews(numRtv, s.rtv, s.dsv, firstUav, D3D11_PS_CS_UAV_REGISTER_COUNT - firstUav, s.omUav + firstUav, keep);
    else
        c->OMSetRenderTargets(numRtv, s.rtv, s.dsv);
    c->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    c->OMSetDepthStencilState(s.dss, s.stencilRef);
    UINT soOffsets[D3D11_SO_BUFFER_SLOT_COUNT] = {(UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1};
    c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, s.so, soOffsets);
    c->SetPredication(s.predicate, s.predicateValue);
}
#undef SET_STAGE

static void DropStage(StageState& st) {
    Drop(st.shader);
    DropAll(st.cb); DropAll(st.srv); DropAll(st.smp);
}

SavedPipeline::SavedPipeline() : s_(new ContextState()) {
    t_keepRefs = true;
    GetState(g_ctx, *s_);
    t_keepRefs = false;
}

SavedPipeline::~SavedPipeline() {
    ContextState& s = *s_;
    SetState(g_ctx, s);
    Drop(s.il); DropAll(s.vb); Drop(s.ib);
    DropStage(s.vs); DropStage(s.hs); DropStage(s.ds); DropStage(s.gs); DropStage(s.ps); DropStage(s.cs);
    DropAll(s.csUav); Drop(s.rs); DropAll(s.rtv); Drop(s.dsv); DropAll(s.omUav); Drop(s.blend); Drop(s.dss);
    DropAll(s.so); Drop(s.predicate);
    delete s_;
}

// ------------------------------------------------------------------ resource contents
static bool BlockCompressed(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

// Subresources of a resource: count and, per index, rows and depth (blocks for compressed formats).
struct Layout {
    UINT count = 1;
    std::vector<std::pair<UINT, UINT>> rowsDepth;
};

static Layout LayoutOf(ID3D11Resource* r) {
    Layout l;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    auto mip = [](UINT v, UINT m) { return (v >> m) ? (v >> m) : 1u; };
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        l.rowsDepth.push_back({1, 1});
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D*)r)->GetDesc(&d);
        l.count = d.MipLevels * d.ArraySize;
        for (UINT i = 0; i < l.count; i++) l.rowsDepth.push_back({1, 1});
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        l.count = d.MipLevels * d.ArraySize;
        bool bc = BlockCompressed(d.Format);
        for (UINT a = 0; a < d.ArraySize; a++)
            for (UINT m = 0; m < d.MipLevels; m++) {
                UINT h = mip(d.Height, m);
                l.rowsDepth.push_back({bc ? (h + 3) / 4 : h, 1});
            }
    } else {
        D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d);
        l.count = d.MipLevels;
        bool bc = BlockCompressed(d.Format);
        for (UINT m = 0; m < d.MipLevels; m++) {
            UINT h = mip(d.Height, m);
            l.rowsDepth.push_back({bc ? (h + 3) / 4 : h, mip(d.Depth, m)});
        }
    }
    return l;
}

// A staging copy of `r` that the CPU can read.
static ID3D11Resource* StagingCopy(ID3D11Resource* r) {
    OwnObjects own;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    ID3D11Resource* s = nullptr;
    HRESULT hr = E_FAIL;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        hr = g_dev->CreateBuffer(&d, nullptr, (ID3D11Buffer**)&s);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D*)r)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        hr = g_dev->CreateTexture1D(&d, nullptr, (ID3D11Texture1D**)&s);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        hr = g_dev->CreateTexture2D(&d, nullptr, (ID3D11Texture2D**)&s);
    } else {
        D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        hr = g_dev->CreateTexture3D(&d, nullptr, (ID3D11Texture3D**)&s);
    }
    if (FAILED(hr)) return nullptr;
    g_ctx->CopyResource(s, r);
    return s;
}

// ------------------------------------------------------------------ contents kept for a load in this session
static double BitsPerPixel(DXGI_FORMAT f);

static std::string g_memoryWarning;   // set by the last Save
std::string MemoryWarning() { return g_memoryWarning; }

// The adapter's local video memory: current usage and the budget Windows gives this process (Windows 10+).
static bool VideoMemory(uint64_t* usage, uint64_t* budget) {
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* a = nullptr;
    IDXGIAdapter3* a3 = nullptr;
    bool ok = false;
    if (g_dev && SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&a)) &&
        SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            *usage = info.CurrentUsage;
            *budget = info.Budget;
            ok = true;
        }
    }
    if (a3) a3->Release();
    if (a) a->Release();
    if (dd) dd->Release();
    return ok;
}

static D3D11_USAGE UsageOf(ID3D11Resource* r, bool* buffer) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    *buffer = dim == D3D11_RESOURCE_DIMENSION_BUFFER;
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: { D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d); return d.Usage; }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: { D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D*)r)->GetDesc(&d); return d.Usage; }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: { D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d); return d.Usage; }
    default: { D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d); return d.Usage; }
    }
}

// About how many bytes a resource takes (for the log).
static uint64_t BytesOf(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    auto mip = [](UINT v, UINT m) { return (uint64_t)((v >> m) ? (v >> m) : 1u); };
    double total = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        return d.ByteWidth;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D*)r)->GetDesc(&d);
        for (UINT m = 0; m < d.MipLevels; m++) total += mip(d.Width, m) * BitsPerPixel(d.Format) / 8 * d.ArraySize;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        for (UINT m = 0; m < d.MipLevels; m++) total += mip(d.Width, m) * mip(d.Height, m) * BitsPerPixel(d.Format) / 8 * d.ArraySize;
    } else {
        D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d);
        for (UINT m = 0; m < d.MipLevels; m++) total += mip(d.Width, m) * mip(d.Height, m) * mip(d.Depth, m) * BitsPerPixel(d.Format) / 8;
    }
    return (uint64_t)total;
}

// A copy of a DEFAULT resource on the GPU, made from its contents now.
static ID3D11Resource* GpuCopy(ID3D11Resource* r) {
    OwnObjects own;
    const UINT shared = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    ID3D11Resource* s = nullptr;
    HRESULT hr = E_FAIL;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        d.MiscFlags &= ~shared;
        hr = g_dev->CreateBuffer(&d, nullptr, (ID3D11Buffer**)&s);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D*)r)->GetDesc(&d);
        d.MiscFlags &= ~shared;
        hr = g_dev->CreateTexture1D(&d, nullptr, (ID3D11Texture1D**)&s);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        d.MiscFlags &= ~shared;
        hr = g_dev->CreateTexture2D(&d, nullptr, (ID3D11Texture2D**)&s);
    } else {
        D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d);
        d.MiscFlags &= ~shared;
        hr = g_dev->CreateTexture3D(&d, nullptr, (ID3D11Texture3D**)&s);
    }
    if (FAILED(hr)) return nullptr;
    g_ctx->CopyResource(s, r);
    return s;
}

// Writes a staging copy back into a dynamic resource (the GPU can't copy into one): subresource by subresource,
// row by row, as both are laid out in memory.
static bool CopyBackDynamic(ID3D11Resource* dst, ID3D11Resource* staging) {
    Layout l = LayoutOf(staging);
    for (UINT i = 0; i < l.count; i++) {
        D3D11_MAPPED_SUBRESOURCE s, d;
        if (FAILED(g_ctx->Map(staging, i, D3D11_MAP_READ, 0, &s))) return false;
        if (FAILED(g_ctx->Map(dst, i, D3D11_MAP_WRITE_DISCARD, 0, &d))) {
            g_ctx->Unmap(staging, i);
            return false;
        }
        const UINT rows = l.rowsDepth[i].first, slices = l.rowsDepth[i].second;
        const UINT row = s.RowPitch < d.RowPitch ? s.RowPitch : d.RowPitch;
        for (UINT z = 0; z < slices; z++)
            for (UINT y = 0; y < rows; y++)
                memcpy((uint8_t*)d.pData + (size_t)z * d.DepthPitch + (size_t)y * d.RowPitch,
                       (const uint8_t*)s.pData + (size_t)z * s.DepthPitch + (size_t)y * s.RowPitch, row);
        g_ctx->Unmap(dst, i);
        g_ctx->Unmap(staging, i);
    }
    return true;
}

// Contents blob: per subresource {rowPitch, depthPitch, byte count} then the bytes as mapped.
static bool ReadBlob(ID3D11Resource* staging, std::vector<uint8_t>* out) {
    Layout l = LayoutOf(staging);
    for (UINT i = 0; i < l.count; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_ctx->Map(staging, i, D3D11_MAP_READ, 0, &m))) return false;
        D3D11_RESOURCE_DIMENSION dim;
        staging->GetType(&dim);
        uint32_t bytes;
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)staging)->GetDesc(&d);
            bytes = d.ByteWidth;
        } else {
            bytes = m.DepthPitch * (l.rowsDepth[i].second - 1) + m.RowPitch * l.rowsDepth[i].first;
        }
        uint32_t head[3] = {m.RowPitch, m.DepthPitch, bytes};
        out->insert(out->end(), (uint8_t*)head, (uint8_t*)head + 12);
        out->insert(out->end(), (uint8_t*)m.pData, (uint8_t*)m.pData + bytes);
        g_ctx->Unmap(staging, i);
    }
    return true;
}

// Read-back jobs, done a few at a time in the present hook (on the render thread, between frames).
struct Job {
    ID3D11DeviceChild* obj;          // the game's resource (pinned while the job waits)
    ID3D11Resource* staging;         // copy to read (null: still to make)
    uint64_t* result;                // where the hash goes (a capture entry), or null: the object's own content
};
static std::mutex g_jobLock;
static std::deque<Job>* g_jobs;
static std::vector<Job>* g_copied;   // copies issued last frame, read this frame

void Pump() {
    if (!g_ctx || !g_jobs) return;
    std::vector<Job> ready;
    {
        std::lock_guard<std::mutex> l(g_jobLock);
        ready.swap(*g_copied);
    }
    for (Job& j : ready) {
        std::vector<uint8_t> blob;
        uint64_t h = 0;
        if (j.staging && ReadBlob(j.staging, &blob)) {
            h = store::Hash(blob.data(), blob.size());
            store::Put(h, std::move(blob));
        }
        if (j.staging) j.staging->Release();
        j.obj->Release();   // the job's own reference
        if (j.result) {
            *j.result = h ? h : ~0ull;   // ~0: failed, the manifest treats it as missing
        } else {
            AcquireSRWLockExclusive(&g_lock);
            auto it = g_objs->find(j.obj);
            if (it != g_objs->end()) it->second.content = h;
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    // Copy the next batch; the GPU does it by the next frame.
    std::lock_guard<std::mutex> l(g_jobLock);
    size_t n = 0;
    while (!g_jobs->empty() && n < 400) {
        Job j = g_jobs->front();
        g_jobs->pop_front();
        if (!j.staging) j.staging = StagingCopy((ID3D11Resource*)j.obj);
        g_copied->push_back(j);
        n++;
    }
}

// ------------------------------------------------------------------ savestates
#pragma pack(push, 1)
struct EntryHead {
    uint64_t ptr, parent, code, content;
    uint32_t refs, descLen;
    uint8_t kind, flags;
};
#pragma pack(pop)

struct Entry {
    EntryHead h;
    std::vector<uint8_t> desc;
};

struct Capture {
    std::vector<std::pair<ID3D11DeviceChild*, ULONG>> objs;   // pinned objects and the references the game held
    std::vector<IUnknown*> bound;   // other DLLs' objects bound to the pipeline then (a reference each, for SetState)
    // Contents the game changes in place (resources it updates, dynamic textures), as they were: copies on the GPU,
    // and staging copies of dynamic textures. A load in this session copies them back.
    std::vector<std::pair<ID3D11Resource*, ID3D11Resource*>> kept, keptDynamic;
    // The swap chain's buffers then; buffersGone: it let go of its views of them for ResizeBuffers (see UnpinBufferViews).
    SwapInfo swapInfo;
    LONG swapEpoch = 0;
    bool buffersGone = false;
    ContextState state;
    // For another session (the manifest).
    std::vector<Entry> entries;
    uint64_t device = 0, context = 0, swap = 0, backBuffer = 0;
    std::string manifest;     // file to write once every content is stored
    bool written = false;
};

static std::mutex g_capLock;
static std::vector<Capture*>* g_unwritten;   // captures whose manifest isn't written yet

// Every savestate's capture, for UnpinBufferViews (which runs on the game's thread that calls ResizeBuffers: never
// while Save, Load or Drop run, as those stop that thread first).
static SRWLOCK g_capsLock = SRWLOCK_INIT;
static std::vector<Capture*> g_captures;

static int UnpinBufferViews(IDXGISwapChain* sc) {
    std::unordered_set<uint64_t> bufs;
    DXGI_SWAP_CHAIN_DESC d;
    const UINT n = SUCCEEDED(sc->GetDesc(&d)) && d.BufferCount ? d.BufferCount : 1;
    for (UINT i = 0; i < n && i < 16; i++) {
        ID3D11Texture2D* b = nullptr;
        if (SUCCEEDED(RealGetBuffer(sc, i, __uuidof(ID3D11Texture2D), (void**)&b)) && b) {
            bufs.insert((uint64_t)b);
            b->Release();
        }
    }
    // The game's views of them (alive: a savestate or the game still holds them).
    std::unordered_set<ID3D11DeviceChild*> views;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_objs)
        if (IsView(kv.second.kind) && kv.second.parent && bufs.count((uint64_t)kv.second.parent)) views.insert(kv.first);
    ReleaseSRWLockShared(&g_lock);
    if (views.empty()) return 0;
    std::vector<ID3D11DeviceChild*> release;
    int savestates = 0;
    AcquireSRWLockExclusive(&g_capsLock);
    for (Capture* c : g_captures) {
        const size_t before = c->objs.size();
        for (size_t i = 0; i < c->objs.size();) {
            if (views.count(c->objs[i].first)) {
                release.push_back(c->objs[i].first);
                c->objs.erase(c->objs.begin() + i);
            } else {
                i++;
            }
        }
        if (c->objs.size() != before) {
            c->buffersGone = true;
            savestates++;
        }
    }
    ReleaseSRWLockExclusive(&g_capsLock);
    for (ID3D11DeviceChild* o : release) {
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_objs->find(o);
        if (it != g_objs->end() && it->second.pins) it->second.pins--;
        ReleaseSRWLockExclusive(&g_lock);
        o->Release();   // may destroy it (its sentinel takes g_lock)
    }
    if (savestates)
        Log("d3d: %d savestates let go of %u references to views of the back buffer (they can't be loaded any more)",
            savestates, (unsigned)release.size());
    return savestates;
}

Capture* Save(const std::string& manifestPath, const Capture* replacing) {
    if (!g_ctx) return nullptr;
    if (!g_jobs) {
        g_jobs = new std::deque<Job>;
        g_copied = new std::vector<Job>;
        g_unwritten = new std::vector<Capture*>;
    }
    auto* c = new Capture();
    c->manifest = manifestPath;
    c->device = (uint64_t)g_dev;
    c->context = (uint64_t)g_ctx;
    c->swap = (uint64_t)g_swap;
    if (g_swap) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            c->backBuffer = (uint64_t)bb;
            bb->Release();
        }
    }
    c->swapInfo = CurrentSwap(true);
    c->swapEpoch = g_swapEpoch;
    std::unordered_map<uint64_t, uint32_t> bufferIndex;
    for (uint32_t i = 0; i < (uint32_t)c->swapInfo.buffers.size(); i++)
        if (c->swapInfo.buffers[i]) bufferIndex.emplace(c->swapInfo.buffers[i], i);
    // Only what the game uses: objects it holds references to, objects bound to the pipeline, and the resources and
    // class linkages those depend on. Objects alive only because an older savestate pins them stay out.
    std::vector<std::pair<ID3D11DeviceChild*, ID3D11DeviceChild*>> all;   // object, parent
    std::vector<uint32_t> pins;
    std::unordered_set<ID3D11DeviceChild*> used;
    AcquireSRWLockShared(&g_lock);
    all.reserve(g_objs->size());
    std::vector<uint8_t> kinds;
    for (auto& kv : *g_objs) {
        all.push_back({kv.first, kv.second.parent});
        pins.push_back(kv.second.pins);
        kinds.push_back(kv.second.kind);
    }
    ReleaseSRWLockShared(&g_lock);
    for (size_t i = 0; i < all.size(); i++) {
        long game = (long)all[i].first->AddRef() - 1 - (long)pins[i];
        all[i].first->Release();
        if (game > 0) used.insert(all[i].first);
    }
    GetState(g_ctx, c->state);
    int otherBufferViews = 0;
    AcquireSRWLockShared(&g_lock);
    ForEachPointer(c->state, [&](void* p) -> void* {
        if (!p) return p;
        used.insert((ID3D11DeviceChild*)p);
        // Not the game's (ShadowLib, TXAA...): not pinned below, but a load binds it again, so it must stay alive.
        // Except another DLL's view of the back buffer (an overlay's): keeping it would stop the swap chain from
        // making its buffers anew, so a load leaves that slot empty.
        if (!g_objs->count((ID3D11DeviceChild*)p)) {
            if (ViewOfBuffer((IUnknown*)p, bufferIndex)) {
                otherBufferViews++;
                return nullptr;
            }
            ((IUnknown*)p)->AddRef();
            c->bound.push_back((IUnknown*)p);
        }
        return p;
    });
    ReleaseSRWLockShared(&g_lock);
    for (auto& [o, parent] : all)
        if (used.count(o) && parent) used.insert(parent);
    // Resources the game no longer uses that only other savestates keep alive (the savestate this one replaces lets
    // go of its own right after: those are about to be freed).
    std::unordered_set<ID3D11DeviceChild*> replaced;
    if (replacing)
        for (auto& [o, refs] : replacing->objs) replaced.insert(o);
    uint64_t heldBytes = 0, freedSoon = 0;
    for (size_t i = 0; i < all.size(); i++) {
        if (!pins[i] || used.count(all[i].first) || kinds[i] < K_Buffer || kinds[i] > K_Tex3D) continue;
        long others = (long)pins[i] - (replaced.count(all[i].first) ? 1 : 0);
        (others > 0 ? heldBytes : freedSoon) += BytesOf((ID3D11Resource*)all[i].first);
    }
    std::vector<ID3D11DeviceChild*> live;
    int bufferViews = 0;   // the game's views of the swap chain's buffers among them (the log)
    for (size_t i = 0; i < all.size(); i++) {
        if (!used.count(all[i].first)) continue;
        live.push_back(all[i].first);
        if (IsView(kinds[i]) && all[i].second && bufferIndex.count((uint64_t)all[i].second)) bufferViews++;
    }
    c->objs.reserve(live.size());
    c->entries.reserve(live.size());
    std::vector<std::pair<ID3D11DeviceChild*, size_t>> snapshots, immutables;
    for (ID3D11DeviceChild* o : live) {
        ULONG refs = o->AddRef();   // our pin
        Entry e = {};
        AcquireSRWLockExclusive(&g_lock);
        Tracked& t = (*g_objs)[o];
        ULONG game = refs - 1 - t.pins;
        t.pins++;
        e.h = {(uint64_t)o, (uint64_t)t.parent, t.code, t.content, game, (uint32_t)t.desc.size(), t.kind, t.flags};
        e.desc = t.desc;
        ReleaseSRWLockExclusive(&g_lock);
        c->objs.push_back({o, game});
        if (e.h.flags & F_Snapshot) snapshots.push_back({o, c->entries.size()});
        else if ((e.h.flags & F_Immutable) && !e.h.content) immutables.push_back({o, c->entries.size()});
        c->entries.push_back(std::move(e));
    }
    // Contents the game changes in place, kept for a load in this session. The game writes into some resources
    // during play (the text glyph cache, say) and keeps track in its memory of what it wrote where: after a load
    // that memory is the saved one again, so the contents must be too. Render targets are redrawn every frame;
    // immutable resources can't change; dynamic buffers are rewritten before each use.
    uint64_t keptBytes = 0, dynamicBytes = 0;
    int keepFailed = 0;
    for (const Entry& e : c->entries) {
        if (e.h.kind < K_Buffer || e.h.kind > K_Tex3D || (e.h.flags & (F_Target | F_Immutable))) continue;
        auto* r = (ID3D11Resource*)e.h.ptr;
        bool buffer = false;
        D3D11_USAGE usage = UsageOf(r, &buffer);
        if (usage == D3D11_USAGE_DEFAULT) {
            if (ID3D11Resource* k = GpuCopy(r)) {
                c->kept.push_back({r, k});
                keptBytes += BytesOf(r);
            } else {
                keepFailed++;
            }
        } else if (usage == D3D11_USAGE_DYNAMIC && !buffer) {
            if (ID3D11Resource* k = StagingCopy(r)) {
                c->keptDynamic.push_back({r, k});
                dynamicBytes += BytesOf(r);
            } else {
                keepFailed++;
            }
        }
    }
    Log("d3d: contents kept for loads: %u resources (%.1f MB on the GPU), %u dynamic textures (%.1f MB)%s",
        (unsigned)c->kept.size(), keptBytes / 1048576.0, (unsigned)c->keptDynamic.size(), dynamicBytes / 1048576.0,
        keepFailed ? " (some copies could not be made: out of memory?)" : "");
    // Video memory: what the game uses against what Windows lets it use without moving things out to system RAM.
    // Savestates keep the textures of places the game has since left; on a card with little video memory several
    // savestates in different places can push the game over its budget (it stutters, then may fail to load textures).
    g_memoryWarning.clear();
    uint64_t usage = 0, budget = 0;
    if (VideoMemory(&usage, &budget)) {
        Log("d3d: video memory %.0f of %.0f MB in use; %.0f MB of it only for other savestates (%.0f MB more freed with "
            "the one replaced), %.0f MB copies", usage / 1048576.0, budget / 1048576.0, heldBytes / 1048576.0,
            freedSoon / 1048576.0, (keptBytes + dynamicBytes) / 1048576.0);
        const uint64_t after = usage > freedSoon ? usage - freedSoon : 0;
        if (budget && after > budget * 9 / 10 && heldBytes > (64ull << 20)) {
            char b[200];
            sprintf_s(b, "Video memory is nearly full: other savestates keep %.0f MB of it. Save over slots you don't need.",
                      heldBytes / 1048576.0);
            g_memoryWarning = b;
        }
    }
    if (manifestPath.empty()) {
        // No rebuild will need this savestate's objects: no contents to store, no manifest.
        snapshots.clear();
        immutables.clear();
    } else {
        // Contents: default and staging resources change, so copy them now (the render thread is parked); immutable
        // ones are read once, later.
        std::lock_guard<std::mutex> l(g_jobLock);
        // Each job holds a reference: the savestate may be dropped before it runs.
        for (auto& [o, i] : snapshots) {
            o->AddRef();
            g_jobs->push_back({o, StagingCopy((ID3D11Resource*)o), &c->entries[i].h.content});
        }
        for (auto& [o, i] : immutables) {
            o->AddRef();
            g_jobs->push_back({o, nullptr, nullptr});
        }
    }
    if (!manifestPath.empty()) {
        std::lock_guard<std::mutex> l(g_capLock);
        g_unwritten->push_back(c);
    }
    AcquireSRWLockExclusive(&g_capsLock);
    g_captures.push_back(c);
    ReleaseSRWLockExclusive(&g_capsLock);
    Log("d3d: %u objects pinned (%d created, %d destroyed so far); %u to copy, %u to read back", (unsigned)c->objs.size(),
        (int)g_created, (int)g_destroyed, (unsigned)snapshots.size(), (unsigned)immutables.size());
    Log("d3d: display %s; %d views of the back buffer among them%s", ModeName(c->swapInfo).c_str(), bufferViews,
        otherBufferViews ? " (another DLL's bound one left out)" : "");
    std::string others;
    AcquireSRWLockShared(&g_otherLock);
    for (auto& [m, n] : g_otherCreators) {
        char name[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, name, MAX_PATH);
        const char* s = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
        others += std::string(" ") + s + " " + std::to_string(n);
    }
    ReleaseSRWLockShared(&g_otherLock);
    if (!others.empty()) Log("d3d: objects other DLLs created (not the game's):%s", others.c_str());
    return c;
}

static bool WriteManifest(Capture* c) {
    std::string tmp = c->manifest + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") || !f) return false;
    const char magic[8] = {'G', 'T', 'A', '5', 'G', 'P', 'U', '1'};
    uint64_t head[6] = {c->entries.size(), c->device, c->context, c->swap, c->backBuffer, sizeof(ContextState)};
    bool ok = fwrite(magic, 8, 1, f) == 1 && fwrite(head, sizeof(head), 1, f) == 1 &&
              fwrite(&c->state, sizeof(ContextState), 1, f) == 1;
    for (const Entry& e : c->entries) {
        ok = ok && fwrite(&e.h, sizeof(EntryHead), 1, f) == 1;
        if (!e.desc.empty()) ok = ok && fwrite(e.desc.data(), e.desc.size(), 1, f) == 1;
    }
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExA(tmp.c_str(), c->manifest.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void FinishManifests() {
    if (!g_unwritten) return;
    std::vector<Capture*> todo;
    {
        std::lock_guard<std::mutex> l(g_capLock);
        todo = *g_unwritten;
    }
    for (Capture* c : todo) {
        // Fill in immutable contents read back since; done when every content is known.
        size_t missing = 0;
        AcquireSRWLockShared(&g_lock);
        for (Entry& e : c->entries) {
            if (e.h.flags & F_Immutable && !e.h.content) {
                auto it = g_objs->find((ID3D11DeviceChild*)e.h.ptr);
                if (it != g_objs->end()) e.h.content = it->second.content;
            }
            if ((e.h.flags & (F_Immutable | F_Snapshot)) && !e.h.content) missing++;
        }
        ReleaseSRWLockShared(&g_lock);
        if (missing) continue;
        store::Flush();
        bool ok = WriteManifest(c);
        Log("d3d: GPU data of %s %s (%u objects)", c->manifest.c_str(), ok ? "written" : "could not be written", (unsigned)c->entries.size());
        std::lock_guard<std::mutex> l(g_capLock);
        g_unwritten->erase(std::find(g_unwritten->begin(), g_unwritten->end(), c));
        c->written = ok;
    }
}

bool Complete(const Capture* c) { return c && c->written; }

unsigned SwapEpoch() { return (unsigned)g_swapEpoch; }

bool FromEarlierBuffers(const Capture* c) {
    if (!c) return false;
    AcquireSRWLockShared(&g_capsLock);
    const bool gone = c->buffersGone;
    ReleaseSRWLockShared(&g_capsLock);
    return gone || (c->swap && c->swapEpoch != g_swapEpoch);
}

bool Loadable(const Capture* c, std::string* why) {
    if (!FromEarlierBuffers(c)) return true;
    *why = "it was made before the game switched between fullscreen and a window (or changed resolution); "
           "savestates don't last through such a switch";
    return false;
}

void Load(const Capture* c) {
    if (!c || !g_ctx) return;
    std::unordered_map<ID3D11DeviceChild*, ULONG> want;
    want.reserve(c->objs.size());
    for (auto& [o, refs] : c->objs) want[o] = refs;
    std::vector<std::pair<ID3D11DeviceChild*, uint32_t>> live;
    AcquireSRWLockShared(&g_lock);
    live.reserve(g_objs->size());
    for (auto& kv : *g_objs) live.push_back({kv.first, kv.second.pins});
    ReleaseSRWLockShared(&g_lock);
    // Raise counts first, then lower them: lowering may destroy objects (their sentinels update the table).
    int raised = 0, lowered = 0;
    std::vector<std::pair<ID3D11DeviceChild*, long>> down;
    for (auto& [o, pins] : live) {
        long now = (long)o->AddRef() - 1 - (long)pins;
        o->Release();
        auto it = want.find(o);
        long target = it == want.end() ? 0 : (long)it->second;
        for (long i = now; i < target; i++) o->AddRef();
        if (now < target) raised++;
        if (now > target) down.push_back({o, now - target});
    }
    for (auto& [o, n] : down) {
        for (long i = 0; i < n; i++) o->Release();
        lowered++;
    }
    // The contents the game changes in place, as they were (see Save).
    int failed = 0;
    for (auto& [r, k] : c->kept) g_ctx->CopyResource(r, k);
    for (auto& [r, k] : c->keptDynamic)
        if (!CopyBackDynamic(r, k)) failed++;
    if (failed) Log("d3d: %d dynamic textures could not be put back", failed);
    SetState(g_ctx, c->state);
    Log("d3d: references set back on %d objects, lowered on %d (%u tracked before)", raised, lowered, (unsigned)live.size());
    LogBufferViews("after the load");
}

void Drop(Capture* c) {
    if (!c) return;
    AcquireSRWLockExclusive(&g_capsLock);
    g_captures.erase(std::remove(g_captures.begin(), g_captures.end(), c), g_captures.end());
    ReleaseSRWLockExclusive(&g_capsLock);
    {
        std::lock_guard<std::mutex> l(g_capLock);
        if (g_unwritten) {
            auto it = std::find(g_unwritten->begin(), g_unwritten->end(), c);
            if (it != g_unwritten->end()) {
                // Its jobs may still point into it: keep the capture object, just let go of the objects.
                g_unwritten->erase(it);
                c = new Capture(*c);
            }
        }
    }
    for (auto& [o, refs] : c->objs) {
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_objs->find(o);
        if (it != g_objs->end() && it->second.pins) it->second.pins--;
        ReleaseSRWLockExclusive(&g_lock);
        o->Release();
    }
    for (IUnknown* o : c->bound) o->Release();
    for (auto& [r, k] : c->kept) k->Release();
    for (auto& [r, k] : c->keptDynamic) k->Release();
    delete c;
}

// ------------------------------------------------------------------ rebuilding from a manifest
struct Manifest {
    uint64_t device = 0, context = 0, swap = 0, backBuffer = 0;
    ContextState state;
    std::vector<Entry> entries;
};

static bool ReadManifest(const std::string& path, Manifest* m) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    char magic[8];
    uint64_t head[6];
    bool ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, "GTA5GPU1", 8) && fread(head, sizeof(head), 1, f) == 1 &&
              head[5] == sizeof(ContextState) && fread(&m->state, sizeof(ContextState), 1, f) == 1;
    if (ok) {
        m->device = head[1];
        m->context = head[2];
        m->swap = head[3];
        m->backBuffer = head[4];
        m->entries.resize((size_t)head[0]);
        for (Entry& e : m->entries) {
            if (!(ok = fread(&e.h, sizeof(EntryHead), 1, f) == 1)) break;
            e.desc.resize(e.h.descLen);
            if (e.h.descLen && !(ok = fread(e.desc.data(), e.h.descLen, 1, f) == 1)) break;
        }
    }
    fclose(f);
    return ok;
}

bool References(const std::string& manifestPath, std::unordered_set<uint64_t>* out) {
    Manifest m;
    if (!ReadManifest(manifestPath, &m)) return false;
    for (const Entry& e : m.entries) {
        if (e.h.content) out->insert(e.h.content);
        if (e.h.code) out->insert(e.h.code);
    }
    return true;
}

// The manifest a coming rebuild uses, read before the game is stopped.
static Manifest* g_prepared = nullptr;
static std::string g_preparedPath;

bool Prepare(const std::string& manifestPath) {
    auto* m = new Manifest();
    if (!ReadManifest(manifestPath, m)) {
        delete m;
        return false;
    }
    delete g_prepared;
    g_prepared = m;
    g_preparedPath = manifestPath;
    return true;
}
void WarmUp() {
    if (!g_prepared) return;
    const ULONGLONG t0 = GetTickCount64();
    std::unordered_set<uint64_t> unique;
    for (const Entry& e : g_prepared->entries) {
        if (e.h.content && (e.h.flags & (F_Immutable | F_Snapshot))) unique.insert(e.h.content);
        if (e.h.code) unique.insert(e.h.code);
    }
    struct Work {
        std::vector<uint64_t> hashes;
        volatile LONG next = 0;
        volatile LONG64 bytes = 0;
    } work;
    work.hashes.assign(unique.begin(), unique.end());
    auto run = [](void* p) -> DWORD {
        auto* w = (Work*)p;
        std::vector<uint8_t> buf;
        for (LONG i; (i = InterlockedIncrement(&w->next) - 1) < (LONG)w->hashes.size();)
            if (store::Get(w->hashes[i], &buf)) InterlockedAdd64(&w->bytes, (LONG64)buf.size());
        return 0;
    };
    HANDLE th[12];
    int n = 0;
    for (; n < 12; n++)
        if (!(th[n] = CreateThread(nullptr, 0, run, &work, 0, nullptr))) break;
    WaitForMultipleObjects(n, th, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(th[i]);
    Log("d3d: read %u stored contents (%.0f MB) ahead in %u ms", (unsigned)work.hashes.size(), work.bytes / 1048576.0,
        (unsigned)(GetTickCount64() - t0));
}
static bool Subresources(const std::vector<uint8_t>& blob, std::vector<D3D11_SUBRESOURCE_DATA>* out) {
    size_t p = 0;
    while (p + 12 <= blob.size()) {
        uint32_t head[3];
        memcpy(head, blob.data() + p, 12);
        p += 12;
        if (p + head[2] > blob.size()) return false;
        out->push_back({blob.data() + p, head[0], head[1]});
        p += head[2];
    }
    return true;
}

static std::vector<D3D11_INPUT_ELEMENT_DESC> DecodeLayout(const std::vector<uint8_t>& v, std::vector<std::string>* names) {
    Cursor c{v.data(), v.data() + v.size()};
    UINT n = c.U32();
    names->resize(n);
    std::vector<D3D11_INPUT_ELEMENT_DESC> e(n);
    for (UINT i = 0; i < n; i++) {
        (*names)[i] = c.Str();
        e[i].SemanticIndex = c.U32(); e[i].Format = (DXGI_FORMAT)c.U32(); e[i].InputSlot = c.U32();
        e[i].AlignedByteOffset = c.U32(); e[i].InputSlotClass = (D3D11_INPUT_CLASSIFICATION)c.U32(); e[i].InstanceDataStepRate = c.U32();
    }
    for (UINT i = 0; i < n; i++) e[i].SemanticName = (*names)[i].c_str();
    return e;
}

// Makes one object like the manifest entry; `map` already holds new pointers for its parent.
// Contents and shader code of one entry, read ahead by the reader threads of a rebuild.
struct Loaded {
    std::vector<uint8_t> blob, code;
    bool blobOk = true, codeOk = true;
    volatile LONG ready = 0;
};

static bool NeedsContent(const Entry& e) { return e.h.content && (e.h.flags & (F_Immutable | F_Snapshot)); }

static ID3D11DeviceChild* Make(const Entry& e, const std::unordered_map<uint64_t, uint64_t>& map, Loaded& in, std::string* why) {
    const std::vector<uint8_t>& blob = in.blob;
    const std::vector<uint8_t>& code = in.code;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    auto mapped = [&](uint64_t old) -> void* { auto it = map.find(old); return it == map.end() ? nullptr : (void*)it->second; };
    if (NeedsContent(e) && (!in.blobOk || !Subresources(blob, &init))) { *why = "contents missing"; return nullptr; }
    if (e.h.code && !in.codeOk) { *why = "shader code missing"; return nullptr; }
    const D3D11_SUBRESOURCE_DATA* data = init.empty() ? nullptr : init.data();
    const void* d = e.desc.empty() ? nullptr : e.desc.data();
    ID3D11DeviceChild* o = nullptr;
    HRESULT hr = E_FAIL;
    switch (e.h.kind) {
    case K_Buffer: hr = g_dev->CreateBuffer((const D3D11_BUFFER_DESC*)d, data, (ID3D11Buffer**)&o); break;
    case K_Tex1D: hr = g_dev->CreateTexture1D((const D3D11_TEXTURE1D_DESC*)d, data, (ID3D11Texture1D**)&o); break;
    case K_Tex2D: hr = g_dev->CreateTexture2D((const D3D11_TEXTURE2D_DESC*)d, data, (ID3D11Texture2D**)&o); break;
    case K_Tex3D: hr = g_dev->CreateTexture3D((const D3D11_TEXTURE3D_DESC*)d, data, (ID3D11Texture3D**)&o); break;
    case K_SRV: case K_UAV: case K_RTV: case K_DSV: {
        auto* r = (ID3D11Resource*)mapped(e.h.parent);
        if (!r) { *why = "its resource is missing"; return nullptr; }
        const void* vd = (e.h.flags & F_Desc) ? d : nullptr;
        if (e.h.kind == K_SRV) hr = g_dev->CreateShaderResourceView(r, (const D3D11_SHADER_RESOURCE_VIEW_DESC*)vd, (ID3D11ShaderResourceView**)&o);
        if (e.h.kind == K_UAV) hr = g_dev->CreateUnorderedAccessView(r, (const D3D11_UNORDERED_ACCESS_VIEW_DESC*)vd, (ID3D11UnorderedAccessView**)&o);
        if (e.h.kind == K_RTV) hr = g_dev->CreateRenderTargetView(r, (const D3D11_RENDER_TARGET_VIEW_DESC*)vd, (ID3D11RenderTargetView**)&o);
        if (e.h.kind == K_DSV) hr = g_dev->CreateDepthStencilView(r, (const D3D11_DEPTH_STENCIL_VIEW_DESC*)vd, (ID3D11DepthStencilView**)&o);
        break;
    }
    case K_Layout: {
        std::vector<std::string> names;
        auto el = DecodeLayout(e.desc, &names);
        hr = g_dev->CreateInputLayout(el.data(), (UINT)el.size(), code.data(), code.size(), (ID3D11InputLayout**)&o);
        break;
    }
    case K_VS: hr = g_dev->CreateVertexShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11VertexShader**)&o); break;
    case K_GS: hr = g_dev->CreateGeometryShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11GeometryShader**)&o); break;
    case K_PS: hr = g_dev->CreatePixelShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11PixelShader**)&o); break;
    case K_HS: hr = g_dev->CreateHullShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11HullShader**)&o); break;
    case K_DS: hr = g_dev->CreateDomainShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11DomainShader**)&o); break;
    case K_CS: hr = g_dev->CreateComputeShader(code.data(), code.size(), (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11ComputeShader**)&o); break;
    case K_GSSO: {
        Cursor c{e.desc.data(), e.desc.data() + e.desc.size()};
        UINT ne = c.U32();
        std::vector<std::string> names(ne);
        std::vector<D3D11_SO_DECLARATION_ENTRY> so(ne);
        for (UINT i = 0; i < ne; i++) {
            so[i].Stream = c.U32();
            names[i] = c.Str();
            so[i].SemanticIndex = c.U32(); so[i].StartComponent = (BYTE)c.U32(); so[i].ComponentCount = (BYTE)c.U32(); so[i].OutputSlot = (BYTE)c.U32();
        }
        for (UINT i = 0; i < ne; i++) so[i].SemanticName = names[i].empty() ? nullptr : names[i].c_str();
        UINT ns = c.U32();
        std::vector<UINT> strides(ns);
        for (UINT& s : strides) s = c.U32();
        UINT rs = c.U32();
        hr = g_dev->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), so.data(), ne, strides.data(), ns, rs,
                                                         (ID3D11ClassLinkage*)mapped(e.h.parent), (ID3D11GeometryShader**)&o);
        break;
    }
    case K_Linkage: hr = g_dev->CreateClassLinkage((ID3D11ClassLinkage**)&o); break;
    case K_Blend: hr = g_dev->CreateBlendState((const D3D11_BLEND_DESC*)d, (ID3D11BlendState**)&o); break;
    case K_DSS: hr = g_dev->CreateDepthStencilState((const D3D11_DEPTH_STENCIL_DESC*)d, (ID3D11DepthStencilState**)&o); break;
    case K_RS: hr = g_dev->CreateRasterizerState((const D3D11_RASTERIZER_DESC*)d, (ID3D11RasterizerState**)&o); break;
    case K_Sampler: hr = g_dev->CreateSamplerState((const D3D11_SAMPLER_DESC*)d, (ID3D11SamplerState**)&o); break;
    case K_Query: hr = g_dev->CreateQuery((const D3D11_QUERY_DESC*)d, (ID3D11Query**)&o); break;
    case K_Predicate: hr = g_dev->CreatePredicate((const D3D11_QUERY_DESC*)d, (ID3D11Predicate**)&o); break;
    case K_Counter: hr = g_dev->CreateCounter((const D3D11_COUNTER_DESC*)d, (ID3D11Counter**)&o); break;
    default: *why = "unknown kind"; return nullptr;
    }
    if (FAILED(hr) || !o) {
        char b[64];
        sprintf_s(b, "creation failed (%08lX)", (unsigned long)hr);
        *why = b;
        return nullptr;
    }
    return o;
}

static int Order(uint8_t kind) {
    if (kind >= K_Buffer && kind <= K_Tex3D) return 0;
    if (kind == K_Linkage) return 0;
    if (kind >= K_SRV && kind <= K_DSV) return 1;
    return 2;
}

// Reader threads of a rebuild: they read the stored contents and code of the entries in creation order, at most
// kBudget bytes ahead of the rebuild, so reading (many small files) overlaps creating and memory stays bounded.
struct Readers {
    std::vector<const Entry*> order;
    std::unique_ptr<Loaded[]> loaded;
    volatile LONG next = 0, done = -1;   // next entry to read; last entry the rebuild has used
    volatile LONG64 held = 0;            // bytes read and not used yet
    static constexpr LONG64 kBudget = 512ll << 20;
};

static DWORD WINAPI ReadAhead(void* p) {
    auto* r = (Readers*)p;
    for (;;) {
        LONG i = InterlockedIncrement(&r->next) - 1;
        if (i >= (LONG)r->order.size()) return 0;
        // Within the budget; the entries the rebuild needs next always go through.
        while (r->held > Readers::kBudget && i > r->done + 16) Sleep(1);
        const Entry& e = *r->order[i];
        Loaded& l = r->loaded[i];
        if (NeedsContent(e)) l.blobOk = store::Get(e.h.content, &l.blob);
        if (e.h.code) l.codeOk = store::Get(e.h.code, &l.code);
        InterlockedAdd64(&r->held, (LONG64)(l.blob.size() + l.code.size()));
        InterlockedExchange(&l.ready, 1);
    }
}

// Until the GPU has finished everything sent to it so far (at most timeoutMs).
static void WaitForGpu(unsigned timeoutMs) {
    D3D11_QUERY_DESC d = {D3D11_QUERY_EVENT, 0};
    ID3D11Query* q = nullptr;
    {
        OwnObjects own;
        if (FAILED(g_dev->CreateQuery(&d, &q))) return;
    }
    g_ctx->End(q);
    g_ctx->Flush();
    BOOL done = FALSE;
    const ULONGLONG t0 = GetTickCount64();
    while (g_ctx->GetData(q, &done, sizeof(done), 0) != S_OK && GetTickCount64() - t0 < timeoutMs) Sleep(1);
    q->Release();
}

void ReleaseGame() {
    if (!g_ctx) return;
    WaitForGpu(2000);   // the stopped game's last frame may still use them
    g_ctx->ClearState();   // the pipeline's references to them
    std::vector<std::pair<ID3D11DeviceChild*, uint32_t>> live;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_objs) live.push_back({kv.first, kv.second.pins});
    ReleaseSRWLockShared(&g_lock);
    int released = 0;
    for (auto& [o, pins] : live) {
        long now = (long)o->AddRef() - 1 - (long)pins;
        o->Release();
        for (long i = 0; i < now; i++) o->Release();
        if (now > 0) released++;
    }
    g_ctx->Flush();
    Log("d3d: let go of %d objects of this session before the rebuild", released);
}

bool Recreate(const std::string& manifestPath, std::unordered_map<uint64_t, uint64_t>* map, std::string* why) {
    Manifest read;
    Manifest* mp = g_prepared && g_preparedPath == manifestPath ? g_prepared : &read;
    if (mp == &read && !ReadManifest(manifestPath, &read)) {
        *why = "its GPU data is missing or incomplete";
        return false;
    }
    Manifest& m = *mp;
    const ULONGLONG t0 = GetTickCount64();
    map->clear();
    map->reserve(m.entries.size() + 8);
    (*map)[m.device] = (uint64_t)g_dev;
    (*map)[m.context] = (uint64_t)g_ctx;
    if (m.swap) (*map)[m.swap] = (uint64_t)g_swap;
    if (m.backBuffer && g_swap) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            (*map)[m.backBuffer] = (uint64_t)bb;
            bb->Release();
        }
    }
    std::vector<std::pair<ID3D11DeviceChild*, uint32_t>> made;
    made.reserve(m.entries.size());
    int failed = 0;
    struct ForGame { ForGame() { t_forGame++; } ~ForGame() { t_forGame--; } } forGame;
    Readers rd;
    for (int pass = 0; pass < 3; pass++)
        for (const Entry& e : m.entries)
            if (Order(e.h.kind) == pass) rd.order.push_back(&e);
    rd.loaded.reset(new Loaded[rd.order.size()]);
    HANDLE readers[8];
    int nReaders = 0;
    for (; nReaders < 8; nReaders++)
        if (!(readers[nReaders] = CreateThread(nullptr, 0, &ReadAhead, &rd, 0, nullptr))) break;
    uint64_t bytes = 0;
    ULONGLONG waited = 0;
    for (size_t i = 0; i < rd.order.size(); i++) {
        const Entry& e = *rd.order[i];
        Loaded& l = rd.loaded[i];
        if (!l.ready) {
            const ULONGLONG w0 = GetTickCount64();
            while (!l.ready) Sleep(0);
            waited += GetTickCount64() - w0;
        }
        std::string reason;
        ID3D11DeviceChild* o = Make(e, *map, l, &reason);
        const size_t n = l.blob.size() + l.code.size();
        bytes += n;
        std::vector<uint8_t>().swap(l.blob);
        std::vector<uint8_t>().swap(l.code);
        InterlockedAdd64(&rd.held, -(LONG64)n);
        InterlockedExchange(&rd.done, (LONG)i);
        if (!o) {
            if (failed++ < 10) Log("d3d: cannot rebuild object %p (kind %u): %s", (void*)e.h.ptr, e.h.kind, reason.c_str());
            continue;
        }
        (*map)[e.h.ptr] = (uint64_t)o;
        made.push_back({o, e.h.refs});
    }
    WaitForMultipleObjects(nReaders, readers, TRUE, INFINITE);
    for (int i = 0; i < nReaders; i++) CloseHandle(readers[i]);
    // A query the game issued (a GPU fence, an occlusion test) is new and never issued: issue each once, so the
    // game's waits for its result end.
    int issued = 0;
    for (const Entry& e : m.entries) {
        if (e.h.kind != K_Query && e.h.kind != K_Predicate) continue;
        auto it = map->find(e.h.ptr);
        if (it == map->end() || e.desc.size() < sizeof(D3D11_QUERY_DESC)) continue;
        auto* q = (ID3D11Asynchronous*)it->second;
        D3D11_QUERY type = ((const D3D11_QUERY_DESC*)e.desc.data())->Query;
        if (type != D3D11_QUERY_EVENT && type != D3D11_QUERY_TIMESTAMP) g_ctx->Begin(q);
        g_ctx->End(q);
        issued++;
    }
    g_ctx->Flush();
    // The game's reference counts; release ours last, once views hold their resources.
    for (auto& [o, refs] : made)
        for (uint32_t i = 1; i < refs; i++) o->AddRef();
    for (auto& [o, refs] : made)
        if (!refs) o->Release();
    // The pipeline state, with the new objects.
    ContextState s = m.state;
    ForEachPointer(s, [&](void* p) -> void* {
        if (!p) return nullptr;
        auto it = map->find((uint64_t)p);
        return it == map->end() ? nullptr : (void*)it->second;
    });
    SetState(g_ctx, s);
    WaitForGpu(5000);   // the new objects' contents are on the GPU before the game uses them
    Log("d3d: rebuilt %u objects in %u ms (%.0f MB of contents; %u ms of that waiting for the disk; %d failed)",
        (unsigned)made.size(), (unsigned)(GetTickCount64() - t0), bytes / 1048576.0, (unsigned)waited, failed);
    if (failed) {
        *why = "some GPU objects could not be rebuilt (see savestate.log)";
        return false;
    }
    return true;
}

// After a rebuild: every live object the restored game doesn't refer to (all but the rebuilt ones) goes back to the
// references savestates hold on it, which destroys the ones nobody pinned.
void ReleaseUnreferenced(const std::unordered_map<uint64_t, uint64_t>& map) {
    std::unordered_set<uint64_t> fresh;
    for (auto& kv : map) fresh.insert(kv.second);
    std::vector<std::pair<ID3D11DeviceChild*, uint32_t>> live;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_objs)
        if (!fresh.count((uint64_t)kv.first)) live.push_back({kv.first, kv.second.pins});
    ReleaseSRWLockShared(&g_lock);
    int released = 0;
    for (auto& [o, pins] : live) {
        long now = (long)o->AddRef() - 1 - (long)pins;
        o->Release();
        for (long i = 0; i < now; i++) o->Release();
        if (now > 0) released++;
    }
    Log("d3d: let go of %d objects the restored game no longer uses", released);
}

// ------------------------------------------------------------------ lab statistics
static double BitsPerPixel(DXGI_FORMAT f) {
    if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return 4;
    if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return 4;
    if (BlockCompressed(f)) return 8;
    switch (f) {
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return 8;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R8G8_UNORM: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 64;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 128;
    default: return 32;
    }
}

void Stats() {
    struct Row { uint64_t n = 0, bytes = 0; };
    std::map<std::string, Row> rows;
    static const char* kinds[] = {"?", "buffer", "texture1d", "texture2d", "texture3d", "srv", "uav", "rtv", "dsv", "layout",
                                  "vs", "gs", "gs+so", "ps", "hs", "ds", "cs", "linkage", "blend", "depthstencil",
                                  "rasterizer", "sampler", "query", "predicate", "counter"};
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_objs) {
        const Tracked& t = kv.second;
        std::string key = kinds[t.kind < 25 ? t.kind : 0];
        if (t.flags & F_Target) key += " target";
        if (t.flags & F_Immutable) key += t.content ? " immutable (stored)" : " immutable";
        if (t.flags & F_Snapshot) key += " default/staging";
        uint64_t bytes = 0;
        if (t.kind == K_Buffer && t.desc.size() >= sizeof(D3D11_BUFFER_DESC)) bytes = ((D3D11_BUFFER_DESC*)t.desc.data())->ByteWidth;
        if (t.kind == K_Tex2D && t.desc.size() >= sizeof(D3D11_TEXTURE2D_DESC)) {
            auto* d = (D3D11_TEXTURE2D_DESC*)t.desc.data();
            for (UINT m = 0; m < d->MipLevels; m++) {
                double w = (d->Width >> m) ? (d->Width >> m) : 1, h = (d->Height >> m) ? (d->Height >> m) : 1;
                bytes += (uint64_t)(w * h * BitsPerPixel(d->Format) / 8 * d->ArraySize);
            }
        }
        rows[key].n++;
        rows[key].bytes += bytes;
    }
    ReleaseSRWLockShared(&g_lock);
    for (auto& [k, r] : rows) Log("  d3d %-32s %7llu objects %9.1f MB", k.c_str(), r.n, r.bytes / 1048576.0);
}

}  // namespace d3d
