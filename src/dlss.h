#pragma once

struct ID3D11Device;

namespace ds3vr {

// DLSS / DLAA through NVIDIA's NGX, via the ds3vr_ngx.dll helper (src/ngx_api.h).
// For now: a one-off self-test (ds3vr.ini DlssSelfTest=1) that starts NGX on the game's device, checks
// that DLSS is available, creates and releases a small DLAA feature, and shuts NGX down, logging each step.
void DlssOnPresent(ID3D11Device* device);  // every Present, on the render thread

} // namespace ds3vr
