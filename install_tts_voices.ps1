# TTS voice installation script - Standard + Natural/Neural voices

Write-Host "=== Searching ALL speech-related packages ===" -ForegroundColor Cyan

$allCaps = Get-WindowsCapability -Online | Where-Object {
    $_.Name -like "*Speech*" -or
    $_.Name -like "*TTS*"    -or
    $_.Name -like "*Natural*"-or
    $_.Name -like "*Narrator*"
}

Write-Host "Found $($allCaps.Count) packages:" -ForegroundColor Cyan
$allCaps | ForEach-Object { Write-Host "  $($_.Name) [$($_.State)]" }
Write-Host ""

$targets = @("ja-JP", "en-US", "ru-RU", "zh-CN", "ko-KR")

foreach ($cap in $allCaps) {
    $isTarget = $false
    foreach ($lang in $targets) {
        if ($cap.Name -like "*$lang*") { $isTarget = $true; break }
    }
    # Natural/Narrator entries without explicit lang tag are also included
    if ($cap.Name -like "*Natural*") { $isTarget = $true }

    if ($isTarget -and $cap.State -ne "Installed") {
        Write-Host "Installing: $($cap.Name)" -ForegroundColor Yellow
        Add-WindowsCapability -Online -Name $cap.Name
        Write-Host "  Done" -ForegroundColor Green
    } elseif ($isTarget) {
        Write-Host "Already installed: $($cap.Name)" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "--- Installed voices (System.Speech) ---" -ForegroundColor Cyan
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.GetInstalledVoices() | ForEach-Object {
    $info = $_.VoiceInfo
    Write-Host "  $($info.Name) ($($info.Culture))"
}

Write-Host ""
Write-Host "If Natural voices still not found, install manually:" -ForegroundColor Yellow
Write-Host "  Settings > Time & Language > Speech > Add voices" -ForegroundColor Yellow
Write-Host "  (look for voices with 'Natural' in the name)" -ForegroundColor Yellow
Write-Host ""

# Open Settings speech page
$open = Read-Host "Open Settings speech page now? (y/n)"
if ($open -eq "y") {
    Start-Process "ms-settings:speech"
}

Read-Host "Press Enter to close"
