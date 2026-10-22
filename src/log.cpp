#include "log.h"

#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <cstring>

namespace ds3vr {
namespace {

HMODULE g_self = nullptr;
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_tried = false;
bool g_disabled = false;
std::wstring g_dir;
int g_verbose = -1;  // -1 = not read yet; 0 = problems only; 1 = everything (developer keys on)
// The first minutes of every launch are always logged in full: launch problems are the hardest to
// reproduce, and that part of the log stays small.
const ULONGLONG g_startMs = GetTickCount64();
constexpr ULONGLONG kStartupLogMs = 180000;

bool IsProblem(const char* m) {
    static const char* words[] = {"could not", "Could not", "failed", "FAILED", "error", "Error", "ERROR", "cannot", "not hooking",
                                  "crash", "CRASH", "exception", "unexpected"};
    for (const char* w : words)
        if (strstr(m, w)) return true;
    return false;
}

void EnsureDir() {
    if (!g_dir.empty()) return;
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(g_self, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    g_dir = (slash == std::wstring::npos) ? L".\\" : p.substr(0, slash + 1);
}

void EnsureOpen() {
    if (g_file || g_tried) return;
    g_tried = true;
    EnsureDir();
    std::wstring path = g_dir + L"ds3vr.log";
    // Keep the previous launch's log, so a bad launch can still be looked at after relaunching.
    MoveFileExW(path.c_str(), (g_dir + L"ds3vr_prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    // _SH_DENYWR: other processes (Notepad, tail) may read while we write.
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

} // namespace

void LogInit(HMODULE self) { g_self = self; }

void LogDisable() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tried = true;
    g_file = nullptr;
    g_verbose = 0;
    g_disabled = true;
}

void LogSetVerbose(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_verbose = on ? 1 : 0;
}

bool LogVerbose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_verbose < 0) {
        EnsureDir();
        g_verbose = GetPrivateProfileIntW(L"ds3vr", L"DevKeys", 0, (g_dir + L"ds3vr.ini").c_str()) ? 1 : 0;
    }
    return g_verbose == 1;
}

const wchar_t* ModuleDir() {
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureDir();
    return g_dir.c_str();
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_disabled) return;
    if (g_verbose < 0) {
        EnsureDir();
        g_verbose = GetPrivateProfileIntW(L"ds3vr", L"DevKeys", 0, (g_dir + L"ds3vr.ini").c_str()) ? 1 : 0;
    }
    const bool first = !g_tried;
    const bool startup = GetTickCount64() - g_startMs < kStartupLogMs;
    if (!g_verbose && !first && !startup && !IsProblem(msg)) return;
    EnsureOpen();
    if (!g_file) return;
    if (first && !g_verbose)
        fprintf(g_file, "Detailed logging is off (turn on Developer keys in the VR menu): everything is logged for the first "
                        "3 minutes of a launch, then only problems.\n");
    fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, GetCurrentThreadId(), msg);
    fflush(g_file);
}

} // namespace ds3vr
