#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace ds3vr {

// Diagnostics for the HUD work: F2 records every draw, clear and copy the GPU receives during one
// frame (below geo-11, on the real D3D11 context) to ds3vr_draws_N.txt next to the game. Each line
// names the render target, depth buffer, the textures the pixel shader reads, the blend state and
// the draw size, so the path from the game's HUD texture to the screen can be traced.

void DrawLogOnPresent(ID3D11Device* realDevice);  // call once per frame from the Present hook
void DrawLogRequest();                            // F2: record the next frame
// Hook DS3's deferred contexts (worker threads) now, if not done yet. Used by F2 and DLAA.
void DrawLogEnsureDeferredHooks(ID3D11Device* dev);
// geo-11's per-eye shift (|StereoParams.x|) and convergence as last read; false until read once.
bool StereoParamsGet(float* sep, float* conv);

// The same context hooks also drive the HUD panel (hud.cpp). This binds render targets on the real
// context without going through our own hook.
void CtxSetRenderTargetsRaw(ID3D11DeviceContext* c, unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);

} // namespace ds3vr
