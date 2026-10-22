#include "fps.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "log.h"

namespace ds3vr {
namespace {

constexpr uintptr_t kLimiterPtrRva = 0x489dd10;
constexpr uintptr_t kMoveJeRva = 0x106dfa5;
const uint8_t kMoveSig[] = {0x74, 0x0A, 0xF3, 0x0F, 0x10, 0x89, 0xD8, 0x00, 0x00, 0x00, 0xEB, 0x03, 0x0F, 0x28, 0xCE};

int g_setting = 0;       // FrameRate
bool g_fixMove = true;
bool g_read = false;
bool g_active = false;   // limiter set above 60
float g_applied = 0.f;

void ReadSettings() {
    if (g_read) return;
    g_read = true;
    std::wstring ini = std::wstring(ModuleDir()) + L"ds3vr.ini";
    g_setting = static_cast<int>(GetPrivateProfileIntW(L"ds3vr", L"FrameRate", 0, ini.c_str()));
    g_fixMove = GetPrivateProfileIntW(L"ds3vr", L"FixHighFpsMovement", 1, ini.c_str()) != 0;
}

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<const uint8_t*>(p) + n <= reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

} // namespace

void FpsEarly() {
    ReadSettings();
    if (g_setting < 0 || !g_fixMove) return;
    uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    for (int i = 0; i < 300; ++i) {
        if (memcmp(exe + kMoveJeRva, kMoveSig, sizeof(kMoveSig)) == 0) {
            DWORD old = 0, tmp = 0;
            if (VirtualProtect(exe + kMoveJeRva, 1, PAGE_EXECUTE_READWRITE, &old)) {
                exe[kMoveJeRva] = 0xEB;
                VirtualProtect(exe + kMoveJeRva, 1, old, &tmp);
                FlushInstructionCache(GetCurrentProcess(), exe + kMoveJeRva, 1);
                Log("FPS: high frame rate movement fix applied at exe+%llx", static_cast<unsigned long long>(kMoveJeRva));
            }
            return;
        }
        if (exe[kMoveJeRva] == 0xEB) return;
        Sleep(100);
    }
    Log("FPS: movement fix code not found (different game build?)");
}

void FpsTick(float headsetHz) {
    ReadSettings();
    if (g_setting < 0) return;
    float target = g_setting > 0 ? static_cast<float>(g_setting) : static_cast<float>(static_cast<int>(headsetHz + 0.5f));
    if (target < 30.f || target > 360.f) return;
    uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    uintptr_t obj = 0;
    if (!Readable(exe + kLimiterPtrRva, 8)) return;
    memcpy(&obj, exe + kLimiterPtrRva, 8);
    if (!obj || !Readable(reinterpret_cast<void*>(obj), 0x360)) return;
    uint8_t* o = reinterpret_cast<uint8_t*>(obj);
    float frameTime = 0.f, cur = 0.f;
    memcpy(&frameTime, o + 0x18, 4);
    memcpy(&cur, o + 0x354, 4);
    if (!(frameTime > 0.001f && frameTime < 0.2f)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("FPS: the frame limiter does not look as expected (frame time %g); leaving the 60 cap alone", frameTime);
        }
        return;
    }
    if (o[0x358] != 1 || cur != target) {
        memcpy(o + 0x354, &target, 4);
        o[0x358] = 1;
        if (target != g_applied) {
            Log("FPS: frame limiter %p set to %.1f fps (was frame time %.5f s)", static_cast<void*>(o), target, frameTime);
            g_applied = target;
        }
    }
    g_active = true;
}

bool FpsNoVsync() { return g_active; }

} // namespace ds3vr
