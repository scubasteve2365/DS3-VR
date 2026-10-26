#pragma once

namespace ds3vr {

// First person, from First Person Souls by Dasaav (MIT, third_party/ds3fps). Its hooks are installed at
// startup (when every code pattern it needs is found); the VR menu then switches between third person
// (the default) and first person while the game runs. Its own hold-lock-on switch is off.

void FirstPersonEarly();        // from DllMain, before the game runs: installs the hooks
bool FirstPersonAvailable();    // the hooks are installed this run
bool FirstPersonWanted();       // the menu's choice (ds3vr.ini FirstPerson), also the view each load starts in
void FirstPersonSet(bool on);   // from the menu: switch now (done on the game's thread) and save
bool FirstPersonActive();       // the game is showing first person right now

} // namespace ds3vr
