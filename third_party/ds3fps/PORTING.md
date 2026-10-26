# First Person Souls in DS3 VR

Source: https://github.com/Dasaav-dsv/ds3fps (MIT, by Dasaav), commit `5aeef0f` (v1.1.2.1).
Copied: `src/`, `include/mem`, `include/MCF`, `include/mINI`, `dinput8/targetver.h`, `LICENSE`, `README.md`.
Not copied: their `dinput8/` entry point (`dllmain.cpp`, `Hook.cpp`, `MinimalDInput8Hook.cpp`). DS3 VR's own
dll is the dinput8 proxy, and `src/firstperson.cpp` does what their `InitializeHooking()` did.

## asmjit

Their repository has no `include/asmjit/x86/` folder: the Visual Studio `.gitignore` ignores folders named `x86`,
so it was never committed. `include/asmjit` here is upstream asmjit (https://github.com/asmjit/asmjit, Zlib) at
commit `5b5b0b38775938df4d3779604ff1db60b9a9dcbf` (2022-12-10). All 93 asmjit files in their repository match
that commit byte for byte (also the two before it); this is the newest of the three and includes an x86
assembler fix. Only `core/`, `x86/` and the top-level headers are copied (no AArch64).

## Changes to their code (all marked `ds3vr`)

Build with mingw-w64 (gcc), not MSVC:
- `dinput8/targetver.h`, `src/Defs.h`, `include/MCF/windows_include.h`, `include/mem/{protect,prot_flags,module,execution_handler}.h`:
  header names in lower case (`sdkddkver.h`, `psapi.h`, `windows.h`), since Linux file names are case-sensitive.
- `src/Defs.h`, `src/Defs.cpp`: `alignas(16)` moved to the front of the declaration (gcc does not accept it after `volatile extern`).
- `src/stdafx.h`: includes `<cstring>`, and defines `MAXINT32` (an MSVC `basetsd.h` macro) if it is missing.
- `src/asm.h`: function addresses cast with `reinterpret_cast<const void*>` (MSVC converts them implicitly).
- Built with `UNICODE`, `_WIN32_WINNT=0x0A00`, `NTDDI_VERSION=0x0A000006` and `-msse4.1`, as their project was (see `build.sh`).

Behaviour, for DS3 VR:
- `src/AOB.cpp`: a pattern that is not found is counted in `g_ds3vrAobMissing` instead of opening a console window,
  and `AOBScanBase` returns null instead of reading through it. `src/firstperson.cpp` checks the count after the
  scans and patches nothing if any pattern is missing (their code would otherwise write through a bad address).
- `src/camera/PerspectiveCtrl.cpp`: `PerspectiveSwitch()` (called on the game's thread each frame) applies the VR
  menu's request (`g_ds3vrViewRequest`: 1 = first person, 2 = third person). Going to first person drops lock-on
  first, as their hold switch did. Their hold-lock-on switch is off (`g_ds3vrHoldSwitch = false`): in VR it would
  switch views by accident whenever lock-on is held with no target.
- `src/camera/InitializeCamera.cpp`: each load starts in the VR menu's choice (`g_ds3vrStartFirstPerson`), not the
  view stored in the save (event flag 1099).
- `src/ds3vr_hooks.h` (new): declares the four `g_ds3vr*` values above.

Settings their `FPS.ini` held are set by DS3 VR in `src/firstperson.cpp` and no `FPS.ini` is read or written:
head tracking for dodges and heavy hits off (VR comfort), debug console off, VirtualAlloc2 on. Their in-game
Camera tab (FOV, Track Dodges) and the merged Sensitivity setting are unchanged.
