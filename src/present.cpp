#include "present.h"
#include "rendersize.h"

#include <windows.h>

#include <cstdlib>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <string>

#include "MinHook.h"
#include "camhook.h"
#include "drawlog.h"
#include "fps.h"
#include "hud.h"
#include "log.h"
#include "worldscale.h"
#include "menu.h"
#include "xr.h"

namespace ds3vr {
namespace {

constexpr int kPresent = 8;    // IDXGISwapChain
constexpr int kPresent1 = 22;  // IDXGISwapChain1

thread_local int t_presentDepth = 0;
std::atomic<uint64_t> g_frame{0};
bool g_xrEnabled = true;

void** VTable(void* obj) { return *reinterpret_cast<void***>(obj); }

// One step per deliberate press: a key counts as pressed again only after it has been fully up for
// 300 ms (some keyboards report a held key as rapid up/down).
bool KeyTap(int vk) {
    static ULONGLONG lastDown[256] = {};
    static bool armed[256] = {};
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    const bool dn = fg == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    if (!dn) {
        if (now - lastDown[vk & 255] > 300) armed[vk & 255] = true;
        return false;
    }
    lastDown[vk & 255] = now;
    if (!armed[vk & 255]) return false;
    armed[vk & 255] = false;
    return true;
}

// ---------- clean OpenXR shutdown when the game window closes ----------
// Without this, SteamVR's threads can keep DarkSoulsIII.exe alive after the window is gone.
WNDPROC g_origWndProc = nullptr;
HWND g_gameHwnd = nullptr;

// The game window's client size when we started watching it. The game sizes its swap chain once at
// startup; any later resize makes it rebuild the swap chain, which geo-11 does not survive (crash in
// geo-11's d3d11.dll), and even when it survives the stereo image no longer matches. So once the game
// is running, the window keeps the size it started with: resizes are refused, and a WM_SIZE that still
// gets through with another size is not passed on to the game (DXGI simply scales the image).
int g_baseW = 0, g_baseH = 0;

LRESULT CALLBACK GameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_WINDOWPOSCHANGING && g_baseW > 0) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(l);
        if (!(wp->flags & SWP_NOSIZE) && !IsIconic(h)) {
            RECT r{};
            GetWindowRect(h, &r);
            const int cx = r.right - r.left, cy = r.bottom - r.top;
            if (wp->cx != cx || wp->cy != cy) {
                static int logged = 0;
                if (logged++ < 10) Log("game window: something tried to resize it from %dx%d to %dx%d; kept its size", cx, cy, wp->cx, wp->cy);
                wp->flags |= SWP_NOSIZE;
            }
        }
    }
    if (m == WM_SIZE) {
        static int logged = 0;
        const int cw = LOWORD(l), ch = HIWORD(l);
        if (logged++ < 20) Log("game window: WM_SIZE type %u, client %dx%d", static_cast<unsigned>(w), cw, ch);
        if ((w == SIZE_RESTORED || w == SIZE_MAXIMIZED) && g_baseW > 0 && (std::abs(cw - g_baseW) > 2 || std::abs(ch - g_baseH) > 2)) {
            Log("game window: size change to %dx%d not passed to the game (keeps %dx%d, so its graphics are not rebuilt)", cw, ch,
                g_baseW, g_baseH);
            return 0;
        }
    }
    if (m == WM_CLOSE) {
        Log("game window: WM_CLOSE, asking OpenXR to shut down");
        XrRequestShutdown();
    } else if (m == WM_DESTROY) {
        Log("game window: WM_DESTROY, shutting OpenXR down now");
        XrShutdownNow();
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

void SubclassGameWindow(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow || d.OutputWindow == g_gameHwnd) return;
    g_gameHwnd = d.OutputWindow;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWndProc)));
    Log("game window %p: watching for close (%s)", static_cast<void*>(g_gameHwnd), g_origWndProc ? "ok" : "FAILED");
    RECT c{};
    GetClientRect(g_gameHwnd, &c);
    const bool iconic = IsIconic(g_gameHwnd) != 0;
    Log("game window: client %ldx%ld%s", c.right - c.left, c.bottom - c.top, iconic ? ", minimized" : "");
    if (!iconic && c.right > 0 && c.bottom > 0) {
        g_baseW = c.right - c.left;
        g_baseH = c.bottom - c.top;
    }
}

void LogFirstPresent(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d))) {
        Log("game swap chain %p: %ux%u fmt %u, buffers %u, swapEffect %u, windowed %d, hwnd %p",
            static_cast<void*>(sc), d.BufferDesc.Width, d.BufferDesc.Height, static_cast<UINT>(d.BufferDesc.Format),
            d.BufferCount, static_cast<UINT>(d.SwapEffect), d.Windowed ? 1 : 0, static_cast<void*>(d.OutputWindow));
    }
}

void OnPresent(IDXGISwapChain* sc) {
    const uint64_t frame = g_frame.fetch_add(1) + 1;
    if (frame == 1) {
        Log("first Present (hook is live)");
        LogFirstPresent(sc);
        std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
        g_xrEnabled = GetPrivateProfileIntW(L"ds3vr", L"OpenXR", 1, ini.c_str()) != 0;
        if (!g_xrEnabled) Log("ds3vr.ini says OpenXR=0: not starting OpenXR");
        SubclassGameWindow(sc);
        HudInit();
    }

    // Hotkeys (F3 and F4 belong to the geo-11 fix; F13 to F21 are world scale steps the menu presses for geo-11).
    // F1 snapshot, F2 draw recording, F6 HUD size, F7 HUD mode, F8 recenter (world scale: mod menu).
    // Developer keys (switched on in the VR menu): F1 snapshot, F2 draw recording.
    const bool f1 = KeyTap(VK_F1), f2 = KeyTap(VK_F2);
    if (LogVerbose()) {
        if (f1) XrRequestCapture();
        if (f2) DrawLogRequest();
    }
    if (KeyTap(VK_F6)) XrCycleHudSize();
    if (KeyTap(VK_F7)) XrCycleHudMode();
    if (KeyTap(VK_F8)) XrRecenter();

    {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
            DrawLogOnPresent(dev);
            dev->Release();
        }
    }

    CamHookTick();

    if (g_xrEnabled) {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
            {
                ID3D11DeviceContext* mc = nullptr;
                dev->GetImmediateContext(&mc);
                if (mc) {
                    MenuTick(dev, mc);
                    mc->Release();
                }
            }
            XrOnPresent(dev, sc);
            ID3D11DeviceContext* ctx = nullptr;
            dev->GetImmediateContext(&ctx);
            if (ctx) {
                HudEndFrame(ctx);
                ctx->Release();
            }
            dev->Release();
        }
        const float hz = XrDisplayHz();
        if (hz > 0.f) {
            static ULONGLONG lastHzLog = 0;
            if (!lastHzLog) { lastHzLog = 1; Log("headset refresh rate %.1f Hz", hz); }
            // SteamVR reports half the refresh rate while it is throttling (frames missed); follow the
            // highest rate seen so the game's cap does not flip between 72 and 36.
            static float best = 0.f;
            if (hz > best) best = hz;
            FpsTick(best);
        }
    }
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_origPresent = nullptr;
HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_presentDepth == 0 && !(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    ++t_presentDepth;
    // With the frame rate unlocked, the headset (xrWaitFrame) paces frames, not the monitor's V-Sync.
    HRESULT hr = g_origPresent(sc, FpsNoVsync() ? 0 : sync, flags);
    --t_presentDepth;
    return hr;
}

using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Present1Fn g_origPresent1 = nullptr;
HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_presentDepth == 0 && !(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    ++t_presentDepth;
    HRESULT hr = g_origPresent1(sc, FpsNoVsync() ? 0 : sync, flags, p);
    --t_presentDepth;
    return hr;
}

bool HookOne(const char* name, void* target, void* detour, void** orig) {
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        Log("hook %s at %p failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    Log("hooked %s at %p", name, target);
    return true;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

std::wstring ModuleName(void* addr, HMODULE* modOut = nullptr) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &m) && m)
        GetModuleFileNameW(m, name, MAX_PATH);
    if (modOut) *modOut = m;
    return name;
}

std::wstring ProbeFilePath() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"ds3vr_dxgi_probe.txt";
}

// Runs rundll32 on our own DLL (ProbeDxgi below) and reads back where Present and Present1 live
// inside dxgi.dll. Making a D3D device and swap chain inside the game process crashes with geo-11
// installed, so the throwaway device lives in that separate helper process instead.
std::wstring g_d3dPath;                 // system d3d11.dll as seen by the helper
unsigned long long g_rvaCreateTex2D = 0;  // ID3D11Device::CreateTexture2D inside it

bool ProbeInHelper(std::wstring& dxgiPath, unsigned long long& rvaPresent, unsigned long long& rvaPresent1) {
    const std::wstring out = ProbeFilePath();
    DeleteFileW(out.c_str());
    wchar_t self[MAX_PATH] = {};
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ProbeFilePath), &me);
    GetModuleFileNameW(me, self, MAX_PATH);
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\rundll32.exe\" \"" + self + L"\",ProbeDxgi " + out;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        Log("probe: could not start rundll32 (error %lu)", GetLastError());
        return false;
    }
    const DWORD w = WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FILE* f = _wfopen(out.c_str(), L"r");
    if (!f) {
        Log("probe: helper produced no result (wait %lu, exit code %lu)", w, code);
        return false;
    }
    char narrow[MAX_PATH * 3] = {};
    const int got = fscanf(f, "%779[^\n]\n%llx\n%llx\n", narrow, &rvaPresent, &rvaPresent1);
    {
        char d3d[MAX_PATH * 3] = {};
        unsigned long long rvaTex = 0;
        if (fscanf(f, "%779[^\n]\n%llx", d3d, &rvaTex) == 2) {
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, d3d, -1, w, MAX_PATH);
            g_d3dPath = w;
            g_rvaCreateTex2D = rvaTex;
        }
    }
    fclose(f);
    DeleteFileW(out.c_str());
    wchar_t path[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, path, MAX_PATH);
    if (got < 2 || !rvaPresent || !path[0]) {
        Log("probe: could not read the helper's result (%d fields)", got);
        return false;
    }
    dxgiPath = path;
    return true;
}

} // namespace

void InstallPresentHooks() {
    Log("waiting for dxgi.dll ...");
    for (int i = 0; i < 1200; ++i) {  // up to 2 minutes
        if (GetModuleHandleW(L"dxgi.dll")) break;
        Sleep(100);
    }
    {
        wchar_t p[MAX_PATH] = {};
        if (GetModuleHandleW(L"d3d11.dll")) {
            GetModuleFileNameW(GetModuleHandleW(L"d3d11.dll"), p, MAX_PATH);
            Log("the game's d3d11.dll is %ls", p);
        }
    }

    std::wstring dxgiPath;
    unsigned long long rva = 0, rva1 = 0;
    if (!ProbeInHelper(dxgiPath, rva, rva1)) return;
    Log("probe: Present at %ls+%llx, Present1 at +%llx", dxgiPath.c_str(), rva, rva1);

    HMODULE dxgi = nullptr;
    for (int i = 0; i < 1200 && !dxgi; ++i) {
        dxgi = GetModuleHandleW(dxgiPath.c_str());
        if (!dxgi) Sleep(100);
    }
    if (!dxgi) {
        Log("the game never loaded %ls; cannot hook Present", dxgiPath.c_str());
        return;
    }
    void* presentTarget = reinterpret_cast<uint8_t*>(dxgi) + rva;
    void* present1Target = rva1 ? reinterpret_cast<uint8_t*>(dxgi) + rva1 : nullptr;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed: %s", MH_StatusToString(s));
        return;
    }
    HookOne("IDXGISwapChain::Present", presentTarget, reinterpret_cast<void*>(&PresentDetour),
            reinterpret_cast<void**>(&g_origPresent));
    if (present1Target)
        HookOne("IDXGISwapChain1::Present1", present1Target, reinterpret_cast<void*>(&Present1Detour),
                reinterpret_cast<void**>(&g_origPresent1));
    MenuInstallInputHooks();
    Log("hooks installed. Keys: F1 snapshot, F2 draw recording, F6 HUD size, F7 HUD mode, F8 recenter; both sticks or Insert: menu");

}

} // namespace ds3vr

// rundll32 entry point (helper process, no geo-11 there): make a throwaway device and swap chain on
// the system d3d11.dll and write dxgi.dll's path and the Present / Present1 offsets to the file
// named on the command line.
extern "C" __declspec(dllexport) void CALLBACK ProbeDxgi(HWND, HINSTANCE, LPSTR cmdLine, int) {
    using namespace ds3vr;
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE d3d = LoadLibraryW((std::wstring(sys) + L"\\d3d11.dll").c_str());
    if (!d3d) return;
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        reinterpret_cast<void*>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain")));
    if (!create) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ds3vr_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)) || !sc)
        return;
    void* p = VTable(sc)[kPresent];
    void* p1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) p1 = VTable(sc1)[kPresent1];
    HMODULE m = nullptr;
    const std::wstring path = ModuleName(p, &m);
    HMODULE m1 = nullptr;
    if (p1) ModuleName(p1, &m1);
    const unsigned long long rva = reinterpret_cast<uint8_t*>(p) - reinterpret_cast<uint8_t*>(m);
    const unsigned long long rva1 = (p1 && m1 == m) ? static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(p1) - reinterpret_cast<uint8_t*>(m)) : 0ULL;
    FILE* f = fopen(cmdLine, "w");
    if (f) {
        char narrow[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        fprintf(f, "%s\n%llx\n%llx\n", narrow, rva, rva1);
        // ID3D11Device::CreateTexture2D (vtable slot 5) inside the system d3d11.dll
        void* ct = VTable(dev)[5];
        HMODULE md = nullptr;
        const std::wstring dpath = ModuleName(ct, &md);
        char dn[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, dpath.c_str(), -1, dn, sizeof(dn), nullptr, nullptr);
        fprintf(f, "%s\n%llx\n", dn, static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(ct) - reinterpret_cast<uint8_t*>(md)));
        fclose(f);
    }
    if (sc1) sc1->Release();
    if (ctx) ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
}
