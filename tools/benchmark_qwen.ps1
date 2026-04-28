# qwen2.5:3b パラメータベンチマーク
# 使い方: .\benchmark_qwen.ps1
# 前提: ollama serve が起動済み、qwen2.5:3b がインストール済み

$endpoint = "http://localhost:11434/api/generate"
$model    = "qwen2.5:3b"

# テスト文 (Foxholeのチャットを想定: 英・露・韓・中)
$testMessages = @(
    "Enemy tanks spotted at grid F5, need artillery support now!",
    "Противник прорвал линию обороны, отступаем на запасные позиции.",
    "보급품이 부족합니다. 탄약과 의료품을 요청합니다.",
    "我们需要更多的工程师来修复这座桥。",
    "All squads fall back to the rally point immediately!"
)

$paramSets = @(
    @{ num_ctx = 128; num_thread = 2 },
    @{ num_ctx = 128; num_thread = 4 },
    @{ num_ctx = 256; num_thread = 2 },
    @{ num_ctx = 256; num_thread = 4 },
    @{ num_ctx = 256; num_thread = 0 },
    @{ num_ctx = 512; num_thread = 4 },
    @{ num_ctx = 512; num_thread = 0 }
)

function Translate($text, $num_ctx, $num_thread) {
    $prompt = "You are a translator. Translate to Japanese. Output ONLY the translated text.`n`n$text"
    $body = @{
        model   = $model
        prompt  = $prompt
        stream  = $false
        options = @{ num_ctx = $num_ctx; num_thread = $num_thread; num_predict = 80 }
    } | ConvertTo-Json -Compress

    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $resp = Invoke-RestMethod -Uri $endpoint -Method POST -Body $body `
                    -ContentType "application/json" -TimeoutSec 60
        $sw.Stop()
        return @{
            ok       = $true
            wallMs   = $sw.ElapsedMilliseconds
            totalMs  = [int]($resp.total_duration / 1e6)
            evalMs   = [int]($resp.eval_duration  / 1e6)
            tokens   = $resp.eval_count
            result   = $resp.response
        }
    } catch {
        $sw.Stop()
        return @{ ok = $false; wallMs = $sw.ElapsedMilliseconds }
    }
}

Write-Host "=== qwen2.5:3b ベンチマーク ===" -ForegroundColor Cyan
Write-Host "テスト文数: $($testMessages.Count)"
Write-Host ""

$results = @()

foreach ($p in $paramSets) {
    $label = "num_ctx=$($p.num_ctx) num_thread=$($p.num_thread)"
    Write-Host "--- $label ---" -ForegroundColor Yellow

    $totalMs = 0
    $success = 0

    foreach ($msg in $testMessages) {
        $r = Translate $msg $p.num_ctx $p.num_thread
        if ($r.ok) {
            $success++
            $totalMs += $r.wallMs
            Write-Host "  [$($r.wallMs)ms] $($r.result.Substring(0, [Math]::Min(40,$r.result.Length)))..."
        } else {
            Write-Host "  [FAIL]" -ForegroundColor Red
        }
    }

    if ($success -gt 0) {
        $avgMs = [int]($totalMs / $success)
        Write-Host "  平均: ${avgMs}ms (成功: $success/$($testMessages.Count))" -ForegroundColor Green
        $results += [PSCustomObject]@{
            Params   = $label
            AvgMs    = $avgMs
            Success  = "$success/$($testMessages.Count)"
        }
    }
    Write-Host ""
    Start-Sleep -Seconds 1
}

Write-Host "=== 結果サマリー ===" -ForegroundColor Cyan
$results | Sort-Object AvgMs | Format-Table -AutoSize

$best = $results | Sort-Object AvgMs | Select-Object -First 1
Write-Host "最速設定: $($best.Params) (平均 $($best.AvgMs)ms)" -ForegroundColor Green
