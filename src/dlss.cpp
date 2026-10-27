#include "dlss.h"

#include <windows.h>
#include <d3d11.h>

#include <string>

#include "log.h"
#include "ngx_api.h"

namespace ds3vr {
namespace {

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

void SelfTest(ID3D11Device* dev) {
    Log("DLSS self-test: device %p (methods in %ls)", static_cast<void*>(dev), VtableModule(dev).c_str());
    if (!LoadHelper()) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    Log("DLSS self-test: immediate context %p (methods in %ls)", static_cast<void*>(ctx), VtableModule(ctx).c_str());
    const std::wstring data = Dir() + L"ds3vr";
    const std::wstring dir1 = Dir() + L"ds3vr", dir2 = Dir().substr(0, Dir().size() - 1);
    const wchar_t* dirs[2] = {dir1.c_str(), dir2.c_str()};  // where nvngx_dlss.dll may be: the ds3vr folder, then the Game folder
    char msg[512] = {};
    int r = g_ngx.Init(dev, data.c_str(), dirs, 2, msg, sizeof(msg));
    Log("DLSS self-test: %s", msg);
    if (r != 1) {
        if (ctx) ctx->Release();
        return;
    }
    int avail = 0, needsDriver = 0;
    unsigned maj = 0, min = 0;
    g_ngx.DlssAvailable(&avail, &needsDriver, &maj, &min, msg, sizeof(msg));
    Log("DLSS self-test: %s", msg);
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
        r = g_ngx.CreateDlss(ctx, 256, 256, 256, 256, DS3VR_NGX_DLAA, DS3VR_NGX_FLAG_MV_LOW_RES | DS3VR_NGX_FLAG_DEPTH_INVERTED, 0,
                             &feature);
        Log("DLSS self-test: create a 256x256 DLAA feature: %s", g_ngx.ResultString(r));
        if (feature) {
            r = g_ngx.ReleaseDlss(feature);
            Log("DLSS self-test: release it: %s", g_ngx.ResultString(r));
        }
    }
    r = g_ngx.Shutdown(dev);
    Log("DLSS self-test: NGX shut down: %s. Self-test finished.", g_ngx.ResultString(r));
    if (ctx) ctx->Release();
}

} // namespace

void DlssOnPresent(ID3D11Device* device) {
    static int frames = 0;
    static bool done = false;
    if (done || !device) return;
    if (++frames < 300) return;  // let the game and VR start first (about 5 s at 60 fps)
    done = true;
    if (GetPrivateProfileIntW(L"ds3vr", L"DlssSelfTest", 0, (Dir() + L"ds3vr.ini").c_str()) == 1) SelfTest(device);
}

} // namespace ds3vr
