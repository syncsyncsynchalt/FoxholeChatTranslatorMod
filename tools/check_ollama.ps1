$max=60
for ($i=0; $i -lt $max; $i++) {
    try {
        $r = Invoke-RestMethod -Uri 'http://127.0.0.1:11434/api/version' -Method Get -TimeoutSec 3 -ErrorAction Stop
        Write-Output "Ollama /api/version OK:"
        $r | ConvertTo-Json -Compress | Write-Output
        exit 0
    } catch {
        Write-Output ("Attempt {0}: not ready" -f ($i+1))
        Start-Sleep -Seconds 2
    }
}
Write-Output "Timeout waiting for Ollama /api/version"
exit 2
