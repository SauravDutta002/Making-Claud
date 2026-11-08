#!/bin/sh
# Claude Buddy - remove what install.sh set up (your stats and streaks are kept)
#
#   curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.sh | sh
set -u
export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"

pkill -f 'bin/claude-buddy' 2>/dev/null || true
if command -v uv >/dev/null 2>&1; then
  BIN=$(uv tool dir --bin)
  [ -x "$BIN/claude-buddy-setup" ] && "$BIN/claude-buddy-setup" --remove     # hooks out
  uv tool uninstall claude-buddy >/dev/null 2>&1 || true
fi
if [ "$(uname -s)" = "Darwin" ]; then
  osascript -e 'tell application "System Events" to delete (every login item whose name is "Claude Buddy.command")' >/dev/null 2>&1 || true
  rm -f "$HOME/Applications/Claude Buddy.command"
  DATA="$HOME/Library/Application Support/ClaudeBuddy"
else
  systemctl --user disable --now claude-buddy.service >/dev/null 2>&1 || true
  rm -f "$HOME/.config/systemd/user/claude-buddy.service"
  DATA="${XDG_DATA_HOME:-$HOME/.local/share}/claude-buddy"
fi
printf '  Claude Buddy removed. The board keeps its firmware and lives on its own.\n'
printf '  (your stats and streaks are still in %s - delete it to forget them)\n' "$DATA"
