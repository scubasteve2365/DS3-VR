#include "d3d11exports.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace ds3vr {
namespace {

// Windows d3d11.dll functions that VR runtimes import and geo-11's d3d11.dll does not export.
const char* const kNames[] = {
    "CreateDirect3D11DeviceFromDXGIDevice",
    "CreateDirect3D11SurfaceFromDXGISurface",
    "D3D11On12CreateDevice",  // Virtual Desktop's VirtualDesktop.LibOVRRT64_1.dll
};

// Export table RVAs are 32-bit offsets from the module base, so the new table has to live above it,
// within 4 GB.
uint8_t* AllocAbove(uint8_t* base, size_t imageSize, size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    for (uintptr_t a = (b + imageSize + gran - 1) & ~(gran - 1); a + bytes < b + 0xF0000000ull; a += gran) {
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

struct NameEntry {
    const char* name;
    DWORD nameRva;
    WORD ordIndex;
};

// Gives `mod` a new export table: its own entries unchanged, plus each missing function as a small
// jump to Windows' function in `sys`. Returns how many were added (0 if none were missing).
int Patch(HMODULE mod, HMODULE sys) {
    uint8_t* base = reinterpret_cast<uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress) return 0;
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dd->VirtualAddress);

    std::vector<std::pair<const char*, FARPROC>> add;
    for (const char* n : kNames) {
        if (GetProcAddress(mod, n)) continue;
        if (FARPROC p = GetProcAddress(sys, n)) add.emplace_back(n, p);
    }
    if (add.empty()) return 0;

    const DWORD F = ed->NumberOfFunctions, N = ed->NumberOfNames, A = static_cast<DWORD>(add.size());
    const DWORD* oldFns = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
    const DWORD* oldNames = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
    const WORD* oldOrds = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);

    // Layout: directory, functions, names, name ordinals, name strings | stubs (outside the directory
    // range, or Windows would read them as forwarder strings).
    size_t off = sizeof(IMAGE_EXPORT_DIRECTORY);
    const size_t offFns = off;
    off += 4ull * (F + A);
    const size_t offNames = off;
    off += 4ull * (N + A);
    const size_t offOrds = off;
    off += 2ull * (N + A);
    const size_t offStr = off;
    for (auto& a : add) off += strlen(a.first) + 1;
    const size_t dataSize = off;
    const size_t offStubs = (off + 15) & ~size_t(15);
    const size_t total = offStubs + 16ull * A;

    uint8_t* blk = AllocAbove(base, nt->OptionalHeader.SizeOfImage, total);
    if (!blk) {
        Log("D3D11: could not reserve memory near %p for the export table", base);
        return 0;
    }
    auto rva = [&](const uint8_t* p) { return static_cast<DWORD>(p - base); };

    auto* nd = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(blk);
    *nd = *ed;
    nd->NumberOfFunctions = F + A;
    nd->NumberOfNames = N + A;
    nd->AddressOfFunctions = rva(blk + offFns);
    nd->AddressOfNames = rva(blk + offNames);
    nd->AddressOfNameOrdinals = rva(blk + offOrds);

    DWORD* fns = reinterpret_cast<DWORD*>(blk + offFns);
    memcpy(fns, oldFns, 4ull * F);
    std::vector<NameEntry> names;
    names.reserve(N + A);
    for (DWORD i = 0; i < N; ++i)
        names.push_back({reinterpret_cast<const char*>(base + oldNames[i]), oldNames[i], oldOrds[i]});
    uint8_t* str = blk + offStr;
    for (DWORD i = 0; i < A; ++i) {
        // Stub: jmp [rip+0]; dq target
        uint8_t* stub = blk + offStubs + 16ull * i;
        stub[0] = 0xFF;
        stub[1] = 0x25;
        memset(stub + 2, 0, 4);
        const uint64_t target = reinterpret_cast<uint64_t>(add[i].second);
        memcpy(stub + 6, &target, 8);
        fns[F + i] = rva(stub);
        const size_t len = strlen(add[i].first) + 1;
        memcpy(str, add[i].first, len);
        names.push_back({reinterpret_cast<const char*>(str), rva(str), static_cast<WORD>(F + i)});
        str += len;
    }
    // Windows looks names up with a binary search, so they must stay sorted.
    std::sort(names.begin(), names.end(), [](const NameEntry& a, const NameEntry& b) { return strcmp(a.name, b.name) < 0; });
    DWORD* nameRvas = reinterpret_cast<DWORD*>(blk + offNames);
    WORD* ords = reinterpret_cast<WORD*>(blk + offOrds);
    for (size_t i = 0; i < names.size(); ++i) {
        nameRvas[i] = names[i].nameRva;
        ords[i] = names[i].ordIndex;
    }
    DWORD old = 0;
    VirtualProtect(blk, total, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), blk, total);

    // Point the module's header at the new table.
    if (!VirtualProtect(dd, sizeof(*dd), PAGE_READWRITE, &old)) {
        Log("D3D11: could not unprotect the export directory of %p", base);
        return 0;
    }
    // Size first: until the address changes, the old table is read with a smaller (harmless) range.
    dd->Size = static_cast<DWORD>(dataSize);
    dd->VirtualAddress = rva(blk);
    VirtualProtect(dd, sizeof(*dd), old, &old);
    return static_cast<int>(A);
}

} // namespace

void D3D11AddMissingExports() {
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t sysPath[MAX_PATH] = {};
    GetSystemDirectoryW(sysPath, MAX_PATH);
    wcscat_s(sysPath, L"\\d3d11.dll");
    HMODULE sys = GetModuleHandleW(sysPath);
    if (!sys) sys = LoadLibraryW(sysPath);
    if (!sys) {
        Log("D3D11: Windows' d3d11.dll could not be loaded");
        return;
    }
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < n; ++i) {
        if (mods[i] == sys) continue;
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH) || _wcsicmp(name, L"d3d11.dll") != 0) continue;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        const int added = Patch(mods[i], sys);
        if (added > 0) {
            const bool ok = GetProcAddress(mods[i], "D3D11On12CreateDevice") != nullptr;
            Log("D3D11: %ls was missing %d Windows d3d11 function(s) VR runtimes need (e.g. D3D11On12CreateDevice for Virtual "
                "Desktop); added, pointing at Windows' own (%s)", path, added, ok ? "ok" : "lookup FAILED");
        }
    }
}

} // namespace ds3vr
