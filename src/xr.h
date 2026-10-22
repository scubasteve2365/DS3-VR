#pragma once

struct ID3D11Device;
struct IDXGISwapChain;

namespace ds3vr {

// Milestone G1: geo-11 renders both eyes and shares them as one double-width texture (Katanga
// interface, direct_mode = katanga_vr). Every Present we copy that texture into an OpenXR swapchain
// and submit it as a stereo projection layer, with the head pose the frame was rendered from.
// Call from the game's render thread (Present) every frame.
void XrOnPresent(ID3D11Device* device, IDXGISwapChain* sc);

// geo-11's measured per-eye separation (from its StereoParams texture); replaces the default shift.
void XrSetMeasuredShift(float s, float convergence, float eye);
int XrGetPoseLag();                // camera updates between a head pose and its frame reaching the headset
void XrSetPoseLag(int lag);        // from the menu (developer section): applied now and saved
void XrRecenter();                 // F8
void XrSwapEyes();                 // F7
void XrStepShift(float delta);     // F5 / F6: per-eye image shift (geo-11's separation at infinity)
void XrRequestCapture();
struct HudSettings {
    int mode;        // 0 fixed in front of you, 1 follows your head, 2 hidden
    float width;     // metres
    float distance;  // metres from the eyes
    float drop;      // metres below eye level
};
HudSettings XrGetHudSettings();
void XrSetHudSettings(const HudSettings& h, bool save);  // save = write ds3vr.ini
void XrCycleHudSize();             // F6: HUD panel size (cycles 0.8 to 2.0 m wide, saved)
void XrCycleHudMode();             // F7: HUD fixed / follows head / hidden           // F8: save both eyes as BMP files next to the game

// Asks the VR runtime for its recommended per-eye height (temporary instance, no session).
bool XrQueryRecommendedHeight(unsigned* height, unsigned* width = nullptr);
// While running (render thread): the per-eye size the VR runtime recommends right now, asked on the
// game's own OpenXR connection.
bool XrRecommendedNow(unsigned* w, unsigned* h);
// The per-eye size of the stereo image the game is rendering this run (geo-11's shared texture).
bool XrEyeImageSize(unsigned* w, unsigned* h);

// Headset refresh rate in Hz while the session runs (0 otherwise).
float XrDisplayHz();

// Clean shutdown when the game window closes.
void XrRequestShutdown();          // WM_CLOSE: done on the next Present
void XrShutdownNow();              // WM_DESTROY: done right away if not done yet

} // namespace ds3vr
