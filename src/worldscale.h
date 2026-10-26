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

// Each eye's own view (Advanced, off by default): each eye's frustum is centred on its own lens, which
// needs a larger geo-11 separation and negative convergence. Decided once per run (see worldscale.cpp);
// the menu choice takes effect at the next launch.
bool WorldScaleEyeViewActive();  // this run uses the each-eye view
bool WorldScaleEyeViewWanted();  // ds3vr.ini EachEyeView = 1
void WorldScaleSetEyeViewWanted(bool on);

} // namespace ds3vr
