#pragma once

namespace ds3vr {

// World scale, 1.0 (life size) to 5.0 in steps of 0.5, set from the mod menu. geo-11 does the
// actual change: d3dx.ini has one [KeyDs3vrWorldScaleN] section per step, each bound to a spare key
// (F13 to F21, which keyboards do not have) that sets that step's convergence. The menu "presses"
// the key for the chosen step, so any step can be reached directly in either direction. The choice
// is saved in ds3vr.ini (WorldScale); at the next launch, before geo-11 reads its settings, the
// matching starting convergence is written into d3dxdm.ini.

void WorldScaleEarly();          // from DllMain, before geo-11 loads
float WorldScaleGet();           // 1.0 to 5.0
void WorldScaleSet(float scale); // from the menu: snapped to 0.5, applied through geo-11, saved
void WorldScaleTick();           // every frame: releases the key pressed for geo-11
bool WorldScaleBusy();           // a change is being applied

} // namespace ds3vr
