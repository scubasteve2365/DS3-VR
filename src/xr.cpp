#include "xr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "camhook.h"
#include "dlss.h"
#include "rendersize.h"
#include "sharpen.h"
#include "hud.h"
#include "menu.h"
#include "log.h"

// We talk to the runtime directly instead of shipping openxr_loader.dll: read the active
// runtime's manifest from the registry, load its DLL and negotiate like the loader would.

namespace ds3vr {
namespace {

#define XRCHECK(call)                                                       \
    do {                                                                    \
        XrResult r_ = (call);                                               \
        if (XR_FAILED(r_)) {                                                \
            Log("XR: %s failed: %d", #call, static_cast<int>(r_));          \
            return false;                                                   \
        }                                                                   \
    } while (0)

constexpr float kR2D = 57.29578f;

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
std::atomic<int> g_poseLag{2};
float g_worldScale = 1.f;
bool g_shiftOverride = false;            // ds3vr.ini StereoShiftOverride: keep it, ignore geo-11's reading
std::atomic<float> g_shift{0.f};      // geo-11's per-eye image shift at infinity, in NDC (2 = full width)
std::atomic<bool> g_swapEyes{false};
int g_aspectMode = 1;                 // ds3vr.ini RenderAspect: 1 = headset shape, 0 = the game's 16:9
int g_leftHalf = 1;                   // which half of geo-11's texture holds the left eye (0 = left, 1 = right)
std::atomic<bool> g_recenter{false};
ULONGLONG g_runtimeRecenterAt = 0;  // SteamVR recentred: follow it once its change has taken effect
std::atomic<bool> g_hudRecenter{false};
std::atomic<int> g_hudMode{0};      // 0 = fixed in front of you, 1 = follows your head, 2 = hidden
float g_hudDist = 1.2f, g_hudWidth = 1.0f, g_hudDrop = 0.10f;
std::atomic<bool> g_capture{false};
std::atomic<bool> g_shutdownReq{false};
std::atomic<float> g_displayHz{0.f};  // headset refresh rate, from xrWaitFrame

void ReadSettings() {
    std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    {
        int lag = GetPrivateProfileIntW(L"ds3vr", L"PoseLag", 2, ini.c_str());
        if (lag < 0 || lag > 8) lag = 2;
        g_poseLag.store(lag);
    }
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"ds3vr", L"WorldScale", L"1.0", v, 32, ini.c_str());
    g_worldScale = static_cast<float>(_wtof(v));
    if (g_worldScale < 0.1f || g_worldScale > 20.f) g_worldScale = 1.f;
    g_aspectMode = GetPrivateProfileIntW(L"ds3vr", L"HeadsetAspect", 1, ini.c_str());
    g_leftHalf = GetPrivateProfileIntW(L"ds3vr", L"LeftEyeHalf", 1, ini.c_str()) ? 1 : 0;
    // geo-11 slides each eye's image sideways by a fixed amount, which the views we send to the headset
    // cancel. geo-11 works it out per PC (from the monitor's physical size), so once the game draws, the
    // real value is read from geo-11's StereoParams texture (drawlog.cpp, XrSetMeasuredShift). Until then
    // 0.0209 is used: geo-11's own value on the PC DS3 VR was developed on, at separation 20 (which the
    // mod resets every launch). Earlier versions used 0.0187, measured from screenshots, about 11% low. Not read from ds3vr.ini any more (a 0 or a typo there gave a doubled
    // picture); StereoShiftOverride (non-zero) is only for testing.
    constexpr float kStereoShiftSep20 = 0.0209f;
    float s = kStereoShiftSep20;
    GetPrivateProfileStringW(L"ds3vr", L"StereoShiftOverride", L"0", v, 32, ini.c_str());
    const float over = static_cast<float>(_wtof(v));
    if (over > 0.001f && over < 0.5f) {
        s = over;
        g_shiftOverride = true;
        Log("settings: StereoShiftOverride %.4f in ds3vr.ini replaces the calculated %.4f", over, kStereoShiftSep20);
    }
    GetPrivateProfileStringW(L"ds3vr", L"StereoShift", L"", v, 32, ini.c_str());
    if (v[0] && std::fabs(static_cast<float>(_wtof(v)) - kStereoShiftSep20) > 0.0005f)
        Log("settings: StereoShift=%ls in ds3vr.ini is no longer used (the mod calculates %.4f itself)", v, kStereoShiftSep20);
    g_shift.store(s);
    g_hudMode.store(GetPrivateProfileIntW(L"ds3vr", L"HudMode", 0, ini.c_str()) % 3);
    GetPrivateProfileStringW(L"ds3vr", L"HudDistance", L"1.2", v, 32, ini.c_str());
    g_hudDist = static_cast<float>(_wtof(v));
    if (g_hudDist < 0.3f || g_hudDist > 10.f) g_hudDist = 1.2f;
    GetPrivateProfileStringW(L"ds3vr", L"HudWidth", L"1.0", v, 32, ini.c_str());
    g_hudWidth = static_cast<float>(_wtof(v));
    if (g_hudWidth < 0.1f || g_hudWidth > 10.f) g_hudWidth = 1.0f;
    GetPrivateProfileStringW(L"ds3vr", L"HudDrop", L"0.10", v, 32, ini.c_str());
    g_hudDrop = static_cast<float>(_wtof(v));
    if (g_hudDrop < -2.f || g_hudDrop > 2.f) g_hudDrop = 0.10f;
    Log("settings: HUD mode %d, distance %.2f m, width %.2f m, %.2f m below eye level", g_hudMode.load(), g_hudDist, g_hudWidth, g_hudDrop);
    Log("settings: PoseLag %d, WorldScale %.2f, StereoShift %.4f, LeftEyeHalf %d", g_poseLag.load(), g_worldScale, s, g_leftHalf);
}

// ---------------------------------------------------------------------------------------------
// Runtime loading
// ---------------------------------------------------------------------------------------------
PFN_xrGetInstanceProcAddr g_gipa = nullptr;

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool LoadRuntime() {
    // Same order as the official OpenXR loader: a runtime chosen for this launch (XR_RUNTIME_JSON, set
    // e.g. when Virtual Desktop starts a game with VDXR) first, then the system's active runtime.
    wchar_t manifest[1024] = {};
    const DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest, 1024);
    if (envLen > 0 && envLen < 1024 && GetFileAttributesW(manifest) != INVALID_FILE_ATTRIBUTES) {
        Log("XR: runtime chosen for this launch (XR_RUNTIME_JSON): %ls", manifest);
    } else {
        if (envLen > 0) Log("XR: XR_RUNTIME_JSON is set but could not be used (%ls); using the system's active runtime", manifest);
        manifest[0] = 0;
        DWORD sz = sizeof(manifest);
        LONG rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr,
                               manifest, &sz);
        if (rc != ERROR_SUCCESS) {
            Log("XR: no ActiveRuntime in HKLM\\SOFTWARE\\Khronos\\OpenXR\\1 (rc=%ld). Is an OpenXR runtime set?", rc);
            return false;
        }
        Log("XR: system's active runtime manifest %ls", manifest);
    }

    FILE* fp = _wfopen(manifest, L"rb");
    if (!fp) {
        Log("XR: cannot open the manifest");
        return false;
    }
    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) json.append(buf, n);
    fclose(fp);

    size_t k = json.find("\"library_path\"");
    if (k == std::string::npos) { Log("XR: manifest has no library_path"); return false; }
    size_t q1 = json.find('"', json.find(':', k) + 1);
    size_t q2 = json.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) { Log("XR: bad library_path"); return false; }
    std::string lib;
    for (size_t i = q1 + 1; i < q2; ++i) {
        if (json[i] == '\\' && i + 1 < q2) ++i;  // unescape \\ and \/
        lib.push_back(json[i]);
    }
    std::wstring libW = Widen(lib);
    if (!(libW.size() > 1 && (libW[1] == L':' || libW[0] == L'\\'))) {  // relative to the manifest
        std::wstring dir(manifest);
        size_t slash = dir.find_last_of(L"\\/");
        libW = dir.substr(0, slash + 1) + libW;
    }
    Log("XR: runtime library %ls", libW.c_str());

    HMODULE m = LoadLibraryW(libW.c_str());
    if (!m) { Log("XR: LoadLibrary failed (%lu)", GetLastError()); return false; }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        reinterpret_cast<void*>(GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface")));
    if (!negotiate) { Log("XR: runtime has no xrNegotiateLoaderRuntimeInterface"); return false; }

    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) { Log("XR: negotiation failed: %d", static_cast<int>(r)); return false; }
    g_gipa = rr.getInstanceProcAddr;
    Log("XR: negotiated, runtime interface %u, api %u.%u.%u", rr.runtimeInterfaceVersion,
        static_cast<unsigned>(XR_VERSION_MAJOR(rr.runtimeApiVersion)), static_cast<unsigned>(XR_VERSION_MINOR(rr.runtimeApiVersion)),
        static_cast<unsigned>(XR_VERSION_PATCH(rr.runtimeApiVersion)));
    return true;
}

template <typename T>
bool Get(XrInstance inst, const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(g_gipa(inst, name, &f)) || !f) {
        Log("XR: missing function %s", name);
        return false;
    }
    out = reinterpret_cast<T>(f);
    return true;
}

PFN_xrGetInstanceProperties xrGetInstanceProperties_ = nullptr;
PFN_xrGetSystem xrGetSystem_ = nullptr;
PFN_xrGetSystemProperties xrGetSystemProperties_ = nullptr;
PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews_ = nullptr;
PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR_ = nullptr;
PFN_xrCreateSession xrCreateSession_ = nullptr;
PFN_xrDestroySession xrDestroySession_ = nullptr;
PFN_xrDestroyInstance xrDestroyInstance_ = nullptr;
PFN_xrRequestExitSession xrRequestExitSession_ = nullptr;
PFN_xrCreateReferenceSpace xrCreateReferenceSpace_ = nullptr;
PFN_xrPollEvent xrPollEvent_ = nullptr;
PFN_xrBeginSession xrBeginSession_ = nullptr;
PFN_xrEndSession xrEndSession_ = nullptr;
PFN_xrWaitFrame xrWaitFrame_ = nullptr;
PFN_xrBeginFrame xrBeginFrame_ = nullptr;
PFN_xrEndFrame xrEndFrame_ = nullptr;
PFN_xrLocateSpace xrLocateSpace_ = nullptr;
PFN_xrLocateViews xrLocateViews_ = nullptr;
PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats_ = nullptr;
PFN_xrCreateSwapchain xrCreateSwapchain_ = nullptr;
PFN_xrDestroySwapchain xrDestroySwapchain_ = nullptr;
PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages_ = nullptr;
PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage_ = nullptr;
PFN_xrWaitSwapchainImage xrWaitSwapchainImage_ = nullptr;
PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage_ = nullptr;

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
std::recursive_mutex g_xrMutex;  // TickXR vs. shutdown from the window thread
bool g_started = false, g_dead = false, g_running = false, g_loggedViews = false;
ID3D11Device* g_device = nullptr;
XrInstance g_inst = XR_NULL_HANDLE;
XrSystemId g_sys = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE, g_viewSpace = XR_NULL_HANDLE;
float g_ipd = 0.f;
uint64_t g_xrFrames = 0;
ULONGLONG g_lastLog = 0;

// One double-width swapchain: left eye in the left half, right eye in the right half.
XrSwapchain g_sc = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D*> g_scImgs;
DXGI_FORMAT g_scFmt = DXGI_FORMAT_UNKNOWN;  // the stereo swapchain's format (sharpening draws with it)
UINT g_scW = 0, g_scH = 0;
DXGI_FORMAT g_scSrcFmt = DXGI_FORMAT_UNKNOWN;
bool g_scFailed = false;

// Virtual screen for menus / loading screens (when the game is not updating its camera).
bool g_havePlacement = false;
XrPosef g_screenPose{};

// HUD panel (the game's HUD draws are redirected into a texture of ours; see hud.h)
XrSwapchain g_hudSc = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D*> g_hudImgs;
UINT g_hudW = 0, g_hudH = 0;
bool g_hudFailed = false;
bool g_hudPlaced = false;
XrPosef g_hudPose{};

// ---------------------------------------------------------------------------------------------
// geo-11's shared stereo texture (Katanga interface)
// ---------------------------------------------------------------------------------------------
HANDLE g_kMap = nullptr;
volatile UINT* g_kView = nullptr;
UINT g_kHandle = 0;
ID3D11Texture2D* g_kTex = nullptr;
IDXGIKeyedMutex* g_kMutex = nullptr;
D3D11_TEXTURE2D_DESC g_kDesc{};
ULONGLONG g_kLastTry = 0;

void ReleaseShared() {
    if (g_kMutex) { g_kMutex->Release(); g_kMutex = nullptr; }
    if (g_kTex) { g_kTex->Release(); g_kTex = nullptr; }
}

// Returns geo-11's current stereo texture, reopening it when geo-11 replaces it (resolution change).
ID3D11Texture2D* SharedStereoTexture() {
    if (!g_kView) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_kLastTry < 1000) return nullptr;
        g_kLastTry = now;
        g_kMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\KatangaMappedFile");
        if (!g_kMap) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                Log("KATANGA: no Local\\KatangaMappedFile yet. Is geo-11 installed with direct_mode = katanga_vr in d3dxdm.ini?");
            }
            return nullptr;
        }
        g_kView = static_cast<volatile UINT*>(MapViewOfFile(g_kMap, FILE_MAP_READ, 0, 0, sizeof(UINT)));
        if (!g_kView) {
            Log("KATANGA: MapViewOfFile failed (%lu)", GetLastError());
            CloseHandle(g_kMap);
            g_kMap = nullptr;
            return nullptr;
        }
        Log("KATANGA: found geo-11's shared mapping");
    }
    const UINT h = *g_kView;
    if (h != g_kHandle) {
        ReleaseShared();
        g_kHandle = h;
        if (!h) {
            Log("KATANGA: geo-11 cleared its shared texture handle");
            return nullptr;
        }
        HRESULT hr = g_device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h)), __uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&g_kTex));
        if (FAILED(hr) || !g_kTex) {
            Log("KATANGA: OpenSharedResource(0x%x) failed: 0x%08lx", h, static_cast<unsigned long>(hr));
            g_kTex = nullptr;
            return nullptr;
        }
        g_kTex->GetDesc(&g_kDesc);
        if (g_kDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
            g_kTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&g_kMutex));
        Log("KATANGA: opened shared stereo texture 0x%x: %ux%u (%ux%u per eye), format %d, mips %u, array %u, misc 0x%x%s", h,
            g_kDesc.Width, g_kDesc.Height, g_kDesc.Width / 2, g_kDesc.Height, static_cast<int>(g_kDesc.Format), g_kDesc.MipLevels,
            g_kDesc.ArraySize, g_kDesc.MiscFlags, g_kMutex ? " (keyed mutex)" : "");
    }
    return g_kTex;
}

// ---------------------------------------------------------------------------------------------
// BMP capture (F8)
// ---------------------------------------------------------------------------------------------
void WriteBmp(const std::wstring& path, const uint8_t* src, UINT pitch, UINT x0, UINT w, UINT h, bool bgra) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const UINT rowBytes = (w * 3 + 3) & ~3u;
    const UINT dataSize = rowBytes * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[off + i] = static_cast<uint8_t>(v >> (8 * i)); };
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, dataSize);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes, 0);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(h - 1 - y) * pitch + static_cast<size_t>(x0) * 4;
        for (UINT x = 0; x < w; ++x) {
            const uint8_t r = bgra ? s[x * 4 + 2] : s[x * 4 + 0];
            const uint8_t g = s[x * 4 + 1];
            const uint8_t b = bgra ? s[x * 4 + 0] : s[x * 4 + 2];
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row.data(), 1, rowBytes, f);
    }
    fclose(f);
}

void CaptureStereo(ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    static int n = 0;
    ++n;
    const int stamp = now.wHour * 10000 + now.wMinute * 100 + now.wSecond;
    D3D11_TEXTURE2D_DESC d = g_kDesc;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &st)) || !st) {
        Log("snapshot: could not create a staging texture");
        return;
    }
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                          d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const UINT half = d.Width / 2;
        wchar_t name[64];
        swprintf(name, 64, L"ds3vr_snap_%06d_%d_righteye.bmp", stamp, n);
        WriteBmp(std::wstring(ModuleDir()) + name, static_cast<const uint8_t*>(m.pData), m.RowPitch, 0, half, d.Height, bgra);
        swprintf(name, 64, L"ds3vr_snap_%06d_%d_lefteye.bmp", stamp, n);
        WriteBmp(std::wstring(ModuleDir()) + name, static_cast<const uint8_t*>(m.pData), m.RowPitch, half, half, d.Height, bgra);
        ctx->Unmap(st, 0);
        Log("snapshot: saved ds3vr_snap_%06d_%d_lefteye.bmp / _righteye.bmp (%ux%u each), stereo shift %.4f", stamp, n, half, d.Height, g_shift.load());
    }
    st->Release();
    // The HUD panel (left-eye slice) too, on black.
    if (ID3D11Texture2D* hud = HudTexture()) {
        D3D11_TEXTURE2D_DESC hd{};
        hud->GetDesc(&hd);
        hd.MipLevels = 1;
        hd.ArraySize = 1;
        hd.Usage = D3D11_USAGE_STAGING;
        hd.BindFlags = 0;
        hd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hd.MiscFlags = 0;
        ID3D11Texture2D* hs = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&hd, nullptr, &hs)) && hs) {
            ctx->CopySubresourceRegion(hs, 0, 0, 0, 0, hud, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE hm{};
            if (SUCCEEDED(ctx->Map(hs, 0, D3D11_MAP_READ, 0, &hm))) {
                const bool bgra = hd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || hd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                  hd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
                wchar_t name[64];
                swprintf(name, 64, L"ds3vr_snap_%06d_%d_hud.bmp", stamp, n);
                WriteBmp(std::wstring(ModuleDir()) + name, static_cast<const uint8_t*>(hm.pData), hm.RowPitch, 0, hd.Width, hd.Height, bgra);
                ctx->Unmap(hs, 0);
                Log("snapshot: saved ds3vr_snap_%06d_%d_hud.bmp", stamp, n);
            }
            hs->Release();
        }
    }
}

// F1 while DLSS upscales: also save each upscaled half (headset size), named like geo-11's halves.
void CaptureUpscaled(ID3D11DeviceContext* ctx, ID3D11Texture2D* const* up, UINT w, UINT h) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    const int stamp = now.wHour * 10000 + now.wMinute * 100 + now.wSecond;
    for (int half = 0; half < 2; ++half) {
        D3D11_TEXTURE2D_DESC d{};
        up[half]->GetDesc(&d);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &st)) || !st) continue;
        ctx->CopyResource(st, up[half]);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
            // geo-11's left half is the right eye (see CaptureStereo).
            wchar_t name[64];
            swprintf(name, 64, L"ds3vr_snap_%06d_dlss_%s.bmp", stamp, half == 0 ? L"righteye" : L"lefteye");
            WriteBmp(std::wstring(ModuleDir()) + name, static_cast<const uint8_t*>(m.pData), m.RowPitch, 0, w, h, bgra);
            ctx->Unmap(st, 0);
            Log("snapshot: saved %ls (%ux%u, DLSS upscaled)", name, w, h);
        }
        st->Release();
    }
}

// ---------------------------------------------------------------------------------------------
// OpenXR setup
// ---------------------------------------------------------------------------------------------
bool InitXR() {
    ReadSettings();
    if (!LoadRuntime()) return false;

    PFN_xrCreateInstance xrCreateInstance_ = nullptr;
    if (!Get(XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstance_)) return false;

    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "ds3vr");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "ds3vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    XRCHECK(xrCreateInstance_(&ici, &g_inst));
    XrInstance inst = g_inst;

    if (!Get(inst, "xrGetInstanceProperties", xrGetInstanceProperties_) || !Get(inst, "xrGetSystem", xrGetSystem_) ||
        !Get(inst, "xrGetSystemProperties", xrGetSystemProperties_) ||
        !Get(inst, "xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews_) ||
        !Get(inst, "xrGetD3D11GraphicsRequirementsKHR", xrGetD3D11GraphicsRequirementsKHR_) ||
        !Get(inst, "xrCreateSession", xrCreateSession_) || !Get(inst, "xrDestroySession", xrDestroySession_) ||
        !Get(inst, "xrDestroyInstance", xrDestroyInstance_) || !Get(inst, "xrRequestExitSession", xrRequestExitSession_) ||
        !Get(inst, "xrCreateReferenceSpace", xrCreateReferenceSpace_) ||
        !Get(inst, "xrPollEvent", xrPollEvent_) || !Get(inst, "xrBeginSession", xrBeginSession_) ||
        !Get(inst, "xrEndSession", xrEndSession_) || !Get(inst, "xrWaitFrame", xrWaitFrame_) ||
        !Get(inst, "xrBeginFrame", xrBeginFrame_) || !Get(inst, "xrEndFrame", xrEndFrame_) ||
        !Get(inst, "xrLocateSpace", xrLocateSpace_) || !Get(inst, "xrLocateViews", xrLocateViews_) ||
        !Get(inst, "xrEnumerateSwapchainFormats", xrEnumerateSwapchainFormats_) ||
        !Get(inst, "xrCreateSwapchain", xrCreateSwapchain_) || !Get(inst, "xrDestroySwapchain", xrDestroySwapchain_) ||
        !Get(inst, "xrEnumerateSwapchainImages", xrEnumerateSwapchainImages_) ||
        !Get(inst, "xrAcquireSwapchainImage", xrAcquireSwapchainImage_) ||
        !Get(inst, "xrWaitSwapchainImage", xrWaitSwapchainImage_) ||
        !Get(inst, "xrReleaseSwapchainImage", xrReleaseSwapchainImage_))
        return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XRCHECK(xrGetInstanceProperties_(inst, &ip));
    Log("XR: runtime \"%s\" %u.%u.%u", ip.runtimeName, static_cast<unsigned>(XR_VERSION_MAJOR(ip.runtimeVersion)),
        static_cast<unsigned>(XR_VERSION_MINOR(ip.runtimeVersion)), static_cast<unsigned>(XR_VERSION_PATCH(ip.runtimeVersion)));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XRCHECK(xrGetSystem_(inst, &sgi, &sys));
    g_sys = sys;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XRCHECK(xrGetSystemProperties_(inst, sys, &sp));
    Log("XR: headset \"%s\"", sp.systemName);

    uint32_t nv = 0;
    XRCHECK(xrEnumerateViewConfigurationViews_(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr));
    std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XRCHECK(xrEnumerateViewConfigurationViews_(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data()));
    for (uint32_t i = 0; i < nv; ++i)
        Log("XR: view %u recommended %ux%u (max %ux%u)", i, vcv[i].recommendedImageRectWidth, vcv[i].recommendedImageRectHeight,
            vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight);
    if (nv > 0) {
        // Remember the requested per-eye height for the next launch (RenderHeight=0 uses it).
        const unsigned want = vcv[0].recommendedImageRectHeight & ~1u;
        wchar_t v[32];
        swprintf(v, 32, L"%u", want);
        WritePrivateProfileStringW(L"state", L"HeadsetHeight", v, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
        unsigned rw = 0, rh = 0;
        if (RenderSizeGet(&rw, &rh) && rh != want)
            Log("XR: the VR runtime asks for %u per eye but this run renders %u tall; with RenderHeight=0 the next launch uses %u",
                want, rh, want);
    }

    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(xrGetD3D11GraphicsRequirementsKHR_(inst, sys, &req));
    {
        IDXGIDevice* dxgi = nullptr;
        LUID luid{};
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && dxgi) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad) {
                DXGI_ADAPTER_DESC d{};
                ad->GetDesc(&d);
                luid = d.AdapterLuid;
                Log("XR: game adapter \"%ls\"", d.Description);
                ad->Release();
            }
            dxgi->Release();
        }
        const bool same = luid.LowPart == req.adapterLuid.LowPart && luid.HighPart == req.adapterLuid.HighPart;
        Log("XR: headset adapter %s the game's adapter", same ? "matches" : "DOES NOT match");
    }

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XRCHECK(xrCreateSession_(inst, &sci, &g_session));
    Log("XR: session created");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRCHECK(xrCreateReferenceSpace_(g_session, &rsci, &g_local));
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHECK(xrCreateReferenceSpace_(g_session, &rsci, &g_viewSpace));
    return true;
}

// A swapchain the size of geo-11's double-width texture. geo-11 shares the game's gamma-encoded
// RGBA8 image as UNORM, so we prefer the _SRGB view of the same bytes: the compositor then shows
// exactly what the monitor would.
bool CreateSwapchain(UINT w, UINT h, DXGI_FORMAT srcFmt) {
    if (g_sc != XR_NULL_HANDLE) {
        xrDestroySwapchain_(g_sc);
        g_sc = XR_NULL_HANDLE;
        g_scImgs.clear();
    }
    uint32_t n = 0;
    XRCHECK(xrEnumerateSwapchainFormats_(g_session, 0, &n, nullptr));
    std::vector<int64_t> fmts(n);
    XRCHECK(xrEnumerateSwapchainFormats_(g_session, n, &n, fmts.data()));
    DXGI_FORMAT want[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    switch (srcFmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            want[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; want[1] = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            want[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; want[1] = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default:
            want[0] = srcFmt; break;
    }
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT f : want) {
        if (f == DXGI_FORMAT_UNKNOWN) continue;
        for (int64_t g : fmts)
            if (g == f) { chosen = f; break; }
        if (chosen != DXGI_FORMAT_UNKNOWN) break;
    }
    if (chosen == DXGI_FORMAT_UNKNOWN) {
        Log("XR: no swapchain format compatible with geo-11's texture format %d", static_cast<int>(srcFmt));
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain_(g_session, &ci, &g_sc));
    uint32_t ni = 0;
    XRCHECK(xrEnumerateSwapchainImages_(g_sc, 0, &ni, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRCHECK(xrEnumerateSwapchainImages_(g_sc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
    for (auto& i : imgs) g_scImgs.push_back(i.texture);
    g_scW = w;
    g_scH = h;
    g_scSrcFmt = srcFmt;
    g_scFmt = chosen;
    Log("XR: stereo swapchain %ux%u format %d, %u images", w, h, static_cast<int>(chosen), ni);
    return true;
}

// Places the menu screen 2.5 m in front of where the head is looking (yaw only), at eye height.
void PlaceScreen(const XrPosef& head) {
    const XrQuaternionf& q = head.orientation;
    float fx = -2.f * (q.x * q.z + q.w * q.y);
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f) { fx = 0.f; fz = -1.f; len = 1.f; }
    fx /= len;
    fz /= len;
    const float dist = 2.5f;
    g_screenPose.position = {head.position.x + fx * dist, head.position.y, head.position.z + fz * dist};
    const float yaw = std::atan2(-fx, -fz);
    g_screenPose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
    g_havePlacement = true;
}

bool CreateHudSwapchain(UINT w, UINT h, DXGI_FORMAT src) {
    uint32_t n = 0;
    XRCHECK(xrEnumerateSwapchainFormats_(g_session, 0, &n, nullptr));
    std::vector<int64_t> fmts(n);
    XRCHECK(xrEnumerateSwapchainFormats_(g_session, n, &n, fmts.data()));
    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    // Same family as the HUD texture so it can be copied straight across (sRGB view: the game's
    // colours are already gamma-encoded).
    const bool bgra = src == DXGI_FORMAT_B8G8R8A8_UNORM || src == DXGI_FORMAT_B8G8R8A8_TYPELESS || src == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const DXGI_FORMAT want[2] = {bgra ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                 bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM};
    for (DXGI_FORMAT f : want)
        for (int64_t g : fmts)
            if (chosen == DXGI_FORMAT_UNKNOWN && g == f) chosen = f;
    if (chosen == DXGI_FORMAT_UNKNOWN) {
        Log("XR: the runtime offers no swapchain format matching the HUD texture (%d)", static_cast<int>(src));
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain_(g_session, &ci, &g_hudSc));
    uint32_t ni = 0;
    XRCHECK(xrEnumerateSwapchainImages_(g_hudSc, 0, &ni, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRCHECK(xrEnumerateSwapchainImages_(g_hudSc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
    g_hudImgs.clear();
    for (auto& i : imgs) g_hudImgs.push_back(i.texture);
    g_hudW = w;
    g_hudH = h;
    Log("XR: HUD panel swapchain %ux%u format %d", w, h, static_cast<int>(chosen));
    return true;
}

// Copies the HUD (left-eye slice) into the panel swapchain. Returns false if there is no HUD yet.
bool UpdateHud() {
    ID3D11Texture2D* hud = HudTexture();
    if (!hud || g_hudFailed) return false;
    D3D11_TEXTURE2D_DESC d{};
    hud->GetDesc(&d);
    if (g_hudSc == XR_NULL_HANDLE || d.Width != g_hudW || d.Height != g_hudH) {
        if (g_hudSc != XR_NULL_HANDLE) {
            xrDestroySwapchain_(g_hudSc);
            g_hudSc = XR_NULL_HANDLE;
        }
        if (!CreateHudSwapchain(d.Width, d.Height, d.Format)) {
            g_hudFailed = true;
            return false;
        }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage_(g_hudSc, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(g_hudSc, &wi);
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    if (ctx) {
        ctx->CopySubresourceRegion(g_hudImgs[idx], 0, 0, 0, 0, hud, D3D11CalcSubresource(0, 0, 1), nullptr);
        ctx->Release();
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage_(g_hudSc, &ri);
    return true;
}

// Mod menu panel (menu.cpp draws it; we show it while it is open).
XrSwapchain g_menuSc = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D*> g_menuImgs;
UINT g_menuW = 0, g_menuH = 0;
bool g_menuFailed = false;
unsigned g_menuPlacedGen = 0;
XrPosef g_menuPose{};
constexpr float kMenuDist = 0.9f, kMenuWidth = 0.8f;

bool UpdateMenuPanel() {
    ID3D11Texture2D* tex = MenuTexture();
    if (!tex || g_menuFailed || !MenuIsOpen()) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (g_menuSc == XR_NULL_HANDLE) {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats_(g_session, 0, &n, nullptr);
        std::vector<int64_t> fmts(n);
        xrEnumerateSwapchainFormats_(g_session, n, &n, fmts.data());
        DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
        for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM})
            for (int64_t g : fmts)
                if (chosen == DXGI_FORMAT_UNKNOWN && g == f) chosen = f;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = chosen;
        ci.sampleCount = 1;
        ci.width = d.Width;
        ci.height = d.Height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        uint32_t ni = 0;
        if (chosen == DXGI_FORMAT_UNKNOWN || XR_FAILED(xrCreateSwapchain_(g_session, &ci, &g_menuSc)) ||
            XR_FAILED(xrEnumerateSwapchainImages_(g_menuSc, 0, &ni, nullptr))) {
            Log("XR: could not create the menu panel swapchain");
            g_menuFailed = true;
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages_(g_menuSc, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        g_menuImgs.clear();
        for (auto& i : imgs) g_menuImgs.push_back(i.texture);
        g_menuW = d.Width;
        g_menuH = d.Height;
        Log("XR: menu panel swapchain %ux%u format %d", d.Width, d.Height, static_cast<int>(chosen));
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage_(g_menuSc, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(g_menuSc, &wi);
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    if (ctx) {
        ctx->CopySubresourceRegion(g_menuImgs[idx], 0, 0, 0, 0, tex, 0, nullptr);
        ctx->Release();
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage_(g_menuSc, &ri);
    return true;
}

// The menu: straight ahead of where the head looks when it opens, at eye level.
void PlaceMenu(const XrPosef& head) {
    const XrQuaternionf& q = head.orientation;
    float fx = -2.f * (q.x * q.z + q.w * q.y);
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f) { fx = 0.f; fz = -1.f; len = 1.f; }
    fx /= len;
    fz /= len;
    g_menuPose.position = {head.position.x + fx * kMenuDist, head.position.y - 0.05f, head.position.z + fz * kMenuDist};
    const float yaw = std::atan2(-fx, -fz);
    g_menuPose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
}

// The HUD panel in front of where the head looks (yaw only), a little below eye level.
void PlaceHud(const XrPosef& head) {
    const XrQuaternionf& q = head.orientation;
    float fx = -2.f * (q.x * q.z + q.w * q.y);
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f) { fx = 0.f; fz = -1.f; len = 1.f; }
    fx /= len;
    fz /= len;
    g_hudPose.position = {head.position.x + fx * g_hudDist, head.position.y - g_hudDrop, head.position.z + fz * g_hudDist};
    const float yaw = std::atan2(-fx, -fz);
    g_hudPose.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
    g_hudPlaced = true;
}

void Shutdown(const char* why) {
    std::lock_guard<std::recursive_mutex> lock(g_xrMutex);
    if (g_dead) return;
    g_dead = true;
    Log("XR: shutting down (%s)", why);
    ReleaseShared();
    if (g_kView) { UnmapViewOfFile(const_cast<UINT*>(g_kView)); g_kView = nullptr; }
    if (g_kMap) { CloseHandle(g_kMap); g_kMap = nullptr; }
    if (g_sc != XR_NULL_HANDLE && xrDestroySwapchain_) { xrDestroySwapchain_(g_sc); g_sc = XR_NULL_HANDLE; }
    if (g_hudSc != XR_NULL_HANDLE && xrDestroySwapchain_) { xrDestroySwapchain_(g_hudSc); g_hudSc = XR_NULL_HANDLE; }
    if (g_menuSc != XR_NULL_HANDLE && xrDestroySwapchain_) { xrDestroySwapchain_(g_menuSc); g_menuSc = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE && xrDestroySession_) {
        if (g_running && xrEndSession_) xrEndSession_(g_session);
        xrDestroySession_(g_session);
        g_session = XR_NULL_HANDLE;
    }
    if (g_inst != XR_NULL_HANDLE && xrDestroyInstance_) {
        xrDestroyInstance_(g_inst);
        g_inst = XR_NULL_HANDLE;
    }
    g_running = false;
    Log("XR: shut down");
}

// ---------------------------------------------------------------------------------------------
// One OpenXR frame, on the game's render thread inside Present (SteamVR's D3D11 runtime uses the
// immediate context in its frame calls, so they must not run on another thread).
// ---------------------------------------------------------------------------------------------
bool TickXR() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent_(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            Log("XR: session state %d", static_cast<int>(s->state));
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XRCHECK(xrBeginSession_(g_session, &bi));
                g_running = true;
                Log("XR: session running");
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession_(g_session);
                g_running = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                Log("XR: runtime is ending the session");
                g_running = false;
                return false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            auto* r = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            if (r->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                Log("XR: the runtime recentred the view; recentring the game camera and HUD with it");
                g_runtimeRecenterAt = GetTickCount64() + 250;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("XR: instance loss pending");
            return false;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (g_runtimeRecenterAt && GetTickCount64() >= g_runtimeRecenterAt) {
        g_runtimeRecenterAt = 0;
        XrRecenter();
    }
    if (!g_running) return true;

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XRCHECK(xrWaitFrame_(g_session, nullptr, &fs));
    if (fs.predictedDisplayPeriod > 0) g_displayHz.store(1e9f / static_cast<float>(fs.predictedDisplayPeriod));
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    const bool headValid = XR_SUCCEEDED(xrLocateSpace_(g_viewSpace, g_local, fs.predictedDisplayTime, &hl)) &&
                           (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (headValid) {
        const float q[4] = {hl.pose.orientation.x, hl.pose.orientation.y, hl.pose.orientation.z, hl.pose.orientation.w};
        const float p[3] = {hl.pose.position.x, hl.pose.position.y, hl.pose.position.z};
        CamHookSetHead(q, p);
    }
    XRCHECK(xrBeginFrame_(g_session, nullptr));

    if (!g_loggedViews) {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_viewSpace;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t cnt = 0;
        if (XR_SUCCEEDED(xrLocateViews_(g_session, &vli, &vs, 2, &cnt, v)) && cnt == 2) {
            // Headsets with angled (canted) displays, e.g. Pimax Crystal / 8KX on PimaxXR or SteamVR without
            // parallel projections, report each eye turned outward. The game renders both eyes looking
            // straight ahead, so turn each eye's field of view into head-forward angles (the runtime then
            // reprojects to the angled displays). Straight-ahead headsets have a yaw of 0 and are unchanged.
            float up = 0.f, down = 0.f;
            for (int e = 0; e < 2; ++e) {
                const XrQuaternionf& q = v[e].pose.orientation;
                const float yawLeft = std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y));
                const float pitchUp = std::asin(std::fmax(-1.f, std::fmin(1.f, 2.f * (q.w * q.x - q.y * q.z))));
                Log("XR: eye %d offset (%.4f, %.4f, %.4f) m, turned %.1f deg %s, fov L%.1f R%.1f U%.1f D%.1f deg", e,
                    v[e].pose.position.x, v[e].pose.position.y, v[e].pose.position.z, std::fabs(yawLeft) * kR2D,
                    yawLeft > 0.f ? "left" : "right", v[e].fov.angleLeft * kR2D, v[e].fov.angleRight * kR2D,
                    v[e].fov.angleUp * kR2D, v[e].fov.angleDown * kR2D);
                const float lim = 1.48f;  // about 85 degrees
                v[e].fov.angleLeft = std::fmax(-lim, v[e].fov.angleLeft - yawLeft);
                v[e].fov.angleRight = std::fmin(lim, v[e].fov.angleRight - yawLeft);
                v[e].fov.angleUp = std::fmin(lim, v[e].fov.angleUp + pitchUp);
                v[e].fov.angleDown = std::fmax(-lim, v[e].fov.angleDown + pitchUp);
                if (std::fabs(yawLeft) > 0.002f || std::fabs(pitchUp) > 0.002f)
                    Log("XR: eye %d angled display: head-forward fov L%.1f R%.1f U%.1f D%.1f deg", e, v[e].fov.angleLeft * kR2D,
                        v[e].fov.angleRight * kR2D, v[e].fov.angleUp * kR2D, v[e].fov.angleDown * kR2D);
                up = std::fmax(up, v[e].fov.angleUp);
                down = std::fmax(down, -v[e].fov.angleDown);
            }
            const float ipd = std::fabs(v[1].pose.position.x - v[0].pose.position.x);
            g_ipd = (ipd > 0.04f && ipd < 0.09f) ? ipd : 0.064f;
            Log("XR: IPD %.1f mm", 1000.f * ipd);
            // The game projection is symmetric, so cover the larger of the up and down angles.
            CamHookSetAutoFovY(2.f * std::fmax(up, down));
            unsigned rw = 0, rh = 0;
            const bool fixedSize = RenderSizeGet(&rw, &rh);
            if (fixedSize) CamHookSetAspect(static_cast<float>(rw) / static_cast<float>(rh));
            if (g_aspectMode != 0) {
                // Squeeze the game's view to the headset's shape: wide enough for the outer edge of
                // each eye after geo-11's sideways shift, plus a little margin.
                float outer = 0.f, inner = 0.f;
                for (int e = 0; e < 2; ++e) {
                    const float l = std::tan(-v[e].fov.angleLeft), r = std::tan(v[e].fov.angleRight);
                    outer = std::fmax(outer, e == 0 ? l : r);
                    inner = std::fmax(inner, e == 0 ? r : l);
                }
                const float S = g_shift.load();
                const float tanX = 1.03f * std::fmax(outer / (1.f - S), inner / (1.f + S));
                const float tanY = std::tan(std::fmax(up, down));
                {
                    wchar_t v[32];
                    swprintf(v, 32, L"%.4f", tanX / tanY);
                    WritePrivateProfileStringW(L"state", L"HeadsetAspect", v, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
                }
                Log("XR: headset-shaped render size: aspect %.3f, e.g. RenderWidth=%u RenderHeight=1440 or RenderWidth=%u RenderHeight=2160",
                    tanX / tanY, static_cast<unsigned>(1440.f * tanX / tanY + 1.f) & ~7u, static_cast<unsigned>(2160.f * tanX / tanY + 1.f) & ~7u);
                if (!fixedSize) CamHookSetAspect(tanX / tanY);
            }
            g_loggedViews = true;
        }
    }

    ++g_xrFrames;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastLog >= 10000) {
        g_lastLog = now;
        Log("XR: frame %llu, game camera last updated %llu ms ago", static_cast<unsigned long long>(g_xrFrames),
            CamHookMsSinceUpdate());
    }

    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[3] = {};
    uint32_t layerCount = 0;
    bool inGameView = false;

    ID3D11Texture2D* src = fs.shouldRender ? SharedStereoTexture() : nullptr;
    if (src && !g_scFailed) {
        // DLSS upscaling: the game renders smaller and each half of geo-11's image is upscaled to the headset size.
        unsigned upW = 0, upH = 0;
        const bool upscale = DlssUpscaling() && DlssOutputSize(&upW, &upH);
        const UINT W = upscale ? 2 * upW : g_kDesc.Width, H = upscale ? upH : g_kDesc.Height;
        if (g_sc == XR_NULL_HANDLE || W != g_scW || H != g_scH || g_kDesc.Format != g_scSrcFmt) {
            if (!CreateSwapchain(W, H, g_kDesc.Format)) g_scFailed = true;
        }
        if (g_sc != XR_NULL_HANDLE && W == g_scW && H == g_scH) {
            ID3D11DeviceContext* ctx = nullptr;
            g_device->GetImmediateContext(&ctx);
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XRCHECK(xrAcquireSwapchainImage_(g_sc, &ai, &idx));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XRCHECK(xrWaitSwapchainImage_(g_sc, &wi));
            const bool locked = !g_kMutex || SUCCEEDED(g_kMutex->AcquireSync(0, 20));
            if (ctx && locked) {
                if (upscale) {
                    ID3D11Texture2D* up[2] = {};
                    if (DlssUpscale(ctx, src, up)) {
                        for (UINT h = 0; h < 2; ++h) ctx->CopySubresourceRegion(g_scImgs[idx], 0, h * upW, 0, 0, up[h], 0, nullptr);
                        if (g_capture.load()) CaptureUpscaled(ctx, up, upW, upH);
                    }
                } else {
                    ctx->CopySubresourceRegion(g_scImgs[idx], 0, 0, 0, 0, src, 0, nullptr);
                }
                SharpenStereo(ctx, g_scImgs[idx], g_scFmt);  // off unless chosen in the menu
                if (g_capture.exchange(false)) CaptureStereo(ctx, src);
            }
            if (g_kMutex && locked) g_kMutex->ReleaseSync(0);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XRCHECK(xrReleaseSwapchainImage_(g_sc, &ri));
            if (ctx) ctx->Release();

            const int eyeW = static_cast<int>(W / 2);
            float hq[4], hp[3], fovY = 0.f;
            const bool inGame = CamHookMsSinceUpdate() < 500;
            if (inGame && CamHookAppliedPose(g_poseLag.load(std::memory_order_relaxed), hq, hp, &fovY) && fovY > 0.1f && fovY < 3.0f) {
                // The frame was rendered from this head pose with the game's own symmetric projection.
                // geo-11 shifts each eye's whole image sideways by the stereo shift (its separation at
                // infinity), so each eye's field of view is moved the same amount to cancel it.
                const float tanY = std::tan(fovY * 0.5f);
                float asp = 0.f;
                if (!CamHookAppliedAspect(g_poseLag.load(std::memory_order_relaxed), &asp) || asp <= 0.f) asp = static_cast<float>(eyeW) / static_cast<float>(H);
                const float tanX = tanY * asp;
                const float S = g_shift.load(std::memory_order_relaxed);
                const float rx = 1.f - 2.f * (hq[1] * hq[1] + hq[2] * hq[2]);
                const float ry = 2.f * (hq[0] * hq[1] + hq[2] * hq[3]);
                const float rz = 2.f * (hq[0] * hq[2] - hq[1] * hq[3]);
                const float half = 0.5f * g_ipd;
                const bool swap = g_swapEyes.load(std::memory_order_relaxed);
                for (int e = 0; e < 2; ++e) {
                    // Which half of geo-11's texture this eye sees. Measured: geo-11 puts the LEFT eye in the
                    // RIGHT half of the Katanga texture (LeftEyeHalf=1).
                    const int img = ((e == 0) ? g_leftHalf : 1 - g_leftHalf) ^ (swap ? 1 : 0);
                    // geo-11 slides the left eye's image left and the right eye's image right by S (NDC).
                    const float s = (e == 0) ? S : -S;
                    const float k = (e == 0) ? -half : half;
                    pv[e].pose.orientation = {hq[0], hq[1], hq[2], hq[3]};
                    pv[e].pose.position = {hp[0] + k * rx, hp[1] + k * ry, hp[2] + k * rz};
                    pv[e].fov.angleLeft = std::atan((-1.f + s) * tanX);
                    pv[e].fov.angleRight = std::atan((1.f + s) * tanX);
                    pv[e].fov.angleUp = std::atan(tanY);
                    pv[e].fov.angleDown = -std::atan(tanY);
                    pv[e].subImage.swapchain = g_sc;
                    pv[e].subImage.imageRect.offset = {img * eyeW, 0};
                    pv[e].subImage.imageRect.extent = {eyeW, static_cast<int32_t>(H)};
                    pv[e].subImage.imageArrayIndex = 0;
                }
                proj.space = g_local;
                proj.viewCount = 2;
                proj.views = pv;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                layerCount = 1;
                inGameView = true;
                g_havePlacement = false;  // the menu screen goes in front of the head next time
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    Log("XR: in game: stereo view, game fov %.1f x %.1f deg per eye, pose lag %d", 2.f * std::atan(tanX) * kR2D,
                        fovY * kR2D, g_poseLag.load());
                    const float P00 = 1.f / tanX;
                    const float d = 0.5f * g_ipd * g_worldScale;
                    if (S > 0.f)
                        Log("XR: for WorldScale %.2f with stereo shift %.4f, geo-11 needs dm_convergence = %.3f (at dm_separation = 100)",
                            g_worldScale, S, P00 * d / S);
                }
            } else {
                // Menus and loading screens: the left eye on a flat screen in front of the player.
                if ((!g_havePlacement || g_recenter.exchange(false)) && headValid) PlaceScreen(hl.pose);
                if (g_havePlacement) {
                    quad.space = g_local;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = g_sc;
                    quad.subImage.imageRect.offset = {g_leftHalf * eyeW, 0};
                    quad.subImage.imageRect.extent = {eyeW, static_cast<int32_t>(H)};
                    quad.pose = g_screenPose;
                    const float width = 3.2f;  // metres; 2.5 m away gives about a 65 degree wide screen
                    quad.size = {width, width * static_cast<float>(H) / static_cast<float>(eyeW)};
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                    layerCount = 1;
                }
            }
        }
    }

    // HUD panel on top of the game.
    if (layerCount == 1 && UpdateHud()) {
        const int mode = g_hudMode.load(std::memory_order_relaxed);
        hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hudQuad.subImage.swapchain = g_hudSc;
        hudQuad.subImage.imageRect.offset = {0, 0};
        // Only the part the game draws into (the render size; the texture may be larger).
        const int32_t cw = std::min<int32_t>(static_cast<int32_t>(g_hudW), static_cast<int32_t>(g_kDesc.Width / 2));
        const int32_t ch = std::min<int32_t>(static_cast<int32_t>(g_hudH), static_cast<int32_t>(g_kDesc.Height));
        hudQuad.subImage.imageRect.extent = {cw, ch};
        const float aspect = static_cast<float>(ch) / static_cast<float>(cw);
        bool show = false;
        if (!inGameView) {
            // Menus: the HUD texture holds the menus; lay it exactly over the flat screen.
            hudQuad.space = g_local;
            hudQuad.pose = g_screenPose;
            hudQuad.size = quad.size;
            show = true;
            g_hudPlaced = false;
        } else if (mode == 0) {
            if ((!g_hudPlaced || g_hudRecenter.exchange(false)) && headValid) PlaceHud(hl.pose);
            hudQuad.space = g_local;
            hudQuad.pose = g_hudPose;
            hudQuad.size = {g_hudWidth, g_hudWidth * aspect};
            show = g_hudPlaced;
        } else if (mode == 1) {
            hudQuad.space = g_viewSpace;
            hudQuad.pose.orientation = {0.f, 0.f, 0.f, 1.f};
            hudQuad.pose.position = {0.f, -g_hudDrop, -g_hudDist};
            hudQuad.size = {g_hudWidth, g_hudWidth * aspect};
            show = true;
        }
        if (show) layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
    }

    // Mod menu on top of everything while it is open.
    if (layerCount >= 1 && UpdateMenuPanel()) {
        const unsigned gen = MenuOpenGeneration();
        if (gen != g_menuPlacedGen && headValid) {
            PlaceMenu(hl.pose);
            g_menuPlacedGen = gen;
        }
        if (g_menuPlacedGen == gen) {
            menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            menuQuad.subImage.swapchain = g_menuSc;
            menuQuad.subImage.imageRect.offset = {0, 0};
            // Only the part of the texture the menu uses: the panel is as tall as the menu, nothing scrolls.
            const UINT mh = std::min<UINT>(MenuContentHeight(), g_menuH);
            menuQuad.subImage.imageRect.extent = {static_cast<int32_t>(g_menuW), static_cast<int32_t>(mh)};
            menuQuad.space = g_local;
            menuQuad.pose = g_menuPose;
            menuQuad.size = {kMenuWidth, kMenuWidth * static_cast<float>(mh) / static_cast<float>(g_menuW)};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    XRCHECK(xrEndFrame_(g_session, &fei));
    return true;
}

} // namespace

// Early, before the game has a window: ask the runtime what per-eye size it wants, without a session.
bool XrQueryRecommendedHeight(unsigned* height, unsigned* width) {
    if (!LoadRuntime()) return false;
    PFN_xrCreateInstance create = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumViews = nullptr;
    if (!Get(XR_NULL_HANDLE, "xrCreateInstance", create)) return false;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "ds3vr");
    strcpy_s(ici.applicationInfo.engineName, "ds3vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(create(&ici, &inst))) return false;
    bool ok = false;
    if (Get(inst, "xrDestroyInstance", destroy) && Get(inst, "xrGetSystem", getSystem) &&
        Get(inst, "xrEnumerateViewConfigurationViews", enumViews)) {
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t n = 0;
        if (XR_SUCCEEDED(getSystem(inst, &sgi, &sys)) &&
            XR_SUCCEEDED(enumViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) && n >= 1) {
            *height = v[0].recommendedImageRectHeight & ~1u;
            if (width) *width = v[0].recommendedImageRectWidth;
            ok = true;
        }
    }
    if (destroy) destroy(inst);
    return ok;
}

// The headset's real shape, read in a short OpenXR session before the game starts (rundll32 helper only,
// used on a fresh install, where ds3vr.ini has no HeadsetAspect yet). The aspect is the one the game's
// view is fitted to later (the same formula as the in-game code that saves HeadsetAspect). The
// recommended rectangle is NOT the same shape (on Steve's Dream Air 4336x3772 recommended, but the
// fitted view is 1.298), so the field of view is what counts. Stereo shift: the game's own value is not
// known yet, so the usual value for this setup is used (about 2% different from the in-game result).
bool XrQueryHeadsetShape(unsigned* height, unsigned* width, float* aspect) {
    if (!LoadRuntime()) return false;
    typedef HRESULT(WINAPI * PFN_D3D11CreateDevice_)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                     UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    PFN_D3D11CreateDevice_ createDevice = d3d ? reinterpret_cast<PFN_D3D11CreateDevice_>(GetProcAddress(d3d, "D3D11CreateDevice")) : nullptr;
    if (!createDevice) return false;
    ID3D11Device* dev = nullptr;
    if (FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &dev, nullptr, nullptr)) ||
        !dev)
        return false;

    PFN_xrCreateInstance create = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumViews = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrCreateReferenceSpace createSpace = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    if (!Get(XR_NULL_HANDLE, "xrCreateInstance", create)) {
        dev->Release();
        return false;
    }
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "ds3vr");
    strcpy_s(ici.applicationInfo.engineName, "ds3vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    XrInstance inst = XR_NULL_HANDLE;
    bool ok = false;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    bool begun = false;
    if (XR_SUCCEEDED(create(&ici, &inst)) &&
        Get(inst, "xrDestroyInstance", destroy) && Get(inst, "xrGetSystem", getSystem) &&
        Get(inst, "xrEnumerateViewConfigurationViews", enumViews) &&
        Get(inst, "xrGetD3D11GraphicsRequirementsKHR", getReq) && Get(inst, "xrCreateSession", createSession) &&
        Get(inst, "xrDestroySession", destroySession) && Get(inst, "xrCreateReferenceSpace", createSpace) &&
        Get(inst, "xrPollEvent", pollEvent) && Get(inst, "xrBeginSession", beginSession) &&
        Get(inst, "xrEndSession", endSession) && Get(inst, "xrWaitFrame", waitFrame) &&
        Get(inst, "xrLocateViews", locateViews)) {
        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (XR_SUCCEEDED(getSystem(inst, &sgi, &sys)) && XR_SUCCEEDED(getReq(inst, sys, &req))) {
            XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            gb.device = dev;
            XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
            sci.next = &gb;
            sci.systemId = sys;
            XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            uint32_t n = 0;
            if (XR_SUCCEEDED(enumViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) && n >= 1 &&
                XR_SUCCEEDED(createSession(inst, &sci, &session))) {
                XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                rsci.poseInReferenceSpace.orientation.w = 1.f;
                rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                if (XR_SUCCEEDED(createSpace(session, &rsci, &space))) {
                    // Wait for the runtime to be ready (it can take a few seconds when it has just started), then
                    // read the field of view. Gives up after 15 s; the caller then keeps its defaults.
                    const ULONGLONG t0 = GetTickCount64();
                    bool exiting = false;
                    while (!ok && !exiting && GetTickCount64() - t0 < 15000) {
                        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
                        while (pollEvent(inst, &ev) == XR_SUCCESS) {
                            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                                const XrSessionState st = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                                if (st == XR_SESSION_STATE_READY && !begun) {
                                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                                    if (XR_SUCCEEDED(beginSession(session, &bi))) begun = true;
                                }
                                if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) exiting = true;
                            }
                            ev = {XR_TYPE_EVENT_DATA_BUFFER};
                        }
                        if (!begun || exiting) {
                            Sleep(100);
                            continue;
                        }
                        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
                        XrFrameState fs{XR_TYPE_FRAME_STATE};
                        if (XR_FAILED(waitFrame(session, &fwi, &fs))) break;
                        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
                        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        vli.displayTime = fs.predictedDisplayTime;
                        vli.space = space;
                        XrViewState vs{XR_TYPE_VIEW_STATE};
                        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                        uint32_t cnt = 0;
                        if (XR_SUCCEEDED(locateViews(session, &vli, &vs, 2, &cnt, views)) && cnt == 2 &&
                            views[0].fov.angleRight > views[0].fov.angleLeft) {
                            // The same formula as the in-game view fit (see XrRender's HeadsetAspect code).
                            // geo-11 has not run in this process, so its shift is unknown here: use the value
                            // measured on Steve's setup (0.0209). Zero would make the aspect about 2% too low.
                            const float S = 0.0209f;
                            float outer = 0.f, inner = 0.f, up = 0.f, down = 0.f;
                            for (int e = 0; e < 2; ++e) {
                                const float l = std::tan(-views[e].fov.angleLeft), r = std::tan(views[e].fov.angleRight);
                                outer = std::fmax(outer, e == 0 ? l : r);
                                inner = std::fmax(inner, e == 0 ? r : l);
                                up = std::fmax(up, views[e].fov.angleUp);
                                down = std::fmax(down, -views[e].fov.angleDown);
                            }
                            const float tanX = 1.03f * std::fmax(outer / (1.f - S), inner / (1.f + S));
                            const float tanY = std::tan(std::fmax(up, down));
                            *aspect = tanX / tanY;
                            XrViewConfigurationView rv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                            uint32_t rn = 0;
                            if (XR_SUCCEEDED(enumViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &rn, rv)) && rn >= 1) {
                                *height = rv[0].recommendedImageRectHeight & ~1u;
                                if (width) *width = rv[0].recommendedImageRectWidth;
                            }
                            ok = *aspect > 0.f && *height > 0;
                        }
                        if (!ok) Sleep(50);
                    }
                }
            }
        }
    }
    if (begun && endSession) endSession(session);
    if (session != XR_NULL_HANDLE && destroySession) destroySession(session);
    if (inst != XR_NULL_HANDLE && destroy) destroy(inst);
    dev->Release();
    return ok;
}

bool XrRecommendedNow(unsigned* w, unsigned* h) {
    if (g_inst == XR_NULL_HANDLE || g_sys == XR_NULL_SYSTEM_ID || !xrEnumerateViewConfigurationViews_) return false;
    XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews_(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, v)) || n < 1)
        return false;
    *w = v[0].recommendedImageRectWidth;
    *h = v[0].recommendedImageRectHeight & ~1u;
    return *h > 0;
}

bool XrEyeImageSize(unsigned* w, unsigned* h) {
    if (!g_kDesc.Width || !g_kDesc.Height) return false;
    *w = g_kDesc.Width / 2;
    *h = g_kDesc.Height;
    return true;
}

void XrSetMeasuredShift(float s, float convergence, float eye) {
    static bool first = true;
    // geo-11's convergence (read about every 5 s): logged when it changes, to check world-scale steps arrive.
    static float lastConv = 0.f;
    if (!first && std::fabs(convergence - lastConv) > 0.002f)
        Log("STEREO: geo-11 convergence now %.3f (world scale %.2f)", convergence, convergence / 1.4834f);
    lastConv = convergence;
    const float cur = g_shift.load();
    static float lastLogged = -1.f;
    if (!first && std::fabs(s - lastLogged) < 0.0002f) return;
    lastLogged = s;
    if (g_shiftOverride) {
        if (first) Log("STEREO: geo-11's separation reads %.4f; StereoShiftOverride %.4f in ds3vr.ini is used instead", s, cur);
        first = false;
        return;
    }
    Log("STEREO: geo-11's separation is %.4f (convergence %.3f); the views now cancel it (was %.4f)", s, convergence, cur);
    (void)eye;
    first = false;
    g_shift.store(s);
}

int XrGetPoseLag() { return g_poseLag.load(); }

void XrSetPoseLag(int lag) {
    if (lag < 0) lag = 0;
    if (lag > 4) lag = 4;
    g_poseLag.store(lag);
    HudSetPoseLag(lag);
    wchar_t v[16];
    swprintf(v, 16, L"%d", lag);
    WritePrivateProfileStringW(L"ds3vr", L"PoseLag", v, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
    Log("menu: pose lag %d", lag);
}

void XrSwapEyes() {
    const bool s = !g_swapEyes.load();
    g_swapEyes.store(s);
    Log("F7: eyes %s", s ? "SWAPPED" : "normal");
}

void XrStepShift(float delta) {
    float s = g_shift.load() + delta;
    if (s < -0.3f) s = -0.3f;
    if (s > 0.3f) s = 0.3f;
    g_shift.store(s);
    Log("stereo shift now %.4f (StereoShift=%.4f in ds3vr.ini keeps it)", s, s);
}

float XrDisplayHz() { return g_running ? g_displayHz.load() : 0.f; }

void XrRequestCapture() { g_capture.store(true); }

void SaveSetting(const wchar_t* key, const wchar_t* value) {
    const std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    WritePrivateProfileStringW(L"ds3vr", key, value, ini.c_str());
}

void XrCycleHudMode() {
    const int m = (g_hudMode.load() + 1) % 3;
    g_hudMode.store(m);
    g_hudRecenter.store(true);
    wchar_t v[8];
    swprintf(v, 8, L"%d", m);
    SaveSetting(L"HudMode", v);
    Log("F7: HUD %s (saved)", m == 0 ? "fixed in front of you" : m == 1 ? "follows your head" : "hidden");
}

void XrCycleHudSize() {
    // Panel widths in metres at the same distance; the panel keeps the screen's shape.
    static const float kSizes[] = {0.8f, 1.0f, 1.2f, 1.4f, 1.6f, 1.8f, 2.0f};
    constexpr int kCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));
    int next = 0;
    for (int i = 0; i < kCount; ++i)
        if (kSizes[i] > g_hudWidth + 0.01f) {
            next = i;
            break;
        }
    if (kSizes[kCount - 1] <= g_hudWidth + 0.01f) next = 0;
    g_hudWidth = kSizes[next];
    wchar_t v[16];
    swprintf(v, 16, L"%.1f", g_hudWidth);
    SaveSetting(L"HudWidth", v);
    Log("F6: HUD size %.1f m wide at %.2f m (saved)", g_hudWidth, g_hudDist);
}

HudSettings XrGetHudSettings() { return HudSettings{g_hudMode.load(), g_hudWidth, g_hudDist, g_hudDrop}; }

void XrSetHudSettings(const HudSettings& h, bool save) {
    const bool modeChanged = h.mode != g_hudMode.load();
    g_hudMode.store(((h.mode % 3) + 3) % 3);
    g_hudWidth = std::min(std::max(h.width, 0.3f), 4.f);
    g_hudDist = std::min(std::max(h.distance, 0.4f), 5.f);
    g_hudDrop = std::min(std::max(h.drop, -1.f), 1.5f);
    g_hudRecenter.store(true);  // re-place the panel with the new distance / height
    if (!save) return;
    wchar_t v[16];
    swprintf(v, 16, L"%d", g_hudMode.load());
    SaveSetting(L"HudMode", v);
    swprintf(v, 16, L"%.2f", g_hudWidth);
    SaveSetting(L"HudWidth", v);
    swprintf(v, 16, L"%.2f", g_hudDist);
    SaveSetting(L"HudDistance", v);
    swprintf(v, 16, L"%.2f", g_hudDrop);
    SaveSetting(L"HudDrop", v);
    Log("menu: HUD %s, %.2f m wide, %.2f m away, %.2f m below eye level (saved)",
        g_hudMode.load() == 0 ? "fixed" : g_hudMode.load() == 1 ? "follows head" : "hidden", g_hudWidth, g_hudDist, g_hudDrop);
    (void)modeChanged;
}

void XrRecenter() {
    g_recenter.store(true);
    g_hudRecenter.store(true);
    CamHookRecenter();
}

void XrRequestShutdown() { g_shutdownReq.store(true); }

void XrShutdownNow() {
    // Never block the window thread for long: the render thread might be waiting on it.
    g_shutdownReq.store(true);
    for (int i = 0; i < 100; ++i) {
        if (g_xrMutex.try_lock()) {
            Shutdown("game window destroyed");
            g_xrMutex.unlock();
            return;
        }
        Sleep(10);
    }
    Log("XR: could not shut down from the window thread (render thread busy)");
}

void XrOnPresent(ID3D11Device* device, IDXGISwapChain*) {
    std::lock_guard<std::recursive_mutex> lock(g_xrMutex);
    if (g_dead) return;
    if (g_shutdownReq.load()) {
        Shutdown("game window closing");
        return;
    }
    if (!g_started) {
        g_started = true;
        device->AddRef();
        g_device = device;
        Log("XR: starting (milestone G1: geo-11 stereo)");
        if (!InitXR()) {
            Log("XR: init failed, OpenXR disabled for this run (the game keeps running normally)");
            Shutdown("init failed");
        }
        return;
    }
    if (!TickXR()) Shutdown("runtime stopped");
}

} // namespace ds3vr
