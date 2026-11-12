// How public reference counts of D3D11 objects react to views, pipeline bindings, ClearState and private data.
#include <d3d11.h>
#include <cstdio>

static ULONG Refs(IUnknown* u) { u->AddRef(); return u->Release(); }

struct Sentinel : IUnknown {
    ULONG refs = 1;
    const char* name;
    explicit Sentinel(const char* n) : name(n) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (r == 0) printf("  sentinel of %s released: object destroyed\n", name);
        return r;
    }
};

int main() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        puts("no device");
        return 1;
    }
    D3D11_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    printf("texture created: %lu\n", Refs(tex));
    auto* s = new Sentinel("texture");
    static const GUID g = {0x1234abcd, 0x1, 0x2, {1, 2, 3, 4, 5, 6, 7, 8}};
    tex->SetPrivateDataInterface(g, s);
    printf("after SetPrivateDataInterface: texture %lu, sentinel %lu\n", Refs(tex), s->refs);
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    printf("after SRV: texture %lu, srv %lu\n", Refs(tex), Refs(srv));
    ctx->PSSetShaderResources(0, 1, &srv);
    printf("after PSSetShaderResources: texture %lu, srv %lu\n", Refs(tex), Refs(srv));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(tex, nullptr, &rtv);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    printf("after OMSetRenderTargets: texture %lu, rtv %lu (SRV unbound by hazard tracking? srv %lu)\n", Refs(tex), Refs(rtv), Refs(srv));
    ID3D11ShaderResourceView* got = nullptr;
    ctx->PSGetShaderResources(0, 1, &got);
    printf("PSGetShaderResources returned %p (srv %p), srv refs %lu\n", (void*)got, (void*)srv, Refs(srv));
    if (got) got->Release();
    ctx->ClearState();
    printf("after ClearState: texture %lu, srv %lu, rtv %lu\n", Refs(tex), Refs(srv), Refs(rtv));
    srv->Release();
    rtv->Release();
    printf("after releasing views: texture %lu\n", Refs(tex));
    tex->Release();
    printf("texture released by its owner\n");
    ctx->Release();
    dev->Release();
    return 0;
}
