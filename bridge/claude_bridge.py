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
    python claude_bridge.py --ble          # wireless only (board just needs power)

WIRELESS: if the board isn't plugged into this laptop, the bridge finds it over
Bluetooth LE ("Claude Buddy") and reconnects by itself if the link drops.
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
import threading
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
        """R:/L: lines: branch, repo name and the last 7 commits of the current repo."""
        root = self.current or next(iter(self.repos), None)
        if not root:
            return []
        branch = git(root, "rev-parse", "--abbrev-ref", "HEAD") or "HEAD"
        rows = git(root, "log", "--format=%h%x09%s", "-n", "7").splitlines()
        lines = ["R:%s,%s" % (oled_text(branch, 11).replace(",", " "),
                              oled_text(os.path.basename(root), 19))]
        for i in range(7):
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


class BleLink:
    """Bluetooth LE link to the board ("Claude Buddy", Nordic UART service).

    Looks like the serial port to the rest of the bridge: write(), read(),
    in_waiting, close(). bleak runs on its own asyncio thread.
    """

    NAME = "Claude Buddy"
    NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # laptop -> board
    NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # board -> laptop (music buttons)

    def __init__(self):
        import asyncio
        import threading
        self.asyncio = asyncio
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.client = None
        self.lost = False
        self.rx = bytearray()
        self.rx_lock = threading.Lock()

    @property
    def in_waiting(self):
        return len(self.rx)

    def _on_rx(self, _char, data):
        with self.rx_lock:
            self.rx += data

    def _run(self, coro, timeout):
        return self.asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def connect(self):
        from bleak import BleakClient, BleakScanner

        async def go():
            dev = await BleakScanner.find_device_by_name(self.NAME, timeout=6)
            if dev is None:
                return None
            client = BleakClient(dev, disconnected_callback=lambda _: setattr(self, "lost", True))
            await client.connect()
            try:
                await client.start_notify(self.NUS_TX, self._on_rx)   # hear the board's buttons
            except Exception as e:
                print(f"bluetooth: board buttons unavailable ({e}) - reflash the board")
            return client

        self.lost = False
        try:
            self.client = self._run(go(), 25)
        except Exception as e:                      # bluetooth off, no adapter, timeout...
            print(f"bluetooth: {e or type(e).__name__}")
            self.client = None
        return self.client is not None

    def write(self, data):
        if self.lost or self.client is None:
            raise OSError("bluetooth disconnected")
        try:
            for i in range(0, len(data), 180):
                self._run(self.client.write_gatt_char(self.NUS_RX, data[i:i + 180], response=False), 5)
        except Exception as e:
            raise OSError(f"bluetooth write failed: {e}") from e

    def read(self, n):
        with self.rx_lock:
            data, self.rx = bytes(self.rx), bytearray()
        return data

    def close(self):
        try:
            if self.client is not None:
                self._run(self.client.disconnect(), 5)
        except Exception:
            pass


class MusicWatcher:
    """What's playing on this laptop via Windows media controls - Spotify,
    YouTube / YouTube Music in any browser, Apple Music, VLC, Media Player... -
    plus the live output level from the audio meter (just the volume needle,
    nothing is recorded). Apps that don't report to Windows still count:
    steady sound from the speakers for a few seconds = music."""

    PLAYING, PAUSED = 4, 5         # GlobalSystemMediaTransportControlsSessionPlaybackStatus
    SOUND_LEVEL = 8                # meter level that counts as "sound playing"
    SOUND_SECONDS = 4              # ...for this long, when no app reports a song
    PAUSE_KEEP_S = 120             # a paused song stays on the record player this long

    def __init__(self):
        import asyncio
        import threading
        self.playing, self.title = False, ""
        self.state = 0                 # 0 nothing, 1 playing, 2 paused (recently played)
        self.session_state, self.session_title = 0, ""
        self.last_app, self.last_play_t = None, 0.0
        self.pos, self.dur = 0, 0      # song position / length in seconds
        self.mgr = None
        self.sound_since, self.last_loud = None, 0.0
        self.meter = None
        try:
            from comtypes import CLSCTX_ALL
            from pycaw.pycaw import AudioUtilities, IAudioMeterInformation
            dev = AudioUtilities.GetSpeakers()
            imm = getattr(dev, "_dev", dev)
            self.meter = imm.Activate(IAudioMeterInformation._iid_, CLSCTX_ALL, None) \
                            .QueryInterface(IAudioMeterInformation)
        except Exception as e:
            print(f"music: no audio meter ({e}) - the equalizer will dance without it")
        self.asyncio = asyncio
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        self.asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._poll())

    @staticmethod
    async def _title(s):
        info = await s.try_get_media_properties_async()
        return " - ".join(x for x in (info.title, info.artist) if x)

    async def _poll(self):
        try:
            from winrt.windows.media.control import \
                GlobalSystemMediaTransportControlsSessionManager as Manager
        except ImportError:
            print("music: pip install winrt-Windows.Media.Control  (to see what's playing)")
            return
        while True:
            try:
                if self.mgr is None:
                    self.mgr = await Manager.request_async()
                sessions = list(self.mgr.get_sessions())
                state, title = 0, ""
                # every app's session, not just the "current" one: a paused
                # Spotify mustn't hide YouTube playing in the browser
                playing = next((s for s in sessions
                                if int(s.get_playback_info().playback_status) == self.PLAYING), None)
                active = None
                if playing is not None:
                    state, title, active = 1, await self._title(playing), playing
                    self.last_app, self.last_play_t = playing.source_app_user_model_id, time.time()
                else:                          # the song you just paused stays on the player
                    last = next((s for s in sessions if s.source_app_user_model_id == self.last_app), None)
                    if (last is not None and time.time() - self.last_play_t < self.PAUSE_KEEP_S
                            and int(last.get_playback_info().playback_status) == self.PAUSED):
                        state, title, active = 2, await self._title(last), last
                self.pos, self.dur = self._timeline(active, state == 1)
                self.session_state, self.session_title = state, title
            except Exception:
                self.mgr = None
            await self.asyncio.sleep(1)

    @staticmethod
    def _timeline(s, playing):
        """(position, length) in whole seconds, or (0, 0) if the player doesn't share it."""
        import datetime
        try:
            if s is None:
                return 0, 0
            tl = s.get_timeline_properties()
            dur = (tl.end_time - tl.start_time).total_seconds()
            pos = (tl.position - tl.start_time).total_seconds()
            upd = tl.last_updated_time                     # position was sampled at this time
            if playing and upd and upd.year > 2000:
                pos += (datetime.datetime.now(datetime.timezone.utc) - upd).total_seconds()
            if dur <= 0:
                return 0, 0
            return int(max(0, min(pos, dur))), int(dur)
        except Exception:
            return 0, 0

    def control(self, cmd):
        """The board's buttons: 'toggle', 'next' or 'prev' on the song in play."""
        async def go():
            if self.mgr is None:
                return
            sessions = list(self.mgr.get_sessions())
            s = (next((x for x in sessions if int(x.get_playback_info().playback_status) == self.PLAYING), None)
                 or next((x for x in sessions if x.source_app_user_model_id == self.last_app), None)
                 or self.mgr.get_current_session())
            if s is None:
                return
            if cmd == "toggle":
                await s.try_toggle_play_pause_async()
            elif cmd == "next":
                await s.try_skip_next_async()
            elif cmd == "prev":
                await s.try_skip_previous_async()
        self.asyncio.run_coroutine_threadsafe(go(), self.loop)

    def tick(self):
        """Call often from the main thread (the audio meter lives there).
        An app reporting a song wins; otherwise steady sound counts as music."""
        now = time.time()
        if self.level() >= self.SOUND_LEVEL:
            self.last_loud = now
            if self.sound_since is None:
                self.sound_since = now
        elif self.sound_since is not None and now - self.last_loud > 3:   # real silence, not a gap
            self.sound_since = None
        if self.session_state:
            self.state, self.title = self.session_state, self.session_title
        else:
            loud = self.sound_since is not None and now - self.sound_since >= self.SOUND_SECONDS
            self.state, self.title = (1, "music") if loud else (0, "")
        self.playing = self.state == 1

    def level(self):
        """0-99, perceptual (square root of the peak amplitude)."""
        try:
            return min(99, int((self.meter.GetPeakValue() ** 0.5) * 100)) if self.meter else 0
        except Exception:
            return 0


class UsageFetcher:
    """Your 5-hour and weekly Claude usage, fetched the way /usage does it, so
    the board's usage screen works even from the IDE chat panel (which never
    runs the status line). Reads Claude Code's login token from
    ~/.claude/.credentials.json each time (Claude Code keeps it fresh); the
    token is only ever sent to Anthropic and never printed. Uses an
    undocumented endpoint, so it may stop working after an update."""

    URL = "https://api.anthropic.com/api/oauth/usage"
    EVERY_S = 120

    def __init__(self):
        self.line, self.err = None, None
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                self.line, self.err = self._fetch(), None
            except Exception as e:                 # offline, logged out, endpoint changed...
                if str(e) != self.err:
                    print(f"usage: couldn't fetch ({e})")
                    self.err = str(e)
            time.sleep(self.EVERY_S)

    def _fetch(self):
        import datetime
        import urllib.request
        with open(os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")) as f:
            token = json.load(f)["claudeAiOauth"]["accessToken"]
        req = urllib.request.Request(self.URL, headers={
            "Authorization": "Bearer " + token,
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent": "claude-buddy-bridge"})
        with urllib.request.urlopen(req, timeout=15) as r:
            d = json.load(r)

        def pct(key):
            v = (d.get(key) or {}).get("utilization")
            return -1 if v is None else int(round(v))

        mins = -1
        resets = (d.get("five_hour") or {}).get("resets_at")
        if resets:
            t = datetime.datetime.fromisoformat(resets.replace("Z", "+00:00"))
            mins = max(0, int((t - datetime.datetime.now(datetime.timezone.utc)).total_seconds() // 60))
        return f"U:{pct('five_hour')},{pct('seven_day')},{mins}"


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
    ap.add_argument("--no-music", action="store_true", help="don't react to music playing on this laptop")
    ap.add_argument("--no-usage", action="store_true",
                    help="don't fetch your Claude usage (the usage screen then relies on the status line)")
    ap.add_argument("--ble", action="store_true", help="always use Bluetooth, even if USB is plugged in")
    ap.add_argument("--usb", action="store_true", help="USB cable only, never Bluetooth")
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
    ble = None                     # BleLink, created the first time USB isn't there
    music = None if a.no_music else MusicWatcher()
    usage = None if a.no_usage else UsageFetcher()
    music_state, music_sent = (0, ""), 0.0
    rx_buf = b""                   # partial line from the board
    date_sent = 0.0                # last time the board got the date & time
    level_sent, level_at = -1, 0.0
    keys = None if a.no_typing else KeyWatcher(a.type_windows)
    typing, last_key, last_typing_send = False, 0.0, 0.0
    print(f"bridge listening on udp://{UDP_HOST}:{UDP_PORT}")

    while True:
        if ser is None:
            # USB if the board is plugged into this laptop, else Bluetooth
            port = None if a.ble else (a.port or find_port())
            if port:
                try:
                    ser = open_serial(port, a.baud)
                    print(f"connected on {port} (USB)")
                except (serial.SerialException, OSError) as e:
                    print(f"cannot open {port}: {e}")
                    ser = None
            elif not a.usb:
                print("looking for 'Claude Buddy' over Bluetooth...")
                if ble is None:
                    ble = BleLink()                # one bluetooth thread, reused
                if ble.connect():
                    ser = ble
                    print("connected over Bluetooth")
            if ser is None:
                print("no board found - plug it in or switch it on. retrying...")
                time.sleep(2)
                continue
            try:
                time.sleep(0.3)
                date_sent = 0.0                    # fresh time right after (re)connecting
                for line in list(sticky.values()) + git_lines:
                    ser.write((line + "\n").encode())
            except (serial.SerialException, OSError) as e:
                print(f"link lost while syncing: {e}")
                ser = None
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
                if a.verbose:
                    print(">", line)
                ser.write((line + "\n").encode())
                if stats_changed:
                    sticky["S"] = stats.line()
                    if line != sticky["S"]:
                        ser.write((sticky["S"] + "\n").encode())
                if line[0] == "C":
                    watcher.changed = True         # resend the history with this commit
            if time.time() - date_sent > 60:       # the board's clock: local time every minute
                date_sent = time.time()
                ser.write(("D:%d\n" % (int(date_sent) + time.localtime().tm_gmtoff)).encode())
            if usage is not None and usage.line and usage.line != sticky.get("U"):
                sticky["U"] = usage.line           # your 5-hour / weekly limits -> usage screen
                ser.write((usage.line + "\n").encode())
                if a.verbose:
                    print(">", usage.line)
            if music is not None:                  # music on this laptop -> music faces
                music.tick()
                now = time.time()
                state = (music.state, music.title)
                if state != music_state or (state[0] and now - music_sent > 2):
                    if a.verbose and state != music_state:
                        print("> music:", ("", "playing ", "paused ")[state[0]] + state[1] if state[0] else "stopped")
                    music_state, music_sent = state, now
                    ser.write(("M:%d,%s\n" % (state[0], oled_text(state[1], 40))).encode())
                    if state[0] and music.dur:     # song position for the progress bar
                        ser.write(("Q:%d,%d\n" % (music.pos, music.dur)).encode())
                if state[0] == 1 and now - level_at >= 0.08:
                    lvl = music.level()
                    if abs(lvl - level_sent) >= 2 or now - level_at > 0.5:
                        ser.write(f"A:{lvl}\n".encode())
                        level_sent, level_at = lvl, now
                sock.settimeout(0.03 if state[0] == 1 else 0.1)   # faster loop while music plays
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
                    typing = False     # the board decides how (and whether) to react
            # lines from the board: its debug prints, and P:<cmd> from the music buttons
            rx_buf += ser.read(ser.in_waiting or 1)
            while b"\n" in rx_buf:
                raw, rx_buf = rx_buf.split(b"\n", 1)
                msg = raw.decode(errors="ignore").strip()
                if msg.startswith("P:") and music is not None:
                    if a.verbose:
                        print("< board button:", msg[2:])
                    music.control(msg[2:])
            rx_buf = rx_buf[-200:]
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
