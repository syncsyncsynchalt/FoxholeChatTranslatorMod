# setup_tts.ps1 - Foxhole Chat Translator TTS セットアップ
#
# Sherpa-ONNX (ニューラルTTSエンジン) と Piper VITS 音声モデルを
# ゲームの tools/tts/ ディレクトリにダウンロード・展開します。
#
# 使い方:
#   PowerShell を右クリック → 「管理者として実行」は不要
#   Set-ExecutionPolicy -Scope Process Bypass
#   .\setup_tts.ps1
#
# オプション:
#   -InstallDir <パス>  ゲームバイナリディレクトリを手動指定
#   -LangsOnly ja,en    指定言語のみダウンロード (カンマ区切り)
#   -SkipDll            DLL のみスキップ (再セットアップ時)

param(
    [string]$InstallDir = "",
    [string]$LangsOnly  = "",
    [switch]$SkipDll
)

$ErrorActionPreference = "Stop"
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

# ============================================================
# インストール先の決定
# ============================================================

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($InstallDir -eq "") {
    $candidate = Join-Path $ScriptDir "..\..\War\Binaries\Win64"
    $resolved  = Resolve-Path $candidate -ErrorAction SilentlyContinue
    if ($resolved) {
        $InstallDir = $resolved.Path
    } else {
        Write-Host "ゲームバイナリディレクトリが見つかりません。" -ForegroundColor Red
        Write-Host "  -InstallDir オプションで手動指定してください。" -ForegroundColor Yellow
        Write-Host "  例: .\setup_tts.ps1 -InstallDir 'C:\...\War\Binaries\Win64'"
        exit 1
    }
}

$TtsDir    = Join-Path $InstallDir "tools\tts"
$ModelsDir = Join-Path $TtsDir "models"

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  Foxhole Chat Translator - TTS セットアップ" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "インストール先: $TtsDir" -ForegroundColor Green
Write-Host ""

New-Item -ItemType Directory -Force -Path $TtsDir    | Out-Null
New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

# 一時作業ディレクトリ
$TmpDir = Join-Path $env:TEMP "sherpa_tts_$PID"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

# ============================================================
# ヘルパー関数
# ============================================================

function Download-File {
    param([string]$Url, [string]$Dest, [string]$Label = "")
    $name = if ($Label) { $Label } else { Split-Path $Url -Leaf }
    Write-Host "  ダウンロード中: $name" -ForegroundColor Gray
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

function Extract-Tar {
    param([string]$Archive, [string]$DestDir)
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $Archive -C $DestDir 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "tar 展開失敗: $Archive" }
}

function Copy-IfExists {
    param([string]$Src, [string]$Dest)
    if (Test-Path $Src) {
        Copy-Item $Src $Dest -Force
        return $true
    }
    return $false
}

# ============================================================
# 1. Sherpa-ONNX DLL の取得
# ============================================================

if (-not $SkipDll) {
    Write-Host "=== [1/3] Sherpa-ONNX DLL ===" -ForegroundColor Cyan

    # GitHub Releases API で最新バージョンを取得
    Write-Host "  最新バージョンを確認中..." -ForegroundColor Gray
    $headers = @{ "User-Agent" = "FoxholeChatTranslator-TTS-Setup" }
    $releaseInfo = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/latest" `
        -Headers $headers
    $SherpaVersion = $releaseInfo.tag_name
    Write-Host "  Sherpa-ONNX $SherpaVersion" -ForegroundColor Green

    # Windows x64 shared DLL アセットを検索 (Release優先, -lib や no-tts は除外)
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

    if (-not $dllAsset) {
        Write-Host "  Windows x64 DLL アセットが見つかりません。" -ForegroundColor Red
        Write-Host "  手動でダウンロード: https://github.com/k2-fsa/sherpa-onnx/releases" -ForegroundColor Yellow
        Write-Host "  sherpa-onnx.dll と onnxruntime.dll を $TtsDir に配置してください。"
    } else {
        $archivePath = Join-Path $TmpDir $dllAsset.name
        Download-File $dllAsset.browser_download_url $archivePath $dllAsset.name

        $dllExtractDir = Join-Path $TmpDir "dll_extract"
        Extract-Tar $archivePath $dllExtractDir

        # DLL ファイルをコピー
        $dlls = Get-ChildItem -Recurse $dllExtractDir -Filter "*.dll"
        foreach ($dll in $dlls) {
            # 主要 DLL のみ (不要なものは除外)
            if ($dll.Name -match "sherpa|onnxruntime|espeak") {
                Copy-Item $dll.FullName (Join-Path $TtsDir $dll.Name) -Force
                Write-Host "  コピー: $($dll.Name)" -ForegroundColor Green
            }
        }

        # espeak-ng-data をコピー (DLL パッケージに含まれる場合)
        $espeakInPkg = Get-ChildItem -Recurse $dllExtractDir -Directory |
                       Where-Object { $_.Name -eq "espeak-ng-data" } |
                       Select-Object -First 1
        if ($espeakInPkg) {
            $espeakDest = Join-Path $TtsDir "espeak-ng-data"
            if (Test-Path $espeakDest) { Remove-Item $espeakDest -Recurse -Force }
            Copy-Item $espeakInPkg.FullName $espeakDest -Recurse -Force
            Write-Host "  espeak-ng-data/ をコピー (DLL パッケージ)" -ForegroundColor Green
        }

        # sherpa-onnx.dll がない場合は sherpa-onnx-c-api.dll を代替として使用
        $mainDll = Join-Path $TtsDir "sherpa-onnx.dll"
        $cApiDll = Join-Path $TtsDir "sherpa-onnx-c-api.dll"
        if (-not (Test-Path $mainDll) -and (Test-Path $cApiDll)) {
            Copy-Item $cApiDll $mainDll -Force
            Write-Host "  sherpa-onnx-c-api.dll → sherpa-onnx.dll としてコピー" -ForegroundColor Yellow
        }

        Remove-Item $dllExtractDir -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "=== [1/3] DLL スキップ (-SkipDll) ===" -ForegroundColor Gray
}

# ============================================================
# 2. 言語モデルのダウンロード
# ============================================================

Write-Host ""
Write-Host "=== [2/3] 音声モデル ===" -ForegroundColor Cyan

# lang -> (モデル名, Type: vits|supertonic)
$AllModels = [ordered]@{
    en = @{ Name="vits-piper-en_US-lessac-medium";                  Type="vits";       Desc="英語 (自然な男性音声)" }
    ja = @{ Name="sherpa-onnx-supertonic-3-tts-int8-2026-05-11";   Type="supertonic"; Desc="日本語 (Supertonic 3, 31言語対応)" }
    ru = @{ Name="vits-piper-ru_RU-ruslan-medium";                  Type="vits";       Desc="ロシア語" }
    zh = @{ Name="sherpa-onnx-vits-zh-ll";                         Type="vits";       Desc="中国語" }
    ko = @{ Name="vits-mimic3-ko_KO-kss_low";                      Type="vits";       Desc="韓国語" }
}

$LangFilter = if ($LangsOnly -ne "") { $LangsOnly -split "," | ForEach-Object { $_.Trim() } } else { $null }

$ModelBaseUrl    = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models"
$EspeakInstalled = Test-Path (Join-Path $TtsDir "espeak-ng-data")

foreach ($kv in $AllModels.GetEnumerator()) {
    $lang  = $kv.Key
    $model = $kv.Value

    if ($LangFilter -and ($LangFilter -notcontains $lang)) { continue }

    $langDir = Join-Path $ModelsDir $lang

    # ===== Supertonic モデル =====
    if ($model.Type -eq "supertonic") {
        $dpFile = Join-Path $langDir "duration_predictor.int8.onnx"
        if (Test-Path $dpFile) {
            Write-Host "  [$lang] $($model.Desc) - スキップ (既存)" -ForegroundColor Gray
            continue
        }
        Write-Host "  [$lang] $($model.Desc): $($model.Name)" -ForegroundColor White
        New-Item -ItemType Directory -Force -Path $langDir | Out-Null

        $archiveName = "$($model.Name).tar.bz2"
        $archivePath = Join-Path $TmpDir $archiveName
        $extractDir  = Join-Path $TmpDir "${lang}_extract"
        try {
            Download-File "$ModelBaseUrl/$archiveName" $archivePath $archiveName
            Extract-Tar $archivePath $extractDir

            $suFiles = @("duration_predictor.int8.onnx","text_encoder.int8.onnx",
                         "vector_estimator.int8.onnx","vocoder.int8.onnx",
                         "tts.json","unicode_indexer.bin","voice.bin")
            foreach ($f in $suFiles) {
                $src = Get-ChildItem -Recurse $extractDir -Filter $f | Select-Object -First 1
                if ($src) {
                    Copy-Item $src.FullName (Join-Path $langDir $f) -Force
                    Write-Host "    $f" -ForegroundColor Green
                }
            }
            Write-Host "    [完了]" -ForegroundColor Green
        } catch {
            Write-Host "    エラー: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "    手動DL: $ModelBaseUrl/$archiveName" -ForegroundColor Yellow
        } finally {
            if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
        }
        continue
    }

    # ===== VITS モデル =====
    $modelOnnx   = Join-Path $langDir "model.onnx"
    $modelTokens = Join-Path $langDir "tokens.txt"

    if ((Test-Path $modelOnnx) -and (Test-Path $modelTokens)) {
        Write-Host "  [$lang] $($model.Desc) - スキップ (既存)" -ForegroundColor Gray
        continue
    }

    Write-Host "  [$lang] $($model.Desc): $($model.Name)" -ForegroundColor White

    New-Item -ItemType Directory -Force -Path $langDir | Out-Null

    $archiveName = "$($model.Name).tar.bz2"
    $archivePath = Join-Path $TmpDir $archiveName
    $extractDir  = Join-Path $TmpDir "${lang}_extract"

    try {
        Download-File "$ModelBaseUrl/$archiveName" $archivePath $archiveName
        Extract-Tar $archivePath $extractDir

        # model.onnx
        $onnxFile = Get-ChildItem -Recurse $extractDir -Filter "*.onnx" | Select-Object -First 1
        if ($onnxFile) {
            Copy-Item $onnxFile.FullName $modelOnnx -Force
            $sizeMB = [math]::Round($onnxFile.Length / 1MB, 0)
            Write-Host "    model.onnx ($sizeMB MB)" -ForegroundColor Green
        } else {
            Write-Host "    model.onnx が見つかりません" -ForegroundColor Yellow
        }

        # tokens.txt
        $tokensFile = Get-ChildItem -Recurse $extractDir -Filter "tokens.txt" | Select-Object -First 1
        if ($tokensFile) {
            Copy-Item $tokensFile.FullName $modelTokens -Force
            Write-Host "    tokens.txt" -ForegroundColor Green
        }

        # lexicon.txt (一部モデルに含まれる)
        $lexFile = Get-ChildItem -Recurse $extractDir -Filter "lexicon.txt" | Select-Object -First 1
        if ($lexFile) {
            Copy-Item $lexFile.FullName (Join-Path $langDir "lexicon.txt") -Force
            Write-Host "    lexicon.txt" -ForegroundColor Green
        }

        # 中国語: jieba 辞書
        if ($lang -eq "zh") {
            $dictSrc = Get-ChildItem -Recurse $extractDir -Directory |
                       Where-Object { $_.Name -eq "dict" } | Select-Object -First 1
            if ($dictSrc) {
                $dictDest = Join-Path $langDir "dict"
                if (Test-Path $dictDest) { Remove-Item $dictDest -Recurse -Force }
                Copy-Item $dictSrc.FullName $dictDest -Recurse -Force
                Write-Host "    dict/" -ForegroundColor Green
            }
        }

        # espeak-ng-data (モデルに含まれる場合)
        if (-not $EspeakInstalled) {
            $espeakSrc = Get-ChildItem -Recurse $extractDir -Directory |
                         Where-Object { $_.Name -eq "espeak-ng-data" } | Select-Object -First 1
            if ($espeakSrc) {
                $espeakDest = Join-Path $TtsDir "espeak-ng-data"
                if (Test-Path $espeakDest) { Remove-Item $espeakDest -Recurse -Force }
                Copy-Item $espeakSrc.FullName $espeakDest -Recurse -Force
                Write-Host "    espeak-ng-data/" -ForegroundColor Green
                $EspeakInstalled = $true
            }
        }

        Write-Host "    [完了]" -ForegroundColor Green

    } catch {
        Write-Host "    エラー: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "    手動DL: $ModelBaseUrl/$archiveName" -ForegroundColor Yellow
    } finally {
        if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ============================================================
# 3. VOICEVOX Core (日本語ずんだもん TTS)
# ============================================================

Write-Host ""
Write-Host "=== [3/3] VOICEVOX Core (日本語ずんだもん) ===" -ForegroundColor Cyan

$VvDir     = Join-Path $TtsDir "voicevox"
$VvCoreDll = Join-Path $VvDir "c_api\voicevox_core.dll"

if (Test-Path $VvCoreDll) {
    Write-Host "  VOICEVOX Core - スキップ (既存)" -ForegroundColor Gray
} else {
    try {
        # GitHub API で最新バージョンを取得
        Write-Host "  最新バージョンを確認中 (VOICEVOX Core)..." -ForegroundColor Gray
        $vvHeaders  = @{ "User-Agent" = "FoxholeChatTranslator-TTS-Setup" }
        $vvRelease  = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/VOICEVOX/voicevox_core/releases/latest" `
            -Headers $vvHeaders
        $VvVersion  = $vvRelease.tag_name
        Write-Host "  VOICEVOX Core $VvVersion" -ForegroundColor Green

        $vvAsset = $vvRelease.assets |
            Where-Object { $_.name -eq "download-windows-x64.exe" } |
            Select-Object -First 1
        if (-not $vvAsset) { throw "download-windows-x64.exe アセットが見つかりません" }

        $VvDownloaderPath = Join-Path $TmpDir "voicevox_downloader.exe"
        Download-File $vvAsset.browser_download_url $VvDownloaderPath "voicevox_downloader.exe"

        New-Item -ItemType Directory -Force -Path $VvDir | Out-Null

        # UTF-8 に切り替えてダウンローダーを直接実行
        # (パイプすると stdin が切断されライセンス同意プロンプトに応答できない)
        # (CP932 のままだと Rust の pager が日本語でパニックする)
        $prevCodePage = [Console]::OutputEncoding.CodePage
        & chcp 65001 | Out-Null
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        [Console]::InputEncoding  = [System.Text.Encoding]::UTF8

        Write-Host ""
        Write-Host "  ライセンス同意画面が表示されます。" -ForegroundColor Yellow
        Write-Host "  [y,n,r] プロンプトが出たら  y  を入力して Enter を押してください。" -ForegroundColor Yellow
        Write-Host ""

        & $VvDownloaderPath --devices cpu --output $VvDir

        & chcp $prevCodePage | Out-Null
        [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding($prevCodePage)

        if (Test-Path $VvCoreDll) {
            Write-Host "  [完了] VOICEVOX Core" -ForegroundColor Green
        } else {
            Write-Host "  [警告] voicevox_core.dll が見つかりません" -ForegroundColor Yellow
            Write-Host "         ライセンスに同意しなかった場合はダウンロードされません。" -ForegroundColor Yellow
            Write-Host "         再度このスクリプトを実行してください。" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  エラー: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "  手動手順:" -ForegroundColor Yellow
        Write-Host "    1. https://github.com/VOICEVOX/voicevox_core/releases" -ForegroundColor Yellow
        Write-Host "    2. download-windows-x64.exe をダウンロード" -ForegroundColor Yellow
        Write-Host "    3. 実行: .\download-windows-x64.exe --devices cpu --output `"$VvDir`"" -ForegroundColor Yellow
    }
}

# ============================================================
# クリーンアップ
# ============================================================

Remove-Item $TmpDir -Recurse -Force -ErrorAction SilentlyContinue

# ============================================================
# インストール結果の表示
# ============================================================

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  結果" -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan

# DLL
$sherpaOk = (Test-Path (Join-Path $TtsDir "sherpa-onnx.dll")) -or
            (Test-Path (Join-Path $TtsDir "sherpa-onnx-c-api.dll"))
if ($sherpaOk) {
    Write-Host "  [OK] sherpa-onnx.dll" -ForegroundColor Green
} else {
    Write-Host "  [NG] sherpa-onnx.dll (TTS が動作しません)" -ForegroundColor Red
}
$p = Join-Path $TtsDir "onnxruntime.dll"
if (Test-Path $p) {
    Write-Host "  [OK] onnxruntime.dll" -ForegroundColor Green
} else {
    Write-Host "  [NG] onnxruntime.dll (TTS が動作しません)" -ForegroundColor Red
}

# espeak-ng-data
$espeakPath = Join-Path $TtsDir "espeak-ng-data"
if (Test-Path $espeakPath) {
    Write-Host "  [OK] espeak-ng-data/" -ForegroundColor Green
} else {
    Write-Host "  [警告] espeak-ng-data/ なし (日英露韓の発音が不安定になる可能性)" -ForegroundColor Yellow
}

# VOICEVOX
if (Test-Path $VvCoreDll) {
    Write-Host "  [OK] voicevox/c_api/voicevox_core.dll (ずんだもん)" -ForegroundColor Green
} else {
    Write-Host "  [--] voicevox/ なし (日本語は Sherpa-ONNX でフォールバック)" -ForegroundColor Gray
}

# モデル
foreach ($lang in "en","ja","ru","zh","ko") {
    $dpFile = Join-Path $ModelsDir "$lang\duration_predictor.int8.onnx"
    $onnx   = Join-Path $ModelsDir "$lang\model.onnx"
    if (Test-Path $dpFile) {
        Write-Host "  [OK] models/$lang/ (Supertonic 3)" -ForegroundColor Green
    } elseif (Test-Path $onnx) {
        $sizeMB = [math]::Round((Get-Item $onnx).Length / 1MB, 0)
        Write-Host "  [OK] models/$lang/model.onnx ($sizeMB MB)" -ForegroundColor Green
    } else {
        Write-Host "  [--] models/$lang/ なし" -ForegroundColor Gray
    }
}

Write-Host ""
Write-Host "ゲームを起動してチャットを送信すると、TTS が動作します。" -ForegroundColor Cyan
Write-Host "デバッグログ: $InstallDir\debug_log.txt" -ForegroundColor Gray
Write-Host ""
Read-Host "Enterで閉じる"
