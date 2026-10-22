#!/bin/sh
# Builds the DS3 VR mod with mingw-w64 (Linux or WSL: apt install g++-mingw-w64-x86-64).
#   build/dinput8.dll      the mod (goes in the game's Game folder)
#   build/ds3vr-patcher.exe  run by install-ds3vr.bat / uninstall-ds3vr.bat
#   build/DS3VR-<version>/  the release layout (extract into the game's Game folder)
set -e
cd "$(dirname "$0")"
mkdir -p build/obj
O=build/obj
M=third_party/minhook
I=third_party/imgui
Z=third_party/7zip/C
CC=x86_64-w64-mingw32-gcc
CXX=x86_64-w64-mingw32-g++

# dinput8.dll
for f in buffer hook trampoline; do $CC -O2 -c -I$M/include $M/src/$f.c -o $O/$f.o; done
$CC -O2 -c $M/src/hde/hde64.c -o $O/hde64.o
for f in imgui imgui_draw imgui_tables imgui_widgets; do $CXX -O2 -std=c++17 -c -I$I $I/$f.cpp -o $O/$f.o; done
$CXX -O2 -std=c++17 -c -I$I $I/backends/imgui_impl_dx11.cpp -o $O/imgui_impl_dx11.o
$CXX -O2 -Wall -std=c++17 -shared -static-libgcc -static-libstdc++ -mthreads -I$M/include -I$I -I$I/backends -Ithird_party/openxr/include -Isrc \
  -o build/dinput8.dll src/dllmain.cpp src/log.cpp src/proxy_dinput8.cpp src/present.cpp src/xr.cpp src/camhook.cpp src/rendersize.cpp \
  src/fps.cpp src/hud.cpp src/drawlog.cpp src/worldscale.cpp src/menu.cpp src/dinput8.def \
  $O/buffer.o $O/hook.o $O/trampoline.o $O/hde64.o $O/imgui.o $O/imgui_draw.o $O/imgui_tables.o $O/imgui_widgets.o $O/imgui_impl_dx11.o \
  -luser32 -ladvapi32 -lpsapi

# ds3vr-patcher.exe (7-Zip's public-domain C decoder unpacks Helifax's fix archive)
for f in Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec Ppmd7 Ppmd7Dec 7zCrc 7zCrcOpt 7zAlloc 7zArcIn 7zBuf 7zBuf2 7zDec 7zFile 7zStream; do
  $CC -O2 -DZ7_PPMD_SUPPORT -DZ7_EXTRACT_ONLY -c $Z/$f.c -o $O/7z_$f.o
done
$CXX -O2 -std=c++17 -municode -static -I$Z -o build/ds3vr-patcher.exe installer/setup.cpp installer/patch.cpp installer/sevenzip.cpp \
  $O/7z_*.o -lbcrypt -lshell32 -lole32 -ladvapi32 -luuid -lurlmon -lwininet
x86_64-w64-mingw32-strip build/ds3vr-patcher.exe
rm -rf $O

# Release layout: everything goes straight into the Game folder.
V=$(sed -n 's/.*DS3VR_VERSION "\(.*\)".*/\1/p' src/version.h)
R=build/DS3VR-$V
rm -rf $R
mkdir -p $R/ds3vr
cp build/dinput8.dll dist/install-ds3vr.bat dist/uninstall-ds3vr.bat $R/
cp build/ds3vr-patcher.exe dist/ds3vr.default.ini dist/README.txt $R/ds3vr/
sed 's/$/\r/' LICENSE > $R/ds3vr/LICENSE.txt
sed 's/$/\r/' THIRD-PARTY-NOTICES.txt > $R/ds3vr/THIRD-PARTY-NOTICES.txt
echo "built: build/dinput8.dll build/ds3vr-patcher.exe $R/"
