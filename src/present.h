#pragma once

namespace ds3vr {

// Waits for the game's DirectX DLLs, then hooks the real (system) IDXGISwapChain::Present and
// Present1. With geo-11 installed, the game talks to geo-11's d3d11.dll; geo-11 calls the real
// Present after it has drawn both eyes, so our hook sees every finished stereo frame.
// Blocks the calling thread, so run it off the loader lock.
void InstallPresentHooks();

} // namespace ds3vr
