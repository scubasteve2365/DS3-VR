#include "camhook.h"
#include "worldscale.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "log.h"

// Camera data flow in DS3 1.15.2 (from disassembly of the in-memory image):
//   mgr = [[exe+0x4796298]+0x10]   camera manager, updated each frame by exe+0xe006a0
//   exe+0xe006a0:
//     - copies [mgr+0x28] (camera params block) to [mgr+0x30]      (previous frame)
//     - writes the active game camera's world matrix into [mgr+0x38]+0x10 (rows right, up,
//       forward, position; +0x50 vertical FOV in radians)
//     - calls [mgr+0x38]->vtable[3](cam38, [mgr+0x28])  -> exports matrix + FOV to [mgr+0x28]
//     - pushes the camera on to the renderer and other systems, all from [mgr+0x38]
//     - copies it to [mgr+0x568]+0xF0 and calls [mgr+0x568]->vtable[21]
// We give the [mgr+0x38] object a private copy of its vtable with slot 3 pointing at our
// function, so we can change the camera right before it is exported. No game code is patched.

namespace ds3vr {
namespace {


constexpr uintptr_t kCamBaseRva = 0x4796298;

uintptr_t g_exeBase = 0, g_exeEnd = 0;
uintptr_t g_rdataLo = 0, g_rdataHi = 0;

bool ReadPtr(uintptr_t a, uintptr_t* out) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), out, 8, &got) && got == 8;
}
bool InCode(uintptr_t v) { return v >= g_exeBase && v < g_exeEnd; }

// A private copy of one object's vtable, with one slot replaced. Slot -1 (RTTI) is kept.
struct VtSwap {
    void* table[1 + 64] = {};
    void** orig = nullptr;
    uintptr_t obj = 0;
    int slot = 0;
    void* origFn = nullptr;

    void** NewVt() { return table + 1; }

    // Returns true if (re)installed or already installed on obj.
    bool Ensure(uintptr_t o, int s, void* detour, const char* name) {
        uintptr_t vptr = 0;
        if (!o || !ReadPtr(o, &vptr)) return false;
        if (o == obj && vptr == reinterpret_cast<uintptr_t>(NewVt())) return true;
        if (vptr < g_rdataLo || vptr >= g_rdataHi) {
            static uintptr_t warned = 0;
            if (warned != vptr) Log("CAMHOOK: %s %016llx has unexpected vtable %016llx, not hooking", name,
                                    static_cast<unsigned long long>(o), static_cast<unsigned long long>(vptr));
            warned = vptr;
            return false;
        }
        void** ov = reinterpret_cast<void**>(vptr);
        int n = 0;
        while (n < 64 && InCode(reinterpret_cast<uintptr_t>(ov[n]))) ++n;
        if (n <= s) {
            Log("CAMHOOK: %s vtable has only %d entries, not hooking", name, n);
            return false;
        }
        table[0] = ov[-1];
        for (int i = 0; i < 64; ++i) table[1 + i] = i < n ? ov[i] : nullptr;
        orig = ov;
        slot = s;
        origFn = ov[s];
        table[1 + s] = detour;
        *reinterpret_cast<void***>(o) = NewVt();
        obj = o;
        Log("CAMHOOK: hooked %s %016llx (vtable exe+%llx, %d entries, slot %d = exe+%llx)", name, static_cast<unsigned long long>(o),
            static_cast<unsigned long long>(vptr - g_exeBase), n, s,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(origFn) - g_exeBase));
        return true;
    }
};

VtSwap g_cam38;

// ---------- head tracking ----------
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
std::mutex g_headMutex;
bool g_haveHead = false;
Quat g_headQ{0, 0, 0, 1};
Vec3 g_headP{0, 0, 0};
bool g_haveRef = false;
float g_refYaw = 0.f;
Vec3 g_refP{0, 0, 0};
std::atomic<bool> g_recenterReq{true};
std::atomic<bool> g_headTrack{true};
std::atomic<bool> g_scaleHead{true};  // ds3vr.ini ScaleHeadMovement
std::atomic<int> g_horizonLock{0};  // ds3vr.ini HorizonLock (VR menu "Decoupled pitch"): 0 off, 1 level view, 2 level view + stick locked
float g_pullback = 0.f;  // ds3vr.ini CameraPullback: extra metres behind the game's camera
float g_fovY = 0.f;  // ds3vr.ini FovY (degrees -> radians); 0 = automatic (from the headset)
std::atomic<float> g_autoFovY{0.f};  // from the headset field of view, used when FovY=0
std::atomic<float> g_aspect{0.f};    // render aspect (width / height of the view, not of the image); 0 = the game's
std::atomic<unsigned long long> g_lastUpdateMs{0};

// Poses applied to the game camera, newest last, so the compositor can be told which head pose
// a given rendered frame was made with.
struct Applied { Quat q; Vec3 p; float fovY; float aspect; float marker[16]; bool markerOk; uint64_t seq; };
constexpr int kRing = 16;
Applied g_applied[kRing];
std::atomic<uint64_t> g_appliedSeq{0};
Applied* g_markerPending = nullptr;  // set while the export runs

void QuatToMat(const Quat& q, float m[3][3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

float YawOf(const Quat& q) {
    float m[3][3];
    QuatToMat(q, m);
    // Angle of the head's +Z (backward) axis about +Y. Rotating by -this angle brings the backward
    // axis to +Z, i.e. the forward axis to -Z: looking straight ahead at the moment of recentering.
    return std::atan2(m[0][2], m[2][2]);
}

// Horizon lock: keeps the game camera's heading but drops the up/down tilt the right stick gives it,
// so the view stays level and only your head pitches it. The camera keeps the position the game gave
// it (the stick still raises and lowers it around the character). World up is +Y; the camera's right
// row is horizontal for DS3's yaw-and-pitch camera, so it defines the level heading.
void LevelCamera(float* cam) {
    float rx = cam[0], rz = cam[2];
    const float len = std::sqrt(rx * rx + rz * rz);
    if (len < 1e-4f) return;  // no usable heading: leave the camera as the game made it
    rx /= len;
    rz /= len;
    // Keep the frame's own handedness and up direction: forward is +/-(right x up), with the sign the
    // game's rows already have.
    const float* r = cam;
    const float* u = cam + 4;
    const float* f = cam + 8;
    const float cx = r[1] * u[2] - r[2] * u[1], cy = r[2] * u[0] - r[0] * u[2], cz = r[0] * u[1] - r[1] * u[0];
    const float hand = (cx * f[0] + cy * f[1] + cz * f[2]) >= 0.f ? 1.f : -1.f;
    const float up = u[1] >= 0.f ? 1.f : -1.f;
    // right = (rx, 0, rz), up = (0, up, 0), forward = hand * (right x up) = hand * up * (-rz, 0, rx)
    cam[0] = rx;                cam[1] = 0.f; cam[2] = rz;
    cam[4] = 0.f;               cam[5] = up;  cam[6] = 0.f;
    cam[8] = -hand * up * rz;   cam[9] = 0.f; cam[10] = hand * up * rx;
}

// Rotates the camera rows (right, up, forward, then position) by the head pose relative to the
// recenter reference. OpenXR is x right, y up, z back; the game camera is x right, y up, z forward.
void ApplyHead(float* cam, const Quat& q, const Vec3& p) {
    float r[3][3];
    QuatToMat(q, r);
    // relative to the reference yaw: Ry(-refYaw) * R
    const float c = std::cos(-g_refYaw), s = std::sin(-g_refYaw);
    float ry[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    float rr[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rr[i][j] = ry[i][0] * r[0][j] + ry[i][1] * r[1][j] + ry[i][2] * r[2][j];
    // to game local space: G = S rr S with S = diag(1,1,-1)
    float g[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) g[i][j] = rr[i][j] * ((i == 2) ? -1.f : 1.f) * ((j == 2) ? -1.f : 1.f);
    float rows[3][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) rows[i][k] = cam[i * 4 + k];
    // new axis i (world) = sum_k g[k][i] * old row k
    for (int i = 0; i < 3; ++i)
        for (int a = 0; a < 3; ++a) cam[i * 4 + a] = g[0][i] * rows[0][a] + g[1][i] * rows[1][a] + g[2][i] * rows[2][a];
    // head translation relative to the reference, in the reference-yaw frame, then game local
    // geo-11's world scale widens the stereo baseline (the eyes act WorldScale times further apart), so
    // head movement has to grow by the same factor or nearby things swing along when the head turns.
    const float k = g_scaleHead.load(std::memory_order_relaxed) ? WorldScaleGet() : 1.f;
    const float dx = k * (p.x - g_refP.x), dy = k * (p.y - g_refP.y), dz = k * (p.z - g_refP.z);
    const float lx = c * dx + s * dz, ly = dy, lz = -(-s * dx + c * dz);
    for (int a = 0; a < 3; ++a) cam[12 + a] += lx * rows[0][a] + ly * rows[1][a] + lz * rows[2][a];
}
using ExportFn = void (*)(void*, void*);
std::atomic<uint64_t> g_calls{0};

void ExportDetour(void* self, void* out) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastUpdateMs.store(GetTickCount64(), std::memory_order_relaxed);
    float* cam = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x10);
    if (g_headTrack.load(std::memory_order_relaxed)) {
        Quat q;
        Vec3 p;
        bool have;
        {
            std::lock_guard<std::mutex> lock(g_headMutex);
            have = g_haveHead;
            q = g_headQ;
            p = g_headP;
            if (have && (g_recenterReq.exchange(false) || !g_haveRef)) {
                g_refYaw = YawOf(q);
                g_refP = p;
                g_haveRef = true;
                Log("CAMHOOK: recentered (reference yaw %.1f deg)", g_refYaw * 57.29578f);
            }
        }
        if (have) {
            const float fy = g_fovY > 0.f ? g_fovY : g_autoFovY.load(std::memory_order_relaxed);
            // The game's own camera (before head rotation and VR field of view): DS3 places the
            // lock-on marker on screen with it, so the HUD shader needs it to move the marker.
            float pre[12];
            memcpy(pre, cam, sizeof(pre));
            const float fovGame = cam[16];
            if (fy > 0.f) cam[16] = fy;  // +0x50: vertical field of view used for projection and culling
            if (g_pullback != 0.f)
                for (int a = 0; a < 3; ++a) cam[12 + a] -= g_pullback * cam[8 + a];  // back along the forward row
            if (g_horizonLock.load(std::memory_order_relaxed) != 0) LevelCamera(cam);
            ApplyHead(cam, q, p);
            const uint64_t seq = g_appliedSeq.load(std::memory_order_relaxed) + 1;
            Applied& a = g_applied[seq % kRing];
            a.q = q;
            a.p = p;
            a.fovY = cam[16];  // +0x50: vertical field of view (radians)
            a.aspect = g_aspect.load(std::memory_order_relaxed);
            // Rows 0-2: a direction in the game camera's frame -> the VR camera's frame (post row i . pre row j).
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j)
                    a.marker[i * 4 + j] = cam[i * 4 + 0] * pre[j * 4 + 0] + cam[i * 4 + 1] * pre[j * 4 + 1] + cam[i * 4 + 2] * pre[j * 4 + 2];
                a.marker[i * 4 + 3] = 0.f;
            }
            // Row 3: game tan(half fov) x/y (aspect filled in after the export), VR 1/tan x/y.
            const float ty0 = std::tan(fovGame * 0.5f), ty1 = std::tan(cam[16] * 0.5f);
            a.marker[12] = 0.f;
            a.marker[13] = ty0;
            a.marker[14] = (a.aspect > 0.3f && ty1 > 0.f) ? 1.f / (ty1 * a.aspect) : 0.f;
            a.marker[15] = ty1 > 0.f ? 1.f / ty1 : 0.f;
            a.markerOk = false;
            a.seq = seq;
            g_markerPending = &a;
            g_appliedSeq.store(seq, std::memory_order_release);
        }
    }
    reinterpret_cast<ExportFn>(g_cam38.origFn)(self, out);
    if (g_markerPending) {
        // The game's own aspect, as the export just wrote it (before we override it below).
        const float asp0 = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(out) + 0x50)[5];
        Applied* a = g_markerPending;
        g_markerPending = nullptr;
        if (asp0 > 0.3f && asp0 < 4.f && a->marker[13] > 0.f && a->marker[14] > 0.f) {
            a->marker[12] = a->marker[13] * asp0;
            a->markerOk = true;
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("CAMHOOK: lock-on marker: game camera tanY %.3f aspect %.3f -> VR 1/tanX %.3f 1/tanY %.3f", a->marker[13], asp0,
                    a->marker[14], a->marker[15]);
            }
        }
    }
    // The export writes (out+0x50 fovY, +0x54 near, +0x58 far, +0x5c width, +0x60 height, +0x64 aspect).
    // Override the aspect so the game's 16:9 image holds a narrower, headset-shaped view; the headset
    // un-stretches it because we tell OpenXR the real field of view.
    const float asp = g_aspect.load(std::memory_order_relaxed);
    if (asp > 0.3f && g_headTrack.load(std::memory_order_relaxed)) {
        float* o = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(out) + 0x50);
        const float fy = g_fovY > 0.f ? g_fovY : g_autoFovY.load(std::memory_order_relaxed);
        if (fy > 0.f) o[0] = fy;
        o[5] = asp;
    }
}

uintptr_t g_mgr = 0;

void Init() {
    {
        std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
        wchar_t v[32] = {};
        GetPrivateProfileStringW(L"ds3vr", L"FovY", L"0", v, 32, ini.c_str());
        const float deg = static_cast<float>(_wtof(v));
        g_fovY = (deg >= 30.f && deg <= 130.f) ? deg * 3.14159265f / 180.f : 0.f;
        Log("CAMHOOK: vertical field of view %s", g_fovY > 0.f ? "from ds3vr.ini" : "automatic (from the headset)");
        g_scaleHead.store(GetPrivateProfileIntW(L"ds3vr", L"ScaleHeadMovement", 1, ini.c_str()) != 0);
        {
            const int hl = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"HorizonLock", 0, ini.c_str()));
            g_horizonLock.store(hl < 0 ? 0 : hl > 2 ? 2 : hl);
        }
        GetPrivateProfileStringW(L"ds3vr", L"CameraPullback", L"0", v, 32, ini.c_str());
        g_pullback = static_cast<float>(_wtof(v));
        if (g_pullback < -5.f || g_pullback > 20.f) g_pullback = 0.f;
        Log("CAMHOOK: camera pull-back %.2f m", g_pullback);
    }
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
    g_exeBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    g_exeEnd = g_exeBase + mi.SizeOfImage;
    g_rdataLo = g_exeBase + 0x2728000;
    g_rdataHi = g_exeBase + 0x2728000 + 0x1e0fa00;
}

} // namespace

void CamHookTick() {
    if (!g_exeBase) Init();
    uintptr_t a0 = 0, mgr = 0, cam38 = 0;
    if (!ReadPtr(g_exeBase + kCamBaseRva, &a0) || !a0 || !ReadPtr(a0 + 0x10, &mgr) || !mgr || !ReadPtr(mgr + 0x38, &cam38) || !cam38)
        return;
    g_mgr = mgr;
    g_cam38.Ensure(cam38, 3, reinterpret_cast<void*>(&ExportDetour), "main camera [mgr+0x38]");
    static uint64_t last = 0;
    static ULONGLONG lastT = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastT > 10000) {
        const uint64_t c = g_calls.load();
        if (lastT) Log("CAMHOOK: %llu export calls in the last %.0f s", static_cast<unsigned long long>(c - last), (now - lastT) / 1000.0);
        {
            // Render-size globals: exe+0x46601e8/ec are set by exe+0xedaa60 (SetRenderSize(w, h)), which also
            // derives the values at exe+0x489d5d0..e4.
            uint32_t rs[2] = {}, ui[6] = {};
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(g_exeBase + 0x46601e8), rs, sizeof(rs), &got);
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(g_exeBase + 0x489d5d0), ui, sizeof(ui), &got);
            float uf[6];
            memcpy(uf, ui, sizeof(uf));
            Log("CAMHOOK: render size globals %u x %u; derived %.4g %.4g %u %u %.4g %.4g", rs[0], rs[1], uf[0], uf[1], ui[2], ui[3], uf[4], uf[5]);
        }
        // Diagnostics for the render-shape work: the camera block after the matrix (FOV at +0x50; the
        // aspect ratio and near/far planes are expected nearby), and the exported params block.
        for (uintptr_t base : {cam38 + 0x50, mgr ? mgr + 0x28 : 0}) {
            if (!base) continue;
            uintptr_t blk = base;
            if (base == mgr + 0x28 && !ReadPtr(mgr + 0x28, &blk)) continue;
            float f[16] = {};
            SIZE_T got = 0;
            if (blk && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(blk), f, sizeof(f), &got) && got == sizeof(f))
                Log("CAMHOOK: %s %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g",
                    base == mgr + 0x28 ? "[mgr+0x28] block:" : "camera +0x50:", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
        last = c;
        lastT = now;
    }
}






void CamHookRecenter() { g_recenterReq.store(true); }

void CamHookSetHead(const float q[4], const float p[3]) {
    std::lock_guard<std::mutex> lock(g_headMutex);
    g_headQ = {q[0], q[1], q[2], q[3]};
    g_headP = {p[0], p[1], p[2]};
    g_haveHead = true;
}

void CamHookSetAutoFovY(float radians) {
    if (radians > 0.5f && radians < 2.3f) {
        g_autoFovY.store(radians);
        Log("CAMHOOK: automatic vertical field of view %.1f deg (from the headset)", radians * 57.29578f);
    }
}

void CamHookSetAspect(float aspect) {
    g_aspect.store(aspect);
    Log("CAMHOOK: render aspect %.3f (the view the 16:9 image is stretched over)", aspect);
}

bool CamHookAppliedAspect(int lag, float* aspect) {
    const uint64_t seq = g_appliedSeq.load(std::memory_order_acquire);
    if (seq == 0 || static_cast<uint64_t>(lag) >= seq || lag >= kRing - 2) return false;
    *aspect = g_applied[(seq - lag) % kRing].aspect;
    return true;
}

unsigned long long CamHookMsSinceUpdate() {
    const unsigned long long t = g_lastUpdateMs.load(std::memory_order_relaxed);
    return t ? GetTickCount64() - t : ~0ULL;
}

bool CamHookMarkerParams(int lag, float out[16]) {
    const uint64_t seq = g_appliedSeq.load(std::memory_order_acquire);
    if (seq == 0 || static_cast<uint64_t>(lag) >= seq || lag >= kRing - 2) return false;
    const Applied& a = g_applied[(seq - lag) % kRing];
    if (!a.markerOk) return false;
    memcpy(out, a.marker, sizeof(a.marker));
    return true;
}

int CamHookGetDecoupledPitch() { return g_horizonLock.load(); }

void CamHookSetDecoupledPitch(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_horizonLock.store(mode);
    wchar_t v[4];
    swprintf(v, 4, L"%d", mode);
    WritePrivateProfileStringW(L"ds3vr", L"HorizonLock", v, (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
    Log("menu: decoupled pitch %s", mode == 0 ? "off" : mode == 1 ? "level view" : "level view, stick up/down locked");
}

bool CamHookGetScaleHead() { return g_scaleHead.load(); }

void CamHookSetScaleHead(bool on) {
    g_scaleHead.store(on);
    WritePrivateProfileStringW(L"ds3vr", L"ScaleHeadMovement", on ? L"1" : L"0", (std::wstring(ModuleDir()) + L"ds3vr.ini").c_str());
    Log("menu: head movement %s", on ? "scaled by world scale" : "life size");
}

bool CamHookAppliedPose(int lag, float q[4], float p[3], float* fovY) {
    const uint64_t seq = g_appliedSeq.load(std::memory_order_acquire);
    if (seq == 0 || static_cast<uint64_t>(lag) >= seq || lag >= kRing - 2) return false;
    const Applied& a = g_applied[(seq - lag) % kRing];
    q[0] = a.q.x; q[1] = a.q.y; q[2] = a.q.z; q[3] = a.q.w;
    p[0] = a.p.x; p[1] = a.p.y; p[2] = a.p.z;
    *fovY = a.fovY;
    return true;
}

} // namespace ds3vr
