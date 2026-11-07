"""
claude_statusline.py - Claude Code status line command.

Claude Code pipes session JSON in on stdin (rate limits, context usage...).
We forward the numbers to the OLED bridge and print a short status line.
"""
import json
import os
import socket
import sys
import tempfile
import time

ADDR = ("127.0.0.1", 47800)


def pct(window):
    v = (window or {}).get("used_percentage")
    return None if v is None else int(round(v))


DEBUG_LOG = os.path.join(tempfile.gettempdir(), "claude_oled_debug.log")


def debug(msg):
    try:
        with open(DEBUG_LOG, "a") as f:
            f.write(time.strftime("%H:%M:%S ") + msg + "\n")
    except OSError:
        pass


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        debug("statusline: no JSON on stdin")
        return

    rl = d.get("rate_limits") or {}
    debug("statusline: rate_limits=%s keys=%s" % (json.dumps(rl), ",".join(sorted(d))))
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
    cost = d.get("cost") or {}
    if cost:                                    # session cost / lines / time -> bridge stats
        lines.append("K:%d,%d,%d,%d" % (
            round((cost.get("total_cost_usd") or 0) * 100),
            cost.get("total_lines_added") or 0,
            cost.get("total_lines_removed") or 0,
            (cost.get("total_duration_ms") or 0) // 60000))
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
