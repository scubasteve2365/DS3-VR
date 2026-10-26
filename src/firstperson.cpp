// First person: DS3 VR's side of First Person Souls (third_party/ds3fps, MIT, by Dasaav).
// Built with that code's settings (see build.sh): C++17, SSE 4.1, UNICODE, Windows 10 headers.

#include "firstperson.h"

#include <src/stdafx.h>
#include <src/AOB.h>
#include <src/EventFlag.h>
#include <src/asm.h>
#include <src/camera/Viewmodel.h>
#include <src/ds3vr_hooks.h>

#include <string>

#include "log.h"
#include "worldscale.h"

namespace ds3vr {
namespace {

bool g_available = false;
bool g_wanted = false;  // ds3vr.ini FirstPerson, read once at startup and kept up to date by the menu

std::wstring IniPath() { return std::wstring(ModuleDir()) + L"ds3vr.ini"; }

} // namespace

// The same steps as First Person Souls' own InitializeHooking(), with three differences: its FPS.ini and
// debug console are not used (its settings below are DS3 VR's), the game's code is only made writable once
// every pattern has been found, and nothing is patched if one is missing.
void FirstPersonEarly() {
    g_wanted = GetPrivateProfileIntW(L"ds3vr", L"FirstPerson", 0, IniPath().c_str()) == 1;
    ::g_ds3vrStartFirstPerson = g_wanted;
    ::g_ds3vrHoldSwitch = false;
    // VR comfort: the view does not follow the head through rolls and heavy hits.
    ::track_roll = false;
    ::track_hit = false;
    ::test_out = false;
    ::use_virtualalloc2 = true;

    HMODULE exe = GetModuleHandleA("DarkSoulsIII.exe");
    if (!exe || !GetModuleInformation(GetCurrentProcess(), exe, &::mInfo, sizeof(::mInfo))) {
        Log("FIRSTPERSON: could not read the game's module; first person is unavailable");
        return;
    }
    ::GetText();
    if (::g_ds3vrAobMissing == 0) {
        ::PopulateBases();
        ::ScanAOBs();
    }
    if (::g_ds3vrAobMissing != 0) {
        Log("FIRSTPERSON: %d of First Person Souls' code patterns were not found (game version?); first person is "
            "unavailable and nothing was changed", ::g_ds3vrAobMissing);
        return;
    }
    DWORD old = 0;
    VirtualProtect(::mInfo.lpBaseOfDll, ::mInfo.SizeOfImage, PAGE_EXECUTE_READWRITE, &old);
    ::GetEventFunctions(::GetEventFlagAOB, ::SetEventFlagAOB);
    ::InjectAsm();
    ::PopulateViewmodelTables();
    g_available = true;
    Log("FIRSTPERSON: First Person Souls hooks installed; this run starts in %s person (switch in the VR menu)",
        ::g_ds3vrStartFirstPerson ? "first" : "third");
}

bool FirstPersonAvailable() { return g_available; }

bool FirstPersonWanted() { return g_wanted; }

void FirstPersonSet(bool on) {
    if (!g_available) return;
    g_wanted = on;
    ::g_ds3vrStartFirstPerson = on;
    ::g_ds3vrViewRequest.store(on ? 1 : 2);
    WorldScaleFirstPerson(on);  // life size in first person; the third-person scale comes back after
    WritePrivateProfileStringW(L"ds3vr", L"FirstPerson", on ? L"1" : L"0", IniPath().c_str());
    Log("menu: %s person (saved)", on ? "first" : "third");
}

bool FirstPersonActive() { return g_available && ::IsFPS; }

} // namespace ds3vr
