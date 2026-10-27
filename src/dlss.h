#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace ds3vr {

// DLAA through NVIDIA's NGX (DLSS at native resolution), via the ds3vr_ngx.dll helper (src/ngx_api.h).
//
// How it fits DS3 under geo-11 (see the project's DS3VR/DLAA_FRAME_MAP.md):
// - Jitter: DS3's camera constants (cbSceneParam, register b8) are found where the game binds them and
//   moved by a sub-pixel amount each frame as the game uploads them (projection and view-projection).
// - Scene colour: the HDR target the game lights the scene into (RGBA16F, both eyes as a 2-slice array,
//   drawn with the main depth buffer). Main depth: the 2-slice R32G8X24 buffer drawn with it.
// - Each eye: copy its slice out, make camera motion vectors from depth and the current / previous camera
//   (undoing geo-11's per-eye shift), run DLSS (one feature per eye, so each keeps its own history), and
//   copy the result back, just before the game's first post-processing pass reads the scene colour.
//
// Modes (VR menu, saved as ds3vr.ini DlaaMode): 0 off, 1 DLAA, 2 jitter test (the image wobbles by a few
// pixels and DLAA stays off: shows the jitter reaches both eyes through geo-11).
// ds3vr.ini DlssSelfTest=1 still runs the start-up NGX self-test.
void DlssOnPresent(ID3D11Device* device);  // every Present, on the render thread

int DlaaMode();
void DlaaSetMode(int mode);   // from the menu: applied now and saved
const char* DlaaStatus();     // one short line for the menu
float DlaaJitterSign();       // +1 / -1: direction of the jitter told to DLSS
float DlaaEyeSign();          // +1 / -1: which slice geo-11 shifts right
void DlaaSetSigns(float jitter, float eye);  // from the menu: applied now (history reset) and saved

// Called from the context hooks in drawlog.cpp.
void DlaaOnSetRenderTargets(unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);  // any context
void DlaaBeforeDraw(ID3D11DeviceContext* immediate);  // immediate context, before each Draw
bool DlaaBusy();              // true while DLAA itself is using the immediate context

} // namespace ds3vr
