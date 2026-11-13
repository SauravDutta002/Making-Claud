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
    # Windows PowerShell 5.1 turns a program's error-stream text into a stop. uv says "already in PATH"
    # (on stderr) when you update, so run such commands where only a real failure counts.
    function Quiet { $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
                     try { & @args *> $null } catch { } finally { $ErrorActionPreference = $old } }

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
    # (and pause its task, which would otherwise start it again within a minute)
    function Stop-Buddy {
        try { Disable-ScheduledTask -TaskName "Claude Buddy" -ErrorAction Stop | Out-Null } catch { }
        Stop-ScheduledTask -TaskName "Claude Buddy" -ErrorAction SilentlyContinue
        Get-Process claude-buddy, claude-buddy-service -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Get-CimInstance Win32_Process -Filter "Name like 'python%'" |
            Where-Object { $_.CommandLine -match 'claude_bridge|claude_buddy\.(claude_bridge|service)' } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    Stop-Buddy

    # 2. the claude-buddy commands (Python 3.12 comes with them)
    Step "Installing Claude Buddy"
    & $uv tool install --force --reinstall --python 3.12 $Pkg
    if ($LASTEXITCODE -ne 0) { throw "install failed - see the messages above." }
    $bin = (& $uv tool dir --bin).Trim()
    Quiet $uv tool update-shell                                 # claude-buddy on your PATH
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
            Stop-Buddy; Start-Sleep -Seconds 1                   # make sure nothing holds the port
            & "$bin\claude-buddy-flash.exe" --port $port
            if ($LASTEXITCODE -ne 0) { Note "flashing didn't work - replug the board and run this again" }
        }
    } else {
        Note "no board on USB - that's fine: power it and the bridge finds it over USB or Bluetooth."
        Note "to update its firmware later: plug it in over USB and run  claude-buddy-flash"
    }

    # 5. run it as a background task: started at login, and started again within a minute
    #    if it ever stops. No window, no admin rights needed.
    Step "Starting"
    $exe = "$bin\claude-buddy-service.exe"
    $shell = New-Object -ComObject WScript.Shell
    $startup = Join-Path ([Environment]::GetFolderPath("Startup")) "Claude Buddy.lnk"
    $lnk = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Programs")) "Claude Buddy.lnk"))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $env:USERPROFILE
    $lnk.Description = "Claude Buddy (runs in the background)"
    $lnk.Save()
    $url = Join-Path ([Environment]::GetFolderPath("Programs")) "Claude Buddy Settings.url"
    Set-Content -Path $url -Value "[InternetShortcut]`r`nURL=http://localhost:47801/`r`n" -Encoding ASCII

    # claude-buddy:// - lets the settings page start it with one click if it's ever off
    $proto = "HKCU:\Software\Classes\claude-buddy"
    New-Item -Path "$proto\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path $proto -Name "(Default)" -Value "URL:Claude Buddy"
    Set-ItemProperty -Path $proto -Name "URL Protocol" -Value ""
    Set-ItemProperty -Path "$proto\shell\open\command" -Name "(Default)" -Value "`"$exe`""

    $task = $false
    try {
        $me = "$env:USERDOMAIN\$env:USERNAME"
        $triggers = @(
            (New-ScheduledTaskTrigger -AtLogOn -User $me),
            (New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 1))
        )
        $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
                 -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew `
                 -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
        Register-ScheduledTask -TaskName "Claude Buddy" -Force -Action (New-ScheduledTaskAction -Execute $exe) `
            -Trigger $triggers -Settings $set -Description "Claude Buddy: the little screen that reacts to Claude Code" `
            -Principal (New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited) | Out-Null
        Start-ScheduledTask -TaskName "Claude Buddy"
        Remove-Item $startup -Force -ErrorAction SilentlyContinue      # the task starts it at login now
        $task = $true
    } catch {
        Note "couldn't create the background task ($($_.Exception.Message)) - using a startup shortcut instead"
    }
    if (-not $task) {
        $lnk = $shell.CreateShortcut($startup)
        $lnk.TargetPath = $exe
        $lnk.WorkingDirectory = $env:USERPROFILE
        $lnk.Save()
        Start-Process -FilePath $exe
    }
    Done "Claude Buddy is running in the background: it starts when you log in, and again by itself if it ever stops"
    Done "status and settings: http://localhost:47801  (Start menu > Claude Buddy Settings)"

    # say hello in the browser: the welcome page (and its "install the app" offer).
    # Give Claude Buddy a moment to come up; the page also waits for it by itself.
    Note "opening the welcome page..."
    for ($i = 0; $i -lt 40; $i++) {
        try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:47801/" | Out-Null; break }
        catch { Start-Sleep -Seconds 1 }
    }
    try { Start-Process "http://localhost:47801/?welcome"; Done "opened the welcome page in your browser" }
    catch { Note "open http://localhost:47801/?welcome in your browser" }

    Write-Host ""
    Write-Host "  All set. Restart Claude Code and watch the screen." -ForegroundColor DarkYellow
    Write-Host ""
}

try { Install-ClaudeBuddy }
catch { Write-Host ""; Write-Host "  Setup stopped: $($_.Exception.Message)" -ForegroundColor Red }
