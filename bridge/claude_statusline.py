"""
claude_statusline.py - Claude Code status line command.

Claude Code pipes session JSON in on stdin (rate limits, context usage...).
We forward the numbers to the OLED bridge and print a short status line.
"""
import json
import socket
import sys
import time

ADDR = ("127.0.0.1", 47800)


def pct(window):
    v = (window or {}).get("used_percentage")
    return None if v is None else int(round(v))


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        return

    rl = d.get("rate_limits") or {}
    five, week = rl.get("five_hour"), rl.get("seven_day")
    a, w = pct(five), pct(week)
    ctx = pct(d.get("context_window"))

    lines = []
    if a is not None or w is not None:
        resets = (five or {}).get("resets_at")
        mins = max(0, int((resets - time.time()) // 60)) if resets else -1
        lines.append(f"U:{-1 if a is None else a},{-1 if w is None else w},{mins}")
    if ctx is not None:
        lines.append(f"X:{ctx}")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for line in lines:
            s.sendto(line.encode(), ADDR)
    except OSError:
        pass

    model = (d.get("model") or {}).get("display_name", "Claude")
    parts = [model]
    if a is not None:
        parts.append(f"5h {a}%")
    if w is not None:
        parts.append(f"wk {w}%")
    if ctx is not None:
        parts.append(f"ctx {ctx}%")
    print(" | ".join(parts))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("Claude")
