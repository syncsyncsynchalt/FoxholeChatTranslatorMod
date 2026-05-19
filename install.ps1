# Foxhole Chat Translator インストーラー
# このスクリプトを解凍したフォルダ内で実行してください。
# 右クリック → "PowerShell で実行" でも動作します。
#
# 使い方:
#   .\install.ps1                         # Foxhole パスを自動検出
#   .\install.ps1 -FoxholePath "D:\Games\Foxhole"  # パスを手動指定

param (
    [string]$FoxholePath = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step([string]$msg) { Write-Host "  $msg" -ForegroundColor Gray }
function Write-Ok([string]$msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "[!!] $msg" -ForegroundColor Yellow }
function Write-Fail([string]$msg) { Write-Host "[NG] $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Foxhole Chat Translator インストーラー" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""

# ================================================================
# 1. Foxhole インストールパスの特定
# ================================================================
Write-Host ">> Foxhole のパスを確認中..." -ForegroundColor White

if (-not $FoxholePath) {
    try {
        $steamReg  = Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction Stop
        $steamPath = $steamReg.SteamPath -replace "/", "\"
        $candidate = Join-Path $steamPath "steamapps\common\Foxhole"
        if (Test-Path $candidate) {
            $FoxholePath = $candidate
            Write-Ok "Steam から自動検出: $FoxholePath"
        }
    } catch {
        Write-Warn "Steam レジストリを読み取れませんでした"
    }
}

if (-not $FoxholePath -or -not (Test-Path $FoxholePath)) {
    Write-Warn "Foxhole のパスを自動検出できませんでした。"
    Write-Host "  例: C:\Program Files (x86)\Steam\steamapps\common\Foxhole" -ForegroundColor Gray
    $FoxholePath = Read-Host "  Foxhole のインストールフォルダを入力してください"
}

$targetDir = Join-Path $FoxholePath "War\Binaries\Win64"

if (-not (Test-Path $targetDir)) {
    Write-Fail "指定されたパスに War\Binaries\Win64 が見つかりません:"
    Write-Fail "  $targetDir"
    Write-Host ""
    Read-Host "Enterで終了"
    exit 1
}

Write-Ok "インストール先: $targetDir"
Write-Host ""

# ================================================================
# 2. ファイルのコピー
# ================================================================
Write-Host ">> ファイルをコピー中..." -ForegroundColor White

$rootFiles = @("version.dll", "chat_translator.dll", "config.ini", "term_protection.txt")
foreach ($f in $rootFiles) {
    $src = Join-Path $scriptDir $f
    if (-not (Test-Path $src)) {
        Write-Fail "$f が見つかりません。ZIP が壊れている可能性があります。"
        Read-Host "Enterで終了"
        exit 1
    }
    Copy-Item -Path $src -Destination $targetDir -Force
    Write-Step $f
}

# assets フォルダ
$assetsTarget = Join-Path $targetDir "assets"
New-Item -ItemType Directory -Path $assetsTarget -Force | Out-Null
Copy-Item -Path (Join-Path $scriptDir "assets\*") -Destination $assetsTarget -Recurse -Force
Write-Step "assets\"

# tools\ollama フォルダ (Ollama バイナリ)
$toolsTarget = Join-Path $targetDir "tools"
New-Item -ItemType Directory -Path $toolsTarget -Force | Out-Null
$ollamaTarget = Join-Path $toolsTarget "ollama"
New-Item -ItemType Directory -Path $ollamaTarget -Force | Out-Null
Copy-Item -Path (Join-Path $scriptDir "tools\ollama\*") -Destination $ollamaTarget -Recurse -Force
Write-Step "tools\ollama\"

Write-Ok "ファイルのコピー完了"
Write-Host ""

# ================================================================
# 3. TTS (音声読み上げ) のセットアップ (任意)
# ================================================================
Write-Host ">> 音声読み上げ機能のセットアップ" -ForegroundColor White
Write-Host "   チャットを音声で読み上げる機能です。約 500MB ~ 1GB のダウンロードが発生します。" -ForegroundColor Gray
Write-Host "   スキップしてもテキスト翻訳は正常に動作します。" -ForegroundColor Gray
Write-Host ""
$ttsChoice = Read-Host "  音声読み上げをセットアップしますか? [y/N]"

if ($ttsChoice -match "^[Yy]") {
    $setupTts = Join-Path $scriptDir "setup_tts.ps1"
    if (Test-Path $setupTts) {
        Write-Host ""
        Write-Host "  TTS セットアップを開始します..." -ForegroundColor Cyan
        & $setupTts -InstallDir $FoxholePath
    } else {
        Write-Warn "setup_tts.ps1 が見つかりません。TTS のセットアップはスキップします。"
        Write-Warn "後から実行する場合は setup_tts.ps1 を GitHub Releases からダウンロードしてください。"
    }
} else {
    Write-Step "TTS セットアップをスキップしました"
}

Write-Host ""

# ================================================================
# 4. 完了
# ================================================================
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  インストール完了！" -ForegroundColor Green
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "次のステップ:"
Write-Host "  1. Foxhole を起動してください"
Write-Host "  2. 初回起動時に翻訳 AI モデル (約 3GB) が自動ダウンロードされます"
Write-Host "     (翻訳が始まるまで数分かかります)"
Write-Host "  3. 画面右下のラジオアイコンで ON/OFF を切り替えられます"
Write-Host ""
Write-Host "アンインストール:"
Write-Host "  War\Binaries\Win64\ から version.dll / chat_translator.dll /"
Write-Host "  config.ini / assets\ / tools\ を削除してください"
Write-Host ""

Read-Host "Enterで終了"
