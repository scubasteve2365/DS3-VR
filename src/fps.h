#pragma once

namespace ds3vr {

// Frame rate above 60 (the same approach as Luke Ross's DS3UncapFPS / DS3FixSprinting patterns):
//  - The frame limiter object (pointer at exe+0x489dd10, the one exe+0xeed810 runs on) has a custom
//    frame rate: byte +0x358 = use it, float +0x354 = frames per second. With it set, the limiter's
//    frame time (+0x18, normally 1/60) becomes 1/fps. Data only, no code patch.
//  - exe+0x106dfa5: "je" chooses between a per-object fixed time step and the real frame time. Making it
//    always take the real frame time keeps movement (sprinting, rolling) right above 60 fps.
//  - The game's own V-Sync is turned off while the headset is running; OpenXR paces the frames.
// Settings (ds3vr.ini): FrameRate (0 = the headset's refresh rate, -1 = leave the 60 cap alone),
// FixHighFpsMovement (1 = on).

void FpsEarly();                  // init thread: the movement patch (retries until the code matches)
void FpsTick(float headsetHz);    // every Present: keep the limiter at the target
bool FpsNoVsync();                // true while the game's V-Sync should be off

} // namespace ds3vr
