# Foxhole Chat Translator - Installer
#
# Usage:
#   .\install.ps1                                    # user mode (run from extracted ZIP)
#   .\install.ps1 -Dev                               # dev mode  (DLLs from build\Release\)
#   .\install.ps1 -FoxholePath "D:\Games\Foxhole"    # manual Foxhole path
#   .\install.ps1 -LangsOnly ja,en                   # limit TTS model languages

param(
    [string]$FoxholePath = "",
    [switch]$Dev,
    [string]$LangsOnly = ""
)

$ErrorActionPreference = "Stop"
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step([string]$msg) { Write-Host "  $msg" -ForegroundColor Gray }
function Write-Ok([string]$msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "[!!] $msg" -ForegroundColor Yellow }
function Write-Fail([string]$msg) { Write-Host "[NG] $msg" -ForegroundColor Red }
function Write-Head([string]$msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

function Invoke-Download {
    param([string]$Url, [string]$Dest, [string]$Label = "")
    $name = if ($Label) { $Label } else { Split-Path $Url -Leaf }
    Write-Step "Downloading: $name"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

function Expand-Tar {
    param([string]$Archive, [string]$DestDir)
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $Archive -C $DestDir 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "tar failed: $Archive" }
}

Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Foxhole Chat Translator Installer" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

# ================================================================
# [1/5] Source resolution
# ================================================================
Write-Head "[1/5] Source"

if ($Dev) {
    $dllDir    = Join-Path $scriptDir "build\Release"
    $assetsDir = Join-Path $scriptDir "assets"
    $configSrc = Join-Path $scriptDir "config.ini"
    $termSrc   = Join-Path $scriptDir "term_protection.txt"
    Write-Ok "Dev mode: DLLs from $dllDir"
} else {
    $dllDir    = $scriptDir
    $assetsDir = Join-Path $scriptDir "assets"
    $configSrc = Join-Path $scriptDir "config.ini"
    $termSrc   = Join-Path $scriptDir "term_protection.txt"
    Write-Ok "User mode: $scriptDir"
}

# ================================================================
# [2/5] Foxhole path
# ================================================================
Write-Head "[2/5] Foxhole path"

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
# [3/5] Mod files
# ================================================================
Write-Head "[3/5] Mod files"

foreach ($dll in @("version.dll", "chat_translator.dll")) {
    $src = Join-Path $dllDir $dll
    if (-not (Test-Path $src)) {
        Write-Fail "$dll not found: $src"
        if ($Dev) { Write-Fail "Run: cmake --build build --config Release" }
        try { Read-Host "Press Enter to exit" } catch { }
        exit 1
    }
    Copy-Item $src $targetDir -Force
    Write-Step $dll
}

foreach ($src in @($configSrc, $termSrc)) {
    if (-not (Test-Path $src)) {
        Write-Fail "File not found: $src"
        try { Read-Host "Press Enter to exit" } catch { }
        exit 1
    }
    Copy-Item $src $targetDir -Force
    Write-Step (Split-Path -Leaf $src)
}

$assetsTarget = Join-Path $targetDir "assets"
New-Item -ItemType Directory -Path $assetsTarget -Force | Out-Null
Copy-Item (Join-Path $assetsDir "*") $assetsTarget -Recurse -Force
Write-Step "assets\"

Write-Ok "Mod files copied."

# ================================================================
# [4/5] Ollama (translation AI runtime)
# ================================================================
Write-Head "[4/5] Ollama"

$ollamaExe = $null
$cmd = Get-Command ollama -ErrorAction SilentlyContinue
if ($cmd) { $ollamaExe = $cmd.Source }
if (-not $ollamaExe -and (Test-Path "$env:LOCALAPPDATA\Programs\Ollama\ollama.exe")) {
    $ollamaExe = "$env:LOCALAPPDATA\Programs\Ollama\ollama.exe"
}
if (-not $ollamaExe -and (Test-Path "$env:ProgramFiles\Ollama\ollama.exe")) {
    $ollamaExe = "$env:ProgramFiles\Ollama\ollama.exe"
}

if ($ollamaExe) {
    Write-Ok "Ollama found: $ollamaExe"
} else {
    Write-Step "Ollama not found. Installing silently via winget..."
    try {
        # 公式と同じマーカーファイルを事前作成 → アプリ起動時にウィンドウを非表示にする
        $markerDir  = Join-Path $env:LOCALAPPDATA "Ollama"
        $markerFile = Join-Path $markerDir "upgraded"
        New-Item -ItemType Directory -Force -Path $markerDir | Out-Null
        New-Item -ItemType File      -Force -Path $markerFile | Out-Null

        # --force: winget レジストリが stale でも強制再インストール
        winget install Ollama.Ollama `
            --accept-package-agreements --accept-source-agreements `
            --silent --disable-interactivity --force 2>&1 |
            Where-Object { $_ -match "\S" } | ForEach-Object { Write-Step "  $_" }
        # インストール後に実際に ollama.exe が存在するか確認
        $verifyCmd = Get-Command ollama -ErrorAction SilentlyContinue
        $verifyPath = "$env:LOCALAPPDATA\Programs\Ollama\ollama.exe"
        if ($verifyCmd -or (Test-Path $verifyPath)) {
            Write-Ok "Ollama installed."
        } else {
            Write-Warn "Ollama exe not found after install. Install manually: https://ollama.com/download"
        }
    } catch {
        Write-Warn "winget install failed: $($_.Exception.Message)"
        Write-Warn "Install manually: https://ollama.com/download"
    }
}

# ================================================================
# [5/5] TTS (Sherpa-ONNX + models + VOICEVOX)
# ================================================================
Write-Head "[5/5] TTS"

$ttsDir    = Join-Path $targetDir "tools\tts"
$modelsDir = Join-Path $ttsDir "models"
$tmpDir    = Join-Path $env:TEMP "fct_tts_$PID"

New-Item -ItemType Directory -Force -Path $ttsDir    | Out-Null
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null
New-Item -ItemType Directory -Force -Path $tmpDir    | Out-Null

# ---- Sherpa-ONNX DLL ----
Write-Step "[TTS 1/3] Sherpa-ONNX DLL"
$sherpaOk = (Test-Path (Join-Path $ttsDir "sherpa-onnx.dll")) -or
            (Test-Path (Join-Path $ttsDir "sherpa-onnx-c-api.dll"))

if ($sherpaOk) {
    Write-Ok "Sherpa-ONNX DLL - already installed, skipped."
} else {
    try {
        $headers     = @{ "User-Agent" = "FoxholeChatTranslator-Setup" }
        $releaseInfo = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/latest" `
            -Headers $headers
        Write-Step "  Latest: $($releaseInfo.tag_name)"

        $dllAsset = $releaseInfo.assets | Where-Object {
            $_.name -match "win-x64-shared" -and
            $_.name -match "MD-Release" -and
            $_.name -notmatch "-lib\." -and
            $_.name -notmatch "no-tts" -and
            $_.name -match "\.(tar\.bz2|zip)$"
        } | Select-Object -First 1

        if (-not $dllAsset) {
            $dllAsset = $releaseInfo.assets | Where-Object {
                $_.name -match "win.*x64.*shared" -and
                $_.name -notmatch "-lib\." -and
                $_.name -notmatch "no-tts" -and
                $_.name -match "\.(tar\.bz2|zip)$"
            } | Select-Object -First 1
        }

        if (-not $dllAsset) { throw "Windows x64 DLL asset not found." }

        $archivePath   = Join-Path $tmpDir $dllAsset.name
        $dllExtractDir = Join-Path $tmpDir "dll_extract"
        Invoke-Download $dllAsset.browser_download_url $archivePath $dllAsset.name
        Expand-Tar $archivePath $dllExtractDir

        Get-ChildItem -Recurse $dllExtractDir -Filter "*.dll" |
            Where-Object { $_.Name -match "sherpa|onnxruntime|espeak" } |
            ForEach-Object {
                Copy-Item $_.FullName (Join-Path $ttsDir $_.Name) -Force
                Write-Step "  $($_.Name)"
            }

        $espeakInPkg = Get-ChildItem -Recurse $dllExtractDir -Directory |
                       Where-Object { $_.Name -eq "espeak-ng-data" } | Select-Object -First 1
        if ($espeakInPkg) {
            $espeakDest = Join-Path $ttsDir "espeak-ng-data"
            if (Test-Path $espeakDest) { Remove-Item $espeakDest -Recurse -Force }
            Copy-Item $espeakInPkg.FullName $espeakDest -Recurse -Force
            Write-Step "  espeak-ng-data/"
        }
        Remove-Item $dllExtractDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Ok "Sherpa-ONNX DLL installed."
    } catch {
        Write-Warn "Sherpa-ONNX DLL failed: $($_.Exception.Message)"
        Write-Warn "Manual: https://github.com/k2-fsa/sherpa-onnx/releases"
    }
}

# ---- TTS models ----
Write-Step "[TTS 2/3] Voice models"

$AllModels = [ordered]@{
    en = @{ Name="vits-piper-en_US-lessac-medium";                Type="vits";       Desc="English" }
    ja = @{ Name="sherpa-onnx-supertonic-3-tts-int8-2026-05-11"; Type="supertonic"; Desc="Japanese (Supertonic 3)" }
    ru = @{ Name="vits-piper-ru_RU-ruslan-medium";                Type="vits";       Desc="Russian" }
    zh = @{ Name="sherpa-onnx-vits-zh-ll";                       Type="vits";       Desc="Chinese" }
    ko = @{ Name="vits-mimic3-ko_KO-kss_low";                    Type="vits";       Desc="Korean" }
}

$LangFilter    = if ($LangsOnly) { $LangsOnly -split "," | ForEach-Object { $_.Trim() } } else { $null }
$ModelBaseUrl  = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models"
$espeakPresent = Test-Path (Join-Path $ttsDir "espeak-ng-data")

foreach ($kv in $AllModels.GetEnumerator()) {
    $lang  = $kv.Key
    $model = $kv.Value
    if ($LangFilter -and ($LangFilter -notcontains $lang)) { continue }

    $langDir    = Join-Path $modelsDir $lang
    $extractDir = Join-Path $tmpDir "${lang}_extract"

    if ($model.Type -eq "supertonic") {
        if (Test-Path (Join-Path $langDir "duration_predictor.int8.onnx")) {
            Write-Ok "  [$lang] $($model.Desc) - already installed, skipped."
            continue
        }
        Write-Step "  [$lang] $($model.Desc)"
        New-Item -ItemType Directory -Force -Path $langDir | Out-Null
        $archivePath = Join-Path $tmpDir "$($model.Name).tar.bz2"
        try {
            Invoke-Download "$ModelBaseUrl/$($model.Name).tar.bz2" $archivePath $model.Name
            Expand-Tar $archivePath $extractDir
            foreach ($f in @("duration_predictor.int8.onnx","text_encoder.int8.onnx",
                              "vector_estimator.int8.onnx","vocoder.int8.onnx",
                              "tts.json","unicode_indexer.bin","voice.bin")) {
                $s = Get-ChildItem -Recurse $extractDir -Filter $f | Select-Object -First 1
                if ($s) { Copy-Item $s.FullName (Join-Path $langDir $f) -Force }
            }
            Write-Ok "  [$lang] done."
        } catch {
            Write-Warn "  [$lang] failed: $($_.Exception.Message)"
        } finally {
            if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
        }
        continue
    }

    # VITS
    if ((Test-Path (Join-Path $langDir "model.onnx")) -and (Test-Path (Join-Path $langDir "tokens.txt"))) {
        Write-Ok "  [$lang] $($model.Desc) - already installed, skipped."
        continue
    }
    Write-Step "  [$lang] $($model.Desc)"
    New-Item -ItemType Directory -Force -Path $langDir | Out-Null
    $archivePath = Join-Path $tmpDir "$($model.Name).tar.bz2"
    try {
        Invoke-Download "$ModelBaseUrl/$($model.Name).tar.bz2" $archivePath $model.Name
        Expand-Tar $archivePath $extractDir

        $s = Get-ChildItem -Recurse $extractDir -Filter "*.onnx" | Select-Object -First 1
        if ($s) { Copy-Item $s.FullName (Join-Path $langDir "model.onnx") -Force }

        $s = Get-ChildItem -Recurse $extractDir -Filter "tokens.txt" | Select-Object -First 1
        if ($s) { Copy-Item $s.FullName (Join-Path $langDir "tokens.txt") -Force }

        $s = Get-ChildItem -Recurse $extractDir -Filter "lexicon.txt" | Select-Object -First 1
        if ($s) { Copy-Item $s.FullName (Join-Path $langDir "lexicon.txt") -Force }

        if ($lang -eq "zh") {
            $d = Get-ChildItem -Recurse $extractDir -Directory |
                 Where-Object { $_.Name -eq "dict" } | Select-Object -First 1
            if ($d) {
                $dictDest = Join-Path $langDir "dict"
                if (Test-Path $dictDest) { Remove-Item $dictDest -Recurse -Force }
                Copy-Item $d.FullName $dictDest -Recurse -Force
            }
        }

        if (-not $espeakPresent) {
            $e = Get-ChildItem -Recurse $extractDir -Directory |
                 Where-Object { $_.Name -eq "espeak-ng-data" } | Select-Object -First 1
            if ($e) {
                $espeakDest = Join-Path $ttsDir "espeak-ng-data"
                if (Test-Path $espeakDest) { Remove-Item $espeakDest -Recurse -Force }
                Copy-Item $e.FullName $espeakDest -Recurse -Force
                $espeakPresent = $true
            }
        }
        Write-Ok "  [$lang] done."
    } catch {
        Write-Warn "  [$lang] failed: $($_.Exception.Message)"
    } finally {
        if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---- VOICEVOX Core ----
Write-Step "[TTS 3/3] VOICEVOX Core (Japanese Zundamon)"
$vvDir     = Join-Path $ttsDir "voicevox"
$vvCoreDll = Join-Path $vvDir "c_api\lib\voicevox_core.dll"

if (Test-Path $vvCoreDll) {
    Write-Ok "VOICEVOX Core - already installed, skipped."
} else {
    try {
        $vvHeaders = @{ "User-Agent" = "FoxholeChatTranslator-Setup" }
        $vvRelease = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/VOICEVOX/voicevox_core/releases/latest" `
            -Headers $vvHeaders
        Write-Step "  VOICEVOX Core $($vvRelease.tag_name)"

        $vvAsset = $vvRelease.assets |
            Where-Object { $_.name -eq "download-windows-x64.exe" } |
            Select-Object -First 1
        if (-not $vvAsset) { throw "download-windows-x64.exe not found in release assets." }

        $vvDownloaderPath = Join-Path $tmpDir "voicevox_downloader.exe"
        Invoke-Download $vvAsset.browser_download_url $vvDownloaderPath "voicevox_downloader.exe"
        New-Item -ItemType Directory -Force -Path $vvDir | Out-Null

        Write-Step "  Downloading VOICEVOX Core (auto-accepting license)..."

        # PowerShell のパイプは chcp 65001 後に Console.InputEncoding が BOM 付きになり
        # stdin に BOM が混入する。cmd.exe の < リダイレクトは純粋バイト転送なので回避できる
        $prevTerm = $env:TERM
        $env:TERM = "dumb"

        # "y\n" を ASCII バイトとしてファイルに書き込み stdin ファイルとして使用
        # --models-pattern 0.vvm: style_id=3 (ずんだもんノーマル) は 0.vvm に収録
        #   全モデルをダウンロードすると GitHub レートリミットに当たるため絞り込む
        $vvStdinFile = Join-Path $tmpDir "vv_stdin.txt"
        $vvBatchFile = Join-Path $tmpDir "vv_run.bat"
        [System.IO.File]::WriteAllBytes($vvStdinFile, [byte[]]@(0x79, 0x0A))  # ASCII "y\n"
        $vvBatch  = "@echo off`r`n"
        $vvBatch += "`"$vvDownloaderPath`" --devices cpu --models-pattern 0.vvm --output `"$vvDir`" < `"$vvStdinFile`"`r`n"
        [System.IO.File]::WriteAllText($vvBatchFile, $vvBatch, [System.Text.Encoding]::ASCII)
        & cmd.exe /c $vvBatchFile

        $env:TERM = $prevTerm

        $vvCoreExists = (Test-Path $vvCoreDll) -or ([System.IO.File]::Exists($vvCoreDll))
        if ($vvCoreExists) {
            Write-Ok "VOICEVOX Core installed."
        } else {
            Write-Warn "VOICEVOX Core not found after download."
            Write-Warn "Re-run install.ps1 to retry."
        }
    } catch {
        Write-Warn "VOICEVOX Core failed: $($_.Exception.Message)"
        Write-Warn "Manual: https://github.com/VOICEVOX/voicevox_core/releases"
    }
}

Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

# ================================================================
# Summary
# ================================================================
Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Installation complete!" -ForegroundColor Green
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Launch Foxhole"
Write-Host "  2. On first run, translation AI model (~3 GB) will be downloaded"
Write-Host "     (Translation may take a few minutes to start)"
Write-Host "  3. Toggle translation with the radio icon in the bottom-right corner"
Write-Host ""
Write-Host "Uninstall:"
Write-Host "  Delete version.dll / chat_translator.dll / config.ini"
Write-Host "  / term_protection.txt / assets\ / tools\ from War\Binaries\Win64\"
Write-Host ""

try { Read-Host "Press Enter to exit" } catch { }
