#pragma once

namespace ds3vr {

// World scale, 0.25 (largest world) to 10.0 (smallest), set from the mod menu in steps. geo-11 does the
// actual change: d3dx.ini has one [KeyDs3vrWorldScaleN] section per step, each bound to a spare key
// (F13 to F24, which keyboards do not have) that sets that step's convergence. The menu "presses"
// the key for the chosen step, so any step can be reached directly in either direction. The choice
// is saved in ds3vr.ini (WorldScale); at each launch, before geo-11 reads its settings, the step
// sections and the matching starting convergence are written into d3dx.ini and d3dxdm.ini.

void WorldScaleEarly();          // from DllMain, before geo-11 loads
float WorldScaleGet();           // 0.25 to 10.0
void WorldScaleSet(float scale); // from the menu: snapped to the nearest step, applied through geo-11, saved
void WorldScaleStep(int delta);  // from the menu: one step up (+1) or down (-1)
void WorldScaleTick();           // every frame: releases the key pressed for geo-11
bool WorldScaleBusy();           // a change is being applied

} // namespace ds3vr
