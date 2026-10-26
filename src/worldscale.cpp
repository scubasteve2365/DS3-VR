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
constexpr int kSepBase = 20;                 // geo-11 separation the normal view is aligned for
constexpr float kPerUnit = 0.001045f;        // geo-11 NDC eye shift per unit of dm_separation (0.0209 at 20)
float g_scale = 1.f;
int g_heldVk = 0;          // key currently held down for geo-11
int g_frames = 0;          // frames left before releasing it
int g_sep = kSepBase;                        // dm_separation written at this launch
float g_convPerScale = kBaseConvergence;     // dm_convergence per unit of world scale, this launch

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
    snprintf(b, sizeof(b), "%.3f", g_convPerScale * s);
    return b;
}

void Key(int vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

struct EyeViewDecision {
    bool on;
    int sep;               // dm_separation
    float convPerScale;    // dm_convergence per unit of world scale (negative in the each-eye view)
};

// Decided once per run, on the first call (DllMain, before the game reads its settings), from
// ds3vr.ini (EachEyeView = 1) and the headset values an earlier run measured in VR: state
// HeadsetEyeK (each eye's lens centre as a fraction of the declared half-width) and HeadsetAspectEye.
// Kept after that, so a change made in the menu, or a measurement taken later, cannot switch the
// view while the game runs (geo-11's settings are fixed at launch).
const EyeViewDecision& Decided() {
    static const EyeViewDecision d = [] {
        EyeViewDecision r{false, kSepBase, kBaseConvergence};
        const std::wstring ini = Path(L"ds3vr.ini");
        if (GetPrivateProfileIntW(L"ds3vr", L"EachEyeView", 0, ini.c_str()) != 1) return r;
        if (GetPrivateProfileIntW(L"ds3vr", L"HeadsetAspect", 1, ini.c_str()) == 0) {
            Log("WORLDSCALE: each-eye view needs the headset-shaped render size (RenderAspect); using the normal view");
            return r;
        }
        wchar_t v[32] = {};
        GetPrivateProfileStringW(L"state", L"HeadsetEyeK", L"", v, 32, ini.c_str());
        const float k = static_cast<float>(_wtof(v));
        GetPrivateProfileStringW(L"state", L"HeadsetAspectEye", L"", v, 32, ini.c_str());
        const float asp = static_cast<float>(_wtof(v));
        if (k < 0.005f || k > 0.3f || asp < 0.6f || asp > 2.5f) {
            Log("WORLDSCALE: each-eye view is on, but this headset's eye offsets are not measured yet; using the normal view this run");
            return r;
        }
        const int sep = static_cast<int>(std::lround(k / kPerUnit));
        if (sep < kSepBase || sep > 200) {
            Log("WORLDSCALE: each-eye separation %d is out of range; using the normal view", sep);
            return r;
        }
        // The product dm_separation x dm_convergence sets the eye translation, and so the world scale.
        // Keeping it equal to the normal view's (20 x 1.4834) keeps the world the same size.
        r.on = true;
        r.sep = sep;
        r.convPerScale = -(kSepBase * kBaseConvergence) / static_cast<float>(sep);
        Log("WORLDSCALE: each-eye view on: separation %d, convergence per scale %.4f (eye offset %.4f)", sep, r.convPerScale, k);
        return r;
    }();
    return d;
}

} // namespace

void WorldScaleEarly() {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"ds3vr", L"WorldScale", L"1", v, 32, Path(L"ds3vr.ini").c_str());
    g_scale = Snap(static_cast<float>(_wtof(v)));
    const EyeViewDecision& ev = Decided();
    g_sep = ev.sep;
    g_convPerScale = ev.convPerScale;
    const std::string sepStr = std::to_string(g_sep);
    std::string dm;
    bool ok = false;
    if (ReadAll(Path(L"d3dxdm.ini"), dm)) {
        const std::string before = dm;
        ok = SetLine(dm, "dm_convergence", Conv(g_scale));
        // The VR image alignment is calculated for geo-11 separation 20 with automatic convergence off
        // (or, in the each-eye view, the separation and convergence derived from the headset).
        // Put them back every launch, so a change made by hand or with geo-11's hotkeys cannot leave
        // the two eyes misaligned (a doubled, out-of-focus picture).
        const std::string sep = GetLine(dm, "dm_separation"), autoConv = GetLine(dm, "dm_auto_convergence");
        if (!sep.empty() && sep != sepStr) {
            SetLine(dm, "dm_separation", sepStr);
            Log("WORLDSCALE: geo-11 separation was %s; reset to %s (DS3 VR is aligned for it)", sep.c_str(), sepStr.c_str());
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
    Log("WORLDSCALE: starting at %.1f (geo-11 separation %s, convergence %s, each-eye view %s): d3dxdm.ini %s",
        g_scale, sepStr.c_str(), Conv(g_scale).c_str(), ev.on ? "on" : "off", ok ? "ok" : "NOT updated");
}

float WorldScaleGet() { return g_scale; }

bool WorldScaleBusy() { return g_heldVk != 0; }

bool WorldScaleEyeViewActive() { return Decided().on; }

bool WorldScaleEyeViewWanted() {
    return GetPrivateProfileIntW(L"ds3vr", L"EachEyeView", 0, Path(L"ds3vr.ini").c_str()) == 1;
}

void WorldScaleSetEyeViewWanted(bool on) {
    WritePrivateProfileStringW(L"ds3vr", L"EachEyeView", on ? L"1" : L"0", Path(L"ds3vr.ini").c_str());
    Log("menu: each-eye view %s (applies at the next launch)", on ? "on" : "off");
}

void WorldScaleSet(float scale) {
    scale = Snap(scale);
    if (g_heldVk) return;  // one change at a time (takes a few frames)
    const int step = static_cast<int>(std::lround((scale - 1.f) * 2.f));
    if (step < 0 || step >= kSteps) return;
    wchar_t v[16];
    swprintf(v, 16, L"%.1f", scale);
    if (Decided().on) {
        // The convergence keys set absolute values for the normal view; in the each-eye view they would
        // break the eye offset. The new scale is saved and applied at the next launch.
        g_scale = scale;
        WritePrivateProfileStringW(L"ds3vr", L"WorldScale", v, Path(L"ds3vr.ini").c_str());
        Log("menu: world scale %.1f saved; it applies at the next launch (each-eye view)", scale);
        return;
    }
    g_heldVk = VK_F13 + step;
    g_frames = 4;  // geo-11 checks keys once per frame; hold long enough to be seen
    Key(g_heldVk, true);
    g_scale = scale;
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
