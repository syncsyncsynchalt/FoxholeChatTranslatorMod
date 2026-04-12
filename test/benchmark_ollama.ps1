# ============================================================
# benchmark_ollama.ps1 - Ollama Translation Speed Benchmark
# Stage 10: 13 patterns x 3 sentences x 3 runs = 117 requests
# ============================================================

param(
    [string]$OllamaUrl = "http://localhost:11434",
    [string]$OutputCsv = "benchmark_results.csv"
)

$ErrorActionPreference = "Stop"

# ============================================================
# Test sentences (based on chat log length distribution)
# ============================================================

$TestSentences = @(
    @{ Label = "Short"; Text = "need ammo" },
    @{ Label = "Medium"; Text = "build the gates back up we need defenses now" },
    @{ Label = "Long"; Text = "hey guys we need more people at the front line the enemy is pushing hard and we are running low on supplies" }
)

# ============================================================
# Prompt template (same as translate.cpp)
# ============================================================

$TargetLang = "Japanese"
function Build-Prompt([string]$text) {
    return "You are a translator. The user sends a chat message in any language. Translate it to $TargetLang. Output ONLY the translated text, nothing else. No explanations. If the message is already in $TargetLang, output it unchanged.`n`n$text"
}

# ============================================================
# Benchmark pattern definitions
# ============================================================

$PhysicalCores = (Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfCores -Sum).Sum

$Patterns = @(
    @{ Name = "4b-Baseline";        Model = "gemma3:4b";   NumCtx = 256; NumThread = 2 },
    @{ Name = "4b-Thread4";         Model = "gemma3:4b";   NumCtx = 256; NumThread = 4 },
    @{ Name = "4b-ThreadMax";       Model = "gemma3:4b";   NumCtx = 256; NumThread = $PhysicalCores },
    @{ Name = "4b-ThreadAuto";      Model = "gemma3:4b";   NumCtx = 256; NumThread = 0 },
    @{ Name = "4b-Ctx128";          Model = "gemma3:4b";   NumCtx = 128; NumThread = 4 },
    @{ Name = "4b-Ctx512";          Model = "gemma3:4b";   NumCtx = 512; NumThread = 4 },
    @{ Name = "4b-MinCtx+MaxThread"; Model = "gemma3:4b";  NumCtx = 128; NumThread = $PhysicalCores },
    @{ Name = "1b-Thread4";         Model = "gemma3:1b";   NumCtx = 256; NumThread = 4 },
    @{ Name = "1b-ThreadAuto";      Model = "gemma3:1b";   NumCtx = 256; NumThread = 0 },
    @{ Name = "1b-ThreadMax";       Model = "gemma3:1b";   NumCtx = 256; NumThread = $PhysicalCores },
    @{ Name = "270m-Thread4";       Model = "gemma3:270m"; NumCtx = 256; NumThread = 4 },
    @{ Name = "270m-ThreadAuto";    Model = "gemma3:270m"; NumCtx = 256; NumThread = 0 },
    @{ Name = "270m-ThreadMax";     Model = "gemma3:270m"; NumCtx = 256; NumThread = $PhysicalCores }
)

$RunsPerPattern = 3

# ============================================================
# Ollama startup check
# ============================================================

function Test-OllamaRunning {
    try {
        $null = Invoke-RestMethod -Uri "$OllamaUrl/api/version" -TimeoutSec 3
        return $true
    } catch {
        return $false
    }
}

function Start-BundledOllama {
    $scriptDir = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $scriptDir "tools\ollama\ollama.exe"),
        (Join-Path $PSScriptRoot "..\tools\ollama\ollama.exe")
    )
    $ollamaExe = $null
    foreach ($c in $candidates) {
        if (Test-Path $c) { $ollamaExe = (Resolve-Path $c).Path; break }
    }
    if (-not $ollamaExe) {
        Write-Error "Ollama is not running. Please start 'ollama serve' manually."
        exit 1
    }

    $ollamaDir = Split-Path -Parent $ollamaExe
    $modelsDir = Join-Path $ollamaDir "models"
    if (-not (Test-Path $modelsDir)) { New-Item -ItemType Directory -Path $modelsDir | Out-Null }
    $env:OLLAMA_MODELS = $modelsDir

    Write-Host "[Benchmark] Starting ollama serve: $ollamaExe" -ForegroundColor Yellow
    Start-Process -FilePath $ollamaExe -ArgumentList "serve" -WindowStyle Hidden

    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-OllamaRunning) {
            $elapsed = [int](($i + 1) * 0.5)
            Write-Host "[Benchmark] Ollama ready (${elapsed}s)" -ForegroundColor Green
            return
        }
    }
    Write-Error "Ollama startup timeout (30s)"
    exit 1
}

# ============================================================
# Model download check
# ============================================================

function Test-ModelAvailable([string]$modelName) {
    try {
        $body = @{ name = $modelName } | ConvertTo-Json
        $null = Invoke-RestMethod -Uri "$OllamaUrl/api/show" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 10
        return $true
    } catch {
        Write-Host "[Benchmark] Model '$modelName' not found. Downloading..." -ForegroundColor Yellow
        try {
            $body = @{ name = $modelName; stream = $false } | ConvertTo-Json
            $null = Invoke-RestMethod -Uri "$OllamaUrl/api/pull" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 1800
            Write-Host "[Benchmark] Model '$modelName' downloaded" -ForegroundColor Green
            return $true
        } catch {
            Write-Error "Failed to download model '${modelName}': $_"
            return $false
        }
    }
}

# ============================================================
# Translation request
# ============================================================

function Invoke-Translation([string]$model, [int]$numCtx, [int]$numThread, [string]$text) {
    $prompt = Build-Prompt $text
    $body = @{
        model   = $model
        prompt  = $prompt
        stream  = $false
        options = @{
            num_ctx    = $numCtx
            num_thread = $numThread
        }
    } | ConvertTo-Json -Depth 3

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $response = Invoke-RestMethod -Uri "$OllamaUrl/api/generate" -Method Post -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) -ContentType "application/json; charset=utf-8" -TimeoutSec 300
        $sw.Stop()

        $evalCount    = if ($response.eval_count)    { $response.eval_count }    else { 0 }
        $evalDuration = if ($response.eval_duration)  { $response.eval_duration }  else { 1 }
        $tokensPerSec = if ($evalDuration -gt 0) { [math]::Round($evalCount / ($evalDuration / 1e9), 2) } else { 0 }

        return @{
            ElapsedMs    = [math]::Round($sw.Elapsed.TotalMilliseconds, 0)
            TokensPerSec = $tokensPerSec
            Response     = $response.response
            EvalCount    = $evalCount
            Success      = $true
        }
    } catch {
        $sw.Stop()
        return @{
            ElapsedMs    = [math]::Round($sw.Elapsed.TotalMilliseconds, 0)
            TokensPerSec = 0
            Response     = "ERROR: $_"
            EvalCount    = 0
            Success      = $false
        }
    }
}

# ============================================================
# Memory measurement
# ============================================================

function Get-OllamaMemoryMB {
    try {
        $procs = Get-Process -Name "ollama*" -ErrorAction SilentlyContinue
        if ($procs) {
            return [math]::Round(($procs | Measure-Object -Property WorkingSet64 -Sum).Sum / 1MB, 1)
        }
    } catch {}
    return 0
}

# ============================================================
# Main
# ============================================================

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Ollama Translation Benchmark - Stage 10" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# Ollama startup
if (-not (Test-OllamaRunning)) {
    Start-BundledOllama
}

# Environment info
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$gpu = (Get-CimInstance Win32_VideoController | Select-Object -First 1).Name
$ollamaVer = try { (Invoke-RestMethod -Uri "$OllamaUrl/api/version" -TimeoutSec 5).version } catch { "unknown" }

Write-Host "[Env] CPU: $cpu ($PhysicalCores cores)"
Write-Host "[Env] RAM: ${ram} GB"
Write-Host "[Env] GPU: $gpu"
Write-Host "[Env] Ollama: $ollamaVer"
Write-Host ""

# Model check
$requiredModels = $Patterns | ForEach-Object { $_.Model } | Select-Object -Unique
foreach ($m in $requiredModels) {
    if (-not (Test-ModelAvailable $m)) {
        Write-Error "Required model not available: $m"
        exit 1
    }
}
Write-Host ""

# Show test sentences
Write-Host "[Test sentences]" -ForegroundColor Yellow
foreach ($s in $TestSentences) {
    Write-Host "  $($s.Label) ($($s.Text.Length) chars): $($s.Text)"
}
Write-Host ""

# CSV header
$csvRows = [System.Collections.ArrayList]@()
$csvHeader = "Pattern,Model,NumCtx,NumThread,Sentence,SentenceLen,Run,ElapsedMs,TokensPerSec,EvalCount,MemoryMB,Translation"

$totalRequests = $Patterns.Count * $TestSentences.Count * $RunsPerPattern
$currentRequest = 0

Write-Host "[Start] $($Patterns.Count) patterns x $($TestSentences.Count) sentences x $RunsPerPattern runs = $totalRequests requests" -ForegroundColor Yellow
Write-Host ""

$overallSw = [System.Diagnostics.Stopwatch]::StartNew()

foreach ($pattern in $Patterns) {
    Write-Host "--- $($pattern.Name) (model=$($pattern.Model), ctx=$($pattern.NumCtx), thread=$($pattern.NumThread)) ---" -ForegroundColor Cyan

    foreach ($sentence in $TestSentences) {
        for ($run = 1; $run -le $RunsPerPattern; $run++) {
            $currentRequest++

            $result = Invoke-Translation -model $pattern.Model -numCtx $pattern.NumCtx -numThread $pattern.NumThread -text $sentence.Text

            $memAfter = Get-OllamaMemoryMB
            $runType = if ($run -eq 1) { "cold" } else { "warm" }
            $statusIcon = if ($result.Success) { "[OK]" } else { "[NG]" }

            # Console output
            $translationPreview = $result.Response
            if ($translationPreview.Length -gt 40) {
                $translationPreview = $translationPreview.Substring(0, 40) + "..."
            }
            Write-Host ("  {0} {1} run{2}({3}): {4}ms, {5} tok/s, mem={6}MB" -f $statusIcon, $sentence.Label, $run, $runType, $result.ElapsedMs, $result.TokensPerSec, $memAfter)

            # CSV row
            $escapedTranslation = $result.Response -replace [char]34, ([char]34 + [char]34)
            $null = $csvRows.Add("$($pattern.Name),$($pattern.Model),$($pattern.NumCtx),$($pattern.NumThread),$($sentence.Label),$($sentence.Text.Length),$run,$($result.ElapsedMs),$($result.TokensPerSec),$($result.EvalCount),$memAfter,`"$escapedTranslation`"")

            Write-Progress -Activity "Benchmark" -Status "$currentRequest / $totalRequests" -PercentComplete ([math]::Round($currentRequest / $totalRequests * 100))
        }
    }
    Write-Host ""
}

$overallSw.Stop()
Write-Progress -Activity "Benchmark" -Completed

# ============================================================
# CSV output
# ============================================================

$csvContent = $csvHeader + "`n" + ($csvRows -join "`n")
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $OutputCsv), $csvContent, [System.Text.UTF8Encoding]::new($true))
Write-Host "[Done] CSV: $OutputCsv" -ForegroundColor Green

# ============================================================
# Summary (warm cache = run 2,3 average)
# ============================================================

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Summary (warm cache = avg of run 2,3)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$data = $csvRows | ForEach-Object {
    $fields = $_ -split ","
    [PSCustomObject]@{
        Pattern      = $fields[0]
        Sentence     = $fields[4]
        Run          = [int]$fields[6]
        ElapsedMs    = [double]$fields[7]
        TokensPerSec = [double]$fields[8]
        MemoryMB     = [double]$fields[10]
    }
}

Write-Host ""
Write-Host ("{0,-25} {1,10} {2,10} {3,10} {4,10} {5,10}" -f "Pattern", "Short(ms)", "Med(ms)", "Long(ms)", "tok/s", "Mem(MB)")
Write-Host ("{0,-25} {1,10} {2,10} {3,10} {4,10} {5,10}" -f ("-" * 25), ("-" * 10), ("-" * 10), ("-" * 10), ("-" * 10), ("-" * 10))

foreach ($patternName in ($Patterns | ForEach-Object { $_.Name })) {
    $warmData = $data | Where-Object { $_.Pattern -eq $patternName -and $_.Run -ge 2 }

    $shortMs = ($warmData | Where-Object { $_.Sentence -eq "Short" } | Measure-Object -Property ElapsedMs -Average).Average
    $medMs   = ($warmData | Where-Object { $_.Sentence -eq "Medium" } | Measure-Object -Property ElapsedMs -Average).Average
    $longMs  = ($warmData | Where-Object { $_.Sentence -eq "Long" } | Measure-Object -Property ElapsedMs -Average).Average
    $avgTokS = ($warmData | Measure-Object -Property TokensPerSec -Average).Average
    $avgMem  = ($warmData | Measure-Object -Property MemoryMB -Average).Average

    Write-Host ("{0,-25} {1,10:N0} {2,10:N0} {3,10:N0} {4,10:N1} {5,10:N1}" -f $patternName, $shortMs, $medMs, $longMs, $avgTokS, $avgMem)
}

Write-Host ""
Write-Host "[Total time] $([math]::Round($overallSw.Elapsed.TotalMinutes, 1)) min" -ForegroundColor Green