"""
claude_hook.py - Claude Code hook. Turns hook events into one-line messages
for claude_bridge.py. Never prints, never fails, never blocks Claude.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile

ADDR = ("127.0.0.1", 47800)
HEAD_FILE = os.path.join(tempfile.gettempdir(), "claude_oled_head.txt")

TEST_RE = re.compile(r"\b(pytest|jest|vitest|mocha|phpunit|rspec|unittest|go test|cargo test|"
                     r"dotnet test|(npm|pnpm|yarn)( run)? test)\b")
FAIL_RE = re.compile(r"[1-9]\d* (failed|failures?)|\bFAILED\b|failures?:\s*[1-9]|Traceback|✗|✖")
PKG_RE = re.compile(r"\b(npm (i|install|add)|pnpm (i|install|add)|yarn add|pip3? install|"
                    r"cargo add|go get|brew install|winget install)\b")

TOOL_EVENT = {
    "Read": "read", "NotebookRead": "read",
    "Edit": "type", "Write": "type", "MultiEdit": "type", "NotebookEdit": "type",
    "Grep": "search", "Glob": "search", "LS": "search",
    "WebSearch": "web", "WebFetch": "web",
}

SIMPLE_EVENT = {
    "UserPromptSubmit": "think", "SubagentStart": "think", "SubagentStop": "think",
    "Notification": "need", "PermissionRequest": "need",
    "Stop": "done", "StopFailure": "fail",
    "PreCompact": "compact",
    "SessionStart": "idle", "SessionEnd": "idle",
}


def send(*lines):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for line in lines:
            s.sendto(line.encode(), ADDR)
    except OSError:
        pass


def git_head(cwd):
    try:
        out = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"], cwd=cwd,
                             capture_output=True, text=True, timeout=3)
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


def bash_event(cmd, phase, cwd, resp_text):
    """phase: 'pre' or 'post'. Returns the lines to send."""
    if re.search(r"\bgit\s+commit\b", cmd):
        if phase == "pre":
            try:
                with open(HEAD_FILE, "w") as f:
                    f.write(git_head(cwd))
            except OSError:
                pass
            return ["E:git"]
        before = ""
        try:
            with open(HEAD_FILE) as f:
                before = f.read().strip()
        except OSError:
            pass
        after = git_head(cwd)
        return [f"C:{after}"] if after and after != before else []
    if re.search(r"\bgit\s+push\b", cmd):
        return ["E:plane"] if phase == "pre" else []
    if re.search(r"\bgit\s+(merge|rebase|pull|cherry-pick)\b", cmd):
        return ["E:merge"] if phase == "pre" else []
    if TEST_RE.search(cmd):
        if phase == "pre":
            return ["E:test"]
        return ["E:fail"] if FAIL_RE.search(resp_text) else ["E:bug"]
    if PKG_RE.search(cmd):
        return ["E:pkg"] if phase == "pre" else []
    return ["E:bash"] if phase == "pre" else []


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        return
    ev = d.get("hook_event_name", "")
    tool = d.get("tool_name", "") or ""
    inp = d.get("tool_input") or {}
    cwd = d.get("cwd") or None

    if ev in SIMPLE_EVENT:
        send(f"E:{SIMPLE_EVENT[ev]}")
    elif ev in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
        phase = "pre" if ev == "PreToolUse" else "post"
        if tool == "Bash":
            resp = json.dumps(d.get("tool_response", ""), ensure_ascii=False)
            if ev == "PostToolUseFailure" and TEST_RE.search(inp.get("command", "")):
                send("E:fail")
            elif ev != "PostToolUseFailure":
                send(*bash_event(inp.get("command", ""), phase, cwd, resp))
        elif phase == "pre":
            send("E:" + TOOL_EVENT.get(tool, "think"))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
