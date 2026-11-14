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
    # Opened through a small local "starting..." page that waits for Claude Buddy's
    # server and then switches to the welcome, so a slow first start never shows
    # the browser's "can't connect" page.
    $hello = Join-Path $env:TEMP "claude-buddy-starting.html"
    try {
        Set-Content -Path $hello -Encoding UTF8 -Value @'
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Starting Claude Buddy</title>
<style>
html,body{margin:0;height:100%;background:#ece9e2;color:#1b1a17;font:16px/1.5 "Segoe UI",system-ui,-apple-system,sans-serif}
body{display:grid;place-items:center;background:radial-gradient(rgba(27,26,23,.13) 1px,transparent 1.4px) 0 0/22px 22px,#ece9e2}
main{text-align:center;padding:16px}
svg{width:150px;animation:bob 1.1s ease-in-out infinite alternate}
@keyframes bob{to{transform:translateY(-6px)}}
h1{margin:22px 0 4px;font-size:26px;letter-spacing:-.02em}
p{margin:0;color:#48453f}
i{display:inline-block;width:7px;height:7px;margin:14px 3px 0;border-radius:50%;background:#e0532c;animation:d 1s ease-in-out infinite}
i:nth-child(2){animation-delay:.15s}i:nth-child(3){animation-delay:.3s}
@keyframes d{50%{opacity:.25;transform:translateY(-4px)}}
#slow{display:none;margin-top:18px;font-size:14px;color:#7a766d}
@media (prefers-reduced-motion:reduce){svg,i{animation:none}}
</style></head><body><main>
<svg viewBox="0 0 16 12" shape-rendering="crispEdges" aria-hidden="true"><path d="M3 4h10v1h-10zM3 5h2v1h-2zM6 5h4v1h-4zM11 5h2v1h-2zM3 6h2v1h-2zM6 6h4v1h-4zM11 6h2v1h-2zM0 7h16v1h-16zM0 8h16v1h-16zM3 9h10v1h-10zM4 10h1v1h-1zM6 10h1v1h-1zM9 10h1v1h-1zM11 10h1v1h-1zM4 11h1v1h-1zM6 11h1v1h-1zM9 11h1v1h-1zM11 11h1v1h-1z" fill="#e0532c"/><path d="M5 0h6v1h-6zM3 1h2v1h-2zM11 1h2v1h-2zM2 2h1v1h-1zM13 2h1v1h-1zM1 3h1v1h-1zM14 3h1v1h-1zM1 4h2v1h-2zM13 4h2v1h-2zM1 5h2v1h-2zM13 5h2v1h-2zM1 6h2v1h-2zM13 6h2v1h-2z" fill="#1b1a17"/></svg>
<h1>Starting Claude Buddy</h1><p>Getting everything ready, this takes a few seconds.</p>
<div aria-hidden="true"><i></i><i></i><i></i></div>
<p id="slow">Taking a little longer than usual. It starts by itself, so keep this tab open.<br>Or open Start menu &gt; Claude Buddy Settings.</p>
</main><script>
var t0=Date.now();
function probe(){var img=new Image();
img.onload=function(){location.replace("http://localhost:47801/?welcome")};
img.onerror=function(){if(Date.now()-t0>45000)document.getElementById("slow").style.display="block";setTimeout(probe,1000)};
img.src="http://localhost:47801/icon-192.png?t="+Date.now()}
probe();
</script></body></html>
'@
        Start-Process $hello
        Done "opened the welcome page in your browser"
    } catch {
        Note "open http://localhost:47801/?welcome in your browser"
    }

    Write-Host ""
    Write-Host "  All set. Restart Claude Code and watch the screen." -ForegroundColor DarkYellow
    Write-Host ""
}

try { Install-ClaudeBuddy }
catch { Write-Host ""; Write-Host "  Setup stopped: $($_.Exception.Message)" -ForegroundColor Red }
