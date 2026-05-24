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

:: --- Remove files ---
for %%f in (version.dll chat_translator.dll chat_translator_live.dll config.ini term_protection.txt chat_log.txt loader_log.txt debug_log.txt imgui.ini translation_log.csv) do (
    del /f /q "!TARGET!\%%f" >nul 2>&1
)
for %%d in (assets tools) do (
    rd /s /q "!TARGET!\%%d" 2>nul
)

echo Uninstalled from !TARGET!
pause
