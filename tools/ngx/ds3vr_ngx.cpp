// ds3vr_ngx.dll: DS3 VR's helper around NVIDIA's NGX library (DLSS SDK, nvsdk_ngx_s.lib).
// Built with Visual Studio (.github/workflows/ngx.yml), because NVIDIA ships that library for Visual
// Studio only; the mod (built with mingw) loads this dll and calls the functions in src/ngx_api.h.
// Uses NVIDIA's DLSS SDK headers and library (NVIDIA RTX SDKs license), fetched at build time.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#include "../../src/ngx_api.h"

#define DS3VR_NGX_EXPORT extern "C" __declspec(dllexport)

namespace {

// DS3 VR's project id for NGX (NVIDIA: apps without an NVIDIA id use a project id with the custom engine type).
const char* const kProjectId = "6c1a8f0e-3b5d-4e7a-9a2c-d53f0e7b4a91";

NVSDK_NGX_Parameter* g_caps = nullptr;  // capability parameters, valid between Init and Shutdown
bool g_inited = false;

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
};

void Narrow(const wchar_t* w, char* out, int len) {
    if (!out || len <= 0) return;
    int i = 0;
    for (; w && w[i] && i < len - 1; ++i) out[i] = (w[i] < 128) ? static_cast<char>(w[i]) : '?';
    out[i] = 0;
}

void Msg(char* msg, int len, const char* text, NVSDK_NGX_Result r) {
    if (!msg || len <= 0) return;
    char name[160];
    Narrow(GetNGXResultAsString(r), name, sizeof(name));
    snprintf(msg, static_cast<size_t>(len), "%s: %s (0x%08x)", text, name, static_cast<unsigned>(r));
}

} // namespace

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_ApiVersion(void) { return DS3VR_NGX_API_VERSION; }

DS3VR_NGX_EXPORT const char* __cdecl Ds3vrNgx_ResultString(int result) {
    static char buf[160];
    Narrow(GetNGXResultAsString(static_cast<NVSDK_NGX_Result>(result)), buf, sizeof(buf));
    return buf;
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_Init(ID3D11Device* device, const wchar_t* dataPath, const wchar_t* const* featureDirs,
                                           int featureDirCount, char* msg, int msgLen) {
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = featureDirs;
    info.PathListInfo.Length = featureDirCount > 0 ? static_cast<unsigned>(featureDirCount) : 0u;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "2.0", dataPath, device,
                                                             &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        Msg(msg, msgLen, "NGX init", r);
        return static_cast<int>(r);
    }
    g_inited = true;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
    Msg(msg, msgLen, NVSDK_NGX_FAILED(r) ? "NGX started, capability query" : "NGX started", r);
    return static_cast<int>(r);
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_DlssAvailable(int* available, int* needsDriver, unsigned* minDriverMajor,
                                                    unsigned* minDriverMinor, char* msg, int msgLen) {
    if (!g_caps) {
        if (msg && msgLen > 0) snprintf(msg, static_cast<size_t>(msgLen), "NGX not started");
        return static_cast<int>(NVSDK_NGX_Result_FAIL_NotInitialized);
    }
    int a = 0, n = 0;
    unsigned maj = 0, min = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &a);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &n);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &maj);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min);
    int initResult = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
    if (available) *available = a;
    if (needsDriver) *needsDriver = n;
    if (minDriverMajor) *minDriverMajor = maj;
    if (minDriverMinor) *minDriverMinor = min;
    if (msg && msgLen > 0) {
        char name[160];
        Narrow(GetNGXResultAsString(static_cast<NVSDK_NGX_Result>(initResult)), name, sizeof(name));
        snprintf(msg, static_cast<size_t>(msgLen), "DLSS available %d, needs newer driver %d (minimum %u.%u), feature check: %s", a, n,
                 maj, min, name);
    }
    return static_cast<int>(NVSDK_NGX_Result_Success);
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_GetOptimal(unsigned outW, unsigned outH, int quality, unsigned* renderW, unsigned* renderH,
                                                 float* sharpness) {
    if (!g_caps) return static_cast<int>(NVSDK_NGX_Result_FAIL_NotInitialized);
    unsigned w = 0, h = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float s = 0.f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, outW, outH, static_cast<NVSDK_NGX_PerfQuality_Value>(quality),
                                                            &w, &h, &maxW, &maxH, &minW, &minH, &s);
    if (renderW) *renderW = w;
    if (renderH) *renderH = h;
    if (sharpness) *sharpness = s;
    return static_cast<int>(r);
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_CreateDlss(ID3D11DeviceContext* ctx, unsigned inW, unsigned inH, unsigned outW, unsigned outH,
                                                 int quality, int createFlags, int preset, void** handle) {
    if (handle) *handle = nullptr;
    if (!g_inited || !ctx || !handle) return static_cast<int>(NVSDK_NGX_Result_FAIL_NotInitialized);
    Feature* f = new Feature;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_AllocateParameters(&f->params);
    if (NVSDK_NGX_FAILED(r)) {
        delete f;
        return static_cast<int>(r);
    }
    if (preset > 0) {
        // The same preset for every quality mode, so whichever mode NGX applies uses it.
        const char* const names[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
        for (const char* n : names) NVSDK_NGX_Parameter_SetUI(f->params, n, static_cast<unsigned>(preset));
    }
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = inW;
    cp.Feature.InHeight = inH;
    cp.Feature.InTargetWidth = outW;
    cp.Feature.InTargetHeight = outH;
    cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(quality);
    cp.InFeatureCreateFlags = createFlags;
    cp.InEnableOutputSubrects = false;
    r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &f->handle, f->params, &cp);
    if (NVSDK_NGX_FAILED(r)) {
        NVSDK_NGX_D3D11_DestroyParameters(f->params);
        delete f;
        return static_cast<int>(r);
    }
    *handle = f;
    return static_cast<int>(r);
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_EvaluateDlss(ID3D11DeviceContext* ctx, void* handle, const Ds3vrNgxEval* e) {
    Feature* f = static_cast<Feature*>(handle);
    if (!f || !f->handle || !ctx || !e) return static_cast<int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
    NVSDK_NGX_D3D11_DLSS_Eval_Params p = {};
    p.Feature.pInColor = e->color;
    p.Feature.pInOutput = e->output;
    p.Feature.InSharpness = e->sharpness;
    p.pInDepth = e->depth;
    p.pInMotionVectors = e->motion;
    p.pInExposureTexture = e->exposure;
    p.pInBiasCurrentColorMask = e->reactive;
    p.InJitterOffsetX = e->jitterX;
    p.InJitterOffsetY = e->jitterY;
    p.InMVScaleX = e->mvScaleX;
    p.InMVScaleY = e->mvScaleY;
    p.InReset = e->reset;
    p.InRenderSubrectDimensions.Width = e->renderW;
    p.InRenderSubrectDimensions.Height = e->renderH;
    p.InColorSubrectBase.X = e->colorX;
    p.InColorSubrectBase.Y = e->colorY;
    p.InDepthSubrectBase.X = e->depthX;
    p.InDepthSubrectBase.Y = e->depthY;
    p.InMVSubrectBase.X = e->motionX;
    p.InMVSubrectBase.Y = e->motionY;
    p.InOutputSubrectBase.X = e->outX;
    p.InOutputSubrectBase.Y = e->outY;
    p.InPreExposure = e->preExposure;
    p.InFrameTimeDeltaInMsec = e->frameTimeMs;
    return static_cast<int>(NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f->handle, f->params, &p));
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_ReleaseDlss(void* handle) {
    Feature* f = static_cast<Feature*>(handle);
    if (!f) return static_cast<int>(NVSDK_NGX_Result_Success);
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    if (f->handle) r = NVSDK_NGX_D3D11_ReleaseFeature(f->handle);
    if (f->params) NVSDK_NGX_D3D11_DestroyParameters(f->params);
    delete f;
    return static_cast<int>(r);
}

DS3VR_NGX_EXPORT int __cdecl Ds3vrNgx_Shutdown(ID3D11Device* device) {
    if (!g_inited) return static_cast<int>(NVSDK_NGX_Result_Success);
    if (g_caps) NVSDK_NGX_D3D11_DestroyParameters(g_caps);
    g_caps = nullptr;
    g_inited = false;
    return static_cast<int>(NVSDK_NGX_D3D11_Shutdown1(device));
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
