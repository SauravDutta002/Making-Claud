"""
claude_bridge.py - owns the USB serial port to the OLED face and forwards
events from Claude Code (hooks + status line) to it.

    Claude Code hook / statusline --UDP 127.0.0.1:47800--> this bridge --USB serial--> ESP32

Only this process opens the COM port, so hooks stay fast and never reset the
board. Run it once and leave it open:

    pip install pyserial
    python claude_bridge.py                # auto-detects the ESP32
    python claude_bridge.py --port COM5    # or pick the port yourself
    python claude_bridge.py --send E:think # inject a line (testing)
    python claude_bridge.py --list         # show serial ports
    python claude_bridge.py --repo D:\code\myproject   # also watch this repo's commits

Commits are picked up from ANY tool (Antigravity / VS Code source control, a
terminal, GitHub Desktop...): the bridge watches each repo's HEAD. Repos you
use Claude Code in are added automatically and remembered in repos.json.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

UDP_HOST, UDP_PORT = "127.0.0.1", 47800

# USB vendor ids: Espressif (ESP32-C3 native USB), then common USB-UART chips.
ESPRESSIF = 0x303A
UART_CHIPS = {0x10C4, 0x1A86, 0x0403}


def find_port():
    from serial.tools import list_ports
    cands = [p for p in list_ports.comports() if p.vid is not None]
    for p in cands:
        if p.vid == ESPRESSIF:
            return p.device
    for p in cands:
        if p.vid in UART_CHIPS:
            return p.device
    return None


def list_ports_cmd():
    from serial.tools import list_ports
    for p in list_ports.comports():
        vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----"
        print(f"{p.device:8} {vid}  {p.description}")


class KeyWatcher:
    """Reports whether *any* key is held while a matching window is in front.

    It never records which key it is - only a yes/no "someone is typing".
    Windows only; elsewhere active() is always False.
    """

    def __init__(self, titles):
        self.titles = [t.strip().lower() for t in titles.split(",") if t.strip()]
        self.ok = sys.platform == "win32"
        if self.ok:
            import ctypes
            self.u = ctypes.windll.user32
            self.ct = ctypes

    def _title(self):
        hwnd = self.u.GetForegroundWindow()
        buf = self.ct.create_unicode_buffer(256)
        self.u.GetWindowTextW(hwnd, buf, 256)
        return buf.value.lower()

    def active(self):
        if not self.ok:
            return False
        down = False
        for vk in range(8, 255):               # 1-6 are mouse buttons
            if self.u.GetAsyncKeyState(vk) & 0x8000:
                down = True
                break
        if not down:
            return False
        title = self._title()
        return any(t in title for t in self.titles)


STATS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stats.json")


class Stats:
    """Today's commits / tasks / passed tests (saved, reset at midnight),
    plus the current session's cost, lines and time from the status line."""

    def __init__(self):
        self.day = {"date": "", "commits": 0, "tasks": 0, "tests": 0}
        self.session = [0, 0, 0, 0]          # cost cents, lines added, removed, minutes
        try:
            with open(STATS_FILE) as f:
                self.day.update(json.load(f))
        except (OSError, ValueError):
            pass
        self._roll()

    def _roll(self):
        today = time.strftime("%Y-%m-%d")
        if self.day["date"] != today:
            self.day = {"date": today, "commits": 0, "tasks": 0, "tests": 0}

    def bump(self, key):
        self._roll()
        self.day[key] += 1
        try:
            with open(STATS_FILE, "w") as f:
                json.dump(self.day, f)
        except OSError:
            pass

    def line(self):
        self._roll()
        d = self.day
        return "S:%d,%d,%d,%d,%d,%d,%d" % (d["commits"], d["tasks"], d["tests"], *self.session)


REPOS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "repos.json")


def git(repo, *args):
    try:
        r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True,
                           timeout=5, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return r.stdout.strip() if r.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def oled_text(text, limit=40):
    """One short ASCII line the OLED font can draw."""
    text = re.sub(r"\s+", " ", text).strip()
    text = "".join(c if 32 <= ord(c) < 127 else "?" for c in text)
    return text[:limit - 2] + ".." if len(text) > limit else text


class CommitWatcher:
    """Notices new commits in watched repos, whoever made them."""

    POLL_S = 4

    def __init__(self, extra):
        self.repos = {}                       # repo root -> {"head": sha, "since": time}
        self.current = None                   # repo shown on the board's git graph
        self.changed = True                   # history moved -> resend it to the board
        self.next_poll = 0.0
        try:
            with open(REPOS_FILE) as f:
                saved = json.load(f)
        except (OSError, ValueError):
            saved = []
        for path in list(saved) + list(extra):
            self.add(path, save=False)
        self.save()

    def add(self, path, save=True):
        root = git(path, "rev-parse", "--show-toplevel")
        if not root:
            return False
        root = os.path.normpath(root)
        if root != self.current:              # the repo you're working in now
            self.current, self.changed = root, True
        if root in self.repos:
            return False
        self.repos[root] = {"head": git(root, "rev-parse", "HEAD"), "since": time.time()}
        print(f"watching commits in {root}")
        if save:
            self.save()
        return True

    def snapshot(self):
        """R:/L: lines: branch, repo name and the last 5 commits of the current repo."""
        root = self.current or next(iter(self.repos), None)
        if not root:
            return []
        branch = git(root, "rev-parse", "--abbrev-ref", "HEAD") or "HEAD"
        rows = git(root, "log", "--format=%h%x09%s", "-n", "5").splitlines()
        lines = ["R:%s,%s" % (oled_text(branch, 11).replace(",", " "),
                              oled_text(os.path.basename(root), 19))]
        for i in range(5):
            if i < len(rows):
                h, _, subj = rows[i].partition("\t")
                lines.append("L:%d,%s,%s" % (i, h[:7], oled_text(subj, 19)))
            else:
                lines.append("L:%d,," % i)   # clear unused rows
        return lines

    def save(self):
        try:
            with open(REPOS_FILE, "w") as f:
                json.dump(sorted(self.repos), f, indent=1)
        except OSError:
            pass

    def poll(self):
        """Returns [(short hash, subject, repo name)] for commits made since the last poll."""
        now = time.time()
        if now < self.next_poll:
            return []
        self.next_poll = now + self.POLL_S
        found = []
        for root, st in self.repos.items():
            head = git(root, "rev-parse", "HEAD")
            if not head or head == st["head"]:
                continue
            if root == self.current:              # commit, pull, reset, branch switch...
                self.changed = True
            rng = f"{st['head']}..{head}" if st["head"] else head
            out = git(root, "log", "--format=%h%x09%ct%x09%s", "-n", "5", rng)
            for ln in reversed(out.splitlines()):
                h, ct, subj = (ln.split("\t", 2) + ["", ""])[:3]
                # only freshly made commits: a branch switch or pull brings in old ones
                if ct.isdigit() and int(ct) >= st["since"] - 120:
                    found.append((h[:7], subj, os.path.basename(root)))
                    if root != self.current:      # show the repo that just got a commit
                        self.current, self.changed = root, True
            st["head"], st["since"] = head, now
        return found


def send_one(line):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(line.encode(), (UDP_HOST, UDP_PORT))


def open_serial(port, baud):
    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate = port, baud
    ser.timeout, ser.write_timeout = 0, 1
    ser.dtr = False   # keep DTR/RTS low so opening the port doesn't reset the board
    ser.rts = False
    ser.open()
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--send", metavar="LINE")
    ap.add_argument("--repo", action="append", default=[], metavar="PATH",
                    help="also watch this git repo for commits (repeatable; remembered)")
    ap.add_argument("--no-typing", action="store_true",
                    help="don't show the typing face while you type")
    ap.add_argument("--type-windows",
                    default="claude,antigravity,powershell,command prompt,terminal,cmd",
                    help="comma-separated window-title words that count as 'typing to Claude'")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.list:
        return list_ports_cmd()
    if a.send:
        return send_one(a.send)

    import serial

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((UDP_HOST, UDP_PORT))
    except OSError:
        sys.exit(f"UDP port {UDP_PORT} is busy - is the bridge already running?")
    sock.settimeout(0.1)

    stats = Stats()
    sticky = {"S": stats.line()}   # last U:/X:/S: line per kind, replayed after a reconnect
    watcher = CommitWatcher(a.repo)
    seen_commits = []              # short hashes already sent (hook + watcher both report)
    git_lines = []                 # last R:/L: history sent, replayed after a reconnect
    last_event = ("", 0.0)
    ser = None
    keys = None if a.no_typing else KeyWatcher(a.type_windows)
    typing, last_key, last_typing_send = False, 0.0, 0.0
    last_hook = "E:idle"     # what to show again when you stop typing
    print(f"bridge listening on udp://{UDP_HOST}:{UDP_PORT}")

    while True:
        if ser is None:
            port = a.port or find_port()
            if not port:
                print("no ESP32 found - plug it in (or pass --port). retrying...")
                time.sleep(2)
                continue
            try:
                ser = open_serial(port, a.baud)
                print(f"connected on {port}")
                time.sleep(0.3)
                for line in list(sticky.values()) + git_lines:
                    ser.write((line + "\n").encode())
            except (serial.SerialException, OSError) as e:
                print(f"cannot open {port}: {e}")
                ser = None
                time.sleep(2)
                continue

        try:
            data = sock.recvfrom(512)[0].decode(errors="ignore")
        except socket.timeout:
            data = ""

        try:
            incoming = data.splitlines()
            for h, subj, repo in watcher.poll():   # commits from any editor or tool
                if a.verbose:
                    print(f"commit {h} in {repo}: {subj}")
                incoming += ["T:" + oled_text("commit: " + subj), "C:" + h]
            for line in incoming:
                line = line.strip()
                if len(line) < 3 or line[1] != ":":
                    continue
                if line[0] == "W":                 # a folder Claude Code works in
                    watcher.add(line[2:])
                    continue
                if line[0] in "UX":
                    sticky[line[0]] = line
                stats_changed = False
                if line[0] == "K":                 # session cost/lines/time -> S: line
                    try:
                        stats.session = [int(x) for x in line[2:].split(",")][:4]
                    except ValueError:
                        continue
                    line, stats_changed = stats.line(), True
                elif line[0] == "C":
                    h = line[2:9]
                    if h in seen_commits:          # already counted (hook + watcher)
                        continue
                    seen_commits = (seen_commits + [h])[-50:]
                    line = "C:" + h
                    stats.bump("commits")
                    stats_changed = True
                elif line[0] == "E":
                    ev = line[2:]
                    # drop identical events fired back-to-back
                    now = time.time()
                    if line == last_event[0] and now - last_event[1] < 0.4:
                        continue
                    last_event = (line, now)
                    if ev in ("done", "bug"):
                        stats.bump("tasks" if ev == "done" else "tests")
                        stats_changed = True
                    last_hook = "E:idle" if ev in ("done", "need", "idle") else line
                    if typing and ev not in ("done", "need"):
                        continue       # the typing face wins while you type
                if a.verbose:
                    print(">", line)
                ser.write((line + "\n").encode())
                if stats_changed:
                    sticky["S"] = stats.line()
                    if line != sticky["S"]:
                        ser.write((sticky["S"] + "\n").encode())
                if line[0] == "C":
                    watcher.changed = True         # resend the history with this commit
            if watcher.changed:                    # board's git graph = your real history
                watcher.changed = False
                git_lines = watcher.snapshot()
                for gl in git_lines:
                    ser.write((gl + "\n").encode())
                if a.verbose and git_lines:
                    print("> git history:", git_lines[0])
            if keys is not None:
                now = time.time()
                if keys.active():
                    last_key = now
                    if not typing or now - last_typing_send > 1.0:
                        if a.verbose and not typing:
                            print("> typing")
                        typing, last_typing_send = True, now
                        ser.write(b"E:key\n")
                elif typing and now - last_key > 2.0:
                    typing = False
                    ser.write((last_hook + "\n").encode())
            ser.read(ser.in_waiting or 1)   # drain the board's debug prints
        except (serial.SerialException, OSError) as e:
            if isinstance(e, serial.SerialTimeoutException):
                print("board is not reading the port (write timed out).\n"
                      "  -> Arduino IDE: Tools > USB CDC On Boot > Enabled, re-upload the sketch,\n"
                      "     and close the Arduino Serial Monitor.")
            else:
                print(f"serial lost ({e}) - reconnecting...")
            try:
                ser.close()
            except Exception:
                pass
            ser = None


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
