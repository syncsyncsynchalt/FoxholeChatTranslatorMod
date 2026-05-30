@echo off
setlocal EnableDelayedExpansion

:: --- Find Foxhole ---
set "FOXHOLE=%~1"

for /f "tokens=2*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul') do (
    if /i "%%a"=="REG_SZ" set "STEAM=%%b"
)
if not defined FOXHOLE if defined STEAM (
    set "FOXHOLE=!STEAM!\steamapps\common\Foxhole"
    set "FOXHOLE=!FOXHOLE:/=\!"
)
if not defined FOXHOLE set /p "FOXHOLE=Foxhole path not found. Enter manually: "

set "TARGET=!FOXHOLE!\War\Binaries\Win64"
if not exist "!TARGET!\." (
    echo Not found: !TARGET!
    pause & exit /b 1
)

:: --- Toggle ---
if exist "!TARGET!\version.dll" (
    rename "!TARGET!\version.dll" "version.dll.disabled"
    echo Mod DISABLED  [version.dll.disabled]
    echo Launch Foxhole without the mod. Run this script again to re-enable.
    pause & exit /b 0
)

if exist "!TARGET!\version.dll.disabled" (
    rename "!TARGET!\version.dll.disabled" "version.dll"
    echo Mod ENABLED  [version.dll]
    echo Launch Foxhole to use the mod.
    pause & exit /b 0
)

echo Neither version.dll nor version.dll.disabled found in:
echo   !TARGET!
echo Run install.bat first.
pause & exit /b 1
