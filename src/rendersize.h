#pragma once

namespace ds3vr {

// Lets DS3 render at a non-16:9, headset-shaped size (the same idea Luke Ross's R.E.A.L. VR uses):
//  1. The windowed-mode resolution list is built from a 12-entry table in the game's data
//     (exe+0x4662d70, {width, height} pairs). Entry 0 is always offered, with no desktop-size check,
//     so we write our size into it and the game accepts it from GraphicsConfig.xml.
//  2. The game fits its 3D view into 16:9 with black bars (exe+0x86b4: "test eax,eax / je"). We turn
//     that conditional jump into an unconditional one so the view fills the whole image.
//  3. GraphicsConfig.xml is set to that windowed size before the game reads it.
// Settings (ds3vr.ini): RenderHeight (0 = the headset's requested height, -1 = leave the game alone),
// RenderWidth (0 = matches the headset's shape).

// Call from DllMain (before the game reads its settings): table entry + GraphicsConfig.xml.
void RenderSizeEarly();
// Call from the init thread: the black-bar code patch (retries until the code matches).
void RenderSizePatchLetterbox();
// RenderHeight=0: ask the VR runtime right away and switch to its current request if it changed
// since the last run (works when it answers before the game reads its settings). Blocking; run on a
// thread of its own.
void RenderSizeRefreshFromHeadset();
// The configured size (the headset size per eye), or false if not in use.
bool RenderSizeGet(unsigned* w, unsigned* h);
// The size the game renders at this run: the headset size, or smaller when DLSS upscales (dlss.h).
bool RenderSizeGame(unsigned* w, unsigned* h);
// The size the next launch will use (from ds3vr.ini as it stands now), or false if the mod leaves the
// game's resolution alone (RenderHeight=-1).
bool RenderSizeNextLaunch(unsigned* w, unsigned* h, bool* fixed);
// The VR runtime asked for this per-eye height while the game was running: remember it for the next launch.
void RenderSizeRememberHeadsetHeight(unsigned h);
// Asks SteamVR for its current per-eye size from a fresh helper process (the game's own connection
// keeps reporting the size it started with). Runs in the background; saves the answer for the next launch.
void RenderSizeCheckSteamVR();
// 0 = not asked yet, 1 = asking, 2 = answered (w, h filled in), 3 = no answer.
int RenderSizeSteamVRAnswer(unsigned* w, unsigned* h);

} // namespace ds3vr
