#pragma once
// ds3vr: the few places DS3 VR connects to First Person Souls (see PORTING.md).

#include <atomic>

// The VR menu's view request, applied on the game's thread in PerspectiveSwitch():
// 0 = none, 1 = first person, 2 = third person.
extern std::atomic<int> g_ds3vrViewRequest;
// Holding lock-on to switch view (First Person Souls' own control). Off in DS3 VR: the VR menu switches.
extern volatile bool g_ds3vrHoldSwitch;
// The view each load starts in (the VR menu's choice, not the save file's flag).
extern volatile bool g_ds3vrStartFirstPerson;
// Patterns not found by the scanner (AOB.cpp).
extern int g_ds3vrAobMissing;
