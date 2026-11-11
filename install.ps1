# Claude Buddy - one-command setup for Windows
#
#   irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.ps1 | iex
#
# Installs uv (which brings its own Python), the claude-buddy commands, connects
# Claude Code, flashes the board if it's plugged in over USB, and starts Claude
# Buddy in the background now and at every login. Run it again any time to update.
# Uninstall: irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.ps1 | iex

function Install-ClaudeBuddy {
    $ErrorActionPreference = "Stop"
    $ProgressPreference = "SilentlyContinue"
    $Repo = "SauravDutta002/Making-Claud"
    $Pkg  = "claude-buddy @ https://github.com/$Repo/archive/refs/heads/main.zip"

    function Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Yellow }
    function Done($text) { Write-Host "    $text" -ForegroundColor Green }
    function Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }

    Write-Host ""
    Write-Host "  * Claude Buddy" -ForegroundColor DarkYellow
    Write-Host "    the little screen that reacts to Claude Code" -ForegroundColor DarkGray

    # 1. uv: installs Python and the bridge in their own place
    Step "uv"
    $uv = (Get-Command uv -ErrorAction SilentlyContinue).Source
    if (-not $uv) {
        Note "installing uv (astral.sh)..."
        Invoke-RestMethod https://astral.sh/uv/install.ps1 | Invoke-Expression
        foreach ($c in @("$env:USERPROFILE\.local\bin\uv.exe", "$env:USERPROFILE\.cargo\bin\uv.exe")) {
            if (Test-Path $c) { $uv = $c; break }
        }
        if (-not $uv) { throw "uv didn't install - see https://docs.astral.sh/uv/ and run this again." }
    }
    Done (& $uv --version)

    # a running bridge holds the board's port and its own files: stop it while we update
    Get-Process claude-buddy, claude-buddy-service -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
        Where-Object { $_.CommandLine -match 'claude_bridge' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

    # 2. the claude-buddy commands (Python 3.12 comes with them)
    Step "Installing Claude Buddy"
    & $uv tool install --force --reinstall --python 3.12 $Pkg
    if ($LASTEXITCODE -ne 0) { throw "install failed - see the messages above." }
    $bin = (& $uv tool dir --bin).Trim()
    & $uv tool update-shell 2>$null | Out-Null                  # claude-buddy on your PATH
    Done "installed: $bin\claude-buddy.exe"

    # 3. Claude Code: hooks + status line (your settings are backed up first)
    Step "Connecting Claude Code"
    & "$bin\claude-buddy-setup.exe"

    # 4. the board: flash the latest firmware if it's plugged in over USB
    Step "The board"
    $port = (& "$bin\claude-buddy-flash.exe" --check | Out-String).Trim()
    if ($port) {
        $answer = Read-Host "    Found the board on $port. Install the latest firmware on it? [Y/n]"
        if ($answer -notmatch '^[nN]') {
            & "$bin\claude-buddy-flash.exe" --port $port
            if ($LASTEXITCODE -ne 0) { Note "flashing didn't work - replug the board and run this again" }
        }
    } else {
        Note "no board on USB - that's fine: power it and the bridge finds it over USB or Bluetooth."
        Note "to update its firmware later: plug it in over USB and run  claude-buddy-flash"
    }

    # 5. start now and at every login: in the background, no window, restarting itself if it stops
    Step "Starting"
    $exe = "$bin\claude-buddy-service.exe"
    $shell = New-Object -ComObject WScript.Shell
    foreach ($place in @([Environment]::GetFolderPath("Startup"), [Environment]::GetFolderPath("Programs"))) {
        $lnk = $shell.CreateShortcut((Join-Path $place "Claude Buddy.lnk"))
        $lnk.TargetPath = $exe
        $lnk.WorkingDirectory = $env:USERPROFILE
        $lnk.Description = "Claude Buddy (runs in the background)"
        $lnk.Save()
    }
    $url = Join-Path ([Environment]::GetFolderPath("Programs")) "Claude Buddy Settings.url"
    Set-Content -Path $url -Value "[InternetShortcut]`r`nURL=http://localhost:47801/`r`n" -Encoding ASCII
    Start-Process -FilePath $exe
    Done "Claude Buddy is running in the background, and starts by itself when you log in"
    Done "status and settings: http://localhost:47801  (Start menu > Claude Buddy Settings)"

    Write-Host ""
    Write-Host "  All set. Restart Claude Code and watch the screen." -ForegroundColor DarkYellow
    Write-Host ""
}

try { Install-ClaudeBuddy }
catch { Write-Host ""; Write-Host "  Setup stopped: $($_.Exception.Message)" -ForegroundColor Red }
