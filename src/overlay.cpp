#include "overlay.h"
#include "common.h"
#include "d3d.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <vector>

namespace overlay {

static const char kShaders[] = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Rect : register(b0) { float4 rect; };   // left, top, right, bottom in clip space
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(V v) : SV_Target { return tex.Sample(smp, v.uv); }
)";

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11SamplerState* g_smp = nullptr;
static ID3D11BlendState* g_blend = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;
static ID3D11DepthStencilState* g_dss = nullptr;
static bool g_failed = false;
static bool g_installed = false;

static CRITICAL_SECTION g_cs;
static std::string g_text;           // wanted message
static ULONGLONG g_until = 0;
static unsigned g_version = 0;       // bumped by Show
static unsigned g_builtVersion = 0;  // version the texture shows
static ID3D11ShaderResourceView* g_srv = nullptr;
static int g_texW = 0, g_texH = 0, g_builtForHeight = 0, g_builtForWidth = 0;

static HRESULT(STDMETHODCALLTYPE* RealPresent)(IDXGISwapChain*, UINT, UINT);

template <typename T>
static void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

static bool CreatePipeline() {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (decltype(&D3DCompile))GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) return false;
    ID3DBlob *vsCode = nullptr, *psCode = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(compile(kShaders, sizeof(kShaders) - 1, "overlay", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsCode, &err)) &&
              SUCCEEDED(compile(kShaders, sizeof(kShaders) - 1, "overlay", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psCode, &err));
    if (err) {
        Log("overlay shaders: %s", (const char*)err->GetBufferPointer());
        err->Release();
    }
    if (ok) {
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                              D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd = {};
        ok = SUCCEEDED(g_dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(g_dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &g_ps)) &&
             SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_cb)) && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_smp)) &&
             SUCCEEDED(g_dev->CreateBlendState(&bl, &g_blend)) && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_rs)) &&
             SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_dss));
    }
    SafeRelease(vsCode);
    SafeRelease(psCode);
    return ok;
}

// Renders `text` with GDI: white letters on a translucent black box, straight alpha. Lines break at "\n" and where
// the text would run past the screen's right edge.
static void BuildTexture(const std::string& text, int screenWidth, int screenHeight) {
    SafeRelease(g_srv);
    g_texW = g_texH = 0;
    int fontPx = screenHeight / 36;
    if (fontPx < 14) fontPx = 14;
    const int pad = fontPx / 2;
    const int maxText = screenWidth - 2 * (screenHeight / 40) - 2 * pad;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-fontPx, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    if (!dc || !font) {
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT r = {0, 0, maxText > 100 ? maxText : 100, 0};
    const UINT fmt = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    DrawTextA(dc, text.c_str(), (int)text.size(), &r, fmt | DT_CALCRECT);
    g_texW = (int)(r.right - r.left) + 2 * pad;
    g_texH = (int)(r.bottom - r.top) + pad;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), g_texW, -g_texH, 1, 32, BI_RGB};
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!bmp || !bits) {   // out of memory: no message this time
        if (bmp) DeleteObject(bmp);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        g_texW = g_texH = 0;
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)g_texW * g_texH * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT dr = {pad, pad / 2, g_texW - pad, g_texH};
    DrawTextA(dc, text.c_str(), (int)text.size(), &dr, fmt);
    GdiFlush();
    std::vector<uint32_t> rgba((size_t)g_texW * g_texH);
    for (size_t i = 0; i < rgba.size(); i++) {
        uint32_t c = bits[i] & 0xFF;               // coverage of the white text
        uint32_t a = c > 170 ? c : 170;            // the box behind it
        rgba[i] = (a << 24) | (c << 16) | (c << 8) | c;
    }
    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);

    D3D11_TEXTURE2D_DESC td = {(UINT)g_texW, (UINT)g_texH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                               D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {rgba.data(), (UINT)g_texW * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &init, &tex))) {
        g_dev->CreateShaderResourceView(tex, nullptr, &g_srv);
        tex->Release();
    }
    g_builtForHeight = screenHeight;
    g_builtForWidth = screenWidth;
}

static void Draw(IDXGISwapChain* sc) {
    std::string text;
    unsigned version;
    EnterCriticalSection(&g_cs);
    bool visible = g_until && GetTickCount64() < g_until;
    text = g_text;
    version = g_version;
    LeaveCriticalSection(&g_cs);
    if (!visible || g_failed) return;

    d3d::OwnObjects scope;   // what we create here is ours, not the game's
    if (!g_vs && !CreatePipeline()) {
        g_failed = true;
        Log("overlay: cannot create its shaders; messages only in the window title");
        return;
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    if (version != g_builtVersion || (int)bd.Height != g_builtForHeight || (int)bd.Width != g_builtForWidth) {
        BuildTexture(text, (int)bd.Width, (int)bd.Height);
        g_builtVersion = version;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (!rtv || !g_srv) {
        SafeRelease(rtv);
        return;
    }

    d3d::SavedPipeline saved;   // the game's pipeline state, put back after our draw
    const float margin = (float)bd.Height / 40.0f;
    float rect[4] = {-1.0f + 2.0f * margin / bd.Width, 1.0f - 2.0f * margin / bd.Height,
                     -1.0f + 2.0f * (margin + g_texW) / bd.Width, 1.0f - 2.0f * (margin + g_texH) / bd.Height};
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, rect, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)bd.Width, (float)bd.Height, 0, 1};
    g_ctx->ClearState();
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_srv);
    g_ctx->PSSetSamplers(0, 1, &g_smp);
    g_ctx->RSSetState(g_rs);
    g_ctx->RSSetViewports(1, &vp);
    float factor[4] = {};
    g_ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_dss, 0);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->Draw(4, 0);
    rtv->Release();
}

static volatile LONG g_presents = 0;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT interval, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        d3d::Pump();
        Draw(sc);
    }
    HRESULT hr = RealPresent(sc, interval, flags);
    InterlockedIncrement(&g_presents);
    return hr;
}

void WaitFrames(int frames, unsigned timeoutMs) {
    if (!g_installed) return;
    const LONG start = g_presents;
    for (ULONGLONG t0 = GetTickCount64(); g_presents - start < frames && GetTickCount64() - t0 < timeoutMs;) Sleep(5);
}

void Install(IDXGISwapChain* swapChain, ID3D11Device* device) {
    InitializeCriticalSection(&g_cs);
    g_dev = device;
    device->GetImmediateContext(&g_ctx);
    void** v = *(void***)swapChain;
    RealPresent = (decltype(RealPresent))v[8];
    Unprotect(&v[8], sizeof(void*));
    v[8] = (void*)&Hook_Present;
    g_installed = true;
}

void Show(const std::string& text, unsigned ms) {
    if (!g_installed) return;
    EnterCriticalSection(&g_cs);
    g_text = text;
    g_until = GetTickCount64() + ms;
    g_version++;
    LeaveCriticalSection(&g_cs);
}

}  // namespace overlay
