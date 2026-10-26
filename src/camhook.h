#pragma once

namespace ds3vr {

// Hooks the game camera without patching game code.
// The camera object at [[[exe+0x4796298]+0x10]+0x38] gets the camera matrix (rows right, up,
// forward, position) and vertical FOV written by the camera manager (exe+0xe006a0), which then
// calls the object's virtual function 3 to export it. We give that one object a copy of its vtable
// with slot 3 pointing at our function, so we can apply the head pose right before the export.

void CamHookTick();     // every Present: (re)install on the current camera object
void CamHookRecenter();

// From the OpenXR side: latest head pose in LOCAL space (x,y,z,w quaternion; metres).
void CamHookSetHead(const float q[4], const float p[3]);
// Vertical field of view to render with when ds3vr.ini has FovY=0 (automatic), in radians.
void CamHookSetAutoFovY(float radians);
// The head pose that was applied to the game camera `lag` camera updates ago, and the game's
// vertical field of view (radians) for that update.
bool CamHookAppliedPose(int lag, float q[4], float p[3], float* fovY);
// Aspect ratio (view width / height) the game renders with; 0 = the game's own (16:9).
void CamHookSetAspect(float aspect);
// The aspect the frame `lag` camera updates ago was rendered with (0 = the game's own).
bool CamHookAppliedAspect(int lag, float* aspect);
// Milliseconds since the game last updated its camera (large in menus and loading screens).
unsigned long long CamHookMsSinceUpdate();
// For the HUD shaders' lock-on marker fix: rows 0-2 = game camera frame -> VR camera frame,
// row 3 = (game tanX, game tanY, VR 1/tanX, VR 1/tanY), for the camera update `lag` updates ago.
bool CamHookMarkerParams(int lag, float out[16]);

// Head movement multiplied by the world scale (matches geo-11's wider stereo baseline). ds3vr.ini ScaleHeadMovement.
bool CamHookGetScaleHead();
void CamHookSetScaleHead(bool on);  // from the menu (developer section): applied now and saved

// Horizon lock (VR menu, ds3vr.ini HorizonLock, default off): the right stick no longer tilts the view up
// or down; the view stays level and only your head pitches it.
bool CamHookGetHorizonLock();
void CamHookSetHorizonLock(bool on);

// How far the head is turned left (+) or right (-) of the game camera, in radians, as last applied to the
// view. First person uses it to make the left stick move where you look.
float CamHookHeadYaw();

} // namespace ds3vr
