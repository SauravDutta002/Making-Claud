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
from urllib.parse import urlparse

ADDR = ("127.0.0.1", 47800)
HEAD_FILE = os.path.join(tempfile.gettempdir(), "claude_oled_head.txt")

TEST_RE = re.compile(r"\b(pytest|jest|vitest|mocha|phpunit|rspec|unittest|go test|cargo test|"
                     r"dotnet test|(npm|pnpm|yarn)( run)? test)\b")
FAIL_RE = re.compile(r"[1-9]\d* (failed|failures?)|\bFAILED\b|failures?:\s*[1-9]|Traceback|✗|✖")
PKG_RE = re.compile(r"\b(npm (i|install|add)|pnpm (i|install|add)|yarn add|pip3? install|"
                    r"cargo add|go get|brew install|winget install)\b")
BUILD_RE = re.compile(r"\b((npm|pnpm|yarn)( run)? build|make|cmake|cargo build|go build|gradlew?|mvn|"
                      r"dotnet (build|publish)|tsc|webpack|vite build|pyinstaller|"
                      r"arduino-cli(\.exe)?\W+compile|pio run|docker build)\b")

TOOL_EVENT = {
    "Read": "read", "NotebookRead": "read",
    "Edit": "type", "MultiEdit": "type", "NotebookEdit": "type",
    "Write": "write",                                   # creating a file -> the crane builds it
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


def bash_event(cmd, phase, cwd, resp_text, shell="bash"):
    """phase: 'pre' or 'post'; shell: 'bash' or 'ps' (PowerShell). Returns the lines to send."""
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
    if BUILD_RE.search(cmd):
        return ["E:build"] if phase == "pre" else []
    return ["E:" + shell] if phase == "pre" else []


def clean(text, limit=40):
    """One short ASCII line the OLED font can draw."""
    text = re.sub(r"\s+", " ", str(text)).strip()
    text = "".join(c if 32 <= ord(c) < 127 else "?" for c in text)
    return text[:limit - 2] + ".." if len(text) > limit else text


def ticker(tool, inp):
    """What Claude is doing, in a few words: 'editing app.py', '$ npm test'..."""
    name = lambda p: os.path.basename(str(p).rstrip("/\\")) or str(p)
    if tool == "Read":
        return "reading " + name(inp.get("file_path", ""))
    if tool in ("Edit", "MultiEdit"):
        return "editing " + name(inp.get("file_path", ""))
    if tool == "Write":
        return "writing " + name(inp.get("file_path", ""))
    if tool == "NotebookEdit":
        return "editing " + name(inp.get("notebook_path", ""))
    if tool in ("Bash", "PowerShell"):
        return "$ " + str(inp.get("command", "")).splitlines()[0] if inp.get("command") else "$"
    if tool == "Grep":
        return "grep " + str(inp.get("pattern", ""))
    if tool == "Glob":
        return "find " + str(inp.get("pattern", ""))
    if tool == "WebSearch":
        return "search: " + str(inp.get("query", ""))
    if tool == "WebFetch":
        return "fetch " + (urlparse(str(inp.get("url", ""))).netloc or "page")
    if tool in ("Task", "Agent"):
        return "agent: " + str(inp.get("description", ""))
    if tool == "TodoWrite":
        return "planning todos"
    return tool.split("__")[-1].lower()             # mcp__server__tool -> tool


def todo_msg(tool, inp, session):
    """Claude's to-do list changes, for the board's progress bar (J: line)."""
    def text(x):
        return str(x.get("activeForm") or x.get("content") or x.get("subject") or "")[:60]
    if tool == "TodoWrite":                         # the whole list, every time
        todos = [[t.get("status", "pending"), text(t)] for t in (inp.get("todos") or [])[:40]]
        return {"op": "write", "s": session, "todos": todos}
    if tool == "TaskCreate":                        # one new item
        return {"op": "create", "s": session, "text": text(inp)}
    return {"op": "update", "s": session, "id": str(inp.get("taskId", "")),
            "status": inp.get("status"), "text": text(inp)}


def is_limit(d):
    """StopFailure because you hit your usage / rate limit (not some other API error)."""
    err = d.get("error") or d.get("error_type") or ""
    if isinstance(err, dict):
        err = err.get("type") or err.get("message") or ""
    text = (str(err) + " " + str(d.get("error_details") or d.get("message") or "")).lower()
    return "rate_limit" in text or "limit" in text or "usage" in text


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        return
    ev = d.get("hook_event_name", "")
    tool = d.get("tool_name", "") or ""
    inp = d.get("tool_input") or {}
    cwd = d.get("cwd") or None

    if ev == "PreToolUse" and tool in ("TodoWrite", "TaskCreate", "TaskUpdate"):
        send("J:" + json.dumps(todo_msg(tool, inp, d.get("session_id", "")), separators=(",", ":")))

    if cwd and ev in ("SessionStart", "UserPromptSubmit"):
        send("W:" + cwd)                            # bridge starts watching this repo's commits
    if ev == "PreToolUse":                          # (your prompt is never sent to the screen)
        send("T:" + clean(ticker(tool, inp)))

    if ev == "StopFailure" and is_limit(d):
        send("E:limit")                             # out of usage -> the "limit hit" face
    elif ev in SIMPLE_EVENT:
        send(f"E:{SIMPLE_EVENT[ev]}")
    elif ev in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
        phase = "pre" if ev == "PreToolUse" else "post"
        if tool in ("Bash", "PowerShell"):
            resp = json.dumps(d.get("tool_response", ""), ensure_ascii=False)
            if ev == "PostToolUseFailure" and TEST_RE.search(inp.get("command", "")):
                send("E:fail")
            elif ev != "PostToolUseFailure":
                send(*bash_event(inp.get("command", ""), phase, cwd, resp,
                                 "ps" if tool == "PowerShell" else "bash"))
        elif phase == "pre":
            send("E:" + TOOL_EVENT.get(tool, "think"))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
