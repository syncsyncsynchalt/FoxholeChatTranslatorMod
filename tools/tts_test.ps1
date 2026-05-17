$ErrorActionPreference = "Stop"

# Python 3.12 を優先
$py312 = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
if (Test-Path $py312) { $python = $py312 } else { $python = "python" }

Write-Host "Python: $python"
Write-Host "依存パッケージを確認中..."

# 不足パッケージを自動インストール
$packages = @("sherpa-onnx", "sounddevice", "numpy")
foreach ($pkg in $packages) {
    $mod = $pkg -replace "-", "_"
    $check = & $python -c "import $mod; print('ok')" 2>$null
    if ($check -ne "ok") {
        Write-Host "インストール中: $pkg ..."
        & $python -m pip install $pkg --quiet
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ERROR] $pkg のインストールに失敗しました" -ForegroundColor Red
            Read-Host "Enterを押して終了"
            exit 1
        }
        Write-Host "  OK: $pkg"
    } else {
        Write-Host "  OK (インストール済み): $pkg"
    }
}

Write-Host "全パッケージOK — TTS テストツールを起動します`n"
$script = Join-Path $PSScriptRoot "tts_test.py"
& $python $script