#include <windows.h>

#include <string>

#include "log.h"
#include "version.h"
#include "present.h"
#include "rendersize.h"
#include "worldscale.h"
#include "fps.h"
#include "firstperson.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

// DS3's expected exe size for the final Steam patch (1.15.2.0). A mismatch means a
// different build, and the camera addresses we use would be wrong.
constexpr unsigned long long kExpectedExeSize = 88960032ULL;

void LogBanner() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0;
    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) {
        size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }

    ds3vr::Log("ds3vr milestone " DS3VR_VERSION ", built %s %s", __DATE__, __TIME__);
    ds3vr::Log("pid %lu, exe base %p", GetCurrentProcessId(), static_cast<void*>(GetModuleHandleW(nullptr)));
    ds3vr::Log("exe: %ls (%llu bytes)", exe, size);
    if (size != kExpectedExeSize) {
        ds3vr::Log("WARNING: exe size differs from DS3 1.15.2.0 (%llu). Different game build?", kExpectedExeSize);
    }
}

// ds3vr.ini (next to dinput8.dll):
//   [ds3vr]
//   Enabled=1    ; 0 = pure DirectInput passthrough, no hooks, behaves like stock
// Set up = install-ds3vr.bat has run: geo-11 is present and switched to VR output. Without that (the
// zip extracted but not installed yet, or Helifax's fix missing) the mod stays out of the way and says so.
int g_setupState = -1;  // -1 = not checked, 0 = not set up, 1 = set up
const wchar_t* g_setupProblem = L"";

bool IsSetUp() {
    if (g_setupState >= 0) return g_setupState == 1;
    const std::wstring dir = ds3vr::ModuleDir();
    g_setupState = 0;
    if (GetFileAttributesW((dir + L"d3d11.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_setupProblem = L"DS3 VR has not been set up yet: Helifax's Dark Souls 3 geo-11 fix is not in this Game folder.\n\n"
                         L"Close the game and run install-ds3vr.bat (in the Game folder). It downloads and sets up the fix for you.";
        return false;
    }
    HANDLE f = CreateFileW((dir + L"d3dxdm.ini").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string text;
    if (f != INVALID_HANDLE_VALUE) {
        char buf[16384];
        DWORD got = 0;
        if (ReadFile(f, buf, sizeof(buf), &got, nullptr)) text.assign(buf, got);
        CloseHandle(f);
    }
    if (text.find("katanga_vr") == std::string::npos) {
        g_setupProblem = L"DS3 VR is in this Game folder but has not been set up yet.\n\n"
                         L"Close the game and run install-ds3vr.bat (in the Game folder), then start the game again.";
        return false;
    }
    g_setupState = 1;
    return true;
}

bool IsEnabled() {
    std::wstring ini = std::wstring(ds3vr::ModuleDir()) + L"ds3vr.ini";
    return GetPrivateProfileIntW(L"ds3vr", L"Enabled", 1, ini.c_str()) != 0 && IsSetUp();
}

// Logs the first few access violations (where and in which module) so a crash leaves a trail.
LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) &&
        InterlockedIncrement(&count) <= 5) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &m) && m)
            GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        ds3vr::Log("EXCEPTION 0x%08lx at %p (%ls+%llx), thread %lu, accessing %p", code, addr, base ? base + 1 : name,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)),
                   GetCurrentThreadId(),
                   ep->ExceptionRecord->NumberParameters >= 2 ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]) : nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Settings files from older versions: PoseLag 1 was the default up to 0.9.1; 2 is steadier when
// turning the head, so an untouched 1 moves to 2 once.
void MigrateSettings() {
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    const std::wstring ini = std::wstring(dir) + L"ds3vr.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetPrivateProfileIntW(L"state", L"SettingsVersion", 1, ini.c_str()) >= 2) return;
    if (GetPrivateProfileIntW(L"ds3vr", L"PoseLag", 1, ini.c_str()) == 1) WritePrivateProfileStringW(L"ds3vr", L"PoseLag", L"2", ini.c_str());
    WritePrivateProfileStringW(L"state", L"SettingsVersion", L"2", ini.c_str());
}

DWORD WINAPI InitThread(LPVOID) {
    AddVectoredExceptionHandler(1, CrashLogger);
    LogBanner();
    if (!IsEnabled()) {
        if (g_setupState == 0) {
            ds3vr::Log("not set up, so DS3 VR stays off this run: %ls", g_setupProblem);
            MessageBoxW(nullptr, g_setupProblem, L"DS3 VR", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        } else {
            ds3vr::Log("ds3vr.ini says Enabled=0: hooks NOT installed, passthrough only");
        }
        return 0;
    }
    ds3vr::RenderSizePatchLetterbox();
    ds3vr::FpsEarly();
    ds3vr::InstallPresentHooks();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        // When rundll32 loads us for the Present probe, do nothing here (and do not touch the log).
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = wcsrchr(exe, L'\\');
        if (_wcsicmp(base ? base + 1 : exe, L"DarkSoulsIII.exe") != 0) return TRUE;
        ds3vr::LogInit(self);
        MigrateSettings();
        // Before the game reads its settings: offer our render size and select it.
        if (IsEnabled()) {
            ds3vr::RenderSizeEarly();
            // First Person Souls patches the game's code here, as its own dll does at load. Before the
            // world scale, which starts at life size when the run starts in first person.
            ds3vr::FirstPersonEarly();
            ds3vr::WorldScaleEarly();
        }
        // Do the real work off the loader lock.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        if (IsEnabled()) {
            t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { ds3vr::RenderSizeRefreshFromHeadset(); return 0; }, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
