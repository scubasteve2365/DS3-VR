#include "drawlog.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "MinHook.h"
#include "hud.h"
#include "log.h"

namespace ds3vr {
namespace {

// ID3D11DeviceContext vtable slots
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21, kOMSetRenderTargets = 33,
              kOMSetRTAndUAV = 34, kCopySubresourceRegion = 46, kCopyResource = 47, kClearRTV = 50, kClearDSV = 53,
              kResolve = 57;

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

bool Recording(ID3D11DeviceContext* c) { return g_state.load(std::memory_order_relaxed) == 2 && c == g_ctx; }

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
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        snprintf(b, sizeof(b), "R%d=[%ux%u f%d a%u m%u s%u b0x%x misc0x%x]", id, d.Width, d.Height, static_cast<int>(d.Format),
                 d.ArraySize, d.MipLevels, d.SampleDesc.Count, d.BindFlags, d.MiscFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
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
    return s;
}

std::string Srv(ID3D11ShaderResourceView* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = Res(r);
    if (r) r->Release();
    return s;
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

    ID3D11ShaderResourceView* srv[4] = {};
    c->PSGetShaderResources(0, 4, srv);
    s += " t:";
    for (int i = 0; i < 4; ++i) {
        s += (i ? "," : "") + Srv(srv[i]);
        if (srv[i]) srv[i]->Release();
    }

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

void STDMETHODCALLTYPE DrawIndexedDetour(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    if (Recording(c)) {
        Add("%u DrawIndexed %u start %u base %d", ++g_seq, n, start, base);
        DrawState(c);
    }
    if (c == g_ctx && !HudBeforeDraw(c, n, true))
        return;
    o_DrawIndexed(c, n, start, base);
}
void STDMETHODCALLTYPE DrawDetour(ID3D11DeviceContext* c, UINT n, UINT start) {
    if (Recording(c)) {
        Add("%u Draw %u start %u", ++g_seq, n, start);
        DrawState(c);
    }
    if (c == g_ctx && !HudBeforeDraw(c, n, false))
        return;
    o_Draw(c, n, start);
}
void STDMETHODCALLTYPE DrawIndexedInstancedDetour(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base, UINT si) {
    if (Recording(c)) {
        Add("%u DrawIndexedInstanced %u x%u start %u base %d si %u", ++g_seq, n, inst, start, base, si);
        DrawState(c);
    }
    if (c == g_ctx && !HudBeforeDraw(c, n, true))
        return;
    o_DrawIndexedInstanced(c, n, inst, start, base, si);
}
void STDMETHODCALLTYPE DrawInstancedDetour(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT si) {
    if (Recording(c)) {
        Add("%u DrawInstanced %u x%u start %u si %u", ++g_seq, n, inst, start, si);
        DrawState(c);
    }
    if (c == g_ctx && !HudBeforeDraw(c, n, false))
        return;
    o_DrawInstanced(c, n, inst, start, si);
}
void STDMETHODCALLTYPE OMSetRTDetour(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (Recording(c)) {
        std::string s;
        for (UINT i = 0; i < n && rtv; ++i) s += " " + Rtv(rtv[i]);
        Add("%u SetRT n%u%s ds:%s\n", ++g_seq, n, s.c_str(), Dsv(dsv).c_str());
    }
    o_OMSetRT(c, n, rtv, dsv);
    if (c == g_ctx) HudOnSetRenderTargets(c, n, rtv, dsv);
}
void STDMETHODCALLTYPE OMSetRTUAVDetour(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                                        UINT us, UINT un, ID3D11UnorderedAccessView* const* uav, const UINT* cnt) {
    if (Recording(c)) {
        std::string s;
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            for (UINT i = 0; i < n && rtv; ++i) s += " " + Rtv(rtv[i]);
        Add("%u SetRTUAV n%u%s ds:%s uav %u+%u\n", ++g_seq, n, s.c_str(),
            n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? Dsv(dsv).c_str() : "keep", us, un);
    }
    o_OMSetRTUAV(c, n, rtv, dsv, us, un, uav, cnt);
    if (c == g_ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) HudOnSetRenderTargets(c, n, rtv, dsv);
}
void STDMETHODCALLTYPE CopySubDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                     UINT ss, const D3D11_BOX* box) {
    if (Recording(c)) {
        const std::string d = Res(dst), s = Res(src);
        Add("%u CopySub %s[%u] @%u,%u <- %s[%u]", ++g_seq, d.c_str(), ds, x, y, s.c_str(), ss);
        if (box) Add(" box %u,%u-%u,%u", box->left, box->top, box->right, box->bottom);
        Add("\n");
    }
    o_CopySub(c, dst, ds, x, y, z, src, ss, box);
}
void STDMETHODCALLTYPE CopyResDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (Recording(c)) {
        const std::string d = Res(dst), s = Res(src);
        Add("%u CopyResource %s <- %s\n", ++g_seq, d.c_str(), s.c_str());
    }
    o_CopyRes(c, dst, src);
}
void STDMETHODCALLTYPE ClearRTVDetour(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* col) {
    if (Recording(c))
        Add("%u ClearRT %s (%.2f %.2f %.2f %.2f)\n", ++g_seq, Rtv(v).c_str(), col ? col[0] : 0.f, col ? col[1] : 0.f, col ? col[2] : 0.f,
            col ? col[3] : 0.f);
    o_ClearRTV(c, v, col);
}
void STDMETHODCALLTYPE ClearDSVDetour(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 st) {
    if (Recording(c)) Add("%u ClearDS %s flags %u depth %.2f st %u\n", ++g_seq, Dsv(v).c_str(), flags, depth, st);
    o_ClearDSV(c, v, flags, depth, st);
}
void STDMETHODCALLTYPE ResolveDetour(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (Recording(c)) {
        const std::string d = Res(dst), s = Res(src);
        Add("%u Resolve %s[%u] <- %s[%u] f%d\n", ++g_seq, d.c_str(), ds, s.c_str(), ss, static_cast<int>(f));
    }
    o_Resolve(c, dst, ds, src, ss, f);
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
    ok += Hook(vt, kDrawIndexed, reinterpret_cast<void*>(&DrawIndexedDetour), reinterpret_cast<void**>(&o_DrawIndexed), "DrawIndexed");
    ok += Hook(vt, kDraw, reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&o_Draw), "Draw");
    ok += Hook(vt, kDrawIndexedInstanced, reinterpret_cast<void*>(&DrawIndexedInstancedDetour),
               reinterpret_cast<void**>(&o_DrawIndexedInstanced), "DrawIndexedInstanced");
    ok += Hook(vt, kDrawInstanced, reinterpret_cast<void*>(&DrawInstancedDetour), reinterpret_cast<void**>(&o_DrawInstanced),
               "DrawInstanced");
    ok += Hook(vt, kOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRTDetour), reinterpret_cast<void**>(&o_OMSetRT),
               "OMSetRenderTargets");
    ok += Hook(vt, kOMSetRTAndUAV, reinterpret_cast<void*>(&OMSetRTUAVDetour), reinterpret_cast<void**>(&o_OMSetRTUAV),
               "OMSetRenderTargetsAndUnorderedAccessViews");
    ok += Hook(vt, kCopySubresourceRegion, reinterpret_cast<void*>(&CopySubDetour), reinterpret_cast<void**>(&o_CopySub),
               "CopySubresourceRegion");
    ok += Hook(vt, kCopyResource, reinterpret_cast<void*>(&CopyResDetour), reinterpret_cast<void**>(&o_CopyRes), "CopyResource");
    ok += Hook(vt, kClearRTV, reinterpret_cast<void*>(&ClearRTVDetour), reinterpret_cast<void**>(&o_ClearRTV), "ClearRenderTargetView");
    ok += Hook(vt, kClearDSV, reinterpret_cast<void*>(&ClearDSVDetour), reinterpret_cast<void**>(&o_ClearDSV), "ClearDepthStencilView");
    ok += Hook(vt, kResolve, reinterpret_cast<void*>(&ResolveDetour), reinterpret_cast<void**>(&o_Resolve), "ResolveSubresource");
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(vt[kDraw]), &m) && m)
        GetModuleFileNameW(m, mod, MAX_PATH);
    Log("context hooks: %d of 11 on %p (Draw lives in %ls). F2 records one frame to ds3vr_draws_N.txt", ok,
        static_cast<void*>(ctx), mod);
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

void CtxSetRenderTargetsRaw(ID3D11DeviceContext* c, unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (o_OMSetRT) o_OMSetRT(c, n, rtv, dsv);
}

void DrawLogOnPresent(ID3D11Device* dev) {
    if (!g_installed && dev) Install(dev);
    if (g_state == 2) {
        Add("=== Present ===\n");
        g_state = 0;
        WriteFile();
    }
    if (g_state == 0 && g_request.exchange(false) && g_ctx) {
        g_buf.clear();
        g_buf.reserve(4 << 20);
        g_resIds.clear();
        g_shaderIds.clear();
        g_nextRes = g_nextShader = 0;
        g_seq = 0;
        g_buf += "=== frame start (after Present) ===\n";
        g_state = 2;
    }
}

} // namespace ds3vr
