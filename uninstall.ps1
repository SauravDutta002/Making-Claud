# Claude Buddy - remove what install.ps1 set up (your stats and streaks are kept)
#
#   irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.ps1 | iex

function Uninstall-ClaudeBuddy {
    $ErrorActionPreference = "Stop"
    Get-Process claude-buddy -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $uv = (Get-Command uv -ErrorAction SilentlyContinue).Source
    if (-not $uv -and (Test-Path "$env:USERPROFILE\.local\bin\uv.exe")) { $uv = "$env:USERPROFILE\.local\bin\uv.exe" }
    if ($uv) {
        $bin = (& $uv tool dir --bin).Trim()
        if (Test-Path "$bin\claude-buddy-setup.exe") { & "$bin\claude-buddy-setup.exe" --remove }   # hooks out
        & $uv tool uninstall claude-buddy 2>$null | Out-Null
    }
    foreach ($place in @([Environment]::GetFolderPath("Startup"), [Environment]::GetFolderPath("Programs"))) {
        Remove-Item (Join-Path $place "Claude Buddy.lnk") -Force -ErrorAction SilentlyContinue
    }
    Write-Host "  Claude Buddy removed. The board keeps its firmware and lives on its own." -ForegroundColor DarkYellow
    Write-Host "  (your stats and streaks are still in $env:LOCALAPPDATA\ClaudeBuddy - delete it to forget them)" -ForegroundColor DarkGray
}

try { Uninstall-ClaudeBuddy }
catch { Write-Host "  Uninstall stopped: $($_.Exception.Message)" -ForegroundColor Red }
