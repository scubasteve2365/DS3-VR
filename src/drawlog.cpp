#include "drawlog.h"
#include "xr.h"

#include <cmath>
#include <cstdint>

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MinHook.h"
#include "dlss.h"
#include "hud.h"
#include "log.h"

namespace ds3vr {
namespace {

// ID3D11DeviceContext vtable slots
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21, kOMSetRenderTargets = 33,
              kOMSetRTAndUAV = 34, kDispatch = 41, kCopySubresourceRegion = 46, kCopyResource = 47, kClearRTV = 50, kClearDSV = 53,
              kResolve = 57;
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
DispatchFn o_Dispatch = nullptr;

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                              UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using CopySubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                           const D3D11_BOX*);
using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ClearRTVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);

DrawIndexedFn o_DrawIndexed = nullptr;
DrawFn o_Draw = nullptr;
DrawIndexedInstancedFn o_DrawIndexedInstanced = nullptr;
DrawInstancedFn o_DrawInstanced = nullptr;
OMSetRTFn o_OMSetRT = nullptr;
OMSetRTUAVFn o_OMSetRTUAV = nullptr;
CopySubFn o_CopySub = nullptr;
CopyResFn o_CopyRes = nullptr;
ClearRTVFn o_ClearRTV = nullptr;
ClearDSVFn o_ClearDSV = nullptr;
ResolveFn o_Resolve = nullptr;

// The same calls on DS3's deferred contexts, when their vtable has its own functions (see Install).
DrawIndexedFn d_DrawIndexed = nullptr;
DrawFn d_Draw = nullptr;
DrawIndexedInstancedFn d_DrawIndexedInstanced = nullptr;
DrawInstancedFn d_DrawInstanced = nullptr;
OMSetRTFn d_OMSetRT = nullptr;
OMSetRTUAVFn d_OMSetRTUAV = nullptr;
DispatchFn d_Dispatch = nullptr;
CopySubFn d_CopySub = nullptr;
CopyResFn d_CopyRes = nullptr;
ClearRTVFn d_ClearRTV = nullptr;
ClearDSVFn d_ClearDSV = nullptr;
ResolveFn d_Resolve = nullptr;
constexpr int kExecuteCommandList = 58, kFinishCommandList = 114;
using ExecuteCLFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCLFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
ExecuteCLFn o_ExecuteCL = nullptr;
FinishCLFn d_FinishCL = nullptr;

ID3D11DeviceContext* g_ctx = nullptr;  // the real immediate context
bool g_installed = false;
std::atomic<int> g_state{0};           // 0 idle, 1 start at the next Present, 2 recording
std::atomic<bool> g_request{false};
int g_fileNo = 0;
unsigned g_seq = 0;
std::string g_buf;
std::unordered_map<void*, int> g_resIds;
std::unordered_map<void*, int> g_shaderIds;
int g_nextRes = 0, g_nextShader = 0;
std::unordered_map<void*, int> g_ctxIds;   // deferred contexts, numbered as first seen ([d1], [d2], ...)
std::unordered_map<void*, int> g_listIds;  // command lists (L1, L2, ...)
std::recursive_mutex g_recMutex;           // deferred contexts record from the game's worker threads

// Any context while a frame is being recorded (the immediate one and DS3's deferred ones).
bool Recording(ID3D11DeviceContext* c) { return c && g_state.load(std::memory_order_relaxed) == 2; }

std::string CtxTag(ID3D11DeviceContext* c) {
    if (c == g_ctx) return std::string();
    auto it = g_ctxIds.find(c);
    int id = 0;
    if (it != g_ctxIds.end()) {
        id = it->second;
    } else {
        id = static_cast<int>(g_ctxIds.size()) + 1;
        g_ctxIds[c] = id;
    }
    return "[d" + std::to_string(id) + "] ";
}

int ListId(void* l) {
    auto it = g_listIds.find(l);
    if (it != g_listIds.end()) return it->second;
    const int id = static_cast<int>(g_listIds.size()) + 1;
    g_listIds[l] = id;
    return id;
}

// Start of one recorded line: hold the lock for the rest of the scope, and tag deferred contexts.
#define REC_BEGIN(c)                                              \
    std::lock_guard<std::recursive_mutex> recLock(g_recMutex);    \
    g_buf += CtxTag(c)

void Add(const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    g_buf += b;
}

// "R7" for a resource seen before; "R7=[3728x2880 f87 a2 m1 s1 b0x28]" the first time.
std::string Res(ID3D11Resource* r) {
    if (!r) return "-";
    auto it = g_resIds.find(r);
    if (it != g_resIds.end()) return "R" + std::to_string(it->second);
    const int id = ++g_nextRes;
    g_resIds[r] = id;
    char b[200];
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        { ID3D11Texture2D* q = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&q))) && q) { q->GetDesc(&d); q->Release(); } }
        snprintf(b, sizeof(b), "R%d=[%ux%u f%d a%u m%u s%u b0x%x misc0x%x]", id, d.Width, d.Height, static_cast<int>(d.Format),
                 d.ArraySize, d.MipLevels, d.SampleDesc.Count, d.BindFlags, d.MiscFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        { ID3D11Buffer* q = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&q))) && q) { q->GetDesc(&d); q->Release(); } }
        snprintf(b, sizeof(b), "R%d=[buf %u b0x%x]", id, d.ByteWidth, d.BindFlags);
    } else {
        snprintf(b, sizeof(b), "R%d=[dim%d]", id, static_cast<int>(dim));
    }
    return b;
}

std::string Rtv(ID3D11RenderTargetView* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = Res(r);
    if (r) r->Release();
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    v->GetDesc(&d);
    char b[64];
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
        snprintf(b, sizeof(b), "{f%d sl%u+%u}", static_cast<int>(d.Format), d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else
        snprintf(b, sizeof(b), "{f%d dim%d}", static_cast<int>(d.Format), static_cast<int>(d.ViewDimension));
    return s + b;
}

std::string Dsv(ID3D11DepthStencilView* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = Res(r);
    if (r) r->Release();
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    v->GetDesc(&d);
    char b[64];
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
        snprintf(b, sizeof(b), "{f%d sl%u+%u}", static_cast<int>(d.Format), d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else
        snprintf(b, sizeof(b), "{f%d dim%d}", static_cast<int>(d.Format), static_cast<int>(d.ViewDimension));
    return s + b;
}

std::string Uav(ID3D11UnorderedAccessView* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = Res(r);
    if (r) r->Release();
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    v->GetDesc(&d);
    char b[64];
    if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
        snprintf(b, sizeof(b), "{f%d sl%u+%u}", static_cast<int>(d.Format), d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else
        snprintf(b, sizeof(b), "{f%d dim%d}", static_cast<int>(d.Format), static_cast<int>(d.ViewDimension));
    return s + b;
}

std::string Srv(ID3D11ShaderResourceView* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = Res(r);
    if (r) r->Release();
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    v->GetDesc(&d);
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
        char b[48];
        snprintf(b, sizeof(b), "{sl%u+%u}", d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
        s += b;
    }
    return s;
}

// The bound shader resources of one stage, as " i:R<n>" for each slot in use (slots 0 to 15).
template <class Getter>
std::string BoundSrvs(Getter get) {
    ID3D11ShaderResourceView* srv[16] = {};
    get(srv);
    std::string s;
    for (int i = 0; i < 16; ++i)
        if (srv[i]) {
            s += " " + std::to_string(i) + ":" + Srv(srv[i]);
            srv[i]->Release();
        }
    return s.empty() ? " -" : s;
}

int ShaderId(void* s) {
    if (!s) return 0;
    auto it = g_shaderIds.find(s);
    if (it != g_shaderIds.end()) return it->second;
    return g_shaderIds[s] = ++g_nextShader;
}

// Everything bound for a draw: targets, the pixel shader's first textures, shaders, blend, depth, viewport.
void DrawState(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(8, rtv, &dsv);
    std::string s = " rt";
    for (int i = 0; i < 8; ++i)
        if (rtv[i]) {
            s += " " + std::to_string(i) + ":" + Rtv(rtv[i]);
            rtv[i]->Release();
        }
    s += " ds:" + Dsv(dsv);
    if (dsv) dsv->Release();

    s += " t:" + BoundSrvs([c](ID3D11ShaderResourceView** v) { c->PSGetShaderResources(0, 16, v); });

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    char b[400];
    snprintf(b, sizeof(b), " vs%d ps%d", ShaderId(vs), ShaderId(ps));
    s += b;
    if (vs) vs->Release();
    if (ps) ps->Release();

    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    c->OMGetBlendState(&bs, factor, &mask);
    if (bs) {
        D3D11_BLEND_DESC d{};
        bs->GetDesc(&d);
        const auto& t = d.RenderTarget[0];
        snprintf(b, sizeof(b), " blend[%d %d/%d op%d a:%d/%d op%d w%x a2c%d ind%d]", t.BlendEnable ? 1 : 0, static_cast<int>(t.SrcBlend),
                 static_cast<int>(t.DestBlend), static_cast<int>(t.BlendOp), static_cast<int>(t.SrcBlendAlpha),
                 static_cast<int>(t.DestBlendAlpha), static_cast<int>(t.BlendOpAlpha), t.RenderTargetWriteMask,
                 d.AlphaToCoverageEnable ? 1 : 0, d.IndependentBlendEnable ? 1 : 0);
        s += b;
        bs->Release();
    } else {
        s += " blend[default]";
    }

    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&dss, &ref);
    if (dss) {
        D3D11_DEPTH_STENCIL_DESC d{};
        dss->GetDesc(&d);
        snprintf(b, sizeof(b), " depth[%d w%d f%d st%d]", d.DepthEnable ? 1 : 0, static_cast<int>(d.DepthWriteMask),
                 static_cast<int>(d.DepthFunc), d.StencilEnable ? 1 : 0);
        s += b;
        dss->Release();
    } else {
        s += " depth[default]";
    }

    UINT nvp = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&nvp, &vp);
    if (nvp) {
        snprintf(b, sizeof(b), " vp[%.0f,%.0f %.0fx%.0f]", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
        s += b;
    }
    g_buf += s;
    g_buf += "\n";
}

// ---------- geo-11's stereo separation, read from its StereoParams texture ----------
// geo-11 (3Dmigoto) binds a small float4 texture in slot 125 for its shaders: x = this eye's final
// separation (+/-), y = convergence, z = eye (-1/+1). |x| is how far geo-11 slides each eye's image at
// infinity, which the views sent to the headset must cancel. It depends on the PC (geo-11 works it out
// from the monitor's physical size), so it is read here rather than assumed. Copy one texel, then read
// it a few frames later without waiting, so the game never stalls; repeat every few seconds.
ID3D11Resource* g_probeStaging = nullptr;  // staging copy matching geo-11's resource type
bool g_probePending = false;
ULONGLONG g_probeAt = 0;        // when to do the next step
ULONGLONG g_probeStartMs = 0;   // first attempt (for the "could not read" note)
bool g_probeEverRead = false, g_probeWarned = false;
unsigned g_probeTexW = 1;                       // texels copied (first row, up to 8)
uint64_t g_probeDraws = 0, g_probeHits = 0;     // diagnostics: draws looked at, draws with slot 125 bound
int g_probeRawLogs = 0;
std::atomic<float> g_stereoSep{0.f}, g_stereoConv{0.f};  // last values read (for DLAA's motion vectors)
ULONGLONG g_probeStatAt = 0;

void StereoProbe(ID3D11DeviceContext* c) {
    const ULONGLONG now = GetTickCount64();
    if (!g_probeStartMs) g_probeStartMs = now;
    if (!g_probeEverRead && now >= g_probeStatAt && now - g_probeStartMs < 60000) {
        if (g_probeStatAt) Log("STEREO: probe looked at %llu draws in the last 10 s, slot 125 bound in %llu", g_probeDraws, g_probeHits);
        g_probeDraws = g_probeHits = 0;
        g_probeStatAt = now + 10000;
    }
    if (now < g_probeAt) return;
    if (g_probePending) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = c->Map(g_probeStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        g_probePending = false;
        g_probeAt = now + 5000;
        if (FAILED(hr)) return;
        const float* f = static_cast<const float*>(m.pData);
        const float sep = f[0], conv = f[1], eye = f[2];
        if (g_probeRawLogs < 1) {
            ++g_probeRawLogs;
            char b[512];
            int n = snprintf(b, sizeof(b), "STEREO: StereoParams row 0 (%u texels):", g_probeTexW);
            for (unsigned i = 0; i < g_probeTexW && n < 480; ++i)
                n += snprintf(b + n, sizeof(b) - n, " [%.5g %.5g %.5g %.5g]", f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]);
            Log("%s", b);
        }
        c->Unmap(g_probeStaging, 0);
        if (std::fabs(sep) > 0.0005f && std::fabs(sep) < 0.3f) {
            g_stereoSep.store(std::fabs(sep));
            g_stereoConv.store(conv);
            XrSetMeasuredShift(std::fabs(sep), conv, eye);
            g_probeEverRead = true;
        } else if (g_probeRawLogs <= 1) {
            Log("STEREO: separation value %.5g is out of range; not used", sep);
        }
        return;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    ++g_probeDraws;
    c->VSGetShaderResources(125, 1, &srv);
    if (!srv) c->PSGetShaderResources(125, 1, &srv);
    if (srv) ++g_probeHits;
    if (!srv) {
        if (!g_probeEverRead && !g_probeWarned && now - g_probeStartMs > 120000) {
            g_probeWarned = true;
            Log("STEREO: could not read geo-11's separation (no StereoParams texture seen); using the default shift");
        }
        // Until it is found, look at every draw; after two minutes without it, only once a second.
        if (!g_probeEverRead && now - g_probeStartMs > 120000) g_probeAt = now + 1000;
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    srv->GetDesc(&vd);
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    srv->Release();
    if (!r) {
        g_probeAt = now + 1000;
        return;
    }
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    // What kind of resource geo-11 uses for StereoParams: log it once.
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 1, samples = 1, bytes = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        { ID3D11Texture2D* q = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&q))) && q) { q->GetDesc(&d); q->Release(); } }
        fmt = d.Format; width = d.Width; height = d.Height; samples = d.SampleDesc.Count;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d{};
        { ID3D11Texture1D* q = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture1D), reinterpret_cast<void**>(&q))) && q) { q->GetDesc(&d); q->Release(); } }
        fmt = d.Format; width = d.Width;
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        { ID3D11Buffer* q = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&q))) && q) { q->GetDesc(&d); q->Release(); } }
        bytes = d.ByteWidth;
        fmt = vd.Format;
        width = bytes / 16;
    }
    static bool loggedDesc = false;
    if (!loggedDesc) {
        loggedDesc = true;
        Log("STEREO: StereoParams found: resource type %d (2 = 1D texture, 3 = 2D texture, 1 = buffer), %ux%u, format %d, view format %d, %u bytes",
            static_cast<int>(dim), width, height, static_cast<int>(fmt), static_cast<int>(vd.Format), bytes);
    }
    const bool floatRgba = fmt == DXGI_FORMAT_R32G32B32A32_FLOAT || vd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (!floatRgba || samples != 1 || width == 0 ||
        (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D && dim != D3D11_RESOURCE_DIMENSION_TEXTURE1D && dim != D3D11_RESOURCE_DIMENSION_BUFFER)) {
        if (!g_probeWarned) {
            g_probeWarned = true;
            Log("STEREO: could not read geo-11's separation (unexpected StereoParams layout); using the default shift");
        }
        r->Release();
        g_probeAt = now + 60000;
        return;
    }
    if (!g_probeStaging) {
        g_probeTexW = width < 8 ? width : 8;
        ID3D11Device* dev = nullptr;
        r->GetDevice(&dev);
        if (dev) {
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = g_probeTexW; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D* t = nullptr;
                dev->CreateTexture2D(&sd, nullptr, &t);
                g_probeStaging = t;
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
                D3D11_TEXTURE1D_DESC sd{};
                sd.Width = g_probeTexW; sd.MipLevels = 1; sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture1D* t = nullptr;
                dev->CreateTexture1D(&sd, nullptr, &t);
                g_probeStaging = t;
            } else {
                D3D11_BUFFER_DESC sd{};
                sd.ByteWidth = g_probeTexW * 16;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Buffer* t = nullptr;
                dev->CreateBuffer(&sd, nullptr, &t);
                g_probeStaging = t;
            }
            dev->Release();
        }
        if (!g_probeStaging) {
            Log("STEREO: could not read geo-11's separation (no staging copy); using the default shift");
            r->Release();
            g_probeAt = now + 60000;
            return;
        }
    }
    // First row (or first bytes of the buffer) only.
    const D3D11_BOX box = dim == D3D11_RESOURCE_DIMENSION_BUFFER ? D3D11_BOX{0, 0, 0, g_probeTexW * 16, 1, 1}
                                                                : D3D11_BOX{0, 0, 0, g_probeTexW, 1, 1};
    c->CopySubresourceRegion(g_probeStaging, 0, 0, 0, 0, r, 0, &box);
    r->Release();
    g_probePending = true;
    g_probeAt = now + 50;  // read it back a few frames later
}

// Detours, one template per call: D = false for the immediate context's vtable, true for DS3's deferred
// contexts (it builds parts of the frame on worker threads). A function both vtables share has only the
// immediate hook, which then sees both kinds. Recording takes the lock (deferred contexts run on other
// threads) and tags deferred lines [dN]; the HUD and stereo probe only ever act on the immediate context.
template <bool D>
void STDMETHODCALLTYPE DispatchT(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (Recording(c)) {
        REC_BEGIN(c);
        ID3D11ComputeShader* cs = nullptr;
        c->CSGetShader(&cs, nullptr, nullptr);
        std::string u;
        ID3D11UnorderedAccessView* uav[8] = {};
        c->CSGetUnorderedAccessViews(0, 8, uav);
        for (int i = 0; i < 8; ++i)
            if (uav[i]) {
                u += " " + std::to_string(i) + ":" + Uav(uav[i]);
                uav[i]->Release();
            }
        const std::string t = BoundSrvs([c](ID3D11ShaderResourceView** v) { c->CSGetShaderResources(0, 16, v); });
        Add("%u Dispatch %ux%ux%u cs%d t:%s u:%s\n", ++g_seq, x, y, z, ShaderId(cs), t.c_str(), u.empty() ? " -" : u.c_str());
        if (cs) cs->Release();
    }
    (D ? d_Dispatch : o_Dispatch)(c, x, y, z);
}

template <bool D>
void STDMETHODCALLTYPE DrawIndexedT(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u DrawIndexed %u start %u base %d", ++g_seq, n, start, base);
        DrawState(c);
    }
    if (!D && c == g_ctx && !DlaaBusy()) {
        StereoProbe(c);
        if (!HudBeforeDraw(c, n, true)) return;
    }
    (D ? d_DrawIndexed : o_DrawIndexed)(c, n, start, base);
}
template <bool D>
void STDMETHODCALLTYPE DrawT(ID3D11DeviceContext* c, UINT n, UINT start) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u Draw %u start %u", ++g_seq, n, start);
        DrawState(c);
    }
    if (!D && c == g_ctx && !DlaaBusy()) {
        DlaaBeforeDraw(c);
        StereoProbe(c);
        if (!HudBeforeDraw(c, n, false)) return;
    }
    (D ? d_Draw : o_Draw)(c, n, start);
}
template <bool D>
void STDMETHODCALLTYPE DrawIndexedInstancedT(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base, UINT si) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u DrawIndexedInstanced %u x%u start %u base %d si %u", ++g_seq, n, inst, start, base, si);
        DrawState(c);
    }
    if (!D && c == g_ctx && !DlaaBusy()) {
        StereoProbe(c);
        if (!HudBeforeDraw(c, n, true)) return;
    }
    (D ? d_DrawIndexedInstanced : o_DrawIndexedInstanced)(c, n, inst, start, base, si);
}
template <bool D>
void STDMETHODCALLTYPE DrawInstancedT(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT si) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u DrawInstanced %u x%u start %u si %u", ++g_seq, n, inst, start, si);
        DrawState(c);
    }
    if (!D && c == g_ctx && !DlaaBusy()) {
        StereoProbe(c);
        if (!HudBeforeDraw(c, n, false)) return;
    }
    (D ? d_DrawInstanced : o_DrawInstanced)(c, n, inst, start, si);
}
template <bool D>
void STDMETHODCALLTYPE OMSetRTT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (Recording(c)) {
        REC_BEGIN(c);
        std::string s;
        for (UINT i = 0; i < n && rtv; ++i) s += " " + Rtv(rtv[i]);
        Add("%u SetRT n%u%s ds:%s\n", ++g_seq, n, s.c_str(), Dsv(dsv).c_str());
    }
    (D ? d_OMSetRT : o_OMSetRT)(c, n, rtv, dsv);
    if (!DlaaBusy() || c != g_ctx) DlaaOnSetRenderTargets(n, rtv, dsv);
    if (!D && c == g_ctx && !DlaaBusy()) HudOnSetRenderTargets(c, n, rtv, dsv);
}
template <bool D>
void STDMETHODCALLTYPE OMSetRTUAVT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                                   UINT us, UINT un, ID3D11UnorderedAccessView* const* uav, const UINT* cnt) {
    if (Recording(c)) {
        REC_BEGIN(c);
        std::string s;
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            for (UINT i = 0; i < n && rtv; ++i) s += " " + Rtv(rtv[i]);
        std::string u;
        for (UINT i = 0; i < un && uav && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS; ++i) u += " " + Uav(uav[i]);
        Add("%u SetRTUAV n%u%s ds:%s uav %u+%u%s\n", ++g_seq, n, s.c_str(),
            n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? Dsv(dsv).c_str() : "keep", us, un, u.c_str());
    }
    (D ? d_OMSetRTUAV : o_OMSetRTUAV)(c, n, rtv, dsv, us, un, uav, cnt);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && (!DlaaBusy() || c != g_ctx)) DlaaOnSetRenderTargets(n, rtv, dsv);
    if (!D && c == g_ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && !DlaaBusy()) HudOnSetRenderTargets(c, n, rtv, dsv);
}
template <bool D>
void STDMETHODCALLTYPE CopySubT(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                UINT ss, const D3D11_BOX* box) {
    if (Recording(c)) {
        REC_BEGIN(c);
        const std::string d = Res(dst), s = Res(src);
        Add("%u CopySub %s[%u] @%u,%u <- %s[%u]", ++g_seq, d.c_str(), ds, x, y, s.c_str(), ss);
        if (box) Add(" box %u,%u-%u,%u", box->left, box->top, box->right, box->bottom);
        Add("\n");
    }
    (D ? d_CopySub : o_CopySub)(c, dst, ds, x, y, z, src, ss, box);
}
template <bool D>
void STDMETHODCALLTYPE CopyResT(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (Recording(c)) {
        REC_BEGIN(c);
        const std::string d = Res(dst), s = Res(src);
        Add("%u CopyResource %s <- %s\n", ++g_seq, d.c_str(), s.c_str());
    }
    (D ? d_CopyRes : o_CopyRes)(c, dst, src);
}
template <bool D>
void STDMETHODCALLTYPE ClearRTVT(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* col) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u ClearRT %s (%.2f %.2f %.2f %.2f)\n", ++g_seq, Rtv(v).c_str(), col ? col[0] : 0.f, col ? col[1] : 0.f, col ? col[2] : 0.f,
            col ? col[3] : 0.f);
    }
    (D ? d_ClearRTV : o_ClearRTV)(c, v, col);
}
template <bool D>
void STDMETHODCALLTYPE ClearDSVT(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 st) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u ClearDS %s flags %u depth %.2f st %u\n", ++g_seq, Dsv(v).c_str(), flags, depth, st);
    }
    (D ? d_ClearDSV : o_ClearDSV)(c, v, flags, depth, st);
}
template <bool D>
void STDMETHODCALLTYPE ResolveT(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (Recording(c)) {
        REC_BEGIN(c);
        const std::string d = Res(dst), s = Res(src);
        Add("%u Resolve %s[%u] <- %s[%u] f%d\n", ++g_seq, d.c_str(), ds, s.c_str(), ss, static_cast<int>(f));
    }
    (D ? d_Resolve : o_Resolve)(c, dst, ds, src, ss, f);
}
// Where command lists come from (a deferred context finishing one) and where they run (the immediate context).
void STDMETHODCALLTYPE ExecuteCLDetour(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
    if (Recording(c)) {
        REC_BEGIN(c);
        Add("%u ExecuteCommandList L%d\n", ++g_seq, ListId(list));
    }
    o_ExecuteCL(c, list, restore);
}
HRESULT STDMETHODCALLTYPE FinishCLDetour(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out) {
    const HRESULT hr = d_FinishCL(c, restore, out);
    if (Recording(c) && SUCCEEDED(hr) && out && *out) {
        REC_BEGIN(c);
        Add("%u FinishCommandList -> L%d\n", ++g_seq, ListId(*out));
    }
    return hr;
}

bool Hook(void** vt, int slot, void* detour, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(vt[slot], detour, orig);
    if (s == MH_OK) s = MH_EnableHook(vt[slot]);
    if (s != MH_OK) Log("drawlog: could not hook %s (%s)", name, MH_StatusToString(s));
    return s == MH_OK;
}

void Install(ID3D11Device* dev) {
    g_installed = true;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        Log("drawlog: no immediate context");
        return;
    }
    g_ctx = ctx;
    ctx->Release();  // the device keeps it alive
    void** vt = *reinterpret_cast<void***>(ctx);
    int ok = 0;
    ok += Hook(vt, kDrawIndexed, reinterpret_cast<void*>(&DrawIndexedT<false>), reinterpret_cast<void**>(&o_DrawIndexed), "DrawIndexed");
    ok += Hook(vt, kDraw, reinterpret_cast<void*>(&DrawT<false>), reinterpret_cast<void**>(&o_Draw), "Draw");
    ok += Hook(vt, kDrawIndexedInstanced, reinterpret_cast<void*>(&DrawIndexedInstancedT<false>),
               reinterpret_cast<void**>(&o_DrawIndexedInstanced), "DrawIndexedInstanced");
    ok += Hook(vt, kDrawInstanced, reinterpret_cast<void*>(&DrawInstancedT<false>), reinterpret_cast<void**>(&o_DrawInstanced),
               "DrawInstanced");
    ok += Hook(vt, kOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRTT<false>), reinterpret_cast<void**>(&o_OMSetRT),
               "OMSetRenderTargets");
    ok += Hook(vt, kOMSetRTAndUAV, reinterpret_cast<void*>(&OMSetRTUAVT<false>), reinterpret_cast<void**>(&o_OMSetRTUAV),
               "OMSetRenderTargetsAndUnorderedAccessViews");
    ok += Hook(vt, kCopySubresourceRegion, reinterpret_cast<void*>(&CopySubT<false>), reinterpret_cast<void**>(&o_CopySub),
               "CopySubresourceRegion");
    ok += Hook(vt, kCopyResource, reinterpret_cast<void*>(&CopyResT<false>), reinterpret_cast<void**>(&o_CopyRes), "CopyResource");
    ok += Hook(vt, kClearRTV, reinterpret_cast<void*>(&ClearRTVT<false>), reinterpret_cast<void**>(&o_ClearRTV), "ClearRenderTargetView");
    ok += Hook(vt, kClearDSV, reinterpret_cast<void*>(&ClearDSVT<false>), reinterpret_cast<void**>(&o_ClearDSV), "ClearDepthStencilView");
    ok += Hook(vt, kResolve, reinterpret_cast<void*>(&ResolveT<false>), reinterpret_cast<void**>(&o_Resolve), "ResolveSubresource");
    ok += Hook(vt, kDispatch, reinterpret_cast<void*>(&DispatchT<false>), reinterpret_cast<void**>(&o_Dispatch), "Dispatch");
    ok += Hook(vt, kExecuteCommandList, reinterpret_cast<void*>(&ExecuteCLDetour), reinterpret_cast<void**>(&o_ExecuteCL),
               "ExecuteCommandList");
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(vt[kDraw]), &m) && m)
        GetModuleFileNameW(m, mod, MAX_PATH);
    Log("context hooks: %d of 13 on %p (Draw lives in %ls). F2 records one frame to ds3vr_draws_N.txt", ok,
        static_cast<void*>(ctx), mod);

    // DS3's deferred contexts (it records parts of the frame on worker threads): every deferred context of
    // a device shares one vtable, so a throwaway one shows where those functions are. A function the
    // immediate context also uses is already hooked above, and that hook records any context.
    ID3D11DeviceContext* dc = nullptr;
    if (FAILED(dev->CreateDeferredContext(0, &dc)) || !dc) {
        Log("drawlog: could not create a deferred context; deferred draws are not recorded");
        return;
    }
    void** vd = *reinterpret_cast<void***>(dc);
    int own = 0, shared = 0;
    auto hookD = [&](int slot, void* detour, void** orig, const char* name) {
        if (vd[slot] == vt[slot]) {
            ++shared;
            return;
        }
        own += Hook(vd, slot, detour, orig, name);
    };
    hookD(kDrawIndexed, reinterpret_cast<void*>(&DrawIndexedT<true>), reinterpret_cast<void**>(&d_DrawIndexed), "deferred DrawIndexed");
    hookD(kDraw, reinterpret_cast<void*>(&DrawT<true>), reinterpret_cast<void**>(&d_Draw), "deferred Draw");
    hookD(kDrawIndexedInstanced, reinterpret_cast<void*>(&DrawIndexedInstancedT<true>), reinterpret_cast<void**>(&d_DrawIndexedInstanced),
          "deferred DrawIndexedInstanced");
    hookD(kDrawInstanced, reinterpret_cast<void*>(&DrawInstancedT<true>), reinterpret_cast<void**>(&d_DrawInstanced), "deferred DrawInstanced");
    hookD(kOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRTT<true>), reinterpret_cast<void**>(&d_OMSetRT), "deferred OMSetRenderTargets");
    hookD(kOMSetRTAndUAV, reinterpret_cast<void*>(&OMSetRTUAVT<true>), reinterpret_cast<void**>(&d_OMSetRTUAV),
          "deferred OMSetRenderTargetsAndUnorderedAccessViews");
    hookD(kDispatch, reinterpret_cast<void*>(&DispatchT<true>), reinterpret_cast<void**>(&d_Dispatch), "deferred Dispatch");
    hookD(kCopySubresourceRegion, reinterpret_cast<void*>(&CopySubT<true>), reinterpret_cast<void**>(&d_CopySub), "deferred CopySubresourceRegion");
    hookD(kCopyResource, reinterpret_cast<void*>(&CopyResT<true>), reinterpret_cast<void**>(&d_CopyRes), "deferred CopyResource");
    hookD(kClearRTV, reinterpret_cast<void*>(&ClearRTVT<true>), reinterpret_cast<void**>(&d_ClearRTV), "deferred ClearRenderTargetView");
    hookD(kClearDSV, reinterpret_cast<void*>(&ClearDSVT<true>), reinterpret_cast<void**>(&d_ClearDSV), "deferred ClearDepthStencilView");
    hookD(kResolve, reinterpret_cast<void*>(&ResolveT<true>), reinterpret_cast<void**>(&d_Resolve), "deferred ResolveSubresource");
    own += Hook(vd, kFinishCommandList, reinterpret_cast<void*>(&FinishCLDetour), reinterpret_cast<void**>(&d_FinishCL),
                "deferred FinishCommandList");
    dc->Release();
    Log("context hooks: deferred contexts: %d hooked, %d shared with the immediate context", own, shared);
}

void WriteFile() {
    ++g_fileNo;
    wchar_t name[64];
    swprintf(name, 64, L"ds3vr_draws_%d.txt", g_fileNo);
    const std::wstring path = std::wstring(ModuleDir()) + name;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        fwrite(g_buf.data(), 1, g_buf.size(), f);
        fclose(f);
    }
    Log("drawlog: %u events, %d resources, %d shaders -> %ls%s", g_seq, g_nextRes, g_nextShader, name, f ? "" : " (could not write)");
}

} // namespace

void DrawLogRequest() { g_request = true; }

bool StereoParamsGet(float* sep, float* conv) {
    *sep = g_stereoSep.load();
    *conv = g_stereoConv.load();
    return *sep > 0.f;
}

void CtxSetRenderTargetsRaw(ID3D11DeviceContext* c, unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (o_OMSetRT) o_OMSetRT(c, n, rtv, dsv);
}

void DrawLogOnPresent(ID3D11Device* dev) {
    if (!g_installed && dev) Install(dev);
    if (g_state != 2 && !g_request.load(std::memory_order_relaxed)) return;  // the usual case: nothing to do, no lock
    std::lock_guard<std::recursive_mutex> lock(g_recMutex);  // worker threads may be recording deferred draws
    if (g_state == 2) {
        Add("=== Present ===\n");
        g_state = 0;
        WriteFile();
    }
    if (g_state == 0 && g_request.exchange(false) && g_ctx) {
        g_buf.clear();
        g_buf.reserve(16 << 20);
        g_resIds.clear();
        g_shaderIds.clear();
        g_ctxIds.clear();
        g_listIds.clear();
        g_nextRes = g_nextShader = 0;
        g_seq = 0;
        g_buf += "=== frame start (after Present) ===\n";
        g_state = 2;
    }
}

} // namespace ds3vr
