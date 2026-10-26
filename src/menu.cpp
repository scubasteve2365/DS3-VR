#include "menu.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "MinHook.h"
#include "camhook.h"
#include "firstperson.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "log.h"
#include "menu_font.h"
#include "rendersize.h"
#include "version.h"
#include "worldscale.h"
#include "xr.h"

// Dark Souls III menu colours, matched by eye from the game's own menus (no pixel sampling).
static const ImVec4 kMenuAccent(0.85f, 0.68f, 0.32f, 1.f);    // headings and the focus ring (was bright yellow)
static const ImVec4 kMenuSelected(0.62f, 0.44f, 0.16f, 1.f);  // a chosen option (was orange)

// ImGui's DX11 backend compiles its two small shaders with D3DCompile at start-up. Rather than
// making dinput8.dll depend on d3dcompiler_47.dll at load time, load it only when it is needed.
extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, const char* entry, const char* target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    static pD3DCompile fn = nullptr;
    if (!fn) {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
        if (m) fn = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")));
        if (!fn) return E_FAIL;
    }
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace ds3vr {
namespace {

constexpr UINT kW = 1280, kH = 1250;

// ---------- gamepad (XInput) ----------
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn o_GetState = nullptr;    // XInputGetState (ordinal 2)
XInputGetStateFn o_GetStateEx = nullptr;  // XInputGetStateEx (ordinal 100, includes the guide button)
std::atomic<bool> g_open{false};
std::atomic<unsigned> g_openGen{0};
std::atomic<bool> g_swallowThumbs{false};  // both sticks held: hide the clicks from the game until released

void Filter(XINPUT_STATE* s) {
    if (g_open.load(std::memory_order_relaxed)) {
        const DWORD pkt = s->dwPacketNumber;
        ZeroMemory(&s->Gamepad, sizeof(s->Gamepad));
        s->dwPacketNumber = pkt;
    } else {
        if (g_swallowThumbs.load(std::memory_order_relaxed))
            s->Gamepad.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
        if (FirstPersonActive()) {
            // First person: the game moves along its camera, but you look with the headset. Turn the left
            // stick by the head's turn, so pushing up moves where you look (stick up = forward, x = right;
            // a head turned left by y makes "up" point left by y).
            const float y = CamHookHeadYaw();
            if (std::fabs(y) > 0.001f) {
                const float c = std::cos(y), sn = std::sin(y);
                const float x0 = s->Gamepad.sThumbLX, y0 = s->Gamepad.sThumbLY;
                const float x1 = x0 * c - y0 * sn, y1 = x0 * sn + y0 * c;
                auto clamp = [](float v) { return static_cast<SHORT>(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v); };
                s->Gamepad.sThumbLX = clamp(x1);
                s->Gamepad.sThumbLY = clamp(y1);
            }
        }
    }
}

DWORD WINAPI GetStateDetour(DWORD user, XINPUT_STATE* s) {
    const DWORD r = o_GetState(user, s);
    if (r == ERROR_SUCCESS && s) Filter(s);
    return r;
}
DWORD WINAPI GetStateExDetour(DWORD user, XINPUT_STATE* s) {
    const DWORD r = o_GetStateEx(user, s);
    if (r == ERROR_SUCCESS && s) Filter(s);
    return r;
}

// Our own view of the first connected pad (unfiltered).
bool ReadPad(XINPUT_GAMEPAD* g) {
    XInputGetStateFn fn = o_GetStateEx ? o_GetStateEx : o_GetState;
    if (!fn) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE s{};
        if (fn(i, &s) == ERROR_SUCCESS) {
            *g = s.Gamepad;
            return true;
        }
    }
    return false;
}

// ---------- drawing ----------
bool g_imguiReady = false, g_imguiFailed = false;
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
LARGE_INTEGER g_freq{}, g_last{};

bool InitImGui(ID3D11Device* dev, ID3D11DeviceContext* c) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex)) || FAILED(dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv))) {
        Log("menu: could not create the menu texture");
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouse;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
    // EB Garamond is embedded (menu_font.h), so the menu looks the same on every PC.
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false;  // the font bytes are static data
    if (!io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFontData), static_cast<int>(kMenuFontSize), 44.f, &fontCfg)) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.f * 3.f;
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.ScaleAllSizes(2.2f);
    st.WindowRounding = 18.f;
    st.FrameRounding = 10.f;
    // Warm near-black panels, parchment text, bronze-gold accents.
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.045f, 0.035f, 0.94f);
    st.Colors[ImGuiCol_Text] = ImVec4(0.87f, 0.81f, 0.67f, 1.f);
    st.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.45f, 0.36f, 1.f);
    st.Colors[ImGuiCol_Separator] = ImVec4(0.42f, 0.34f, 0.20f, 1.f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.14f, 0.11f, 1.f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.23f, 0.16f, 1.f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.37f, 0.30f, 0.18f, 1.f);
    st.Colors[ImGuiCol_NavCursor] = kMenuAccent;
    if (!ImGui_ImplDX11_Init(dev, c)) {
        Log("menu: ImGui DX11 backend failed to start");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    Log("menu: ready (%ux%u)", kW, kH);
    return true;
}

// One "label   [-]  value  [+]" row. Returns -1, 0 or +1.
int Stepper(const char* label, const char* value, const char* id) {
    int r = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(430.f);
    ImGui::PushID(id);
    if (ImGui::Button(" - ", ImVec2(90.f, 0.f))) r = -1;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const float x = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(value);
    ImGui::SameLine(x + 430.f);
    if (ImGui::Button(" + ", ImVec2(90.f, 0.f))) r = +1;
    ImGui::PopID();
    return r;
}

bool g_wantClose = false;

void SetDevKeys(bool on) {
    const std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    WritePrivateProfileStringW(L"ds3vr", L"DevKeys", on ? L"1" : L"0", ini.c_str());
    if (on) LogSetVerbose(true);
    Log("menu: developer keys %s", on ? "on (F1 snapshot, F2 draw recording, full log)" : "off");
    if (!on) LogSetVerbose(false);
}

int g_advanced = -1;  // -1 = not read from ds3vr.ini yet

bool AdvancedShown() {
    if (g_advanced < 0)
        g_advanced = GetPrivateProfileIntW(L"ds3vr", L"AdvancedSettings", 0, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str()) != 0;
    return g_advanced == 1;
}

void SetAdvancedShown(bool on) {
    g_advanced = on ? 1 : 0;
    WritePrivateProfileStringW(L"ds3vr", L"AdvancedSettings", on ? L"1" : L"0", (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
}

void BuildUi() {
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    static unsigned focusedGen = 0;
    if (focusedGen != g_openGen.load()) {
        ImGui::SetNextWindowFocus();
        focusedGen = g_openGen.load();
    }
    ImGui::Begin("DS3 VR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(kMenuAccent, "DARK SOULS III VR");
    ImGui::SameLine();
    ImGui::TextDisabled("v" DS3VR_VERSION);
    ImGui::Separator();
    ImGui::Spacing();

    // View: third person (the default) or first person (First Person Souls), switched while you play
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("View");
        ImGui::SameLine(430.f);
        if (FirstPersonAvailable()) {
            const bool fp = FirstPersonWanted();
            if (fp) ImGui::PushStyleColor(ImGuiCol_Button, kMenuSelected);
            if (ImGui::Button(fp ? "First person" : "Third person  (default)", ImVec2(0.f, 0.f))) FirstPersonSet(!fp);
            if (fp) ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Third person  (first person unavailable: see ds3vr.log)");
        }
    }

    char v[96];
    // World scale
    const float ws = WorldScaleGet();
    const char* note = ws == 1.f ? "  (life size)" : ws == 0.25f ? "  (largest)" : ws == 10.f ? "  (smallest)" : "";
    snprintf(v, sizeof(v), ws < 1.f ? "%.2f%s" : "%.1f%s", ws, note);
    if (const int d = Stepper("World scale", v, "ws")) WorldScaleStep(d);
    {
        const bool lock = CamHookGetHorizonLock();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Horizon lock");
        ImGui::SameLine(430.f);
        if (lock) ImGui::PushStyleColor(ImGuiCol_Button, kMenuSelected);
        if (ImGui::Button(lock ? "On  (stick does not tilt the view)" : "Off  (free camera)", ImVec2(0.f, 0.f))) CamHookSetHorizonLock(!lock);
        if (lock) ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    // HUD
    HudSettings h = XrGetHudSettings();
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("HUD");
    ImGui::SameLine(430.f);
    const char* modes[3] = {"Fixed", "Follows head", "Hidden"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool sel = h.mode == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, kMenuSelected);
        if (ImGui::Button(modes[i])) {
            h.mode = i;
            changed = true;
        }
        if (sel) ImGui::PopStyleColor();
    }
    snprintf(v, sizeof(v), "%.1f m wide", h.width);
    if (const int d = Stepper("HUD size", v, "hw")) { h.width += 0.1f * d; changed = true; }
    snprintf(v, sizeof(v), "%.1f m away", h.distance);
    if (const int d = Stepper("HUD distance", v, "hd")) { h.distance += 0.1f * d; changed = true; }
    snprintf(v, sizeof(v), h.drop >= 0.f ? "%.2f m below eyes" : "%.2f m above eyes", std::fabs(h.drop));
    if (const int d = Stepper("HUD height", v, "hh")) { h.drop -= 0.05f * d; changed = true; }
    if (changed) XrSetHudSettings(h, true);

    // Resolution (read-only): what the game renders now, what SteamVR asks for, what the next launch uses.
    {
        // Ask the VR runtime (through a fresh helper process) each time the menu opens.
        static unsigned askedGen = 0;
        if (askedGen != g_openGen.load()) {
            askedGen = g_openGen.load();
            RenderSizeCheckSteamVR();
        }
        unsigned askW = 0, askH = 0;
        const int ask = RenderSizeSteamVRAnswer(&askW, &askH);
        unsigned ew = 0, eh = 0, cw = 0, ch = 0, nw = 0, nh = 0;
        bool fixed = false;
        const bool haveNow = XrEyeImageSize(&ew, &eh);
        const bool haveCfg = RenderSizeGet(&cw, &ch);
        const bool haveNext = RenderSizeNextLaunch(&nw, &nh, &fixed);
        ImGui::Spacing();
        ImGui::TextUnformatted("Resolution per eye");
        if (haveNow) snprintf(v, sizeof(v), "%u x %u", ew, eh);
        else snprintf(v, sizeof(v), "-");
        ImGui::TextDisabled("  Game now");
        ImGui::SameLine(430.f);
        ImGui::TextUnformatted(v);
        if (ask == 1) snprintf(v, sizeof(v), "checking...");
        else if (ask == 2) snprintf(v, sizeof(v), "%u x %u", askW, askH);
        else snprintf(v, sizeof(v), "no answer from the VR runtime");
        ImGui::TextDisabled("  VR runtime asks for");
        ImGui::SameLine(430.f);
        ImGui::TextUnformatted(v);
        ImGui::SameLine();
        if (ImGui::Button("Check again") && ask != 1) RenderSizeCheckSteamVR();
        if (haveNext) {
            const bool same = haveCfg && nw == cw && nh == ch;
            snprintf(v, sizeof(v), "%u x %u%s%s", nw, nh, fixed ? "  (fixed in ds3vr.ini)" : "", same ? "  (same as now)" : "  (restart the game to apply)");
        } else {
            snprintf(v, sizeof(v), "game's own setting (RenderHeight=-1)");
        }
        ImGui::TextDisabled("  Next launch");
        ImGui::SameLine(430.f);
        ImGui::TextUnformatted(v);
        ImGui::TextDisabled("  Set it in your VR runtime (SteamVR, Virtual Desktop, Meta, Pimax Play),");
        ImGui::TextDisabled("  then reopen this menu or Check again. It applies at the next launch.");
        ImGui::Spacing();
    }

    // Advanced settings (head-motion steadiness), shown on request
    {
        const bool adv = AdvancedShown();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Advanced settings");
        ImGui::SameLine(430.f);
        if (ImGui::Button(adv ? "Shown" : "Hidden", ImVec2(0.f, 0.f))) SetAdvancedShown(!adv);
        if (adv) {
            const int lag = XrGetPoseLag();
            snprintf(v, sizeof(v), "%d%s", lag, lag == 2 ? "  (default)" : "");
            if (const int d = Stepper("  Pose lag", v, "pl")) XrSetPoseLag(lag + d);
            const bool sh = CamHookGetScaleHead();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("  Head movement");
            ImGui::SameLine(430.f);
            if (ImGui::Button(sh ? "x world scale  (default)" : "life size", ImVec2(0.f, 0.f))) CamHookSetScaleHead(!sh);
        }
    }

    // Developer keys
    {
        bool dev = LogVerbose();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Developer keys");
        ImGui::SameLine(430.f);
        if (ImGui::Button(dev ? "On  (F1 snapshot, F2 recording, full log)" : "Off", ImVec2(0.f, 0.f))) SetDevKeys(!dev);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Recenter view", ImVec2(380.f, 0.f))) XrRecenter();
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults", ImVec2(380.f, 0.f))) {
        XrSetHudSettings(HudSettings{0, 1.0f, 1.2f, 0.10f}, true);
        if (FirstPersonWanted()) FirstPersonSet(false);  // first, so the scale below is saved as the third-person one
        WorldScaleSet(1.f);
        XrSetPoseLag(2);
        CamHookSetScaleHead(true);
        CamHookSetHorizonLock(false);
        SetAdvancedShown(false);
        SetDevKeys(false);
        Log("menu: settings reset to defaults");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(380.f, 0.f))) g_wantClose = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Both sticks / Insert: open & close.  A: select.  B: close.");
    ImGui::TextDisabled("Keys: F6 HUD size, F7 HUD mode, F8 recenter");
    ImGui::End();
}

void Render(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    c->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    c->OMSetRenderTargets(1, &g_rtv, nullptr);
    const FLOAT clear[4] = {0.f, 0.f, 0.f, 0.f};
    c->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    c->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

bool KeyDown(int vk) {
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    return fg == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void SetOpen(bool open) {
    if (open == g_open.load()) return;
    if (open) g_openGen.fetch_add(1);
    g_open.store(open);
    Log("menu: %s", open ? "opened" : "closed");
}

} // namespace

void MenuInstallInputHooks() {
    HMODULE x = GetModuleHandleW(L"xinput1_3.dll");
    if (!x) x = GetModuleHandleW(L"xinput1_4.dll");
    if (!x) x = LoadLibraryW(L"xinput1_3.dll");
    if (!x) {
        Log("menu: no XInput DLL; the gamepad cannot open the menu (Insert still does)");
        return;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return;
    int ok = 0;
    if (void* f = reinterpret_cast<void*>(GetProcAddress(x, reinterpret_cast<LPCSTR>(2)))) {
        if (MH_CreateHook(f, reinterpret_cast<void*>(&GetStateDetour), reinterpret_cast<void**>(&o_GetState)) == MH_OK &&
            MH_EnableHook(f) == MH_OK)
            ++ok;
    }
    if (void* f = reinterpret_cast<void*>(GetProcAddress(x, reinterpret_cast<LPCSTR>(100)))) {
        if (MH_CreateHook(f, reinterpret_cast<void*>(&GetStateExDetour), reinterpret_cast<void**>(&o_GetStateEx)) == MH_OK &&
            MH_EnableHook(f) == MH_OK)
            ++ok;
    }
    Log("menu: %d XInput hook(s) installed", ok);
}

bool MenuIsOpen() { return g_open.load(); }
unsigned MenuOpenGeneration() { return g_openGen.load(); }
ID3D11Texture2D* MenuTexture() { return g_imguiReady ? g_tex : nullptr; }

void MenuTick(ID3D11Device* dev, ID3D11DeviceContext* c) {
    WorldScaleTick();

    // Open / close: both thumbsticks clicked together, or Insert.
    XINPUT_GAMEPAD pad{};
    const bool havePad = ReadPad(&pad);
    const WORD thumbs = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    const bool both = havePad && (pad.wButtons & thumbs) == thumbs;
    const bool anyThumb = havePad && (pad.wButtons & thumbs) != 0;
    static bool bothWas = false, insWas = false;
    if (both && !bothWas) {
        SetOpen(!g_open.load());
        g_swallowThumbs.store(true);
    }
    if (!anyThumb) g_swallowThumbs.store(false);
    bothWas = both;
    const bool ins = KeyDown(VK_INSERT);
    if (ins && !insWas) SetOpen(!g_open.load());
    insWas = ins;

    if (!g_open.load() || g_imguiFailed) return;
    if (!g_imguiReady) {
        if (!InitImGui(dev, c)) {
            g_imguiFailed = true;
            return;
        }
        g_imguiReady = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = std::max(1e-4f, static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart));
    g_last = now;
    // Gamepad
    const WORD b = havePad ? pad.wButtons : 0;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (b & XINPUT_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (b & XINPUT_GAMEPAD_A) != 0);
    const float lx = havePad ? pad.sThumbLX / 32767.f : 0.f, ly = havePad ? pad.sThumbLY / 32767.f : 0.f;
    auto stick = [&](ImGuiKey k, float v) { io.AddKeyAnalogEvent(k, v > 0.5f, v > 0.f ? v : 0.f); };
    stick(ImGuiKey_GamepadLStickLeft, -lx);
    stick(ImGuiKey_GamepadLStickRight, lx);
    stick(ImGuiKey_GamepadLStickUp, ly);
    stick(ImGuiKey_GamepadLStickDown, -ly);
    // Keyboard
    io.AddKeyEvent(ImGuiKey_UpArrow, KeyDown(VK_UP));
    io.AddKeyEvent(ImGuiKey_DownArrow, KeyDown(VK_DOWN));
    io.AddKeyEvent(ImGuiKey_LeftArrow, KeyDown(VK_LEFT));
    io.AddKeyEvent(ImGuiKey_RightArrow, KeyDown(VK_RIGHT));
    io.AddKeyEvent(ImGuiKey_Enter, KeyDown(VK_RETURN));
    // B (or Backspace) closes the menu.
    static bool backWas = false;
    const bool back = (b & XINPUT_GAMEPAD_B) != 0 || KeyDown(VK_BACK);
    if (back && !backWas) g_wantClose = true;
    backWas = back;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();
    Render(c);
    if (g_wantClose) {
        g_wantClose = false;
        SetOpen(false);
    }
    (void)dev;
}

} // namespace ds3vr
