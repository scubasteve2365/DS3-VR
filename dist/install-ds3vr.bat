@echo off
setlocal
title DS3 VR install
cd /d "%~dp0"
echo.
if not exist "%~dp0DarkSoulsIII.exe" (
  echo This file has to be in the game's Game folder, next to DarkSoulsIII.exe.
  echo Extract the whole DS3 VR zip into that folder, for example
  echo   C:\Program Files ^(x86^)\Steam\steamapps\common\DARK SOULS III\Game
  echo and run install-ds3vr.bat from there.
  goto done
)
if not exist "%~dp0ds3vr\ds3vr-patcher.exe" (
  echo The ds3vr folder from the zip is missing. Extract the whole DS3 VR zip into this folder again.
  goto done
)
tasklist /FI "IMAGENAME eq DarkSoulsIII.exe" 2>nul | find /I "DarkSoulsIII.exe" >nul
if not errorlevel 1 (
  echo Dark Souls III is running. Close the game, then run install-ds3vr.bat again.
  goto done
)
"%~dp0ds3vr\ds3vr-patcher.exe" install "%~dp0."
:done
echo.
pause
