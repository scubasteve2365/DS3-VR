#include "dlss.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "drawlog.h"
#include "log.h"
#include "ngx_api.h"

namespace ds3vr {
namespace {

// ---------- the NGX helper ----------
struct NgxApi {
    HMODULE dll = nullptr;
    PFN_Ds3vrNgx_ApiVersion ApiVersion = nullptr;
    PFN_Ds3vrNgx_Init Init = nullptr;
    PFN_Ds3vrNgx_DlssAvailable DlssAvailable = nullptr;
    PFN_Ds3vrNgx_GetOptimal GetOptimal = nullptr;
    PFN_Ds3vrNgx_CreateDlss CreateDlss = nullptr;
    PFN_Ds3vrNgx_EvaluateDlss EvaluateDlss = nullptr;
    PFN_Ds3vrNgx_ReleaseDlss ReleaseDlss = nullptr;
    PFN_Ds3vrNgx_Shutdown Shutdown = nullptr;
    PFN_Ds3vrNgx_ResultString ResultString = nullptr;
};
NgxApi g_ngx;

std::wstring Dir() { return std::wstring(ModuleDir()); }  // the Game folder, with a trailing backslash
std::wstring Ini() { return Dir() + L"ds3vr.ini"; }

template <class T>
bool Get(T& fn, const char* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(g_ngx.dll, name)));
    if (!fn) Log("DLSS: ds3vr_ngx.dll has no %s", name);
    return fn != nullptr;
}

// Loads ds3vr_ngx.dll from the ds3vr folder (release layout) or the Game folder.
bool LoadHelper() {
    if (g_ngx.dll) return true;
    for (const std::wstring& p : {Dir() + L"ds3vr\\ds3vr_ngx.dll", Dir() + L"ds3vr_ngx.dll"}) {
        g_ngx.dll = LoadLibraryW(p.c_str());
        if (g_ngx.dll) {
            Log("DLSS: loaded %ls", p.c_str());
            break;
        }
    }
    if (!g_ngx.dll) {
        Log("DLSS: ds3vr_ngx.dll not found in the ds3vr or Game folder (error %lu)", GetLastError());
        return false;
    }
    bool ok = Get(g_ngx.ApiVersion, "Ds3vrNgx_ApiVersion") & Get(g_ngx.Init, "Ds3vrNgx_Init") &
              Get(g_ngx.DlssAvailable, "Ds3vrNgx_DlssAvailable") & Get(g_ngx.GetOptimal, "Ds3vrNgx_GetOptimal") &
              Get(g_ngx.CreateDlss, "Ds3vrNgx_CreateDlss") & Get(g_ngx.EvaluateDlss, "Ds3vrNgx_EvaluateDlss") &
              Get(g_ngx.ReleaseDlss, "Ds3vrNgx_ReleaseDlss") & Get(g_ngx.Shutdown, "Ds3vrNgx_Shutdown") &
              Get(g_ngx.ResultString, "Ds3vrNgx_ResultString");
    if (ok && g_ngx.ApiVersion() != DS3VR_NGX_API_VERSION) {
        Log("DLSS: ds3vr_ngx.dll is interface %d, the mod expects %d", g_ngx.ApiVersion(), DS3VR_NGX_API_VERSION);
        ok = false;
    }
    if (!ok) {
        FreeLibrary(g_ngx.dll);
        g_ngx = NgxApi();
    }
    return ok;
}

// Which module a COM object's methods live in (the real D3D11 runtime, or geo-11's wrapper).
std::wstring VtableModule(void* obj) {
    if (!obj) return L"(none)";
    void* fn = (*reinterpret_cast<void***>(obj))[0];
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"(unknown)";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(fn), &m) && m)
        GetModuleFileNameW(m, path, MAX_PATH);
    return path;
}

// Starts NGX on the game's device and checks DLSS is there. Logs each step.
bool StartNgx(ID3D11Device* dev, bool* available) {
    *available = false;
    if (!LoadHelper()) return false;
    const std::wstring data = Dir() + L"ds3vr";
    const std::wstring dir1 = Dir() + L"ds3vr", dir2 = Dir().substr(0, Dir().size() - 1);
    const wchar_t* dirs[2] = {dir1.c_str(), dir2.c_str()};  // where nvngx_dlss.dll may be: the ds3vr folder, then the Game folder
    char msg[512] = {};
    const int r = g_ngx.Init(dev, data.c_str(), dirs, 2, msg, sizeof(msg));
    Log("DLSS: %s", msg);
    if (r != 1) return false;
    int avail = 0, needsDriver = 0;
    unsigned maj = 0, min = 0;
    g_ngx.DlssAvailable(&avail, &needsDriver, &maj, &min, msg, sizeof(msg));
    Log("DLSS: %s", msg);
    *available = avail != 0;
    return true;
}

void SelfTest(ID3D11Device* dev) {
    Log("DLSS self-test: device %p (methods in %ls)", static_cast<void*>(dev), VtableModule(dev).c_str());
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    Log("DLSS self-test: immediate context %p (methods in %ls)", static_cast<void*>(ctx), VtableModule(ctx).c_str());
    bool avail = false;
    if (!StartNgx(dev, &avail)) {
        if (ctx) ctx->Release();
        return;
    }
    if (avail) {
        const unsigned w = 3308, h = 3264;  // a typical per-eye size, only for the optimal-size report
        const int modes[] = {DS3VR_NGX_DLAA, DS3VR_NGX_QUALITY, DS3VR_NGX_BALANCED, DS3VR_NGX_PERFORMANCE};
        const char* const names[] = {"DLAA", "Quality", "Balanced", "Performance"};
        for (int i = 0; i < 4; ++i) {
            unsigned rw = 0, rh = 0;
            float sharp = 0.f;
            const int o = g_ngx.GetOptimal(w, h, modes[i], &rw, &rh, &sharp);
            Log("DLSS self-test: %s for %ux%u renders at %ux%u (%s)", names[i], w, h, rw, rh, g_ngx.ResultString(o));
        }
        void* feature = nullptr;
        int r = g_ngx.CreateDlss(ctx, 256, 256, 256, 256, DS3VR_NGX_DLAA, DS3VR_NGX_FLAG_MV_LOW_RES | DS3VR_NGX_FLAG_DEPTH_INVERTED, 0,
                                 &feature);
        Log("DLSS self-test: create a 256x256 DLAA feature: %s", g_ngx.ResultString(r));
        if (feature) {
            r = g_ngx.ReleaseDlss(feature);
            Log("DLSS self-test: release it: %s", g_ngx.ResultString(r));
        }
    }
    const int r = g_ngx.Shutdown(dev);
    Log("DLSS self-test: NGX shut down: %s. Self-test finished.", g_ngx.ResultString(r));
    if (ctx) ctx->Release();
}

// ---------- settings ----------
std::atomic<int> g_mode{0};          // 0 off, 1 DLAA, 2 jitter test
std::atomic<bool> g_ngxFailed{false};  // NGX or DLSS could not start: DLAA stays off this session
std::atomic<bool> g_ready{false};      // DLAA features exist and run
float g_jitterSign = 1.f;              // ds3vr.ini DlaaJitterSign (flip if standing still looks soft)
float g_eyeSign = 1.f;
std::atomic<int> g_preset{6};          // ds3vr.ini DlaaPreset: 0 driver default, 6 CNN (preset F, faster), 11 transformer (preset K)
std::atomic<bool> g_presetChanged{false};                 // ds3vr.ini DlaaEyeSign: geo-11's shift sign for slice 0 (+1) or slice 1
constexpr float kTestScale = 8.f;      // jitter test: offsets made 8x larger so the wobble is easy to see

bool Jittering() {
    const int m = g_mode.load(std::memory_order_relaxed);
    return m == 2 || (m == 1 && !g_ngxFailed.load(std::memory_order_relaxed));
}

// ---------- jitter (Halton 2,3; 16 phases, as NVIDIA suggests for DLAA) ----------
float Halton(unsigned i, unsigned b) {
    float f = 1.f, r = 0.f;
    while (i > 0) {
        f /= static_cast<float>(b);
        r += f * static_cast<float>(i % b);
        i /= b;
    }
    return r;
}
// The jitter phase. With DLAA running it advances right after DLAA (every camera upload before that point
// belongs to the frame DLAA just resolved); otherwise at each Present.
std::atomic<unsigned> g_frame{0};
std::atomic<bool> g_doneSincePresent{false};
void JitterFor(unsigned frame, float* x, float* y) {  // pixels, x right, y down, in [-0.5, 0.5)
    const unsigned i = (frame % 16) + 1;
    *x = Halton(i, 2) - 0.5f;
    *y = Halton(i, 3) - 0.5f;
}

// ---------- the scene targets (found as the game binds them) ----------
std::mutex g_sceneMutex;
ID3D11Texture2D* g_scene = nullptr;  // HDR scene colour, 2 slices (kept alive with a reference)
ID3D11Texture2D* g_depth = nullptr;  // main depth, 2 slices
std::atomic<unsigned> g_sceneW{0}, g_sceneH{0};
std::atomic<void*> g_lastRtv{nullptr}, g_lastDsv{nullptr};  // the pair checked last (skip repeats cheaply)

// ---------- camera constants (cbSceneParam, b8) ----------
// Float4 registers: c1 FC_ScreenSize, c3 SC_CameraPos, c16..c19 FC_MatrixProj, c24..c27 VC_MatrixViewProj.
// In DS3's shaders clip.x = dot(position, c16) (and c24), so each register is one row of clip = M * p.
constexpr unsigned kSceneParamMinBytes = 60 * 16;
constexpr int kMaxCb = 64;
std::atomic<void*> g_cbs[kMaxCb];  // buffers seen bound at VS b8
std::atomic<int> g_cbCount{0};
std::atomic<unsigned> g_cbWidth{0};

bool IsSceneCb(void* r) {
    if (!r) return false;
    const int n = g_cbCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_cbs[i].load(std::memory_order_relaxed) == r) return true;
    return false;
}
void AddSceneCb(ID3D11Buffer* b) {
    if (IsSceneCb(b)) return;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    if (d.ByteWidth < kSceneParamMinBytes || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (IsSceneCb(b)) return;
    const int n = g_cbCount.load();
    if (n >= kMaxCb) return;
    g_cbs[n].store(b);
    g_cbCount.store(n + 1, std::memory_order_release);
    g_cbWidth.store(d.ByteWidth);
    if (n < 4 || n == kMaxCb - 1) Log("DLAA: camera constants buffer %d found (%u bytes)%s", n + 1, d.ByteWidth, n == kMaxCb - 1 ? ", no more are tracked" : "");
}

struct Camera {
    float vp[16] = {};    // VC_MatrixViewProj rows (unjittered)
    float cam[3] = {};    // SC_CameraPos
    float jx = 0.f, jy = 0.f;  // jitter applied with it (pixels)
    unsigned frame = 0;
    bool valid = false;
};
std::mutex g_camMutex;
struct Candidate {
    Camera cam;
    unsigned uploads = 0;
};
constexpr int kMaxCand = 8;
Candidate g_cand[kMaxCand];  // this frame's distinct full-size cameras
int g_candCount = 0;
unsigned g_candFrame = ~0u;
Camera g_camLatest;          // the one DLAA used last (when a frame has no upload of its own)
std::atomic<unsigned> g_statMain{0}, g_statHalf{0}, g_statOther{0}, g_statOtherView{0}, g_statStale{0};

// The game is writing camera constants into memory p (about to be uploaded): note the main camera, jitter.
void OnCameraUpload(float* p) {
    const float sw = p[4], sh = p[5];
    const unsigned W = g_sceneW.load(std::memory_order_relaxed), H = g_sceneH.load(std::memory_order_relaxed);
    if (!W || !H || !(sw > 0.f) || !(sh > 0.f)) return;
    const bool main = std::fabs(sw - W) < 1.5f && std::fabs(sh - H) < 1.5f;
    const bool half = std::fabs(sw - W / 2.f) < 1.5f && std::fabs(sh - H / 2.f) < 1.5f;
    if (!main && !half) {
        g_statOther.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const unsigned frame = g_frame.load(std::memory_order_relaxed);
    float jx = 0.f, jy = 0.f;
    const bool jit = Jittering();
    if (jit) {
        JitterFor(frame, &jx, &jy);
        if (g_mode.load(std::memory_order_relaxed) == 2) {
            jx *= kTestScale;
            jy *= kTestScale;
        }
    }
    if (main) {
        g_statMain.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_camMutex);
        // Several full-size cameras can be uploaded in one frame (about 1 in 5 uploads is another view).
        // Count each distinct one; DLAA uses the one the frame used most (the main scene camera).
        if (g_candFrame != frame) {
            g_candFrame = frame;
            g_candCount = 0;
        }
        int hit = -1;
        for (int i = 0; i < g_candCount && hit < 0; ++i) {
            float diff = 0.f;
            for (int k = 0; k < 16; ++k) diff = std::fmax(diff, std::fabs(g_cand[i].cam.vp[k] - p[24 * 4 + k]));
            if (diff <= 1e-4f) hit = i;
        }
        if (hit < 0 && g_candCount < kMaxCand) {
            hit = g_candCount++;
            Camera& c = g_cand[hit].cam;
            memcpy(c.vp, p + 24 * 4, sizeof(c.vp));
            memcpy(c.cam, p + 3 * 4, sizeof(c.cam));
            c.jx = jx;
            c.jy = jy;
            c.frame = frame;
            c.valid = true;
            g_cand[hit].uploads = 0;
        }
        if (hit > 0) g_statOtherView.fetch_add(1, std::memory_order_relaxed);
        if (hit >= 0) ++g_cand[hit].uploads;
    } else {
        g_statHalf.fetch_add(1, std::memory_order_relaxed);
    }
    if (!jit) return;
    // The same offset in normalised device units for the full- and half-size cameras (same picture).
    const float dx = 2.f * jx / static_cast<float>(W), dy = -2.f * jy / static_cast<float>(H);
    for (int base : {16, 24}) {
        float* r0 = p + base * 4;
        float* r1 = r0 + 4;
        const float* r3 = r0 + 12;
        for (int k = 0; k < 4; ++k) {
            r0[k] += dx * r3[k];
            r1[k] += dy * r3[k];
        }
    }
}

// ---------- context hooks: VSSetConstantBuffers, Map / Unmap, UpdateSubresource(1) ----------
constexpr int kVSSetCB = 7, kMap = 14, kUnmap = 15, kUpdateSub = 48, kUpdateSub1 = 116;
using VSSetCBFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using UpdateSub1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
VSSetCBFn o_VSSetCB = nullptr, d_VSSetCB = nullptr;
MapFn o_Map = nullptr, d_Map = nullptr;
UnmapFn o_Unmap = nullptr, d_Unmap = nullptr;
UpdateSubFn o_UpdateSub = nullptr, d_UpdateSub = nullptr;
UpdateSub1Fn o_UpdateSub1 = nullptr, d_UpdateSub1 = nullptr;

struct PendingMap {
    ID3D11DeviceContext* c;
    ID3D11Resource* r;
    float* p;
};
thread_local PendingMap t_map[4];
thread_local int t_mapCount = 0;

template <bool D>
void STDMETHODCALLTYPE VSSetCBT(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
    if (b && start <= 8 && start + n > 8 && b[8 - start]) AddSceneCb(b[8 - start]);
    (D ? d_VSSetCB : o_VSSetCB)(c, start, n, b);
}
template <bool D>
HRESULT STDMETHODCALLTYPE MapT(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = (D ? d_Map : o_Map)(c, r, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && out->pData && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE) && IsSceneCb(r) &&
        t_mapCount < 4)
        t_map[t_mapCount++] = PendingMap{c, r, static_cast<float*>(out->pData)};
    return hr;
}
template <bool D>
void STDMETHODCALLTYPE UnmapT(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    for (int i = 0; i < t_mapCount; ++i)
        if (t_map[i].c == c && t_map[i].r == r) {
            OnCameraUpload(t_map[i].p);
            t_map[i] = t_map[--t_mapCount];
            break;
        }
    (D ? d_Unmap : o_Unmap)(c, r, sub);
}
// A whole-buffer UpdateSubresource of a camera buffer: change a copy of the game's data.
const void* CameraCopy(ID3D11Resource* r, const D3D11_BOX* box, const void* src) {
    if (box || !src || !IsSceneCb(r)) return src;
    const unsigned w = g_cbWidth.load();
    if (w < kSceneParamMinBytes) return src;
    thread_local std::vector<float> copy;
    copy.resize(w / 4);
    memcpy(copy.data(), src, w);
    OnCameraUpload(copy.data());
    return copy.data();
}
template <bool D>
void STDMETHODCALLTYPE UpdateSubT(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    (D ? d_UpdateSub : o_UpdateSub)(c, r, sub, box, CameraCopy(r, box, src), rp, dp);
}
template <bool D>
void STDMETHODCALLTYPE UpdateSub1T(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp,
                                   UINT fl) {
    (D ? d_UpdateSub1 : o_UpdateSub1)(c, r, sub, box, CameraCopy(r, box, src), rp, dp, fl);
}

bool Hook(void** vt, int slot, void* detour, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(vt[slot], detour, orig);
    if (s == MH_OK) s = MH_EnableHook(vt[slot]);
    if (s != MH_OK) Log("DLAA: could not hook %s (%s)", name, MH_StatusToString(s));
    return s == MH_OK;
}

void InstallHooks(ID3D11Device* dev) {
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    void** vt = *reinterpret_cast<void***>(ctx);
    ctx->Release();
    int ok = 0;
    ok += Hook(vt, kVSSetCB, reinterpret_cast<void*>(&VSSetCBT<false>), reinterpret_cast<void**>(&o_VSSetCB), "VSSetConstantBuffers");
    ok += Hook(vt, kMap, reinterpret_cast<void*>(&MapT<false>), reinterpret_cast<void**>(&o_Map), "Map");
    ok += Hook(vt, kUnmap, reinterpret_cast<void*>(&UnmapT<false>), reinterpret_cast<void**>(&o_Unmap), "Unmap");
    ok += Hook(vt, kUpdateSub, reinterpret_cast<void*>(&UpdateSubT<false>), reinterpret_cast<void**>(&o_UpdateSub), "UpdateSubresource");
    ok += Hook(vt, kUpdateSub1, reinterpret_cast<void*>(&UpdateSub1T<false>), reinterpret_cast<void**>(&o_UpdateSub1), "UpdateSubresource1");
    int own = 0, shared = 0;
    ID3D11DeviceContext* dc = nullptr;
    if (SUCCEEDED(dev->CreateDeferredContext(0, &dc)) && dc) {
        void** vd = *reinterpret_cast<void***>(dc);
        auto hookD = [&](int slot, void* detour, void** orig, const char* name) {
            if (vd[slot] == vt[slot]) {
                ++shared;
                return;
            }
            own += Hook(vd, slot, detour, orig, name);
        };
        hookD(kVSSetCB, reinterpret_cast<void*>(&VSSetCBT<true>), reinterpret_cast<void**>(&d_VSSetCB), "deferred VSSetConstantBuffers");
        hookD(kMap, reinterpret_cast<void*>(&MapT<true>), reinterpret_cast<void**>(&d_Map), "deferred Map");
        hookD(kUnmap, reinterpret_cast<void*>(&UnmapT<true>), reinterpret_cast<void**>(&d_Unmap), "deferred Unmap");
        hookD(kUpdateSub, reinterpret_cast<void*>(&UpdateSubT<true>), reinterpret_cast<void**>(&d_UpdateSub), "deferred UpdateSubresource");
        hookD(kUpdateSub1, reinterpret_cast<void*>(&UpdateSub1T<true>), reinterpret_cast<void**>(&d_UpdateSub1), "deferred UpdateSubresource1");
        dc->Release();
    }
    Log("DLAA: camera hooks: %d of 5 on the immediate context, deferred contexts %d hooked, %d shared", ok, own, shared);
}

// ---------- DLAA resources and the motion-vector shader ----------
const char kMvShader[] = R"(
Texture2DArray<float> gDepth : register(t0);
RWTexture2D<float> gDepthOut : register(u0);
RWTexture2D<float2> gMotion : register(u1);
cbuffer P : register(b0) {
    float4 inv[4];   // inverse of this frame's view-projection (rows)
    float4 prv[4];   // last frame's view-projection (rows)
    float4 size;     // width, height, geo-11 shift for this eye (signed), convergence
    float4 delta;    // xyz: camera-relative positions move by this between the frames; w: slice
};
float4 Mul(float4 m[4], float4 v) { return float4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float d = gDepth.Load(int4(id.xy, (int)delta.w, 0));
    gDepthOut[id.xy] = d;
    float2 ndc = float2((id.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / size.y * 2.0);
    // geo-11 moved this eye's picture by shift * (1 - convergence / w): undo it to get the camera's own view.
    float4 h = Mul(inv, float4(ndc, d, 1.0));
    float w = 1.0 / h.w;
    float2 mono = float2(ndc.x - size.z * (1.0 - size.w / w), ndc.y);
    h = Mul(inv, float4(mono, d, 1.0));
    float4 world = float4(h.xyz / h.w + delta.xyz, 1.0);
    float4 c = Mul(prv, world);
    float2 prev = c.xy / c.w;
    prev.x += size.z * (1.0 - size.w / c.w);
    gMotion[id.xy] = float2((prev.x - ndc.x) * 0.5 * size.x, (ndc.y - prev.y) * 0.5 * size.y);
}
)";

// GPU time of the DLAA work, from timestamp queries read a few frames later (never waits).
struct GpuTimer {
    static constexpr int kRing = 6, kStamps = 5;  // start, eye 0 inputs, eye 0 DLSS, eye 1 inputs, eye 1 DLSS + copy back
    ID3D11Query* disjoint[kRing] = {};
    ID3D11Query* ts[kRing][kStamps] = {};
    bool pending[kRing] = {};
    int next = 0;
    double sumTotal = 0.0, sumDlss = 0.0;
    unsigned samples = 0;
};
GpuTimer g_t;

void TimerStamp(ID3D11DeviceContext* c, int slot, int i) {
    if (g_t.ts[slot][i]) c->End(g_t.ts[slot][i]);
}

void TimerCollect(ID3D11DeviceContext* c) {
    for (int k = 0; k < GpuTimer::kRing; ++k) {
        if (!g_t.pending[k]) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (c->GetData(g_t.disjoint[k], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t[GpuTimer::kStamps] = {};
        bool all = true;
        for (int i = 0; i < GpuTimer::kStamps && all; ++i)
            all = c->GetData(g_t.ts[k][i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!all) continue;
        g_t.pending[k] = false;
        if (dj.Disjoint || !dj.Frequency) continue;
        const double f = 1000.0 / static_cast<double>(dj.Frequency);
        g_t.sumTotal += static_cast<double>(t[4] - t[0]) * f;
        g_t.sumDlss += static_cast<double>((t[2] - t[1]) + (t[4] - t[3])) * f;
        ++g_t.samples;
    }
}

struct MvParams {
    float inv[16];
    float prv[16];
    float size[4];
    float delta[4];
};

struct Dlaa {
    ID3D11Device* dev = nullptr;
    ID3DDeviceContextState* state = nullptr;  // a clean pipeline state to work in (the game's is put back after)
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* cb = nullptr;
    unsigned w = 0, h = 0;
    ID3D11Texture2D* color = nullptr;  // one eye's scene colour
    ID3D11Texture2D* depth = nullptr;  // one eye's depth (R32F)
    ID3D11Texture2D* motion = nullptr;
    ID3D11Texture2D* out = nullptr;
    ID3D11UnorderedAccessView* depthUav = nullptr;
    ID3D11UnorderedAccessView* motionUav = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;  // the game's depth, both slices
    ID3D11Texture2D* depthSrvOf = nullptr;
    void* feature[2] = {};
    bool reset = true;
    Camera prev;
    unsigned lastFrame = ~0u;
    LARGE_INTEGER lastTime{};
    bool started = false;
};
Dlaa g_d;
std::atomic<bool> g_busy{false};
std::atomic<unsigned> g_statEvals{0}, g_statFail{0};
int g_lastEvalResult = 1;
std::atomic<bool> g_wantReset{true};
std::string g_status = "off";
std::mutex g_statusMutex;

void SetStatus(const char* s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = s;
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void FreeTargets() {
    for (void*& f : g_d.feature)
        if (f) {
            g_ngx.ReleaseDlss(f);
            f = nullptr;
        }
    SafeRelease(g_d.color);
    SafeRelease(g_d.depth);
    SafeRelease(g_d.motion);
    SafeRelease(g_d.out);
    SafeRelease(g_d.depthUav);
    SafeRelease(g_d.motionUav);
    SafeRelease(g_d.depthSrv);
    g_d.depthSrvOf = nullptr;
    g_d.w = g_d.h = 0;
}

bool MakeTex(unsigned w, unsigned h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    const HRESULT hr = g_d.dev->CreateTexture2D(&d, nullptr, t);
    if (FAILED(hr)) Log("DLAA: creating a %ux%u texture (format %d) failed: 0x%08lX", w, h, static_cast<int>(f), hr);
    return SUCCEEDED(hr);
}

// One-time setup: NGX, the shader, the clean state.
bool Start(ID3D11DeviceContext* c) {
    g_d.started = true;
    c->GetDevice(&g_d.dev);
    g_d.dev->Release();  // the game keeps it alive
    bool avail = false;
    if (!StartNgx(g_d.dev, &avail) || !avail) {
        Log("DLAA: DLSS is not available on this PC (see the lines above); DLAA stays off this session");
        SetStatus("not available (see ds3vr.log)");
        return false;
    }
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(g_d.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
        const D3D_FEATURE_LEVEL fl = g_d.dev->GetFeatureLevel();
        const HRESULT hr = d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_d.state);
        if (FAILED(hr)) Log("DLAA: CreateDeviceContextState failed: 0x%08lX", hr);
        d1->Release();
    }
    if (!g_d.state) {
        SetStatus("failed (see ds3vr.log)");
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kMvShader, sizeof(kMvShader) - 1, "ds3vr_motion", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr) || !code) {
        Log("DLAA: motion shader did not compile (0x%08lX): %s", hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "d3dcompiler_47.dll missing?");
        SafeRelease(err);
        SetStatus("failed (see ds3vr.log)");
        return false;
    }
    hr = g_d.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_d.cs);
    code->Release();
    SafeRelease(err);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(MvParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = g_d.dev->CreateBuffer(&bd, nullptr, &g_d.cb);
    if (FAILED(hr)) {
        Log("DLAA: shader setup failed: 0x%08lX", hr);
        SetStatus("failed (see ds3vr.log)");
        return false;
    }
    for (int k = 0; k < GpuTimer::kRing; ++k) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        g_d.dev->CreateQuery(&qd, &g_t.disjoint[k]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (int i = 0; i < GpuTimer::kStamps; ++i) g_d.dev->CreateQuery(&qd, &g_t.ts[k][i]);
    }
    QueryPerformanceCounter(&g_d.lastTime);
    Log("DLAA: ready (jitter sign %+.0f, eye sign %+.0f)", g_jitterSign, g_eyeSign);
    return true;
}

bool MakeTargets(unsigned w, unsigned h, ID3D11DeviceContext* c) {
    FreeTargets();
    bool ok = MakeTex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, &g_d.color) &&
              MakeTex(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_d.depth) &&
              MakeTex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_d.motion) &&
              MakeTex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_d.out);
    if (ok) ok = SUCCEEDED(g_d.dev->CreateUnorderedAccessView(g_d.depth, nullptr, &g_d.depthUav)) &&
                 SUCCEEDED(g_d.dev->CreateUnorderedAccessView(g_d.motion, nullptr, &g_d.motionUav));
    for (int e = 0; ok && e < 2; ++e) {
        const int r = g_ngx.CreateDlss(c, w, h, w, h, DS3VR_NGX_DLAA,
                                       DS3VR_NGX_FLAG_IS_HDR | DS3VR_NGX_FLAG_MV_LOW_RES | DS3VR_NGX_FLAG_AUTO_EXPOSURE, g_preset.load(),
                                       &g_d.feature[e]);
        if (r != 1) {
            Log("DLAA: creating the DLSS feature for eye %d (%ux%u) failed: %s", e, w, h, g_ngx.ResultString(r));
            ok = false;
        }
    }
    if (!ok) {
        FreeTargets();
        return false;
    }
    g_d.w = w;
    g_d.h = h;
    g_d.reset = true;
    Log("DLAA: two DLAA features created, %ux%u per eye, preset %d (0 = driver default, 6 = CNN F, 11 = transformer K)", w, h, g_preset.load());
    return true;
}

// 4x4 inverse (rows), false if singular.
bool Invert(const float* m, float* o) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-30f) return false;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
    return true;
}

// Camera-relative world positions? (DS3 may draw relative to the camera; then positions shift between frames.)
bool CameraRelative(const Camera& c) {
    const float* r3 = c.vp + 12;
    const float wOrigin = std::fabs(r3[3]);
    const float wCamera = std::fabs(r3[0] * c.cam[0] + r3[1] * c.cam[1] + r3[2] * c.cam[2] + r3[3]);
    return wOrigin < wCamera;
}

void RunDlaa(ID3D11DeviceContext* c) {
    ID3D11Texture2D* scene = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        scene = g_scene;
        depthTex = g_depth;
        if (scene) scene->AddRef();
        if (depthTex) depthTex->AddRef();
    }
    if (!scene || !depthTex) {
        SafeRelease(scene);
        SafeRelease(depthTex);
        return;
    }
    Camera cur;
    {
        std::lock_guard<std::mutex> lock(g_camMutex);
        int best = -1;
        if (g_candFrame == g_frame.load())
            for (int i = 0; i < g_candCount; ++i)
                if (best < 0 || g_cand[i].uploads > g_cand[best].uploads) best = i;
        if (best >= 0) g_camLatest = g_cand[best].cam;
        cur = g_camLatest;
    }
    if (cur.valid && cur.frame != g_frame.load()) g_statStale.fetch_add(1, std::memory_order_relaxed);
    D3D11_TEXTURE2D_DESC sd{};
    scene->GetDesc(&sd);
    bool ok = cur.valid;
    if (ok && (g_d.w != sd.Width || g_d.h != sd.Height || g_presetChanged.exchange(false))) ok = MakeTargets(sd.Width, sd.Height, c);
    if (ok && g_d.depthSrvOf != depthTex) {
        SafeRelease(g_d.depthSrv);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 2;
        const HRESULT hr = g_d.dev->CreateShaderResourceView(depthTex, &vd, &g_d.depthSrv);
        if (FAILED(hr)) {
            Log("DLAA: the depth buffer cannot be read (0x%08lX); DLAA off", hr);
            g_ngxFailed = true;
            SetStatus("failed (see ds3vr.log)");
            ok = false;
        }
        g_d.depthSrvOf = depthTex;
    }
    if (!ok) {
        SafeRelease(scene);
        SafeRelease(depthTex);
        return;
    }
    // Motion: from this frame's camera back to the last one's. A first frame, a toggle or a big jump resets.
    const bool havePrev = g_d.prev.valid;
    const Camera& prev = havePrev ? g_d.prev : cur;
    bool reset = g_d.reset || g_wantReset.exchange(false) || !havePrev;
    MvParams mp{};
    if (!Invert(cur.vp, mp.inv)) reset = true;
    memcpy(mp.prv, prev.vp, sizeof(mp.prv));
    const bool rel = CameraRelative(cur);
    for (int k = 0; k < 3; ++k) mp.delta[k] = rel ? cur.cam[k] - prev.cam[k] : 0.f;
    const float dx = cur.cam[0] - prev.cam[0], dy = cur.cam[1] - prev.cam[1], dz = cur.cam[2] - prev.cam[2];
    if (dx * dx + dy * dy + dz * dz > 25.f) reset = true;  // more than 5 m in one frame: a warp or a cut
    float sep = 0.f, conv = 0.f;
    StereoParamsGet(&sep, &conv);
    static bool loggedCam = false;
    if (!loggedCam) {
        loggedCam = true;
        Log("DLAA: camera %s (pos %.2f %.2f %.2f), view-projection rows [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g]",
            rel ? "relative" : "absolute", cur.cam[0], cur.cam[1], cur.cam[2], cur.vp[0], cur.vp[1], cur.vp[2], cur.vp[3], cur.vp[4], cur.vp[5],
            cur.vp[6], cur.vp[7], cur.vp[8], cur.vp[9], cur.vp[10], cur.vp[11], cur.vp[12], cur.vp[13], cur.vp[14], cur.vp[15]);
        Log("DLAA: geo-11 shift %.4f, convergence %.3f; scene %ux%u (format %d), depth format %d", sep, conv, sd.Width, sd.Height,
            static_cast<int>(sd.Format), [&] { D3D11_TEXTURE2D_DESC dd{}; depthTex->GetDesc(&dd); return static_cast<int>(dd.Format); }());
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float ms = static_cast<float>(static_cast<double>(now.QuadPart - g_d.lastTime.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart));
    g_d.lastTime = now;

    // Work in a clean pipeline state; the game's (and geo-11's) bindings come back untouched afterwards.
    ID3D11DeviceContext1* c1 = nullptr;
    c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
    if (!c1) {
        SafeRelease(scene);
        SafeRelease(depthTex);
        return;
    }
    ID3DDeviceContextState* saved = nullptr;
    g_busy = true;
    c1->SwapDeviceContextState(g_d.state, &saved);
    TimerCollect(c);
    const int slot = g_t.next;
    const bool timing = g_t.disjoint[slot] && !g_t.pending[slot];
    if (timing) {
        c->Begin(g_t.disjoint[slot]);
        TimerStamp(c, slot, 0);
    }
    for (UINT e = 0; e < 2; ++e) {
        const UINT sub = D3D11CalcSubresource(0, e, sd.MipLevels);
        c->CopySubresourceRegion(g_d.color, 0, 0, 0, 0, scene, sub, nullptr);
        mp.size[0] = static_cast<float>(g_d.w);
        mp.size[1] = static_cast<float>(g_d.h);
        mp.size[2] = (e == 0 ? g_eyeSign : -g_eyeSign) * sep;
        mp.size[3] = conv;
        mp.delta[3] = static_cast<float>(e);
        c->UpdateSubresource(g_d.cb, 0, nullptr, &mp, 0, 0);
        c->CSSetShader(g_d.cs, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &g_d.cb);
        c->CSSetShaderResources(0, 1, &g_d.depthSrv);
        ID3D11UnorderedAccessView* uavs[2] = {g_d.depthUav, g_d.motionUav};
        c->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        c->Dispatch((g_d.w + 7) / 8, (g_d.h + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav[2] = {};
        c->CSSetShaderResources(0, 1, &nullSrv);
        c->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
        c->CSSetShader(nullptr, nullptr, 0);

        if (timing) TimerStamp(c, slot, e == 0 ? 1 : 3);
        Ds3vrNgxEval ev{};
        ev.color = g_d.color;
        ev.output = g_d.out;
        ev.depth = g_d.depth;
        ev.motion = g_d.motion;
        ev.jitterX = g_jitterSign * cur.jx;
        ev.jitterY = g_jitterSign * cur.jy;
        ev.mvScaleX = ev.mvScaleY = 1.f;
        ev.renderW = g_d.w;
        ev.renderH = g_d.h;
        ev.frameTimeMs = ms;
        ev.reset = reset ? 1 : 0;
        const int r = g_ngx.EvaluateDlss(c, g_d.feature[e], &ev);
        if (r == 1) {
            c->CopySubresourceRegion(scene, sub, 0, 0, 0, g_d.out, 0, nullptr);
            g_statEvals.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_statFail.fetch_add(1, std::memory_order_relaxed);
            if (r != g_lastEvalResult) Log("DLAA: DLSS evaluate failed for eye %u: %s", e, g_ngx.ResultString(r));
        }
        g_lastEvalResult = r;
        if (timing) TimerStamp(c, slot, e == 0 ? 2 : 4);
    }
    if (timing) {
        c->End(g_t.disjoint[slot]);
        g_t.pending[slot] = true;
        g_t.next = (slot + 1) % GpuTimer::kRing;
    }
    c1->SwapDeviceContextState(saved, nullptr);
    g_busy = false;
    SafeRelease(saved);
    c1->Release();
    g_d.reset = false;
    g_d.prev = cur;
    SafeRelease(scene);
    SafeRelease(depthTex);
}

} // namespace

// ---------- public ----------
int DlaaMode() { return g_mode.load(); }

void DlaaSetMode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_mode = mode;
    g_wantReset = true;
    wchar_t v[8];
    swprintf(v, 8, L"%d", mode);
    WritePrivateProfileStringW(L"ds3vr", L"DlaaMode", v, Ini().c_str());
    Log("DLAA: mode %s", mode == 0 ? "off" : mode == 1 ? "DLAA" : "jitter test");
}

const char* DlaaStatus() {
    static std::string copy;
    std::lock_guard<std::mutex> lock(g_statusMutex);
    copy = g_status;
    return copy.c_str();
}

float DlaaJitterSign() { return g_jitterSign; }
float DlaaEyeSign() { return g_eyeSign; }
void DlaaSetSigns(float jitter, float eye) {
    g_jitterSign = jitter < 0.f ? -1.f : 1.f;
    g_eyeSign = eye < 0.f ? -1.f : 1.f;
    g_wantReset = true;
    WritePrivateProfileStringW(L"ds3vr", L"DlaaJitterSign", g_jitterSign < 0.f ? L"-1" : L"1", Ini().c_str());
    WritePrivateProfileStringW(L"ds3vr", L"DlaaEyeSign", g_eyeSign < 0.f ? L"-1" : L"1", Ini().c_str());
    Log("DLAA: jitter sign %+.0f, eye sign %+.0f", g_jitterSign, g_eyeSign);
}

int DlaaPreset() { return g_preset.load(); }
void DlaaSetPreset(int preset) {
    g_preset = preset;
    g_presetChanged = true;
    g_wantReset = true;
    wchar_t v[8];
    swprintf(v, 8, L"%d", preset);
    WritePrivateProfileStringW(L"ds3vr", L"DlaaPreset", v, Ini().c_str());
    Log("DLAA: preset %d (0 = driver default, 6 = CNN F, 11 = transformer K)", preset);
}

bool DlaaBusy() { return g_busy.load(std::memory_order_relaxed); }

void DlaaOnSetRenderTargets(unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (!n || n > 8 || !rtv || !rtv[0] || !dsv) return;
    if (g_lastRtv.load(std::memory_order_relaxed) == rtv[0] && g_lastDsv.load(std::memory_order_relaxed) == dsv) return;
    g_lastRtv.store(rtv[0], std::memory_order_relaxed);
    g_lastDsv.store(dsv, std::memory_order_relaxed);
    ID3D11Resource* cr = nullptr;
    ID3D11Resource* dr = nullptr;
    rtv[0]->GetResource(&cr);
    dsv->GetResource(&dr);
    ID3D11Texture2D* ct = nullptr;
    ID3D11Texture2D* dt = nullptr;
    if (cr) cr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&ct));
    if (dr) dr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dt));
    SafeRelease(cr);
    SafeRelease(dr);
    if (ct && dt) {
        D3D11_TEXTURE2D_DESC c{}, d{};
        ct->GetDesc(&c);
        dt->GetDesc(&d);
        const bool match = c.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && c.ArraySize == 2 && c.SampleDesc.Count == 1 &&
                           d.Format == DXGI_FORMAT_R32G8X24_TYPELESS && d.ArraySize == 2 && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
                           c.Width == d.Width && c.Height == d.Height;
        // DS3 also lights a half-size copy the same way: keep the full-size one (the largest seen).
        const bool largest = static_cast<unsigned long long>(c.Width) * c.Height >=
                             static_cast<unsigned long long>(g_sceneW.load()) * g_sceneH.load();
        if (match && largest) {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            if (g_scene != ct || g_depth != dt) {
                SafeRelease(g_scene);
                SafeRelease(g_depth);
                g_scene = ct;
                g_depth = dt;
                ct = dt = nullptr;  // kept
                g_sceneW = c.Width;
                g_sceneH = c.Height;
                Log("DLAA: scene colour and depth found, %ux%u per eye", c.Width, c.Height);
            }
        }
    }
    SafeRelease(ct);
    SafeRelease(dt);
}

void DlaaBeforeDraw(ID3D11DeviceContext* c) {
    if (g_busy.load(std::memory_order_relaxed) || g_mode.load(std::memory_order_relaxed) != 1 || g_ngxFailed.load(std::memory_order_relaxed))
        return;
    if (g_doneSincePresent.load(std::memory_order_relaxed)) return;  // once per frame
    ID3D11Texture2D* scene = g_scene;  // read without the lock: only compared
    if (!scene) return;
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    srv->Release();
    const bool hit = r == scene;
    SafeRelease(r);
    if (!hit) return;
    if (!g_d.started && !Start(c)) {
        g_ngxFailed = true;
        return;
    }
    RunDlaa(c);
    g_frame.fetch_add(1, std::memory_order_relaxed);  // uploads from here on are the next frame's
    g_doneSincePresent = true;
    if (!g_ready && g_statEvals.load() > 0) {
        g_ready = true;
        SetStatus("running");
        Log("DLAA: running (first frame done)");
    }
}

void DlssOnPresent(ID3D11Device* device) {
    static int frames = 0;
    static bool installed = false;
    if (!device) return;
    if (!g_doneSincePresent.exchange(false)) g_frame.fetch_add(1, std::memory_order_relaxed);
    if (!installed) {
        installed = true;
        g_mode = GetPrivateProfileIntW(L"ds3vr", L"DlaaMode", 0, Ini().c_str());
        if (g_mode < 0 || g_mode > 2) g_mode = 0;
        g_jitterSign = GetPrivateProfileIntW(L"ds3vr", L"DlaaJitterSign", 1, Ini().c_str()) < 0 ? -1.f : 1.f;
        g_eyeSign = GetPrivateProfileIntW(L"ds3vr", L"DlaaEyeSign", 1, Ini().c_str()) < 0 ? -1.f : 1.f;
        g_preset = GetPrivateProfileIntW(L"ds3vr", L"DlaaPreset", 6, Ini().c_str());  // CNN: about 60% of the transformer's cost, looks the same here
        InstallHooks(device);
        Log("DLAA: mode at start %d (0 off, 1 DLAA, 2 jitter test)", g_mode.load());
    }
    // DLAA switched off: give back its GPU memory (two DLSS features and the per-eye work textures).
    if (g_mode.load() != 1 && g_d.w) {
        FreeTargets();
        g_d.prev.valid = false;
        g_ready = false;
        Log("DLAA: off, its GPU memory released");
    }
    // Every ~10 s while on: what the jitter and DLAA did.
    static ULONGLONG nextStat = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_mode != 0 && now >= nextStat) {
        if (nextStat)
            Log("DLAA: last 10 s: camera uploads main %u (another view %u), half-size %u, other %u; DLSS evaluations %u ok, %u failed, "
                "camera from another jitter phase %u; %d camera buffers",
                g_statMain.exchange(0), g_statOtherView.exchange(0), g_statHalf.exchange(0), g_statOther.exchange(0), g_statEvals.exchange(0),
                g_statFail.exchange(0), g_statStale.exchange(0), g_cbCount.load());
        if (g_t.samples) {
            Log("DLAA: GPU time per frame (both eyes): %.2f ms in all, of which DLSS %.2f ms, copies and motion vectors %.2f ms (%u frames timed)",
                g_t.sumTotal / g_t.samples, g_t.sumDlss / g_t.samples, (g_t.sumTotal - g_t.sumDlss) / g_t.samples, g_t.samples);
            g_t.sumTotal = g_t.sumDlss = 0.0;
            g_t.samples = 0;
        }
        nextStat = now + 10000;
    }
    {
        const int m = g_mode.load();
        if (m == 0) SetStatus("off");
        else if (m == 2) SetStatus("jitter test (DLAA off)");
        else if (g_ngxFailed) {
        } else if (!g_ready) SetStatus("starting...");
    }
    if (++frames == 300 && GetPrivateProfileIntW(L"ds3vr", L"DlssSelfTest", 0, Ini().c_str()) == 1 && !g_d.started) SelfTest(device);
}

} // namespace ds3vr
