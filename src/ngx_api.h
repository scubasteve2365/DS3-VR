#pragma once
// The C interface of ds3vr_ngx.dll, the small helper that links NVIDIA's NGX library (DLSS SDK).
// NVIDIA ships that library for Visual Studio only, so the helper is built with Visual Studio
// (tools/ngx, .github/workflows/ngx.yml) and the mod (mingw) calls these plain C functions.
// Shared by both sides; plain types and D3D11 COM pointers only.

#include <d3d11.h>

#define DS3VR_NGX_API_VERSION 1

// DLSS quality modes (NVIDIA's NVSDK_NGX_PerfQuality_Value numbers).
enum Ds3vrNgxQuality {
    DS3VR_NGX_PERFORMANCE = 0,
    DS3VR_NGX_BALANCED = 1,
    DS3VR_NGX_QUALITY = 2,
    DS3VR_NGX_ULTRA_PERFORMANCE = 3,
    DS3VR_NGX_ULTRA_QUALITY = 4,
    DS3VR_NGX_DLAA = 5,
};

// Create flags (NVIDIA's NVSDK_NGX_DLSS_Feature_Flags bits).
enum Ds3vrNgxCreateFlags {
    DS3VR_NGX_FLAG_IS_HDR = 1 << 0,
    DS3VR_NGX_FLAG_MV_LOW_RES = 1 << 1,
    DS3VR_NGX_FLAG_MV_JITTERED = 1 << 2,
    DS3VR_NGX_FLAG_DEPTH_INVERTED = 1 << 3,
    DS3VR_NGX_FLAG_AUTO_EXPOSURE = 1 << 6,
};

// One DLSS evaluation. Resources may be one array slice of a texture each (geo-11 stereo): the helper
// is given the views the mod made, so it never needs to know.
struct Ds3vrNgxEval {
    ID3D11Resource* color;          // the frame before anti-aliasing (render size)
    ID3D11Resource* output;         // where DLSS writes (output size); may be the same size as color
    ID3D11Resource* depth;          // depth (render size)
    ID3D11Resource* motion;         // motion vectors in pixels (render size), or nullptr = none
    ID3D11Resource* exposure;       // 1x1 exposure, or nullptr
    ID3D11Resource* reactive;       // bias-current-colour mask (effects), or nullptr
    float jitterX, jitterY;         // this frame's sub-pixel jitter, in render pixels
    float mvScaleX, mvScaleY;       // motion vector scale (0 = 1)
    unsigned renderW, renderH;      // the area of color/depth/motion used
    unsigned colorX, colorY;        // its corner in color
    unsigned depthX, depthY;        // ... in depth
    unsigned motionX, motionY;      // ... in motion
    unsigned outX, outY;            // the corner written in output
    float sharpness;                // 0 = off
    float preExposure;              // 0 = 1
    float frameTimeMs;              // time since the last frame
    int reset;                      // 1 = drop the history (camera cut, or first frame)
};

extern "C" {
typedef int(__cdecl* PFN_Ds3vrNgx_ApiVersion)(void);
// NVIDIA result codes (NVSDK_NGX_Result): 1 = success. msg gets a readable line.
typedef int(__cdecl* PFN_Ds3vrNgx_Init)(ID3D11Device* device, const wchar_t* dataPath, const wchar_t* const* featureDirs,
                                         int featureDirCount, char* msg, int msgLen);
typedef int(__cdecl* PFN_Ds3vrNgx_DlssAvailable)(int* available, int* needsDriver, unsigned* minDriverMajor,
                                                  unsigned* minDriverMinor, char* msg, int msgLen);
typedef int(__cdecl* PFN_Ds3vrNgx_GetOptimal)(unsigned outW, unsigned outH, int quality, unsigned* renderW,
                                               unsigned* renderH, float* sharpness);
// preset: 0 = default, otherwise NVIDIA's render preset number for the chosen quality.
typedef int(__cdecl* PFN_Ds3vrNgx_CreateDlss)(ID3D11DeviceContext* ctx, unsigned inW, unsigned inH, unsigned outW,
                                               unsigned outH, int quality, int createFlags, int preset, void** handle);
typedef int(__cdecl* PFN_Ds3vrNgx_EvaluateDlss)(ID3D11DeviceContext* ctx, void* handle, const Ds3vrNgxEval* eval);
typedef int(__cdecl* PFN_Ds3vrNgx_ReleaseDlss)(void* handle);
typedef int(__cdecl* PFN_Ds3vrNgx_Shutdown)(ID3D11Device* device);
// Readable name of an NVIDIA result code.
typedef const char*(__cdecl* PFN_Ds3vrNgx_ResultString)(int result);
}
