# Foxhole Chat Translator - Uninstaller
#
# Usage:
#   .\uninstall.ps1                                    # auto-detect Foxhole path
#   .\uninstall.ps1 -FoxholePath "D:\Games\Foxhole"    # manual Foxhole path

param(
    [string]$FoxholePath = ""
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$msg) { Write-Host "  $msg" -ForegroundColor Gray }
function Write-Ok([string]$msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "[!!] $msg" -ForegroundColor Yellow }
function Write-Fail([string]$msg) { Write-Host "[NG] $msg" -ForegroundColor Red }
function Write-Head([string]$msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Foxhole Chat Translator Uninstaller" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

# ================================================================
# Foxhole path
# ================================================================
Write-Head "Foxhole path"

if (-not $FoxholePath) {
    try {
        $steamReg  = Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction Stop
        $steamPath = $steamReg.SteamPath -replace "/", "\"
        $candidate = Join-Path $steamPath "steamapps\common\Foxhole"
        if (Test-Path $candidate) {
            $FoxholePath = $candidate
            Write-Ok "Auto-detected from Steam: $FoxholePath"
        }
    } catch {}
}

if (-not $FoxholePath -or -not (Test-Path $FoxholePath)) {
    Write-Warn "Could not auto-detect Foxhole."
    Write-Host "  e.g. C:\Program Files (x86)\Steam\steamapps\common\Foxhole" -ForegroundColor Gray
    $FoxholePath = Read-Host "  Foxhole install folder"
}

$targetDir = Join-Path $FoxholePath "War\Binaries\Win64"
if (-not (Test-Path $targetDir)) {
    Write-Fail "War\Binaries\Win64 not found: $targetDir"
    try { Read-Host "Press Enter to exit" } catch { }
    exit 1
}
Write-Ok "Target: $targetDir"

# ================================================================
# Remove files
# ================================================================
Write-Head "Uninstall"

$removeFiles = @(
    "version.dll",
    "chat_translator.dll",
    "chat_translator_live.dll",
    "config.ini",
    "term_protection.txt",
    "chat_log.txt",
    "loader_log.txt",
    "debug_log.txt",
    "imgui.ini"
)
$removeDirs = @("assets", "tools")
$anyRemoved = $false

foreach ($f in $removeFiles) {
    $p = Join-Path $targetDir $f
    if (Test-Path $p) {
        Remove-Item $p -Force
        Write-Ok "Removed: $f"
        $anyRemoved = $true
    } else {
        Write-Step "Not found (skipped): $f"
    }
}

foreach ($d in $removeDirs) {
    $p = Join-Path $targetDir $d
    if (Test-Path $p) {
        Remove-Item $p -Recurse -Force
        Write-Ok "Removed: $d\"
        $anyRemoved = $true
    } else {
        Write-Step "Not found (skipped): $d\"
    }
}

Write-Host ""
if ($anyRemoved) {
    Write-Host "Uninstall complete." -ForegroundColor Green
} else {
    Write-Warn "No mod files found. Already uninstalled?"
}

try { Read-Host "Press Enter to exit" } catch { }
