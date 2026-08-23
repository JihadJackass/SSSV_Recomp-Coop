# SSSV Co-op live log viewer: color-coded tail of coop_log.txt.
# Run watch-log.bat, or: powershell -ExecutionPolicy Bypass -File watch-log.ps1
param([string]$LogPath = "$env:LOCALAPPDATA\SSSVRecompiled\coop_log.txt")

if (-not (Test-Path $LogPath)) {
    Write-Host "Waiting for log file to appear at $LogPath ..." -ForegroundColor DarkGray
    while (-not (Test-Path $LogPath)) { Start-Sleep -Milliseconds 500 }
}
Write-Host "== SSSV Co-op live log ==  ($LogPath)" -ForegroundColor White
Write-Host "Ctrl+C to stop" -ForegroundColor DarkGray

Get-Content -Path $LogPath -Wait -Tail 40 | ForEach-Object {
    $line = $_
    $color = "Gray"
    if     ($line -match "ERROR")        { $color = "Red" }
    elseif ($line -match "\| SYS")       { $color = "DarkGray" }
    elseif ($line -match "\| NET")       { if ($line -match "CONNECTED|connected") { $color = "Green" } else { $color = "Cyan" } }
    elseif ($line -match "\| GHOST")     { if ($line -match "killed|corpse") { $color = "DarkYellow" } else { $color = "Yellow" } }
    elseif ($line -match "\| POSE")      { $color = "Magenta" }
    elseif ($line -match "^=+$")         { $color = "White" }
    Write-Host $line -ForegroundColor $color
}
