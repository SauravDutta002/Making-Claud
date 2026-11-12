# Claude Buddy - remove what install.ps1 set up (your stats and streaks are kept)
#
#   irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.ps1 | iex

function Uninstall-ClaudeBuddy {
    $ErrorActionPreference = "Stop"
    # Windows PowerShell 5.1 turns a program's error-stream text into a stop. uv says "already in PATH"
    # (on stderr) when you update, so run such commands where only a real failure counts.
    function Quiet { $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
                     try { & @args *> $null } catch { } finally { $ErrorActionPreference = $old } }
    # the background task first, or its every-minute check would start Claude Buddy again
    Stop-ScheduledTask -TaskName "Claude Buddy" -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName "Claude Buddy" -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item "HKCU:\Software\Classes\claude-buddy" -Recurse -Force -ErrorAction SilentlyContinue
    Get-Process claude-buddy, claude-buddy-service -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
        Where-Object { $_.CommandLine -match 'claude_bridge' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

    $uv = (Get-Command uv -ErrorAction SilentlyContinue).Source
    if (-not $uv -and (Test-Path "$env:USERPROFILE\.local\bin\uv.exe")) { $uv = "$env:USERPROFILE\.local\bin\uv.exe" }
    if ($uv) {
        $bin = (& $uv tool dir --bin).Trim()
        if (Test-Path "$bin\claude-buddy-setup.exe") { & "$bin\claude-buddy-setup.exe" --remove }   # hooks out
        Quiet $uv tool uninstall claude-buddy
    }
    foreach ($place in @([Environment]::GetFolderPath("Startup"), [Environment]::GetFolderPath("Programs"))) {
        Remove-Item (Join-Path $place "Claude Buddy.lnk") -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $place "Claude Buddy Settings.url") -Force -ErrorAction SilentlyContinue
    }
    Write-Host "  Claude Buddy removed. The board keeps its firmware and lives on its own." -ForegroundColor DarkYellow
    Write-Host "  (your stats and streaks are still in $env:LOCALAPPDATA\ClaudeBuddy - delete it to forget them)" -ForegroundColor DarkGray
}

try { Uninstall-ClaudeBuddy }
catch { Write-Host "  Uninstall stopped: $($_.Exception.Message)" -ForegroundColor Red }
