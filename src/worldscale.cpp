#include "worldscale.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "firstperson.h"
#include "log.h"

namespace ds3vr {
namespace {

// geo-11 convergence for world scale 1 (life size) with dm_separation 20, our measured stereo shift
// and the headset-shaped view; scale s uses s times this.
constexpr float kBaseConvergence = 1.4834f;
constexpr int kSepBase = 20;                 // geo-11 separation the VR alignment (StereoShift) is measured for

// World scale steps the menu can reach: 0.25, then 0.5 to 10.0 in steps of 0.5. Below 1.0 the world
// looks bigger (0.25 is the largest), above 1.0 it looks smaller (10.0 is the smallest).
constexpr float kScales[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f,
                             5.5f,  6.0f, 6.5f, 7.0f, 7.5f, 8.0f, 8.5f, 9.0f, 9.5f, 10.0f};
constexpr int kSteps = static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));
constexpr float kLifeSize = 1.0f;

// One hotkey per step, none of which a keyboard sends: F13 to F24, then the unassigned virtual-key
// codes 0x97 to 0x9F (geo-11 takes hex codes in d3dx.ini).
constexpr int kFKeys = 12;
static_assert(kSteps <= kFKeys + 9, "not enough spare keys for the world scale steps");
int StepVk(int i) { return i < kFKeys ? VK_F13 + i : 0x97 + (i - kFKeys); }
std::string StepKeyName(int i) {
    char b[16];
    if (i < kFKeys) snprintf(b, sizeof(b), "VK_F%d", 13 + i);
    else snprintf(b, sizeof(b), "0x%02X", StepVk(i));
    return b;
}

float g_scale = kLifeSize;
int g_heldVk = 0;          // key currently held down for geo-11
int g_frames = 0;          // frames left before releasing it
float g_pending = 0.f;     // a change asked for while a key was held, applied once it is released
bool g_pendingSave = false;
bool g_firstPerson = false;  // first person: the scale is 1.0 and changes are not saved over the third-person one

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
    snprintf(b, sizeof(b), "%.3f", kBaseConvergence * s);
    return b;
}

// The hotkey sections geo-11 switches between, one per step, with this launch's convergence values.
std::string KeyBlock() {
    std::string out =
        "; ds3vr: world scale 0.25 (largest world) to 10.0 (smallest). The mod menu presses one of F13 to F24 or\n"
        "; 0x97 to 0x9F (keys real keyboards do not have) to pick a step; convergence = 1.4834 x scale for\n"
        "; dm_separation 20. DS3 VR writes this block at each launch.\n";
    for (int i = 0; i < kSteps; ++i) {
        char b[256];
        snprintf(b, sizeof(b),
                 "[KeyDs3vrWorldScale%d]\nKey = no_modifiers %s\ntype = activate\nconvergence = %.3f\ntransition = 300\n"
                 "transition_type = cosine\n\n",
                 i + 1, StepKeyName(i).c_str(), kBaseConvergence * kScales[i]);
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

// The third-person scale saved in ds3vr.ini (WorldScale).
float SavedScale() {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"ds3vr", L"WorldScale", L"1", v, 32, Path(L"ds3vr.ini").c_str());
    return Snap(static_cast<float>(_wtof(v)));
}

// Switches geo-11 to a step (it takes a few frames). save = keep it as the third-person scale.
void Apply(float scale, bool save) {
    const int step = StepIndex(scale);
    scale = kScales[step];
    if (g_heldVk) {  // one key at a time: do this one when the current key is released
        g_pending = scale;
        g_pendingSave = save;
        return;
    }
    g_heldVk = StepVk(step);
    g_frames = 4;  // geo-11 checks keys once per frame; hold long enough to be seen
    Key(g_heldVk, true);
    g_scale = scale;
    if (save) {
        wchar_t v[16];
        swprintf(v, 16, L"%.2f", scale);
        WritePrivateProfileStringW(L"ds3vr", L"WorldScale", v, Path(L"ds3vr.ini").c_str());
    }
    Log("world scale %.2f (geo-11 convergence %s%s)", scale, Conv(scale).c_str(),
        save ? ", saved" : g_firstPerson ? ", first person: not saved" : "");
}

} // namespace

void WorldScaleEarly() {
    // A run that starts in first person starts at life size; the saved third-person scale is kept.
    g_firstPerson = FirstPersonAvailable() && FirstPersonWanted();
    g_scale = g_firstPerson ? kLifeSize : SavedScale();
    const std::string sepStr = std::to_string(kSepBase);
    std::string dm;
    bool ok = false;
    if (ReadAll(Path(L"d3dxdm.ini"), dm)) {
        const std::string before = dm;
        ok = SetLine(dm, "dm_convergence", Conv(g_scale));
        // The VR image alignment is calculated for geo-11 separation 20 with automatic convergence off.
        // Put both back every launch, so a change made by hand or with geo-11's hotkeys cannot leave
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
        // that apply on top). The scale range needs 0.37 (0.25) to 14.8 (10.0), so lift the maximums.
        // The minimums go back to geo-11's own values (0.3 and 0.05), which are below the smallest step.
        SetLine(dm, "dm_max_convergence_soft", "20.0");
        SetLine(dm, "dm_max_convergence_hard", "20.0");
        SetLine(dm, "dm_max_convergence_soft_absolute_override", "20.0");
        SetLine(dm, "dm_max_convergence_hard_absolute_override", "20.0");
        SetLine(dm, "dm_min_convergence", "0.3");
        SetLine(dm, "dm_min_convergence_absolute_override", "0.05");
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
    Log("WORLDSCALE: starting at %.2f%s (geo-11 separation %s, convergence %s): d3dxdm.ini %s, d3dx.ini steps %s",
        g_scale, g_firstPerson ? " (first person; third-person scale kept)" : "", sepStr.c_str(), Conv(g_scale).c_str(),
        ok ? "ok" : "NOT updated", keysOk ? "ok" : "NOT updated (the world-scale block was not found)");
}

float WorldScaleGet() { return g_scale; }

bool WorldScaleBusy() { return g_heldVk != 0; }

void WorldScaleSet(float scale) { Apply(scale, !g_firstPerson); }

void WorldScaleFirstPerson(bool on) {
    if (on == g_firstPerson) return;
    g_firstPerson = on;
    Apply(on ? kLifeSize : SavedScale(), false);
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
        if (g_pending > 0.f) {
            const float s = g_pending;
            g_pending = 0.f;
            Apply(s, g_pendingSave);
        }
    }
}

} // namespace ds3vr
