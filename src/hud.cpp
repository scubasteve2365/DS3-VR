#include <atomic>
#include "hud.h"

#include <windows.h>
#include <d3d11.h>

#include <string>
#include <vector>

#include "camhook.h"
#include "drawlog.h"
#include "log.h"

namespace ds3vr {
namespace {

bool g_enabled = true;

// The game's HUD texture (found from last frame's full-screen copies), AddRef'd.
ID3D11Texture2D* g_appHud = nullptr;
// Ours: same size and format, transparent at the start of every frame.
ID3D11Texture2D* g_myTex = nullptr;
ID3D11RenderTargetView* g_myRtv = nullptr;

// What the game has bound right now (not AddRef'd; only valid while bound).
ID3D11RenderTargetView* g_gameRtv = nullptr;
ID3D11DepthStencilView* g_gameDsv = nullptr;
void* g_gameRt0 = nullptr;     // resource behind g_gameRtv, for comparisons only
bool g_onAppHud = false;       // the game is drawing into its HUD texture (with depth/stencil)
bool g_swapped = false;        // we have put our own texture in its place

bool g_gameplay = false;       // decided once per frame

// Lock-on marker fix: a 4 x float4 buffer the patched HUD shaders read at t60 (see hud.h).
constexpr UINT kMarkerSlot = 60;
ID3D11Buffer* g_markerBuf = nullptr;
ID3D11ShaderResourceView* g_markerSrv = nullptr;
bool g_markerBound = false;
bool g_markerFailed = false;
std::atomic<int> g_poseLag{2};
unsigned g_markerDraws = 0;

bool EnsureMarkerBuffer(ID3D11DeviceContext* c) {
    if (g_markerSrv) return true;
    if (g_markerFailed) return false;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &g_markerBuf);
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.FirstElement = 0;
        sd.Buffer.NumElements = 4;
        hr = dev->CreateShaderResourceView(g_markerBuf, &sd, &g_markerSrv);
    }
    dev->Release();
    if (FAILED(hr)) {
        Log("HUD: could not create the lock-on marker buffer (0x%08lx)", static_cast<unsigned long>(hr));
        g_markerFailed = true;
        return false;
    }
    return true;
}

// Before the lock-on marker draw: hand the HUD shaders the camera difference. After it: take it away
// again, so no other HUD draw is moved.
void MarkerBind(ID3D11DeviceContext* c, bool marker) {
    if (marker) {
        float m[16];
        if (!CamHookMarkerParams(g_poseLag.load(std::memory_order_relaxed), m) || !EnsureMarkerBuffer(c)) marker = false;
        else {
            c->UpdateSubresource(g_markerBuf, 0, nullptr, m, 0, 0);
            c->VSSetShaderResources(kMarkerSlot, 1, &g_markerSrv);
            g_markerBound = true;
            ++g_markerDraws;
            return;
        }
    }
    if (g_markerBound) {
        ID3D11ShaderResourceView* none = nullptr;
        c->VSSetShaderResources(kMarkerSlot, 1, &none);
        g_markerBound = false;
    }
}
unsigned g_redirects = 0, g_redirectsTotal = 0, g_framesWithHud = 0;
ULONGLONG g_lastStats = 0;

// Full-screen 4-vertex draws this frame: (render target, first texture read).
struct Pass {
    void* rt;
    void* t0;
};
std::vector<Pass> g_passes;

void* ResourceOf(ID3D11View* v) {
    if (!v) return nullptr;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (r) r->Release();  // the view keeps it alive
    return r;
}

void ReleaseOurs() {
    if (g_myRtv) g_myRtv->Release();
    if (g_myTex) g_myTex->Release();
    g_myRtv = nullptr;
    g_myTex = nullptr;
}

// Our texture: a copy of the HUD texture's description, with a view like the game's.
bool EnsureOurs(ID3D11DeviceContext* c, ID3D11RenderTargetView* gameRtv) {
    D3D11_TEXTURE2D_DESC want{};
    g_appHud->GetDesc(&want);
    if (g_myTex) {
        D3D11_TEXTURE2D_DESC have{};
        g_myTex->GetDesc(&have);
        if (have.Width == want.Width && have.Height == want.Height && have.Format == want.Format && have.ArraySize == want.ArraySize)
            return g_myRtv != nullptr;
        ReleaseOurs();
    }
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC d = want;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &g_myTex);
    if (SUCCEEDED(hr)) {
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        gameRtv->GetDesc(&vd);
        hr = dev->CreateRenderTargetView(g_myTex, &vd, &g_myRtv);
    }
    dev->Release();
    if (FAILED(hr)) {
        Log("HUD: could not create the HUD panel texture %ux%u fmt %d (0x%08lx); HUD stays in the 3D image", d.Width, d.Height,
            static_cast<int>(d.Format), static_cast<unsigned long>(hr));
        ReleaseOurs();
        g_enabled = false;
        return false;
    }
    Log("HUD: panel texture %ux%u fmt %d, %u slices", d.Width, d.Height, static_cast<int>(d.Format), d.ArraySize);
    return true;
}

void Bind(ID3D11DeviceContext* c, bool ours) {
    ID3D11RenderTargetView* v = ours ? g_myRtv : g_gameRtv;
    CtxSetRenderTargetsRaw(c, 1, &v, g_gameDsv);
    g_swapped = ours;
}

// Next frame's HUD texture: the texture read by a full-screen draw whose target is in turn read by a
// later full-screen draw (HUD texture -> intermediate -> back buffer).
void FindAppHud() {
    void* found = nullptr;
    for (size_t j = 0; j < g_passes.size(); ++j) {
        if (!g_passes[j].t0) continue;
        for (size_t i = 0; i < j; ++i)
            if (g_passes[i].rt == g_passes[j].t0 && g_passes[i].t0 && g_passes[i].t0 != g_passes[i].rt) found = g_passes[i].t0;
    }
    if (!found || found == g_appHud) return;
    ID3D11Resource* r = static_cast<ID3D11Resource*>(found);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    ID3D11Texture2D* t = static_cast<ID3D11Texture2D*>(r);
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET) || d.Width < 320 || d.Height < 240) return;
    t->AddRef();
    if (g_appHud) g_appHud->Release();
    g_appHud = t;
    Log("HUD: the game's HUD texture is %p (%ux%u fmt %d, %u slices)", static_cast<void*>(t), d.Width, d.Height, static_cast<int>(d.Format),
        d.ArraySize);
}

} // namespace

void HudInit() {
    const std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    g_enabled = GetPrivateProfileIntW(L"ds3vr", L"HudPanel", 1, ini.c_str()) != 0;
    {
        int lag = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"PoseLag", 2, ini.c_str()));
        if (lag < 0 || lag > 8) lag = 2;
        g_poseLag.store(lag);
    }
    Log("HUD: panel %s", g_enabled ? "on (HUD drawn into its own texture while in the game world)" : "off (HudPanel=0)");
}

void HudOnSetRenderTargets(ID3D11DeviceContext* c, unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    g_swapped = false;
    g_gameRtv = (n >= 1 && rtv) ? rtv[0] : nullptr;
    g_gameDsv = dsv;
    g_gameRt0 = ResourceOf(g_gameRtv);
    g_onAppHud = g_enabled && g_gameplay && g_appHud && g_gameRt0 == g_appHud && dsv && EnsureOurs(c, g_gameRtv);
}

// The lock-on marker: DS3 draws it into a small square render target (its glow), then pastes that
// onto the HUD texture at the enemy's screen position. It belongs to the enemy in the 3D world, not
// to the panel, so it stays in the game's image (geo-11 puts it at the enemy's depth; see the
// [TextureOverrideDs3vrLockOn] section in d3dx.ini).
bool IsWorldMarker(ID3D11Resource* r) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                      d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    return rgba && (d.BindFlags & D3D11_BIND_RENDER_TARGET) && d.ArraySize == 1 && d.Width == d.Height && d.Width < 512;
}

// The texture the lock-on marker is drawn from (its small atlas), learned from the draws that build
// the marker's glow in its own little render target. DS3 also flashes the marker straight onto the
// HUD from this texture when it locks on, which must stay in the world too.
void* g_markerAtlas = nullptr;
unsigned g_flashSkipped = 0;

bool HudBeforeDraw(ID3D11DeviceContext* c, unsigned count, bool indexed) {
    const bool fullScreen = count == 4 && !indexed;
    if (!fullScreen && !g_onAppHud) {
        if (g_markerBound) MarkerBind(c, false);
        if (g_gameRt0 && IsWorldMarker(static_cast<ID3D11Resource*>(g_gameRt0))) {
            ID3D11ShaderResourceView* srv = nullptr;
            c->PSGetShaderResources(0, 1, &srv);
            void* t = ResourceOf(srv);
            if (srv) srv->Release();
            if (t && !IsWorldMarker(static_cast<ID3D11Resource*>(t)) && t != g_markerAtlas) {
                g_markerAtlas = t;
                Log("HUD: lock-on marker texture %p", t);
            }
        }
        return true;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    ID3D11Resource* t0 = static_cast<ID3D11Resource*>(ResourceOf(srv));
    if (srv) srv->Release();
    if (fullScreen && g_gameRt0 && g_passes.size() < 64) g_passes.push_back({g_gameRt0, t0});
    if (!g_onAppHud) {
        if (g_markerBound) MarkerBind(c, false);
        return true;
    }
    // Quad batches go to our texture; full-screen draws and world markers go to the game's; anything
    // else stays wherever the last decision put it (long text runs are not always a multiple of 6).
    bool want = g_swapped;
    // DS3's one-off "lock acquired" flash is drawn straight onto the HUD from the marker texture. The
    // steady marker (its glow, drawn below) already shows the lock in the world, and the flash cannot
    // be placed correctly in VR, so it is skipped.
    if (count > 4 && t0 && t0 == g_markerAtlas) {
        if (g_markerBound) MarkerBind(c, false);
        ++g_flashSkipped;
        return false;
    }
    const bool marker = count > 4 && t0 && IsWorldMarker(t0);
    if (marker || g_markerBound) MarkerBind(c, marker);
    if (count <= 4 || marker)
        want = false;
    else if (count % 6 == 0)
        want = true;
    if (want != g_swapped) Bind(c, want);
    if (g_swapped) ++g_redirects;
    return true;
}

void HudEndFrame(ID3D11DeviceContext* c) {
    if (g_markerBound) MarkerBind(c, false);
    FindAppHud();
    g_passes.clear();
    if (g_myRtv) {
        const FLOAT clear[4] = {0.f, 0.f, 0.f, 0.f};
        c->ClearRenderTargetView(g_myRtv, clear);
    }
    if (g_redirects) ++g_framesWithHud;
    g_redirectsTotal += g_redirects;
    const ULONGLONG now = GetTickCount64();
    if (g_enabled && now - g_lastStats > 10000) {
        if (g_lastStats)
            Log("HUD: %u of the last frames had HUD draws on the panel (%u draws); lock-on marker draws moved: %u, lock flashes skipped: %u",
                g_framesWithHud, g_redirectsTotal, g_markerDraws, g_flashSkipped);
        g_flashSkipped = 0;
        g_markerDraws = 0;
        g_lastStats = now;
        g_framesWithHud = g_redirectsTotal = 0;
    }
    g_redirects = 0;
    g_onAppHud = false;
    g_swapped = false;
    g_gameplay = g_enabled && CamHookMsSinceUpdate() < 500;
}

void HudSetPoseLag(int lag) { g_poseLag.store(lag); }

ID3D11Texture2D* HudTexture() { return g_myTex; }

} // namespace ds3vr
