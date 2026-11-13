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

# say hello in the browser: the welcome page (and its "install the app" offer)
i=0
while [ $i -lt 30 ] && ! curl -fs -o /dev/null --max-time 2 http://localhost:47801/ 2>/dev/null; do sleep 1; i=$((i+1)); done
if [ $i -lt 30 ]; then
  if command -v open >/dev/null 2>&1 && [ "$(uname -s)" = "Darwin" ]; then open "http://localhost:47801/?welcome" 2>/dev/null || true
  elif command -v xdg-open >/dev/null 2>&1; then xdg-open "http://localhost:47801/?welcome" >/dev/null 2>&1 || true; fi
fi
printf '\n  \033[38;5;209mAll set. Restart Claude Code and watch the screen.\033[0m\n\n'
