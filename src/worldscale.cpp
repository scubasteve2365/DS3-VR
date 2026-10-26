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
constexpr int kSepBase = 20;                 // geo-11 separation the normal view is aligned for
constexpr float kPerUnit = 0.001045f;        // geo-11 NDC eye shift per unit of dm_separation (0.0209 at 20)

// World scale steps the menu can reach. Below 1.0 the world looks bigger (0.25 is the largest),
// above 1.0 it looks smaller (10.0 is the smallest). One hotkey section per step (F13 upwards).
constexpr float kScales[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 7.0f, 10.0f};
constexpr int kSteps = static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));
constexpr float kLifeSize = 1.0f;

float g_scale = kLifeSize;
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

// Writes to a temporary file, then replaces the target, so a failed write cannot leave a half-written file.
bool ReplaceFile(const std::wstring& p, const std::string& s) {
    const std::wstring tmp = p + L".tmp";
    if (!WriteAll(tmp, s)) return false;
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
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

// The index of the step nearest to s.
int StepIndex(float s) {
    int best = 0;
    for (int i = 1; i < kSteps; ++i)
        if (std::fabs(kScales[i] - s) < std::fabs(kScales[best] - s)) best = i;
    return best;
}

float Snap(float s) { return kScales[StepIndex(s)]; }

std::string Conv(float s) {
    char b[32];
    snprintf(b, sizeof(b), "%.3f", g_convPerScale * s);
    return b;
}

// The hotkey sections geo-11 switches between, one per step, with this launch's convergence values.
std::string KeyBlock() {
    std::string out =
        "; ds3vr: world scale 0.25 (largest world) to 10.0 (smallest). The mod menu presses one of F13 to F24\n"
        "; (keys real keyboards do not have) to pick a step. DS3 VR writes these convergence values at each launch.\n";
    for (int i = 0; i < kSteps; ++i) {
        char b[256];
        snprintf(b, sizeof(b),
                 "[KeyDs3vrWorldScale%d]\nKey = no_modifiers VK_F%d\ntype = activate\nconvergence = %.3f\ntransition = 300\n"
                 "transition_type = cosine\n\n",
                 i + 1, 13 + i, g_convPerScale * kScales[i]);
        out += b;
    }
    return out;
}

// Replaces the world-scale hotkey block (from its first comment to the lock-on marker section).
bool UpdateKeyBlock(std::string& s) {
    const size_t a = s.find("; ds3vr: world scale");
    const size_t b = s.find("; ds3vr: the lock-on marker");
    if (a == std::string::npos || b == std::string::npos || b < a) return false;
    std::string block = KeyBlock();
    if (s.find("\r\n") != std::string::npos) {  // match the file's line endings (d3dx.ini from the installer uses CRLF)
        std::string crlf;
        for (char c : block) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        block = crlf;
    }
    s.replace(a, b - a, block);
    return true;
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
        // geo-11 clamps convergence to these limits (the soft and hard ones, and the absolute overrides
        // that apply on top). The scale range needs about 0.1 to 20 in magnitude, so lift them all, and
        // allow magnitudes down to 0.01 (the largest world scale in the each-eye view is about -0.11).
        SetLine(dm, "dm_max_convergence_soft", "20.0");
        SetLine(dm, "dm_max_convergence_hard", "20.0");
        SetLine(dm, "dm_max_convergence_soft_absolute_override", "20.0");
        SetLine(dm, "dm_max_convergence_hard_absolute_override", "20.0");
        SetLine(dm, "dm_min_convergence", "0.01");
        SetLine(dm, "dm_min_convergence_absolute_override", "0.01");
        if (ok && dm != before) ok = ReplaceFile(Path(L"d3dxdm.ini"), dm);
    }
    // The world-scale hotkeys: geo-11 reads d3dx.ini once, at its own start, so the steps are written here.
    std::string dx;
    bool keysOk = false;
    if (ReadAll(Path(L"d3dx.ini"), dx)) {
        const std::string before = dx;
        keysOk = UpdateKeyBlock(dx);
        if (keysOk && dx != before) keysOk = ReplaceFile(Path(L"d3dx.ini"), dx);
    }
    Log("WORLDSCALE: starting at %.2f (geo-11 separation %s, convergence %s, each-eye view %s): d3dxdm.ini %s, d3dx.ini steps %s",
        g_scale, sepStr.c_str(), Conv(g_scale).c_str(), ev.on ? "on" : "off", ok ? "ok" : "NOT updated",
        keysOk ? "ok" : "NOT updated (the world-scale block was not found)");
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
    const int step = StepIndex(scale);
    if (g_heldVk) return;  // one change at a time (takes a few frames)
    scale = kScales[step];
    wchar_t v[16];
    swprintf(v, 16, L"%.2f", scale);
    g_heldVk = VK_F13 + step;
    g_frames = 4;  // geo-11 checks keys once per frame; hold long enough to be seen
    Key(g_heldVk, true);
    g_scale = scale;
    WritePrivateProfileStringW(L"ds3vr", L"WorldScale", v, Path(L"ds3vr.ini").c_str());
    Log("menu: world scale %.2f (geo-11 convergence %s, saved)", scale, Conv(scale).c_str());
}

void WorldScaleStep(int delta) {
    const int cur = StepIndex(g_scale);
    int next = cur + delta;
    if (next < 0) next = 0;
    if (next >= kSteps) next = kSteps - 1;
    if (next != cur) WorldScaleSet(kScales[next]);
}

void WorldScaleTick() {
    if (g_heldVk && --g_frames <= 0) {
        Key(g_heldVk, false);
        g_heldVk = 0;
    }
}

} // namespace ds3vr
