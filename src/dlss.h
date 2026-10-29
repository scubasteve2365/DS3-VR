#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Texture2D;

namespace ds3vr {

// DLSS through NVIDIA's NGX, via the ds3vr_ngx.dll helper (src/ngx_api.h): DLAA (anti-aliasing at the
// render size) or DLSS upscaling (the game renders smaller, each eye is upscaled to the headset size).
//
// How it fits DS3 under geo-11 (see the project's DS3VR/DLAA_FRAME_MAP.md):
// - Jitter: DS3's camera constants (cbSceneParam, register b8) are found where the game binds them and
//   moved by a sub-pixel amount each frame as the game uploads them (projection and view-projection).
// - Scene colour: the HDR target the game lights the scene into (RGBA16F, both eyes as a 2-slice array,
//   drawn with the main depth buffer). Main depth: the 2-slice R32G8X24 buffer drawn with it.
// - DLAA: each eye's slice is copied out, camera motion vectors are made from depth and the current /
//   previous camera (undoing geo-11's per-eye shift), DLSS runs (one feature per eye) and the result is
//   copied back, just before the game's first post-processing pass reads the scene colour.
// - Upscaling (path A in DS3VR/HANDOFF_DLSS.md): at that same point each eye's depth and motion vectors
//   are saved. At Present, each eye of geo-11's finished image (after post-processing, without the HUD,
//   which has its own panel) is upscaled by DLSS to the headset size, and that goes to the headset.
//   The render size is set at launch (rendersize.cpp), so an upscaling mode applies at the next launch.
//
// Modes (VR menu, saved as ds3vr.ini DlssMode).
enum DlssMode {
    kDlssOff = 0,
    kDlssDlaa = 1,
    kDlssQuality = 2,
    kDlssBalanced = 3,
    kDlssPerformance = 4,
    kDlssUltraPerformance = 5,
    kDlssModeCount = 6,
};
const char* DlssModeName(int mode);
float DlssModeScale(int mode);    // render size per axis for the mode (1 for Off and DLAA)

// Called from DllMain (rendersize.cpp), before the game reads its settings: the upscaling mode this run
// renders for (0 = none). Read once from ds3vr.ini: DlssMode, and DLSS known to work on this PC.
int DlssLaunchUpscaleMode();
float DlssLaunchScale();          // DlssModeScale(DlssLaunchUpscaleMode())

int DlssModeSaved();              // the menu's choice (what the next launch uses)
int DlssModeNow();                // what runs now (Off / DLAA switch live; upscaling is fixed at launch)
void DlssSetMode(int mode);       // from the menu: saved; Off / DLAA also applied now when no upscaling runs
int DlssAvailable();              // -1 not known yet, 0 no (not an NVIDIA RTX card, old driver...), 1 yes
const char* DlssStatus();         // one short line for the menu

void DlssOnPresent(ID3D11Device* device);  // every Present, on the render thread

// Upscaling at the copy to the headset (xr.cpp). DlssUpscaling: this run upscales (DLSS started fine).
// DlssUpscale: upscale both halves of geo-11's double-width image; out[h] is half h at the headset size.
// Returns false when there is nothing to show (then show geo-11's image as it is).
bool DlssUpscaling();
bool DlssOutputSize(unsigned* w, unsigned* h);
bool DlssUpscale(ID3D11DeviceContext* ctx, ID3D11Texture2D* stereo, ID3D11Texture2D* out[2]);

// Test settings.
float DlaaJitterSign();           // +1 / -1: direction of the jitter told to DLSS
float DlaaEyeSign();              // +1 / -1: which slice geo-11 shifts right
void DlaaSetSigns(float jitter, float eye);  // from the menu: applied now (history reset) and saved
int DlaaPreset();                 // DLSS model: 0 driver default, 6 CNN (preset F), 11 transformer (preset K)
void DlaaSetPreset(int preset);   // from the menu: the DLSS features are remade on the next frame
bool DlssPairPrevious();          // upscaling: geo-11's image is the previous frame (pair it with that frame's depth)
void DlssSetPairPrevious(bool on);
bool DlssHalvesSwapped();         // upscaling: geo-11's left half holds slice 1
void DlssSetHalvesSwapped(bool on);
float DlssMipBiasOffset();        // added to log2(render / headset) for scene textures (NVIDIA: -1)
void DlssStepMipBiasOffset(int dir);
void DlssResetTuning();           // the test settings above back to their defaults (saved)
float DlssMipBias();              // the bias in use (0 when not upscaling)

// Called from the context hooks in drawlog.cpp.
void DlaaOnSetRenderTargets(unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);  // any context
void DlaaBeforeDraw(ID3D11DeviceContext* immediate);  // immediate context, before each Draw
bool DlaaBusy();                  // true while DLSS itself is using the immediate context

} // namespace ds3vr
