#pragma once

struct ID3D11Texture2D;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace ds3vr {

// HUD panel, done the way Luke Ross's R.E.A.L. mod does it for DS3, but on the real D3D11 context
// underneath geo-11.
//
// DS3 draws the finished 3D frame into a texture (we call it the HUD texture), draws the HUD and
// menus on top of it as batches of quads (with its depth/stencil buffer bound), then copies it to
// the screen with two full-screen 4-vertex draws: HUD texture -> intermediate -> back buffer.
// We find the HUD texture each frame by following that chain backwards, then send the quad batches
// drawn into it to a texture of our own (same size, cleared to transparent every frame). The scene
// stays in the 3D image; the HUD ends up in our texture, which the VR side shows as a flat panel.
// Picking draws by their shape (index count a multiple of 6) rather than by shader keeps every
// glyph of a line of text together.
//
// Only while in the game world (the camera is updating); the title menu and loading screens are untouched.
// ds3vr.ini: HudPanel=1 (0 = leave the HUD in the 3D image as geo-11 draws it).

void HudInit();  // reads ds3vr.ini

// Called from the context hooks (drawlog.cpp) on the real immediate context.
void HudOnSetRenderTargets(ID3D11DeviceContext* c, unsigned n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
bool HudBeforeDraw(ID3D11DeviceContext* c, unsigned count, bool indexed);  // false = skip this draw

// Once per frame from the Present hook, after the VR side has copied the HUD: works out next frame's
// HUD texture and clears ours.
void HudEndFrame(ID3D11DeviceContext* c);

void HudSetPoseLag(int lag);  // same setting as the VR side (lock-on marker uses the matching camera)
ID3D11Texture2D* HudTexture();  // our HUD texture (2 slices, slice 0 = left eye), or null

} // namespace ds3vr
