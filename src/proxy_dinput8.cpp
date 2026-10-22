// Forwarder for the one dinput8 export Dark Souls III imports: DirectInput8Create.
// Our DLL is named dinput8.dll and sits next to DarkSoulsIII.exe, so Windows loads it
// instead of the system copy. We load the real one from System32 and pass calls through.

#include <windows.h>
#include <unknwn.h>

#include <string>

#include "log.h"

namespace {

using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

PFN_DirectInput8Create g_realCreate = nullptr;

bool LoadReal() {
    if (g_realCreate) return true;

    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\dinput8.dll";

    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) {
        ds3vr::Log("proxy: failed to load real dinput8.dll (error %lu)", GetLastError());
        return false;
    }
    g_realCreate = reinterpret_cast<PFN_DirectInput8Create>(reinterpret_cast<void*>(GetProcAddress(real, "DirectInput8Create")));
    if (!g_realCreate) {
        ds3vr::Log("proxy: real dinput8.dll has no DirectInput8Create");
        return false;
    }
    ds3vr::Log("proxy: forwarding DirectInput8Create to the system dinput8.dll");
    return true;
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid,
                                             LPVOID* out, LPUNKNOWN outer) {
    if (!LoadReal()) return E_FAIL;
    return g_realCreate(hinst, version, riid, out, outer);
}
