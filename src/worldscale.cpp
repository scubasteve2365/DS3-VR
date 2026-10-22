#include "worldscale.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "log.h"

namespace ds3vr {
namespace {

// geo-11 convergence for world scale 1 (life size) with dm_separation 20, our measured stereo shift
// and the headset-shaped view; scale s uses s times this.
constexpr float kBaseConvergence = 1.4834f;
constexpr int kSteps = 9;  // 1.0, 1.5, ... 5.0
float g_scale = 1.f;
int g_heldVk = 0;          // key currently held down for geo-11
int g_frames = 0;          // frames left before releasing it

std::wstring Path(const wchar_t* name) { return std::wstring(ModuleDir()) + name; }

bool ReadAll(const std::wstring& p, std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    s.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return true;
}

bool WriteAll(const std::wstring& p, const std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

// Replaces the value of the first "key = value" line that starts a line.
bool SetLine(std::string& s, const std::string& key, const std::string& value) {
    size_t p = 0;
    while ((p = s.find(key, p)) != std::string::npos) {
        const bool lineStart = p == 0 || s[p - 1] == '\n';
        const size_t eq = s.find_first_not_of(" \t", p + key.size());
        if (lineStart && eq != std::string::npos && s[eq] == '=') {
            const size_t a = s.find_first_not_of(" \t", eq + 1);
            const size_t b = s.find_first_of("\r\n", eq + 1);
            s.replace(a, b - a, value);
            return true;
        }
        p += key.size();
    }
    return false;
}

const char* const kSeparation = "20";  // geo-11 dm_separation the VR alignment (StereoShift) is measured for

// The value of "key = value" at the start of a line, or "" if there is none.
std::string GetLine(const std::string& s, const std::string& key) {
    size_t p = 0;
    while ((p = s.find(key, p)) != std::string::npos) {
        const bool lineStart = p == 0 || s[p - 1] == '\n';
        const size_t eq = s.find_first_not_of(" \t", p + key.size());
        if (lineStart && eq != std::string::npos && s[eq] == '=') {
            const size_t a = s.find_first_not_of(" \t", eq + 1);
            const size_t b = s.find_first_of("\r\n", eq + 1);
            if (a == std::string::npos || a >= b) return std::string();
            std::string v = s.substr(a, b - a);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            return v;
        }
        p += key.size();
    }
    return std::string();
}

float Snap(float s) {
    s = std::round(s * 2.f) / 2.f;
    return s < 1.f ? 1.f : s > 5.f ? 5.f : s;
}

std::string Conv(float s) {
    char b[32];
    snprintf(b, sizeof(b), "%.3f", kBaseConvergence * s);
    return b;
}

void Key(int vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

} // namespace

void WorldScaleEarly() {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"ds3vr", L"WorldScale", L"1", v, 32, Path(L"ds3vr.ini").c_str());
    g_scale = Snap(static_cast<float>(_wtof(v)));
    std::string dm;
    bool ok = false;
    if (ReadAll(Path(L"d3dxdm.ini"), dm)) {
        const std::string before = dm;
        ok = SetLine(dm, "dm_convergence", Conv(g_scale));
        // The VR image alignment is calculated for geo-11 separation 20 with automatic convergence off.
        // Put both back every launch, so a change made by hand or with geo-11's hotkeys cannot leave
        // the two eyes misaligned (a doubled, out-of-focus picture).
        const std::string sep = GetLine(dm, "dm_separation"), autoConv = GetLine(dm, "dm_auto_convergence");
        if (!sep.empty() && sep != kSeparation) {
            SetLine(dm, "dm_separation", kSeparation);
            Log("WORLDSCALE: geo-11 separation was %s; reset to %s (DS3 VR is aligned for it)", sep.c_str(), kSeparation);
        }
        if (!autoConv.empty() && autoConv != "0") {
            SetLine(dm, "dm_auto_convergence", "0");
            Log("WORLDSCALE: geo-11 automatic convergence was on; switched off (DS3 VR sets convergence itself)");
        }
        // geo-11's convergence limits (3 / 4) are below the larger world scales; lift them so a saved
        // scale is not clipped at startup.
        SetLine(dm, "dm_max_convergence_soft", "10.0");
        SetLine(dm, "dm_max_convergence_hard", "10.0");
        if (ok && dm != before) ok = WriteAll(Path(L"d3dxdm.ini"), dm);
    }
    Log("WORLDSCALE: starting at %.1f (geo-11 convergence %s): d3dxdm.ini %s", g_scale, Conv(g_scale).c_str(), ok ? "ok" : "NOT updated");
}

float WorldScaleGet() { return g_scale; }

bool WorldScaleBusy() { return g_heldVk != 0; }

void WorldScaleSet(float scale) {
    scale = Snap(scale);
    if (g_heldVk) return;  // one change at a time (takes a few frames)
    const int step = static_cast<int>(std::lround((scale - 1.f) * 2.f));
    if (step < 0 || step >= kSteps) return;
    g_heldVk = VK_F13 + step;
    g_frames = 4;  // geo-11 checks keys once per frame; hold long enough to be seen
    Key(g_heldVk, true);
    g_scale = scale;
    wchar_t v[16];
    swprintf(v, 16, L"%.1f", scale);
    WritePrivateProfileStringW(L"ds3vr", L"WorldScale", v, Path(L"ds3vr.ini").c_str());
    Log("menu: world scale %.1f (geo-11 convergence %s, saved)", scale, Conv(scale).c_str());
}

void WorldScaleTick() {
    if (g_heldVk && --g_frames <= 0) {
        Key(g_heldVk, false);
        g_heldVk = 0;
    }
}

} // namespace ds3vr
