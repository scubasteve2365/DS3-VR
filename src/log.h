#pragma once

#include <windows.h>

namespace ds3vr {

// Remember our module handle. The log file (ds3vr.log, next to dinput8.dll) is
// opened lazily on the first Log() call, so this is safe to call from DllMain.
void LogInit(HMODULE self);

// printf-style, thread safe, flushed on every line.
void Log(const char* fmt, ...);

// Developer keys (ds3vr.ini DevKeys, set from the VR menu): on = full log plus F1 / F2; off = only
// lines that report a problem are written (plus one header line saying so).
void LogSetVerbose(bool on);
bool LogVerbose();

// In a helper process (rundll32): write nothing at all.
void LogDisable();

// Folder that contains dinput8.dll (the game's "Game" folder), with trailing backslash.
const wchar_t* ModuleDir();

} // namespace ds3vr
