@echo off
setlocal
title DS3 VR uninstall
cd /d "%~dp0"
echo.
if not exist "%~dp0ds3vr\ds3vr-patcher.exe" (
  echo The ds3vr folder is missing, so nothing can be restored from here.
  echo To remove DS3 VR by hand: delete dinput8.dll, ds3vr.ini and install-ds3vr.bat, then extract
  echo Helifax's fix over this folder again ^(or delete it^) to undo the VR changes.
  echo.
  pause
  exit /b 1
)
tasklist /FI "IMAGENAME eq DarkSoulsIII.exe" 2>nul | find /I "DarkSoulsIII.exe" >nul
if not errorlevel 1 (
  echo Dark Souls III is running. Close the game, then run uninstall-ds3vr.bat again.
  echo.
  pause
  exit /b 1
)
"%~dp0ds3vr\ds3vr-patcher.exe" uninstall "%~dp0."
if errorlevel 1 (
  echo.
  echo Uninstall did not finish, so the DS3 VR files were left in place.
  echo.
  pause
  exit /b 1
)
rmdir /s /q "%~dp0ds3vr"
del /q "%~dp0install-ds3vr.bat" 2>nul
echo.
echo DS3 VR is removed. Helifax's fix is back to how it was; to play without it, run his
echo uninstall.bat in this folder.
echo.
pause
(goto) 2>nul & del "%~f0"
