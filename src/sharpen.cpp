#include "sharpen.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <string>

#include "log.h"

namespace ds3vr {
namespace {

std::atomic<int> g_level{-1};
std::atomic<bool> g_busy{false};  // -1 = not read from ds3vr.ini yet
const char* const kNames[kSharpenLevels] = {"Off", "Low", "Medium", "High", "Max"};
const float kAmount[kSharpenLevels] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};

std::wstring Ini() { return std::wstring(ModuleDir()) + L"ds3vr.ini"; }

// Contrast-adaptive sharpening (AMD FidelityFX CAS, MIT): the non-scaling form, per colour channel.
const char kShader[] = R"(
Texture2D<float4> gSrc : register(t0);
cbuffer P : register(b0) {
    float gPeak;   // -1 / lerp(8, 5, sharpness)
    uint gHalfW;   // width of one eye
    uint gH;
    uint gPad;
};
struct VSOut { float4 pos : SV_Position; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Tap(int2 p, int x0, int x1) {
    p.x = clamp(p.x, x0, x1);
    p.y = clamp(p.y, 0, (int)gH - 1);
    return gSrc.Load(int3(p, 0)).rgb;
}
float4 PS(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    // Stay inside this pixel's eye: the two eyes sit side by side in one image.
    int x0 = p.x < (int)gHalfW ? 0 : (int)gHalfW;
    int x1 = x0 + (int)gHalfW - 1;
    float3 a = Tap(p + int2(-1, -1), x0, x1), b = Tap(p + int2(0, -1), x0, x1), c = Tap(p + int2(1, -1), x0, x1);
    float3 d = Tap(p + int2(-1, 0), x0, x1),  e = Tap(p, x0, x1),               f = Tap(p + int2(1, 0), x0, x1);
    float3 g = Tap(p + int2(-1, 1), x0, x1),  h = Tap(p + int2(0, 1), x0, x1),  k = Tap(p + int2(1, 1), x0, x1);
    // Soft minimum and maximum of the cross and the whole 3x3.
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));
    // Less sharpening where the neighbourhood already has strong contrast.
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 w = amp * gPeak;
    float3 o = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
    return float4(o, 1.0);
}
)";

struct Params {
    float peak;
    UINT halfW, h, pad;
};

struct State {
    bool tried = false, ok = false;
    ID3D11Device* dev = nullptr;
    ID3DDeviceContextState* state = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Texture2D* copy = nullptr;  // the image as it was, read by the shader
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
State g_s;

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The typeless format of a family, so one copy can be viewed as UNORM or sRGB.
DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default:
            return f;
    }
}

bool Compile(const char* entry, const char* target, ID3DBlob** code) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "ds3vr_sharpen", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, code, &err);
    if (FAILED(hr) || !*code) {
        Log("SHARPEN: %s did not compile (0x%08lX): %s", entry, hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "d3dcompiler_47.dll missing?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

bool Setup(ID3D11DeviceContext* ctx) {
    g_s.tried = true;
    ctx->GetDevice(&g_s.dev);
    g_s.dev->Release();  // the game keeps it alive
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(g_s.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
        const D3D_FEATURE_LEVEL fl = g_s.dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_s.state);
        d1->Release();
    }
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    bool ok = g_s.state && Compile("VS", "vs_5_0", &vs) && Compile("PS", "ps_5_0", &ps);
    if (ok) ok = SUCCEEDED(g_s.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_s.vs)) &&
                 SUCCEEDED(g_s.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_s.ps));
    SafeRelease(vs);
    SafeRelease(ps);
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Params);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = SUCCEEDED(g_s.dev->CreateBuffer(&bd, nullptr, &g_s.cb));
    }
    if (!ok) Log("SHARPEN: could not set up; sharpening is off this session");
    else Log("SHARPEN: ready");
    g_s.ok = ok;
    return ok;
}

bool MakeCopy(ID3D11Texture2D* image, DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC d{};
    image->GetDesc(&d);
    if (g_s.copy && d.Width == g_s.w && d.Height == g_s.h && fmt == g_s.fmt) return true;
    SafeRelease(g_s.srv);
    SafeRelease(g_s.copy);
    D3D11_TEXTURE2D_DESC c{};
    c.Width = d.Width;
    c.Height = d.Height;
    c.MipLevels = 1;
    c.ArraySize = 1;
    c.Format = Typeless(d.Format);
    c.SampleDesc.Count = 1;
    c.Usage = D3D11_USAGE_DEFAULT;
    c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = fmt;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    if (FAILED(g_s.dev->CreateTexture2D(&c, nullptr, &g_s.copy)) || FAILED(g_s.dev->CreateShaderResourceView(g_s.copy, &v, &g_s.srv))) {
        Log("SHARPEN: could not create its %ux%u work texture (format %d)", d.Width, d.Height, static_cast<int>(d.Format));
        SafeRelease(g_s.copy);
        g_s.ok = false;
        return false;
    }
    g_s.w = d.Width;
    g_s.h = d.Height;
    g_s.fmt = fmt;
    return true;
}

} // namespace

int SharpenLevel() {
    int l = g_level.load();
    if (l < 0) {
        l = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"Sharpening", 0, Ini().c_str()));
        if (l < 0 || l >= kSharpenLevels) l = 0;
        g_level.store(l);
    }
    return l;
}

void SharpenSetLevel(int level) {
    if (level < 0 || level >= kSharpenLevels) level = 0;
    g_level.store(level);
    wchar_t v[4];
    swprintf(v, 4, L"%d", level);
    WritePrivateProfileStringW(L"ds3vr", L"Sharpening", v, Ini().c_str());
    Log("menu: sharpening %s", kNames[level]);
}

bool SharpenBusy() { return g_busy.load(std::memory_order_relaxed); }

const char* SharpenLevelName(int level) { return level >= 0 && level < kSharpenLevels ? kNames[level] : "?"; }

void SharpenStereo(ID3D11DeviceContext* ctx, ID3D11Texture2D* image, DXGI_FORMAT fmt) {
    const int level = SharpenLevel();
    if (level == 0 || !ctx || !image) return;
    if (!g_s.tried && !Setup(ctx)) return;
    if (!g_s.ok || !MakeCopy(image, fmt)) return;

    // The render target view of this swapchain image (made once per image; the runtime keeps them alive).
    struct Rtv {
        ID3D11Texture2D* tex;
        ID3D11RenderTargetView* rtv;
    };
    static Rtv rtvs[8] = {};
    ID3D11RenderTargetView* rtv = nullptr;
    for (Rtv& r : rtvs)
        if (r.tex == image) rtv = r.rtv;
    if (!rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = fmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_s.dev->CreateRenderTargetView(image, &rd, &rtv))) {
            Log("SHARPEN: the headset image cannot be drawn to (format %d); sharpening is off this session", static_cast<int>(fmt));
            g_s.ok = false;
            return;
        }
        for (Rtv& r : rtvs)
            if (!r.tex) {
                r = Rtv{image, rtv};
                break;
            }
    }

    ctx->CopyResource(g_s.copy, image);
    Params p{};
    const float s = kAmount[level];
    p.peak = -1.f / (8.f + (5.f - 8.f) * s);
    p.halfW = g_s.w / 2;
    p.h = g_s.h;

    // Work in a clean pipeline state; the game's state comes back untouched.
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || !c1) return;
    ID3DDeviceContextState* saved = nullptr;
    g_busy = true;
    c1->SwapDeviceContextState(g_s.state, &saved);
    ctx->UpdateSubresource(g_s.cb, 0, nullptr, &p, 0, 0);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(g_s.w), static_cast<float>(g_s.h), 0.f, 1.f};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_s.vs, nullptr, 0);
    ctx->PSSetShader(g_s.ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_s.cb);
    ctx->PSSetShaderResources(0, 1, &g_s.srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    c1->SwapDeviceContextState(saved, nullptr);
    g_busy = false;
    SafeRelease(saved);
    c1->Release();
}

} // namespace ds3vr
