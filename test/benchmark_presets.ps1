# ============================================================
# benchmark_presets.ps1 - Preset Comparison Benchmark
# 3 presets x 9 sentences x 5 runs = 135 requests
# Focus: speed / quality / cold-start across Low / Medium / High
# ============================================================

param(
    [string]$OllamaUrl  = "http://localhost:11434",
    [string]$OutputCsv  = "benchmark_presets.csv"
)

$ErrorActionPreference = "Stop"

# ============================================================
# Preset definitions (must match translator config)
# ============================================================

$Presets = @(
    @{ Name = "Low";    Model = "gemma3:1b"; NumCtx = 128; NumThread = 2 },
    @{ Name = "Medium"; Model = "gemma3:4b"; NumCtx = 256; NumThread = 0 },
    @{ Name = "High";   Model = "gemma3:4b"; NumCtx = 512; NumThread = 0 }
)

# ============================================================
# Test sentences: EN / RU / KO x Short / Medium / Long
# Representative of actual Foxhole in-game chat
# ============================================================

$TestSentences = @(
    @{ Label = "Short-EN";  Lang = "EN"; Text = "need ammo" },
    @{ Label = "Short-RU";  Lang = "RU"; Text = "нужны патроны" },
    @{ Label = "Short-KO";  Lang = "KO"; Text = "탄약 필요" },
    @{ Label = "Medium-EN"; Lang = "EN"; Text = "build the gates back up we need defenses now" },
    @{ Label = "Medium-RU"; Lang = "RU"; Text = "постройте ворота снова нам нужна оборона сейчас" },
    @{ Label = "Medium-KO"; Lang = "KO"; Text = "적이 밀고 있습니다 방어선을 강화하세요" },
    @{ Label = "Long-EN";   Lang = "EN"; Text = "hey guys we need more people at the front line the enemy is pushing hard and we are running low on supplies" },
    @{ Label = "Long-RU";   Lang = "RU"; Text = "всем отрядам отступить к точке сбора противник прорвал линию обороны и движется в нашу сторону нужно срочно отступать" },
    @{ Label = "Long-KO";   Lang = "KO"; Text = "여러분 전선에 더 많은 병력이 필요합니다 적이 강하게 밀고 있고 보급품이 부족합니다 지금 당장 지원이 필요해요" }
)

$RunsPerPreset = 5   # run1=cold, run2-5=warm

# ============================================================
# Prompt template (same as translate.cpp)
# ============================================================

$TargetLang = "Japanese"
function Build-Prompt([string]$text) {
    return "Translate the following war game chat message to $TargetLang accurately. Keep the original meaning. End sentences with 〜のだ or 〜なのだ (Zundamon style). Output ONLY the translated text. No explanations, no extra sentences.`n`n$text"
}

# ============================================================
# Ollama startup
# ============================================================

function Test-OllamaRunning {
    try { $null = Invoke-RestMethod -Uri "$OllamaUrl/api/version" -TimeoutSec 3; return $true }
    catch { return $false }
}

function Start-BundledOllama {
    $scriptDir = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $scriptDir "tools\ollama\ollama.exe"),
        (Join-Path $PSScriptRoot "..\tools\ollama\ollama.exe")
    )
    $ollamaExe = $null
    foreach ($c in $candidates) { if (Test-Path $c) { $ollamaExe = (Resolve-Path $c).Path; break } }
    if (-not $ollamaExe) { Write-Error "Ollama is not running. Start 'ollama serve' manually."; exit 1 }

    $ollamaDir  = Split-Path -Parent $ollamaExe
    $modelsDir  = Join-Path $ollamaDir "models"
    if (-not (Test-Path $modelsDir)) { New-Item -ItemType Directory -Path $modelsDir | Out-Null }
    $env:OLLAMA_MODELS = $modelsDir

    Write-Host "[Benchmark] Starting ollama: $ollamaExe" -ForegroundColor Yellow
    Start-Process -FilePath $ollamaExe -ArgumentList "serve" -WindowStyle Hidden
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-OllamaRunning) { Write-Host "[Benchmark] Ollama ready ($([int](($i+1)*0.5))s)" -ForegroundColor Green; return }
    }
    Write-Error "Ollama startup timeout"; exit 1
}

function Test-ModelAvailable([string]$modelName) {
    try {
        $null = Invoke-RestMethod -Uri "$OllamaUrl/api/show" -Method Post `
            -Body (@{ name = $modelName } | ConvertTo-Json) -ContentType "application/json" -TimeoutSec 10
        return $true
    } catch {
        Write-Host "[Benchmark] Downloading $modelName ..." -ForegroundColor Yellow
        try {
            $null = Invoke-RestMethod -Uri "$OllamaUrl/api/pull" -Method Post `
                -Body (@{ name = $modelName; stream = $false } | ConvertTo-Json) -ContentType "application/json" -TimeoutSec 1800
            return $true
        } catch { Write-Error "Failed to download ${modelName}: $_"; return $false }
    }
}

# ============================================================
# Translation request
# ============================================================

function Invoke-Translation([string]$model, [int]$numCtx, [int]$numThread, [string]$text) {
    $options = @{ num_ctx = $numCtx; num_predict = 120; temperature = 0.1 }
    if ($numThread -ne 0) { $options["num_thread"] = $numThread }
    $body = @{
        model   = $model
        prompt  = Build-Prompt $text
        stream  = $false
        options = $options
    } | ConvertTo-Json -Depth 3

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $resp = Invoke-RestMethod -Uri "$OllamaUrl/api/generate" -Method Post `
            -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) `
            -ContentType "application/json; charset=utf-8" -TimeoutSec 300
        $sw.Stop()
        $tps = if ($resp.eval_duration -gt 0) { [math]::Round($resp.eval_count / ($resp.eval_duration / 1e9), 2) } else { 0 }
        return @{ Ok=$true; ElapsedMs=[math]::Round($sw.Elapsed.TotalMilliseconds,0); TokPerSec=$tps; EvalCount=$resp.eval_count; Response=$resp.response }
    } catch {
        $sw.Stop()
        return @{ Ok=$false; ElapsedMs=[math]::Round($sw.Elapsed.TotalMilliseconds,0); TokPerSec=0; EvalCount=0; Response="ERROR: $_" }
    }
}

function Get-OllamaMemoryMB {
    try {
        $procs = Get-Process -Name "ollama*" -ErrorAction SilentlyContinue
        if ($procs) { return [math]::Round(($procs | Measure-Object -Property WorkingSet64 -Sum).Sum / 1MB, 1) }
    } catch {}
    return 0
}

# ============================================================
# Main
# ============================================================

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Preset Comparison Benchmark" -ForegroundColor Cyan
Write-Host " 3 presets x $($TestSentences.Count) sentences x $RunsPerPreset runs = $($Presets.Count * $TestSentences.Count * $RunsPerPreset) requests" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if (-not (Test-OllamaRunning)) { Start-BundledOllama }

$cpu      = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$ram      = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$allGpus  = Get-CimInstance Win32_VideoController
$gpu      = ($allGpus | Where-Object { $_.Name -match "NVIDIA" } | Select-Object -First 1).Name
if (-not $gpu) { $gpu = ($allGpus | Sort-Object AdapterRAM -Descending | Select-Object -First 1).Name }
$ollamaV  = try { (Invoke-RestMethod -Uri "$OllamaUrl/api/version" -TimeoutSec 5).version } catch { "unknown" }
Write-Host "[Env] CPU=$cpu  RAM=${ram}GB  GPU=$gpu  Ollama=$ollamaV"

$requiredModels = $Presets | ForEach-Object { $_.Model } | Select-Object -Unique
foreach ($m in $requiredModels) {
    if (-not (Test-ModelAvailable $m)) { Write-Error "Model not available: $m"; exit 1 }
}
Write-Host ""

# Collect results
$csvRows   = [System.Collections.ArrayList]@()
$csvHeader = "Preset,Model,NumCtx,NumThread,Sentence,Lang,SentenceLen,Run,ElapsedMs,TokensPerSec,EvalCount,MemoryMB,Translation"

# Store warm-run data for summary: $warmData[preset][sentence] = list of results
$warmData = @{}
foreach ($p in $Presets) { $warmData[$p.Name] = @{} }

$total   = $Presets.Count * $TestSentences.Count * $RunsPerPreset
$current = 0
$sw      = [System.Diagnostics.Stopwatch]::StartNew()

foreach ($preset in $Presets) {
    Write-Host "--- [$($preset.Name)] model=$($preset.Model) ctx=$($preset.NumCtx) thread=$($preset.NumThread) ---" -ForegroundColor Cyan

    foreach ($sent in $TestSentences) {
        $warmData[$preset.Name][$sent.Label] = [System.Collections.ArrayList]@()

        for ($run = 1; $run -le $RunsPerPreset; $run++) {
            $current++
            $r    = Invoke-Translation -model $preset.Model -numCtx $preset.NumCtx -numThread $preset.NumThread -text $sent.Text
            $mem  = Get-OllamaMemoryMB
            $type = if ($run -eq 1) { "cold" } else { "warm" }
            $icon = if ($r.Ok) { "[OK]" } else { "[NG]" }

            $preview = if ($r.Response.Length -gt 50) { $r.Response.Substring(0,50) + "…" } else { $r.Response }
            Write-Host ("  {0} {1} run{2}({3}): {4}ms  {5} tok/s  [{6}]" -f $icon, $sent.Label, $run, $type, $r.ElapsedMs, $r.TokPerSec, $preview)

            if ($run -ge 2 -and $r.Ok) {
                $null = $warmData[$preset.Name][$sent.Label].Add($r)
            }

            $esc = $r.Response -replace '"', '""'
            $null = $csvRows.Add("$($preset.Name),$($preset.Model),$($preset.NumCtx),$($preset.NumThread),$($sent.Label),$($sent.Lang),$($sent.Text.Length),$run,$($r.ElapsedMs),$($r.TokPerSec),$($r.EvalCount),$mem,`"$esc`"")

            Write-Progress -Activity "Preset Benchmark" -Status "$current / $total" -PercentComplete ([math]::Round($current / $total * 100))
        }
    }
    Write-Host ""
}

$sw.Stop()
Write-Progress -Activity "Preset Benchmark" -Completed

# ============================================================
# CSV output
# ============================================================

$csvContent = $csvHeader + "`n" + ($csvRows -join "`n")
$csvPath = if ([System.IO.Path]::IsPathRooted($OutputCsv)) { $OutputCsv } else { Join-Path (Get-Location) $OutputCsv }
[System.IO.File]::WriteAllText($csvPath, $csvContent, [System.Text.UTF8Encoding]::new($true))
Write-Host "[Done] CSV: $csvPath" -ForegroundColor Green

# ============================================================
# Summary: speed table (warm avg)
# ============================================================

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Speed Summary  (warm avg = run 2-$RunsPerPreset)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$langs = @("EN","RU","KO")
$lens  = @("Short","Medium","Long")

foreach ($lang in $langs) {
    Write-Host ""
    Write-Host "  [$lang]" -ForegroundColor Yellow
    Write-Host ("  {0,-10} {1,10} {2,10} {3,10} {4,10}" -f "Preset", "Short(ms)", "Med(ms)", "Long(ms)", "tok/s")
    Write-Host ("  {0,-10} {1,10} {2,10} {3,10} {4,10}" -f ("-"*10), ("-"*10), ("-"*10), ("-"*10), ("-"*10))

    foreach ($preset in $Presets) {
        $shortMs = ($warmData[$preset.Name]["Short-$lang"]  | ForEach-Object { $_['ElapsedMs'] }  | Measure-Object -Average).Average
        $medMs   = ($warmData[$preset.Name]["Medium-$lang"] | ForEach-Object { $_['ElapsedMs'] }  | Measure-Object -Average).Average
        $longMs  = ($warmData[$preset.Name]["Long-$lang"]   | ForEach-Object { $_['ElapsedMs'] }  | Measure-Object -Average).Average
        $tpsVals = $warmData[$preset.Name].Values | ForEach-Object { $_ } | Where-Object { $_ -ne $null -and $_['TokPerSec'] -gt 0 } | ForEach-Object { $_['TokPerSec'] }
        $tps     = ($tpsVals | Measure-Object -Average).Average
        Write-Host ("  {0,-10} {1,10:N0} {2,10:N0} {3,10:N0} {4,10:N1}" -f $preset.Name, $shortMs, $medMs, $longMs, $tps)
    }
}

# ============================================================
# Summary: cold-start time
# ============================================================

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Cold-Start Time  (run 1)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$coldData = @{}
foreach ($line in $csvRows) {
    $f = $line -split ","
    if ([int]$f[7] -eq 1) {
        $key = $f[0]
        if (-not $coldData.ContainsKey($key)) { $coldData[$key] = [System.Collections.ArrayList]@() }
        $null = $coldData[$key].Add([double]$f[8])
    }
}

Write-Host ""
Write-Host ("  {0,-10} {1,15}" -f "Preset", "Cold-Avg(ms)")
Write-Host ("  {0,-10} {1,15}" -f ("-"*10), ("-"*15))
foreach ($preset in $Presets) {
    $avg = ($coldData[$preset.Name] | Measure-Object -Average).Average
    Write-Host ("  {0,-10} {1,15:N0}" -f $preset.Name, $avg)
}

# ============================================================
# Summary: translation quality (run 2 output per preset)
# ============================================================

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Translation Quality  (run 2 output)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# Collect run2 translations: [sentence][preset] = translation
$qualData = @{}
foreach ($sent in $TestSentences) { $qualData[$sent.Label] = @{} }

foreach ($line in $csvRows) {
    $f = $line -split ","
    if ([int]$f[7] -eq 2) {
        $preset = $f[0]
        $sentLabel = $f[4]
        $translation = ($line -replace "^[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,", "") -replace '^"(.*)"$', '$1'
        $qualData[$sentLabel][$preset] = $translation
    }
}

foreach ($sent in $TestSentences) {
    Write-Host ""
    Write-Host "  [$($sent.Label)] $($sent.Text)" -ForegroundColor Yellow
    foreach ($preset in $Presets) {
        $t = $qualData[$sent.Label][$preset.Name]
        Write-Host ("  {0,-10}: {1}" -f $preset.Name, $t)
    }
}

Write-Host ""
Write-Host "[Total time] $([math]::Round($sw.Elapsed.TotalMinutes, 1)) min" -ForegroundColor Green
