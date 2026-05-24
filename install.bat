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

:: --- Locate source DLLs (ZIP layout or dev build) ---
set "SRC=%~dp0"
if not exist "!SRC!version.dll" set "SRC=!SRC!build\Release\"
if not exist "!SRC!version.dll" (
    echo version.dll not found in !SRC!
    pause & exit /b 1
)

:: --- Copy files ---
copy /y "!SRC!version.dll"          "!TARGET!\" >nul
copy /y "!SRC!chat_translator.dll"  "!TARGET!\" >nul
copy /y "%~dp0term_protection.txt"  "!TARGET!\" >nul
if not exist "!TARGET!\config.ini" copy /y "%~dp0config.ini" "!TARGET!\" >nul
if not exist "!TARGET!\assets\."   mkdir "!TARGET!\assets"
xcopy /y /e /q "%~dp0assets\*" "!TARGET!\assets\" >nul

echo Installed to !TARGET!
echo On first launch, Ollama / TTS / font will be downloaded automatically.
pause
