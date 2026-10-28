#pragma once

#include <dxgiformat.h>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ds3vr {

// Sharpening of the image sent to the headset, after DLSS / DLAA (or none), before the compositor.
// Contrast-adaptive sharpening as published by AMD (FidelityFX CAS, MIT licence): each pixel is
// sharpened against its 3x3 neighbourhood, more where the area is soft and less where it already has
// strong contrast, so anti-aliased edges are not turned back into stair-steps. The HUD and the mod
// menu are separate panels and are not touched. Each eye (half of the double-width image) is sharpened
// on its own, so the eyes never bleed into each other.
//
// Strength: VR menu (Graphics tab), saved as ds3vr.ini Sharpening: 0 off (default), 1 low, 2 medium,
// 3 high, 4 maximum.
int SharpenLevel();
void SharpenSetLevel(int level);   // from the menu: applied now and saved
const char* SharpenLevelName(int level);
constexpr int kSharpenLevels = 5;

// Sharpens the double-width swapchain image in place (render thread, after the eyes were copied in).
// fmt: the format the image is viewed with (the swapchain's format). Does nothing when off or if the
// shaders could not be set up (logged once).
void SharpenStereo(ID3D11DeviceContext* ctx, ID3D11Texture2D* image, DXGI_FORMAT fmt);
bool SharpenBusy();  // true while it draws (drawlog.cpp leaves those draws alone)

} // namespace ds3vr
