"""
setup_claude.py - connect Claude Buddy to Claude Code.

Adds the hook (claude_hook.py) for every event the board reacts to, and the
status line (claude_statusline.py), to ~/.claude/settings.json. Your existing
settings and hooks are kept; a backup is written next to the file first.

    python setup_claude.py            # add / update
    python setup_claude.py --remove   # take Claude Buddy out again
"""
import argparse
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# event -> tool matcher (None = every tool / no matcher)
EVENTS = {
    "UserPromptSubmit": None, "PreToolUse": None,
    "PostToolUse": "Bash|PowerShell", "PostToolUseFailure": "Bash|PowerShell",
    "Notification": None, "PermissionRequest": None,
    "Stop": None, "SubagentStop": None, "PreCompact": None,
    "SessionStart": None, "SessionEnd": None,
}


def command(script):
    """How Claude Code should run one of our scripts: as a module when we're
    installed (uv tool install), else straight from this folder."""
    if __package__:
        return f'"{sys.executable}" -m {__package__}.{script[:-3]}'
    return f'"{sys.executable}" "{os.path.join(HERE, script)}"'


def is_ours(entry):
    return any("claude_hook" in (h.get("command") or "") for h in entry.get("hooks") or [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--remove", action="store_true", help="remove Claude Buddy from Claude Code")
    ap.add_argument("--settings", default=os.path.join(os.path.expanduser("~"), ".claude", "settings.json"))
    a = ap.parse_args()

    settings = {}
    if os.path.exists(a.settings):
        with open(a.settings, encoding="utf-8") as f:
            settings = json.load(f)
        backup = f"{a.settings}.bak-{time.strftime('%Y%m%d-%H%M%S')}"
        shutil.copy2(a.settings, backup)
        print(f"    backed up your settings to {backup}")
    else:
        os.makedirs(os.path.dirname(a.settings), exist_ok=True)

    hooks = settings.setdefault("hooks", {})
    for event, matcher in EVENTS.items():
        entries = [e for e in hooks.get(event, []) if not is_ours(e)]   # drop old copies of ours
        if not a.remove:
            entry = {"hooks": [{"type": "command", "command": command("claude_hook.py")}]}
            if matcher:
                entry["matcher"] = matcher
            entries.append(entry)
        if entries:
            hooks[event] = entries
        else:
            hooks.pop(event, None)
    if not hooks:
        settings.pop("hooks", None)

    status = settings.get("statusLine") or {}
    mine = "claude_statusline" in (status.get("command") or "")
    if a.remove:
        if mine:
            settings.pop("statusLine", None)
    elif not status or mine:
        settings["statusLine"] = {"type": "command", "command": command("claude_statusline.py")}
    else:
        print("    you already have your own status line - kept it "
              "(the board still gets your usage from the bridge)")

    with open(a.settings, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2)
    print("    Claude Code " + ("no longer talks to Claude Buddy" if a.remove else
                                "now talks to Claude Buddy (restart open Claude Code sessions)"))


if __name__ == "__main__":
    main()
