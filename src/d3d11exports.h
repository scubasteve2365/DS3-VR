#pragma once

namespace ds3vr {

// geo-11's d3d11.dll (3Dmigoto) stands in for Windows' d3d11.dll in the game and exports only part of
// it. A VR runtime DLL loaded later that imports a missing function fails to load, because Windows
// binds its "d3d11.dll" imports to the module already loaded under that name (geo-11's). Virtual
// Desktop's runtime (VDXR) loads VirtualDesktop.LibOVRRT64_1.dll, which imports D3D11On12CreateDevice,
// and then reports "Virtual Desktop Server is not running".
//
// This adds the missing functions to the export table of every loaded d3d11.dll that lacks them, each
// pointing at Windows' own function (System32\d3d11.dll, which geo-11 itself has loaded). geo-11's own
// functions are untouched. Call before the OpenXR runtime is loaded; safe to call more than once.
void D3D11AddMissingExports();

} // namespace ds3vr
