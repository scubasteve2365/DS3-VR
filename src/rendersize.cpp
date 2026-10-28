#include "rendersize.h"

#include <windows.h>

#include <atomic>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "dlss.h"
#include "log.h"
#include "xr.h"

namespace ds3vr {
namespace {

constexpr uintptr_t kResTableRva = 0x4662d70;  // {uint32 w, uint32 h} x 12, windowed resolutions
constexpr uintptr_t kLetterboxJeRva = 0x86b4;  // "74 4F" je -> "EB 4F" jmp: skip the 16:9 fit
// Bytes from exe+0x86a2 (Luke Ross calls this pattern DS3UnlockAspect) up to and including the je.
const uint8_t kLetterboxSig[] = {0x42, 0x8B, 0x94, 0x21, 0x98, 0x01, 0x00, 0x00, 0x46, 0x8B, 0x84, 0x21, 0x9C, 0x01,
                                 0x00, 0x00, 0x85, 0xC0, 0x74, 0x4F};
constexpr uintptr_t kLetterboxSigRva = 0x86a2;

unsigned g_w = 0, g_h = 0;
bool g_read = false;

// The game reads GraphicsConfig.xml early in startup. Until our early headset query has finished,
// opening that file waits for it (up to 10 s), so the size SteamVR asks for applies right away.
HANDLE g_queryDone = nullptr;
// How long the game waits for that query. A fresh install (no HeadsetAspect saved yet) also reads the
// headset's shape in a short VR session, which takes longer, so it waits longer; set in RenderSizeEarly.
DWORD g_waitMs = 5000;
// 0 = the headset query is still running, 1 = it set this run's size, 2 = the game went ahead first
// (the size the game read must then stay for the whole run, or its window and geo-11's output disagree).
std::atomic<int> g_sizePhase{0};

thread_local bool t_ownFileIo = false;  // our own GraphicsConfig.xml reads/writes never wait
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_origCreateFileW = nullptr;
HANDLE WINAPI CreateFileWDetour(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                                HANDLE tmpl) {
    if (name && !t_ownFileIo && g_queryDone && WaitForSingleObject(g_queryDone, 0) != WAIT_OBJECT_0) {
        const size_t n = wcslen(name);
        static const wchar_t kTail[] = L"graphicsconfig.xml";
        const size_t t = sizeof(kTail) / sizeof(wchar_t) - 1;
        if (n >= t && _wcsicmp(name + n - t, kTail) == 0) {
            const ULONGLONG t0 = GetTickCount64();
            // Only a short wait: when the VR runtime is still starting (the first launch of a session), a long
            // stall here leaves a black window that can get closed. A late answer is used from the next launch.
            const DWORD w = WaitForSingleObject(g_queryDone, g_waitMs);
            int expect = 0;
            const bool late = w != WAIT_OBJECT_0 && g_sizePhase.compare_exchange_strong(expect, 2);
            Log("RENDERSIZE: the game opened GraphicsConfig.xml; waited %llu ms for the headset query%s",
                GetTickCount64() - t0, late ? " (timed out: could not get the headset size in time, keeping the remembered size)" : "");
        }
    }
    return g_origCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

// True once ds3vr.ini holds the headset's aspect (saved by an earlier run). A fresh install has none.
bool HeadsetAspectSaved() {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"state", L"HeadsetAspect", L"", v, 32, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
    return v[0] != 0;
}

void ReadSettings() {
    if (g_read) return;
    g_read = true;
    std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    g_w = GetPrivateProfileIntW(L"ds3vr", L"RenderWidth", 0, ini.c_str());
    const int hSetting = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"RenderHeight", 0, ini.c_str()));
    if (hSetting < 0) {  // -1: leave the game's own resolution alone
        g_w = g_h = 0;
        return;
    }
    g_h = static_cast<unsigned>(hSetting);
    if (g_h == 0) {
        // Automatic height: the per-eye height the VR runtime asked for last run (SteamVR's resolution
        // setting changes it). The headset is not known yet this early; the first run uses 2160.
        g_h = GetPrivateProfileIntW(L"state", L"HeadsetHeight", 2160, ini.c_str());
        if (g_h < 480 || g_h > 8192) g_h = 2160;
        g_w = 0;
    }
    if (g_w == 0 && g_h >= 240) {
        // Automatic width: the headset's shape, remembered from the last run (the headset is not
        // known yet this early). The first run uses a typical value.
        wchar_t v[32] = {};
        GetPrivateProfileStringW(L"state", L"HeadsetAspect", L"1.42", v, 32, ini.c_str());
        float a = static_cast<float>(_wtof(v));
        if (a < 0.6f || a > 2.5f) a = 1.42f;
        g_w = static_cast<unsigned>(static_cast<float>(g_h) * a + 0.5f);
    }
    if (g_w < 320 || g_h < 240 || g_w > 8192 || g_h > 8192) g_w = g_h = 0;
    g_w &= ~1u;
    g_h &= ~1u;
}

// The size the game renders at: the headset size, or smaller when DLSS upscales this run (each eye is
// upscaled back to the headset size before it reaches the headset).
unsigned GameW() {
    const float s = DlssLaunchScale();
    return s < 1.f ? static_cast<unsigned>(static_cast<float>(g_w) * s + 0.5f) & ~1u : g_w;
}
unsigned GameH() {
    const float s = DlssLaunchScale();
    return s < 1.f ? static_cast<unsigned>(static_cast<float>(g_h) * s + 0.5f) & ~1u : g_h;
}

bool WriteMem(void* dst, const void* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    DWORD tmp = 0;
    VirtualProtect(dst, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// GraphicsConfig.xml is UTF-16 LE without a BOM. Replace the windowed width/height values.
bool SetTag(std::wstring& xml, const wchar_t* tag, unsigned value) {
    const std::wstring open = std::wstring(L"<") + tag + L">", close = std::wstring(L"</") + tag + L">";
    size_t a = xml.find(open);
    if (a == std::wstring::npos) return false;
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::wstring::npos) return false;
    xml.replace(a, b - a, std::to_wstring(value));
    return true;
}

void UpdateGraphicsConfigImpl();
void UpdateGraphicsConfig() {
    t_ownFileIo = true;
    UpdateGraphicsConfigImpl();
    t_ownFileIo = false;
}

void UpdateGraphicsConfigImpl() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;
    const std::wstring path = std::wstring(appData) + L"\\DarkSoulsIII\\GraphicsConfig.xml";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("RENDERSIZE: no GraphicsConfig.xml yet (run the game once without the mod)");
        return;
    }
    std::string raw;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
    CloseHandle(f);
    size_t off = (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFF && static_cast<uint8_t>(raw[1]) == 0xFE) ? 2 : 0;
    std::wstring xml(reinterpret_cast<const wchar_t*>(raw.data() + off), (raw.size() - off) / 2);
    const std::wstring before = xml;
    bool ok = SetTag(xml, L"Resolution-WindowScreenWidth", GameW()) && SetTag(xml, L"Resolution-WindowScreenHeight", GameH());
    {
        const std::wstring o = L"<ScreenMode>", c = L"</ScreenMode>";
        size_t a = xml.find(o), b = a == std::wstring::npos ? a : xml.find(c, a);
        if (a != std::wstring::npos && b != std::wstring::npos) xml.replace(a + o.size(), b - a - o.size(), L"WINDOW");
    }
    if (!ok) {
        Log("RENDERSIZE: GraphicsConfig.xml has no windowed resolution entries; not changed");
        return;
    }
    if (xml == before) return;
    f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    if (off) WriteFile(f, raw.data(), 2, &put, nullptr);
    WriteFile(f, xml.data(), static_cast<DWORD>(xml.size() * 2), &put, nullptr);
    CloseHandle(f);
}


// geo-11's HUD panel texture ([ResourceDs3vrHUD] in d3dx.ini) must match the render size; its
// relative size option does not work in geo-11's VR mode, so we write the numbers in.
void UpdateD3dxHudSize(unsigned w, unsigned h) {
    const std::wstring path = std::wstring(ModuleDir()) + L"d3dx.ini";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    const size_t sec = s.find("[ResourceDs3vrHUD]");
    if (sec == std::string::npos) return;
    const size_t end = s.find("\n[", sec + 1);
    bool changed = false;
    auto setKey = [&](const char* key, unsigned v) {
        const std::string k = std::string("\n") + key + " = ";
        size_t a = s.find(k, sec);
        if (a == std::string::npos || (end != std::string::npos && a > end)) return;
        a += k.size();
        size_t b = s.find_first_of("\r\n", a);
        const std::string val = std::to_string(v);
        if (s.compare(a, b - a, val) != 0) {
            s.replace(a, b - a, val);
            changed = true;
        }
    };
    setKey("width", w);
    setKey("height", h);
    if (!changed) return;
    f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    Log("RENDERSIZE: HUD panel texture in d3dx.ini set to %ux%u (applies when geo-11 next reads its settings)", w, h);
}

} // namespace

bool RenderSizeGet(unsigned* w, unsigned* h) {
    ReadSettings();
    if (!g_w) return false;
    *w = g_w;
    *h = g_h;
    return true;
}

bool RenderSizeGame(unsigned* w, unsigned* h) {
    ReadSettings();
    if (!g_w) return false;
    *w = GameW();
    *h = GameH();
    return true;
}

bool RenderSizeNextLaunch(unsigned* w, unsigned* h, bool* fixed) {
    const std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    const int hs = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"RenderHeight", 0, ini.c_str()));
    if (hs < 0) return false;
    unsigned nw = GetPrivateProfileIntW(L"ds3vr", L"RenderWidth", 0, ini.c_str());
    unsigned nh = static_cast<unsigned>(hs);
    *fixed = nh != 0;
    if (nh == 0) {
        nh = GetPrivateProfileIntW(L"state", L"HeadsetHeight", 2160, ini.c_str());
        if (nh < 480 || nh > 8192) nh = 2160;
        nw = 0;
    }
    if (nw == 0) {
        wchar_t v[32] = {};
        GetPrivateProfileStringW(L"state", L"HeadsetAspect", L"1.42", v, 32, ini.c_str());
        float a = static_cast<float>(_wtof(v));
        if (a < 0.6f || a > 2.5f) a = 1.42f;
        nw = static_cast<unsigned>(static_cast<float>(nh) * a + 0.5f);
    }
    *w = nw & ~1u;
    *h = nh & ~1u;
    return *w >= 320 && *h >= 240;
}

void RenderSizeRememberHeadsetHeight(unsigned h) {
    if (h < 480 || h > 8192) return;
    const std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    if (GetPrivateProfileIntW(L"state", L"HeadsetHeight", 0, ini.c_str()) == h) return;
    wchar_t v[32];
    swprintf(v, 32, L"%u", h);
    WritePrivateProfileStringW(L"state", L"HeadsetHeight", v, ini.c_str());
    Log("RENDERSIZE: the VR runtime now asks for %u per eye; saved for the next launch", h);
}

void RenderSizeEarly() {
    ReadSettings();
    if (!g_w) return;
    {
        std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
        if (GetPrivateProfileIntW(L"ds3vr", L"RenderHeight", 0, ini.c_str()) == 0) {
            g_waitMs = HeadsetAspectSaved() ? 5000 : 20000;
            g_queryDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW"));
            MH_STATUS s = MH_Initialize();
            if (target && (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) &&
                MH_CreateHook(target, reinterpret_cast<void*>(&CreateFileWDetour), reinterpret_cast<void**>(&g_origCreateFileW)) == MH_OK)
                MH_EnableHook(target);
            else
                Log("RENDERSIZE: could not watch for GraphicsConfig.xml; a SteamVR resolution change may need a second launch");
        }
    }
    uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const uint32_t entry[2] = {GameW(), GameH()};
    const bool okTable = WriteMem(exe + kResTableRva, entry, sizeof(entry));
    UpdateGraphicsConfig();
    UpdateD3dxHudSize(GameW(), GameH());
    Log("RENDERSIZE: windowed resolution entry 0 set to %ux%u (%s); GraphicsConfig.xml set to it%s", GameW(), GameH(),
        okTable ? "ok" : "FAILED", GameW() != g_w ? " (DLSS upscales it to the headset size)" : "");
}

namespace {
struct SignalOnExit {
    ~SignalOnExit() { if (g_queryDone) SetEvent(g_queryDone); }
};
} // namespace

namespace {
std::wstring XrQueryFilePath() {
    static std::atomic<unsigned> n{0};
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[64];
    swprintf(name, 64, L"ds3vr_xr_query_%lu_%u.txt", GetCurrentProcessId(), n.fetch_add(1));
    return std::wstring(tmp) + name;
}

// Asks the VR runtime for its per-eye height from a separate helper process (rundll32 on this DLL,
// QueryXrHeight below), so none of the runtime's code is loaded into the game while the game and
// geo-11 are still setting up graphics. The game itself only connects to the runtime later, once
// it is running (xr.cpp).
bool QueryHeightInHelper(unsigned* height, unsigned* width = nullptr, float* aspect = nullptr, bool shape = false) {
    const std::wstring out = XrQueryFilePath();
    DeleteFileW(out.c_str());
    wchar_t self[MAX_PATH] = {};
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&XrQueryFilePath), &me);
    GetModuleFileNameW(me, self, MAX_PATH);
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\rundll32.exe\" \"" + self + L"\",QueryXrHeight " + out +
                       (shape ? L" shape" : L"");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        Log("RENDERSIZE: could not start the headset query helper (error %lu)", GetLastError());
        return false;
    }
    // Generous: a late answer is still saved for the next launch; the game itself never waits this long.
    const DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        Log("RENDERSIZE: the headset query helper did not finish in 60 s (could not ask the VR runtime)");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    unsigned h = 0, rw = 0;
    float a = 0.f;
    FILE* f = _wfopen(out.c_str(), L"r");
    if (f) {
        if (fscanf(f, "%u %u %f", &h, &rw, &a) < 1) h = 0;
        fclose(f);
    }
    DeleteFileW(out.c_str());
    if (!h) return false;
    *height = h;
    if (width) *width = rw;
    if (aspect) *aspect = a;
    return true;
}
} // namespace

void RenderSizeRefreshFromHeadset() {
    SignalOnExit signal;
    ReadSettings();
    if (!g_w) return;
    std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    if (GetPrivateProfileIntW(L"ds3vr", L"RenderHeight", 0, ini.c_str()) != 0) return;  // fixed height
    const ULONGLONG t0 = GetTickCount64();
    // Virtual Desktop's runtime: no separate query. Its connection from the helper process left the headset
    // "not available" to the game for several seconds afterwards (VR then stayed off for the run). The game
    // saves the size the runtime asks for once VR is running, and the next launch uses it.
    if (XrRuntimeIsVirtualDesktop()) {
        Log("RENDERSIZE: Virtual Desktop's runtime: no early headset query; using the remembered height %u "
            "(a new size from the runtime applies from the next launch)", g_h);
        return;
    }
    // A fresh install has no headset aspect yet: read the headset's shape too (a short VR session in the helper).
    const bool needShape = !HeadsetAspectSaved();
    unsigned want = 0;
    float shape = 0.f;
    if (!QueryHeightInHelper(&want, nullptr, &shape, needShape) || want < 480 || want > 8192) {
        Log("RENDERSIZE: could not ask the VR runtime early; using the remembered height %u", g_h);
        return;
    }
    wchar_t v[32];
    swprintf(v, 32, L"%u", want);
    WritePrivateProfileStringW(L"state", L"HeadsetHeight", v, ini.c_str());
    const bool haveShape = shape >= 0.6f && shape <= 2.5f;
    if (needShape && haveShape) {
        swprintf(v, 32, L"%.4f", shape);
        WritePrivateProfileStringW(L"state", L"HeadsetAspect", v, ini.c_str());
        Log("RENDERSIZE: headset shape read at first launch: aspect %.4f (saved)", shape);
    }
    // Width: the headset's shape (fresh install: just read; otherwise the aspect in use).
    const float aspect = (needShape && haveShape) ? shape : static_cast<float>(g_w) / static_cast<float>(g_h);
    const unsigned newW = static_cast<unsigned>(static_cast<float>(want) * aspect + 0.5f) & ~1u;
    if (want == g_h && newW == g_w) {
        Log("RENDERSIZE: VR runtime asks for %u per eye (same as remembered), %llu ms", want, GetTickCount64() - t0);
        return;
    }
    int expect = 0;
    if (!g_sizePhase.compare_exchange_strong(expect, 1)) {
        // The game already read its settings with the remembered size; changing it now would leave the
        // window and geo-11's output at different sizes (a scrambled view). Next launch uses the new size.
        Log("RENDERSIZE: could not switch to the new headset size %ux%u this run (the game had already started with %ux%u); "
            "the next launch uses it", newW, want, g_w, g_h);
        return;
    }
    g_h = want;
    g_w = newW;
    uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const uint32_t entry[2] = {GameW(), GameH()};
    WriteMem(exe + kResTableRva, entry, sizeof(entry));
    UpdateGraphicsConfig();
    UpdateD3dxHudSize(GameW(), GameH());
    Log("RENDERSIZE: VR runtime now asks for %u per eye: switched to %ux%u (game renders %ux%u) after %llu ms", want, g_w, g_h,
        GameW(), GameH(), GetTickCount64() - t0);
}

void RenderSizePatchLetterbox() {
    ReadSettings();
    if (!g_w) return;
    uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    for (int i = 0; i < 300; ++i) {  // up to 30 s
        if (memcmp(exe + kLetterboxSigRva, kLetterboxSig, sizeof(kLetterboxSig)) == 0) {
            const uint8_t jmp = 0xEB;
            const bool ok = WriteMem(exe + kLetterboxJeRva, &jmp, 1);
            Log("RENDERSIZE: 16:9 black-bar fit disabled at exe+%llx (%s)", static_cast<unsigned long long>(kLetterboxJeRva),
                ok ? "ok" : "FAILED");
            return;
        }
        if (exe[kLetterboxJeRva] == 0xEB) {
            Log("RENDERSIZE: black-bar fit already disabled");
            return;
        }
        Sleep(100);
    }
    Log("RENDERSIZE: black-bar code not found (different game build?); the image will have bars");
}

} // namespace ds3vr

// rundll32 entry point (helper process; no game, no geo-11): ask the VR runtime for the per-eye height
// it wants and write it to the file named on the command line.
// Command line: <file> [shape]. With "shape" the helper also opens a short VR session to read the
// headset's aspect (first launch only); if that fails, the height alone is written (aspect 0).
extern "C" __declspec(dllexport) void CALLBACK QueryXrHeight(HWND, HINSTANCE, LPSTR cmdLine, int) {
    ds3vr::LogDisable();
    std::string args = cmdLine ? cmdLine : "";
    bool shape = false;
    const std::string tag = " shape";
    if (args.size() >= tag.size() && args.compare(args.size() - tag.size(), tag.size(), tag) == 0) {
        shape = true;
        args.erase(args.size() - tag.size());
    }
    if (args.size() >= 2 && args.front() == '"' && args.back() == '"') args = args.substr(1, args.size() - 2);
    unsigned h = 0;
    unsigned w = 0;
    float a = 0.f;
    if (!(shape && ds3vr::XrQueryHeadsetShape(&h, &w, &a) && h)) {
        a = 0.f;
        if (!ds3vr::XrQueryRecommendedHeight(&h, &w) || !h) return;
    }
    FILE* f = fopen(args.c_str(), "w");
    if (!f) return;
    fprintf(f, "%u %u %.5f\n", h, w, a);
    fclose(f);
}
