#!/bin/sh
# Claude Buddy - one-command setup for macOS (and Linux)
#
#   curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.sh | sh
#
# Installs uv (which brings its own Python), the claude-buddy commands, connects
# Claude Code, flashes the board if it's plugged in over USB, and starts the
# bridge now and at every login. Run it again any time to update.
# Uninstall: curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.sh | sh
set -eu

REPO="SauravDutta002/Making-Claud"
PKG="claude-buddy @ https://github.com/$REPO/archive/refs/heads/main.zip"

step() { printf '\n\033[33m==> %s\033[0m\n' "$1"; }
done_() { printf '    \033[32m%s\033[0m\n' "$1"; }
note() { printf '    \033[90m%s\033[0m\n' "$1"; }

printf '\n  \033[38;5;209m* Claude Buddy\033[0m\n    \033[90mthe little screen that reacts to Claude Code\033[0m\n'

# 1. uv: installs Python and the bridge in their own place
step "uv"
if ! command -v uv >/dev/null 2>&1; then
  note "installing uv (astral.sh)..."
  curl -LsSf https://astral.sh/uv/install.sh | sh
fi
export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
UV=$(command -v uv) || { echo "uv didn't install - see https://docs.astral.sh/uv/"; exit 1; }
done_ "$("$UV" --version)"

# a running bridge holds the board's port: stop it while we update
systemctl --user stop claude-buddy.service 2>/dev/null || true
pkill -f 'bin/claude-buddy' 2>/dev/null || true

# 2. the claude-buddy commands (Python 3.12 comes with them)
step "Installing Claude Buddy"
"$UV" tool install --force --reinstall --python 3.12 "$PKG"
BIN=$("$UV" tool dir --bin)
"$UV" tool update-shell >/dev/null 2>&1 || true                # claude-buddy on your PATH
done_ "installed: $BIN/claude-buddy"

# 3. Claude Code: hooks + status line (your settings are backed up first)
step "Connecting Claude Code"
"$BIN/claude-buddy-setup"

# 4. the board: flash the latest firmware if it's plugged in over USB
step "The board"
PORT=$("$BIN/claude-buddy-flash" --check 2>/dev/null || true)
if [ -n "$PORT" ]; then
  printf '    Found the board on %s. Install the latest firmware on it? [Y/n] ' "$PORT"
  ans=y
  read -r ans </dev/tty 2>/dev/null || ans=y
  case "$ans" in
    [nN]*) note "skipped" ;;
    *) "$BIN/claude-buddy-flash" --port "$PORT" || note "flashing didn't work - replug the board and run this again" ;;
  esac
else
  note "no board on USB - that's fine: power it and the bridge finds it over USB or Bluetooth."
  note "to update its firmware later: plug it in over USB and run  claude-buddy-flash"
fi

# 5. start now and at every login
step "Starting"
if [ "$(uname -s)" = "Darwin" ]; then
  # a Terminal window with the live dashboard; Terminal also gets the Bluetooth permission
  mkdir -p "$HOME/Applications"
  CMD="$HOME/Applications/Claude Buddy.command"
  printf '#!/bin/sh\nexec "%s/claude-buddy"\n' "$BIN" > "$CMD"
  chmod +x "$CMD"
  osascript -e 'tell application "System Events" to delete (every login item whose name is "Claude Buddy.command")' >/dev/null 2>&1 || true
  if osascript -e "tell application \"System Events\" to make login item at end with properties {path:\"$CMD\", hidden:false}" >/dev/null 2>&1; then
    done_ "it opens by itself when you log in"
  else
    note "couldn't add a login item - open \"$CMD\" to start it"
  fi
  open "$CMD"
  done_ "the bridge is running in a Terminal window (live dashboard) - or run: claude-buddy"
  note "allow Bluetooth for Terminal if macOS asks (for the wireless link)"
else
  mkdir -p "$HOME/.config/systemd/user"
  cat > "$HOME/.config/systemd/user/claude-buddy.service" <<EOF
[Unit]
Description=Claude Buddy bridge

[Service]
ExecStart=$BIN/claude-buddy --plain
Restart=on-failure

[Install]
WantedBy=default.target
EOF
  if systemctl --user daemon-reload 2>/dev/null && systemctl --user enable --now claude-buddy.service 2>/dev/null; then
    done_ "running as a user service (claude-buddy.service), and at every login"
  else
    note "start it with: claude-buddy"
  fi
  note "USB on Linux: you may need  sudo usermod -aG dialout \$USER  (then log out and in)"
fi

done_ "settings: http://localhost:47801  (or run: claude-buddy-settings)"

# say hello in the browser: a small local "starting..." page waits for Claude
# Buddy's server, then switches to the welcome page (no "can't connect" page)
HELLO="${TMPDIR:-/tmp}/claude-buddy-starting.html"
cat > "$HELLO" <<'HTMLEOF'
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
HTMLEOF
if [ "$(uname -s)" = "Darwin" ]; then open "$HELLO" 2>/dev/null || true
elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$HELLO" >/dev/null 2>&1 || true; fi

printf '\n  \033[38;5;209mAll set. Restart Claude Code and watch the screen.\033[0m\n\n'
