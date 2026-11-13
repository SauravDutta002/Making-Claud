r"""
claude_bridge.py - owns the USB serial port to the OLED face and forwards
events from Claude Code (hooks + status line) to it.

    Claude Code hook / statusline --UDP 127.0.0.1:47800--> this bridge --USB serial--> ESP32

Only this process opens the serial port, so hooks stay fast and never reset the
board. Installed with install.ps1 (Windows) / install.sh (macOS) it runs as
`claude-buddy`; from a checkout:

    python claude_bridge.py                # auto-detects the ESP32
    python claude_bridge.py --port COM5    # or pick the port yourself
    python claude_bridge.py --send E:think # inject a line (testing)
    python claude_bridge.py --list         # show serial ports
    python claude_bridge.py --plain        # plain log lines, no live dashboard
    python claude_bridge.py --ble          # wireless only (board just needs power)

Installed, it runs in the background as `claude-buddy-service` (no window, starts
at login, restarts itself if it ever stops); `claude-buddy` is the same thing
with the live dashboard in a terminal.

WIRELESS: if the board isn't plugged into this laptop, the bridge finds it over
Bluetooth LE ("Claude Buddy") and reconnects by itself if the link drops.
    python claude_bridge.py --repo D:\code\myproject   # also watch this repo's commits

Commits are picked up from ANY tool (Antigravity / VS Code source control, a
terminal, GitHub Desktop...): the bridge watches each repo's HEAD. Repos you
use Claude Code in are added automatically and remembered in repos.json.
"""
import argparse
import atexit
import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from collections import deque

UDP_HOST, UDP_PORT = "127.0.0.1", 47800
VERSION = "1.1.5"
ALREADY_RUNNING = 3                # exit code: another Claude Buddy already runs on this computer

try:
    from .webapp import WebApp
except ImportError:
    from webapp import WebApp

# what the settings page can switch (kept in config.json)
DEFAULT_SETTINGS = {
    "music": True, "lyrics": True, "lyrics_offset": 0.0,
    "notify": True, "notify_apps": "slack,teams,discord,whatsapp,telegram,signal,zoom,outlook",
    "calendar": True, "ics": [], "away": True, "typing": True, "usage": True,
    "link": "auto",                # auto (USB if plugged in, else Bluetooth) | usb | ble
    "auto_update": True,           # install new versions of Claude Buddy by itself
}
# faces the settings page can put on the screen (board scene numbers)
FACES = [
    (0, "Idle"), (1, "Thinking words"), (2, "Thinking"), (3, "Bright idea"), (4, "Reading"),
    (5, "Typing code"), (6, "Keyboard peek"), (7, "Letters fly"), (8, "Web search"), (9, "Test tubes"),
    (10, "Needs you"), (11, "Context meter"), (12, "Coffee"), (13, "Music"), (14, "Headbang"),
    (15, "Dance"), (16, "Equalizer"), (17, "Now playing card"), (18, "Love"), (19, "Petted"),
    (21, "Stargazing"), (22, "Dreaming"), (23, "Low power"), (24, "Feeling great"), (25, "Not amused"),
    (26, "Oh no"), (27, "Hmmm"), (28, "Got it"), (29, "Whoa"), (30, "Terminal"), (31, "Searching files"),
    (32, "Git graph"), (33, "Branch merge"), (34, "Packages"), (35, "Trophy"), (36, "Goal"),
    (37, "Usage"), (38, "Limit hit"), (39, "Test failed"), (40, "Variable check"), (41, "Waiting"),
    (42, "Paper plane"), (44, "Typing along"), (45, "Goal celebration"), (46, "Bicycle kick"),
    (47, "Build crane"), (48, "Flappy Clawd"), (49, "Permission panic"), (50, "Whack a bug"),
    (51, "Today's stats"), (52, "Level up"), (53, "Profile"), (54, "Working"), (55, "Worked for"),
    (56, "Shell window"), (57, "Commit log"), (58, "Merge log"), (59, "Plane throw"),
    (60, "Juggling three"), (63, "Super Claude"), (64, "Record player"),
    (65, "Clock"), (66, "Lyrics"), (67, "Fixing the wiring"), (68, "DJ set"),
    (69, "Night sky"), (70, "Chilling"),
]

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


HERE = os.path.dirname(os.path.abspath(__file__))


def data_dir():
    """Your stats, streaks and settings live here, so updates never touch them:
    Windows %LOCALAPPDATA%\\ClaudeBuddy, macOS ~/Library/Application Support/ClaudeBuddy."""
    d = os.environ.get("CLAUDE_BUDDY_HOME")
    if not d:
        if sys.platform == "win32":
            d = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "ClaudeBuddy")
        elif sys.platform == "darwin":
            d = os.path.expanduser("~/Library/Application Support/ClaudeBuddy")
        else:
            d = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"),
                             "claude-buddy")
    os.makedirs(d, exist_ok=True)
    for name in ("stats.json", "repos.json", "config.json", "progress.json"):
        old, new = os.path.join(HERE, name), os.path.join(d, name)   # older versions kept them here
        if os.path.exists(old) and not os.path.exists(new):
            try:
                shutil.copy2(old, new)
            except OSError:
                pass
    return d


DATA = data_dir()
STATS_FILE = os.path.join(DATA, "stats.json")


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


REPOS_FILE = os.path.join(DATA, "repos.json")


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
        say(f"watching commits in {root}")
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
    in_waiting, close(). bleak runs on its own asyncio thread. write() never
    blocks: lines are queued and a sender on the bluetooth thread packs them
    into MTU-sized packets, so a burst of lines goes out as a few packets.
    """

    NAME = "Claude Buddy"
    NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"      # advertised service: found even without a name
    NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # laptop -> board
    NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # board -> laptop (music buttons)
    MAX_BACKLOG = 8192                                # unsent bytes before we call the link stalled

    def __init__(self):
        import asyncio
        self.asyncio = asyncio
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.client = None
        self.lost, self.error = False, ""
        self.rssi, self.chunk, self.buttons = None, 20, False
        self.rx, self.rx_lock = bytearray(), threading.Lock()
        self.tx, self.tx_lock = bytearray(), threading.Lock()
        self.fails = 0

    @property
    def in_waiting(self):
        return len(self.rx)

    def _on_rx(self, _char, data):
        with self.rx_lock:
            self.rx += data

    def _run(self, coro, timeout):
        return self.asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _dropped(self, client):
        if client is self.client:                   # not a late callback from an older link
            self.lost = True

    def _match(self, dev, adv):
        if (adv.local_name or dev.name) == self.NAME or self.NUS in (adv.service_uuids or []):
            self.rssi = adv.rssi
            return True
        return False

    def connect(self):
        from bleak import BleakClient, BleakScanner

        async def go():
            dev = await BleakScanner.find_device_by_filter(self._match, timeout=8)
            if dev is None:
                return None
            client = BleakClient(dev, disconnected_callback=self._dropped, timeout=15)
            await client.connect()
            self.buttons = False
            try:
                await client.start_notify(self.NUS_TX, self._on_rx)   # hear the board's buttons
                self.buttons = True
            except Exception as e:
                say(f"bluetooth: board buttons unavailable ({e}) - reflash the board", "warn")
            self.chunk = max(20, min(244, (client.mtu_size or 23) - 3))
            return client

        self.lost, self.error, self.rssi = False, "", None
        with self.tx_lock:
            self.tx.clear()
        try:
            self.client = self._run(go(), 30)
        except Exception as e:                      # bluetooth off, no adapter, timeout...
            say(f"bluetooth: {e or type(e).__name__}", "warn")
            self.client = None
        if self.client is None:
            self.fails += 1
            if self.fails == 3:
                say("bluetooth tips: is Bluetooth on and the board powered? If 'Claude Buddy' "
                    "is paired in Windows Settings > Bluetooth, remove it there - Windows then "
                    "keeps it connected and the board stops advertising.", "warn")
            return False
        self.fails = 0
        self.asyncio.run_coroutine_threadsafe(self._sender(self.client), self.loop)
        return True

    async def _sender(self, client):
        """Drains the queue onto the link, one MTU-sized packet at a time."""
        while client is self.client and not self.lost:
            with self.tx_lock:
                data = bytes(self.tx[:self.chunk])
                del self.tx[:len(data)]
            if not data:
                await self.asyncio.sleep(0.008)
                continue
            try:
                # acknowledged writes: a dropped packet would leave half a line that
                # the board could misread (it once opened the settings menu)
                await client.write_gatt_char(self.NUS_RX, data, response=True)
            except Exception as e:
                self.error = str(e) or type(e).__name__
                self.lost = True

    def write(self, data):
        if self.lost or self.client is None:
            raise OSError("bluetooth disconnected" + (f" ({self.error})" if self.error else ""))
        with self.tx_lock:
            if len(self.tx) > self.MAX_BACKLOG:
                raise OSError("bluetooth link stalled")
            self.tx += data

    def read(self, n):
        with self.rx_lock:
            data, self.rx = bytes(self.rx), bytearray()
        return data

    def close(self):
        client, self.client = self.client, None     # stops the sender
        try:
            if client is not None:
                self._run(client.disconnect(), 5)
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
        self.pos_at = 0.0              # when pos was sampled
        self.track, self.artist = "", ""
        self._meta = ("", "")
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
            say(f"music: no audio meter ({e}) - the equalizer will dance without it")
        self.asyncio = asyncio
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        self.asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._poll())

    async def _title(self, s):
        info = await s.try_get_media_properties_async()
        self._meta = (info.title or "", info.artist or "")
        return " - ".join(x for x in (info.title, info.artist) if x)

    async def _poll(self):
        try:
            from winrt.windows.media.control import \
                GlobalSystemMediaTransportControlsSessionManager as Manager
        except ImportError:
            say("music: pip install winrt-Windows.Media.Control  (to see what's playing)")
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
                self.pos_at = time.time()
                self.track, self.artist = self._meta if state else ("", "")
                self.session_state, self.session_title = state, title
            except Exception:
                self.mgr = None
                state = 0
            await self.asyncio.sleep(0.3 if state == 1 else 1)   # playing: keep the lyrics in step

    @staticmethod
    def _timeline(s, playing):
        """(position, length) in seconds (with fractions), or (0, 0) if the player doesn't share it."""
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
            return max(0.0, min(pos, dur)), dur
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

    def position(self):
        """Where the song is right now, in seconds (between the 1 s samples too)."""
        p = self.pos + (time.time() - self.pos_at if self.state == 1 else 0)
        return min(p, self.dur) if self.dur else p

    def level(self):
        """0-99, perceptual (square root of the peak amplitude)."""
        try:
            return min(99, int((self.meter.GetPeakValue() ** 0.5) * 100)) if self.meter else 0
        except Exception:
            return 0


def song_names(track, artist):
    """Clean up what players report: YouTube gives 'Artist - Song (Official
    Video)' with the channel as artist; Spotify adds ' - Remastered 2011'."""
    t = re.sub(r"[\(\[][^\)\]]*[\)\]]", "", track or "")       # (Official Video) [Lyrics]
    t = t.split("|")[0]
    a = re.sub(r"(?i)\s*-\s*topic$|vevo$|\s*official$", "", artist or "").strip()
    if " - " in t:
        left, right = [x.strip() for x in t.split(" - ", 1)]
        if re.search(r"(?i)remaster|live|version|edit|mix|mono|stereo|from", right):
            t = left                                         # 'Song - Remastered 2011'
        else:
            a, t = left, right                               # 'Artist - Song'
    t = re.sub(r"(?i)\s+(ft\.?|feat\.?|featuring)\s.*$", "", t)
    t = re.sub(r"(?i)\b(official|lyrics?|video|audio|full song|hd|4k)\b", "", t)
    return re.sub(r"\s+", " ", t).strip(" -"), a


class Lyrics:
    """Time-synced lyrics for the song that's playing, from lrclib.net (free,
    no key). Only the song name, artist and length are sent."""

    API = "https://lrclib.net/api/"

    def __init__(self, lead=0.3):
        self.key, self.times, self.texts = None, [], []
        self.status = ""                                     # "", "searching", "found", "none"
        self.lead = lead                                     # show each line this much early (s)
        self.last_i = -1

    def want(self, track, artist, dur):
        key = (track, artist)
        if key == self.key:
            return
        self.key, self.times, self.texts = key, [], []
        self.last_i = -1
        self.status = "searching" if track else ""
        if track:
            threading.Thread(target=self._fetch, args=(key, dur), daemon=True).start()

    def _fetch(self, key, dur):
        from urllib.parse import urlencode
        track, artist = song_names(*key)
        hits = []
        first = re.split(r"\s*(?:,|&| and | x | feat\.? | ft\.? )\s*", artist or "", flags=re.I)[0]
        tries = []                                            # most exact first
        if artist:
            tries.append({"track_name": track, "artist_name": artist})
        if first and first != artist:
            tries.append({"track_name": track, "artist_name": first})   # 'Noor, Khan and X' -> 'Noor'
        tries.append({"q": f"{first} {track}".strip()})
        tries.append({"track_name": track})                   # song name only: matched by length below
        for params in tries:
            try:
                found = http_json(self.API + "search?" + urlencode(params))
            except Exception as e:
                say(f"lyrics: couldn't search ({e})", "warn")
                break
            found = [h for h in found or [] if h.get("syncedLyrics")]
            if "artist_name" not in params and "q" not in params and dur:
                found = [h for h in found if abs((h.get("duration") or 0) - dur) < 4]   # same song?
            if found:
                hits = found
                break
        hits = [h for h in hits or [] if h.get("syncedLyrics")]
        if dur:                                              # the right version: closest length
            hits.sort(key=lambda h: abs((h.get("duration") or 0) - dur))
            hits = [h for h in hits if abs((h.get("duration") or 0) - dur) < 15] or hits[:1]
        if self.key != key:                                  # the song changed meanwhile
            return
        if not hits:
            self.status = "none"
            say(f"lyrics: none found for {track}" + (f" - {artist}" if artist else ""))
            return
        times, texts = [], []
        for ln in hits[0]["syncedLyrics"].splitlines():
            stamps = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", ln)
            text = re.sub(r"\[[^\]]*\]", "", ln).strip()
            for m, sec in stamps:
                times.append(int(m) * 60 + float(sec))
                texts.append(text)
        order = sorted(range(len(times)), key=times.__getitem__)
        self.times, self.texts = [times[i] for i in order], [texts[i] for i in order]
        self.status = "found"
        say(f"♪ lyrics found: {hits[0].get('trackName', track)} - {hits[0].get('artistName', artist)}", "music")

    def at(self, pos):
        """(index, text, seconds until the next line) at song position pos, or None."""
        import bisect
        i = bisect.bisect_right(self.times, pos + self.lead) - 1   # a hair early: time to read
        if 0 <= self.last_i < len(self.times) and i < self.last_i and pos + self.lead > self.times[self.last_i] - 1.5:
            i = self.last_i                                  # a wobble, not a seek: stay on this line
        self.last_i = i
        if i < 0:
            return None
        nxt = self.times[i + 1] if i + 1 < len(self.times) else self.times[i] + 6
        return i, self.texts[i], max(0.5, min(nxt - self.times[i], 12))


# common words the letter-by-letter spelling gets wrong -> how people write them
HINGLISH = {
    "nheen": "nahin", "nhin": "nahin", "nhi": "nahi", "too": "tu", "men": "mein", "mai": "main",
    "hain": "hain", "kyon": "kyun", "kyaa": "kya", "yh": "yeh", "vh": "woh", "vo": "woh",
    "jb": "jab", "tb": "tab", "kb": "kab", "sb": "sab", "pr": "par", "dil": "dil", "mn": "man",
    "gliyon": "galiyon", "khoon": "khoon", "pyaar": "pyaar", "jaan": "jaan", "rh": "reh",
    "khn": "kahan", "khaan": "kahan", "yhaan": "yahan", "vhaan": "wahan", "kuchh": "kuch",
}


def lyric_text(text):
    """Lyrics in any script -> something the OLED's ASCII font can draw."""
    if any(ord(c) > 127 for c in text):
        hindi = any("ऀ" <= c <= "ॿ" for c in text)
        try:
            from unidecode import unidecode                  # Hindi, accents... -> Latin letters
            text = unidecode(text)
        except ImportError:
            pass
        if hindi:                                            # ...spelled the way people text it
            text = text.replace("N", "n")                    # nasal: maaNgaa -> maangaa
            text = re.sub(r"([tdTD])\1+", r"\1", text)       # ttuuttte -> tuute
            text = text.replace("uu", "oo").replace("ii", "ee")
            text = re.sub(r"aa\b", "a", text)                # thaa -> tha
            text = re.sub(r"ee\b", "i", text).replace("'", "")   # meree -> meri
            text = re.sub(r"\b\w+\b", lambda m: HINGLISH.get(m.group(0).lower(), m.group(0)), text)
    return oled_text(text, 60)


def claude_token():
    """Claude Code's login token: ~/.claude/.credentials.json, or the macOS Keychain."""
    path = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)["claudeAiOauth"]["accessToken"]
    if sys.platform == "darwin":
        r = subprocess.run(["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode == 0 and r.stdout.strip():
            return json.loads(r.stdout)["claudeAiOauth"]["accessToken"]
    raise RuntimeError("not logged in to Claude Code")


class MacMusicWatcher:
    """What's playing in Spotify or Apple Music on a Mac (AppleScript). Same
    interface as MusicWatcher; there's no output meter, so level() is 0 and
    the board's equalizer dances on its own."""

    APPS = ("Spotify", "Music")
    SCRIPT = """
if application "%(app)s" is running then
  tell application "%(app)s"
    set st to player state as string
    if st is "playing" or st is "paused" then
      set d to duration of current track
      return st & "|" & (name of current track) & "|" & (artist of current track) & "|" & (player position as string) & "|" & (d as string)
    end if
  end tell
end if
return ""
"""

    def __init__(self):
        self.playing, self.title, self.state = False, "", 0
        self.track, self.artist = "", ""
        self.pos, self.dur, self.pos_at = 0, 0, 0.0
        self.app = None
        self._found = (0, "", "", "", 0.0, 0.0)
        threading.Thread(target=self._run, daemon=True).start()

    def _ask(self, app):
        r = subprocess.run(["osascript", "-e", self.SCRIPT % {"app": app}],
                           capture_output=True, text=True, timeout=5)
        return r.stdout.strip()

    def _run(self):
        while True:
            found = (0, "", "", "", 0.0, 0.0)
            for app in self.APPS:
                try:
                    out = self._ask(app)
                except (OSError, subprocess.SubprocessError):
                    continue
                parts = out.split("|")
                if len(parts) < 5:
                    continue
                st, name, artist = parts[0], parts[1], parts[2]
                num = lambda x: float(x.replace(",", ".") or 0)   # some locales write 12,5
                pos, dur = num(parts[3]), num(parts[4])
                if app == "Spotify":
                    dur /= 1000                                    # Spotify says milliseconds
                state = 1 if st == "playing" else 2
                if state == 1 or not found[0]:
                    found = (state, name, artist, app, pos, dur)
                if state == 1:
                    break
            self._found = found
            self.pos_at = time.time()
            time.sleep(1)

    def tick(self):
        state, name, artist, app, pos, dur = self._found
        self.state, self.app = state, app or None
        self.track, self.artist = (name, artist) if state else ("", "")
        self.title = " - ".join(x for x in (name, artist) if x)
        self.pos, self.dur = int(pos), int(dur)
        self.playing = state == 1

    def position(self):
        state, _, _, _, pos, dur = self._found
        p = pos + (time.time() - self.pos_at if state == 1 else 0)
        return min(p, dur) if dur else p

    def level(self):
        return 0

    def control(self, cmd):
        if not self.app:
            return
        verb = {"toggle": "playpause", "next": "next track", "prev": "previous track"}.get(cmd)
        if verb:
            subprocess.Popen(["osascript", "-e", f'tell application "{self.app}" to {verb}'])


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
                    say(f"usage: couldn't fetch ({e})", "warn")
                    self.err = str(e)
            time.sleep(self.EVERY_S)

    def _fetch(self):
        import datetime
        import urllib.request
        token = claude_token()
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


# ------------------------------------------------------------- life around you
# Welcome back, meetings, notifications, to-dos, streaks & badges.

CONFIG_FILE = os.path.join(DATA, "config.json")      # --ics links, remembered
PROGRESS_FILE = os.path.join(DATA, "progress.json")  # streak days + unlocked badges


def load_json(path, default):
    try:
        with open(path, encoding="utf-8-sig") as f:     # -sig: also files saved with a BOM
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, data):
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)
    except OSError:
        pass


def http_json(url, timeout=15):
    import urllib.request
    req = urllib.request.Request(url, headers={"User-Agent": "claude-buddy-bridge"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


FIRMWARE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware")
FIRMWARE_URL = "https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/bridge/firmware/"
FIRMWARE_PARTS = [("0x0", "bootloader.bin"), ("0x8000", "partitions.bin"),
                  ("0xe000", "boot_app0.bin"), ("0x10000", "claude-buddy.bin")]


def vkey(v):
    """'1.10.2' -> (1, 10, 2), for comparing versions."""
    return tuple(int(x) for x in re.findall(r"\d+", str(v or ""))[:3])


class Firmware:
    """Which firmware the board runs (it says so when asked), the newest one this
    computer can install - the one that shipped with Claude Buddy, or a newer one
    published on GitHub (downloaded and checked against its sha256) - and putting
    it on the board over USB, with progress for the settings page."""

    CHECK_EVERY = 6 * 3600

    def __init__(self):
        self.board = None                  # version the board reports; "" = too old to say
        self.best = self._local(FIRMWARE_DIR)
        self.state, self.pct, self.msg = "idle", 0, ""
        self.finished = False              # a flash just ended: the bridge reconnects
        threading.Thread(target=self._check, daemon=True).start()

    @staticmethod
    def _local(folder):
        meta = load_json(os.path.join(folder, "version.json"), {})
        ok = meta.get("version") and all(os.path.exists(os.path.join(folder, f)) for _, f in FIRMWARE_PARTS)
        return (str(meta["version"]), folder) if ok else None

    @property
    def update(self):
        return self.board is not None and self.best is not None and vkey(self.best[0]) > vkey(self.board)

    def _check(self):
        import hashlib
        import urllib.request
        time.sleep(20)
        while True:
            try:
                meta = http_json(FIRMWARE_URL + "version.json")
                ver = str(meta.get("version", ""))
                if vkey(ver) > vkey(self.best[0] if self.best else "0"):
                    folder = os.path.join(DATA, "firmware", re.sub(r"[^0-9A-Za-z.]", "", ver))
                    if not self._local(folder):
                        os.makedirs(folder, exist_ok=True)
                        for _, name in FIRMWARE_PARTS:
                            with urllib.request.urlopen(FIRMWARE_URL + name, timeout=60) as r:
                                data = r.read(8 * 1024 * 1024)
                            if hashlib.sha256(data).hexdigest() != meta["files"][name]:
                                raise ValueError(f"{name} didn't match its checksum")
                            with open(os.path.join(folder, name), "wb") as f:
                                f.write(data)
                        save_json(os.path.join(folder, "version.json"), meta)
                    got = self._local(folder)
                    if got:
                        self.best = got
                        say(f"firmware {ver} is out - update the board from the settings page", "ok")
            except Exception:
                pass                       # offline, or nothing newer published: look again later
            time.sleep(self.CHECK_EVERY)

    def flash(self, port):
        ver, folder = self.best
        files = [(addr, os.path.join(folder, name)) for addr, name in FIRMWARE_PARTS]
        sizes = [max(1, os.path.getsize(f)) for _, f in files]
        self.state, self.pct, self.msg, self.finished = "flashing", 0, f"Installing firmware {ver}", False

        def go():
            cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "--port", port,
                   "--baud", "921600", "write_flash"]
            for addr, f in files:
                cmd += [addr, f]
            last = ""
            try:
                pr = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      creationflags=0x08000000 if sys.platform == "win32" else 0)
                part, buf = 0, ""
                while True:
                    chunk = os.read(pr.stdout.fileno(), 512)
                    if not chunk:
                        break
                    buf += chunk.decode(errors="ignore")
                    *done, buf = re.split(r"[\r\n]", buf)
                    for seg in done:
                        seg = seg.strip()
                        if seg:
                            last = seg
                        if "Hash of data verified" in seg:
                            part = min(part + 1, len(files) - 1)
                        m = re.search(r"(\d+(?:\.\d+)?)\s?%", seg) if "Writing at" in seg else None
                        if m:
                            cur = float(m.group(1)) / 100
                            self.pct = int(100 * (sum(sizes[:part]) + sizes[part] * cur) / sum(sizes))
                ok = pr.wait() == 0
            except Exception as e:
                ok, last = False, str(e)
            if ok:
                self.state, self.pct, self.msg = "done", 100, f"Updated to firmware {ver}"
                say(f"firmware {ver} installed - the board restarts", "ok")
            else:
                self.state, self.msg = "error", ("Update failed: " + last)[:160]
                say(f"firmware update failed: {last}", "err")
            self.finished = True

        threading.Thread(target=go, daemon=True).start()


APP_REPO = "SauravDutta002/Making-Claud"
APP_VERSION_URL = f"https://raw.githubusercontent.com/{APP_REPO}/main/pyproject.toml"
APP_PACKAGE = f"claude-buddy @ https://github.com/{APP_REPO}/archive/refs/heads/main.zip"


def find_uv():
    for c in (shutil.which("uv"), os.path.expanduser("~/.local/bin/uv.exe"), os.path.expanduser("~/.local/bin/uv"),
              os.path.expanduser("~/.cargo/bin/uv.exe"), os.path.expanduser("~/.cargo/bin/uv")):
        if c and os.path.exists(c):
            return c
    return None


class AppUpdates:
    """Keeps Claude Buddy itself up to date: looks at the version on GitHub now and
    then, and installs a newer one (the same way the install command does) when
    Claude Code has been quiet for a while, then starts again. Your stats, streaks
    and settings live in the data folder, so they're kept."""

    CHECK_EVERY = 3 * 3600
    QUIET_FOR = 10 * 60            # no Claude Code activity for this long before restarting

    def __init__(self):
        self.latest, self.state, self.msg = None, "idle", ""
        # only an installed copy updates itself (not one run from a source folder)
        self.can = bool(__package__) and find_uv() is not None
        threading.Thread(target=self._check, daemon=True).start()

    @property
    def available(self):
        return bool(self.latest) and vkey(self.latest) > vkey(VERSION)

    def _check(self):
        import urllib.request
        time.sleep(60)
        while True:
            try:
                req = urllib.request.Request(APP_VERSION_URL, headers={"User-Agent": "claude-buddy-bridge"})
                with urllib.request.urlopen(req, timeout=15) as r:
                    text = r.read(50000).decode("utf-8", "ignore")
                m = re.search(r'^version\s*=\s*"([^"]+)"', text, re.M)
                if m:
                    if m.group(1) != self.latest and vkey(m.group(1)) > vkey(VERSION):
                        say(f"Claude Buddy {m.group(1)} is out (you have {VERSION})", "ok")
                    self.latest = m.group(1)
            except Exception:
                pass                       # offline: look again later
            time.sleep(self.CHECK_EVERY)

    def start(self):
        """Install the new version and come back. On Windows a separate little script
        does it (the running files can't be replaced from inside); elsewhere the
        install runs here and Claude Buddy restarts itself."""
        if self.state == "updating" or not self.can:
            return
        uv = find_uv()
        self.state, self.msg = "updating", f"Updating Claude Buddy to {self.latest}"
        say(f"updating Claude Buddy {VERSION} -> {self.latest}...", "info")
        if sys.platform == "win32":
            try:
                self._windows(uv)
            except Exception as e:
                self.state, self.msg = "error", f"Update didn't start: {e}"
                say(self.msg, "err")
            return

        def go():
            r = subprocess.run([uv, "tool", "install", "--force", "--reinstall", "--python", "3.12", APP_PACKAGE],
                               capture_output=True, text=True)
            if r.returncode:
                self.state, self.msg = "error", "Update failed: " + (r.stderr.strip().splitlines() or ["?"])[-1][:140]
                say(self.msg, "err")
                return
            say("updated - restarting", "ok")
            os.execv(sys.executable, [sys.executable, "-m", "claude_buddy.claude_bridge"] + sys.argv[1:])

        threading.Thread(target=go, daemon=True).start()

    def _windows(self, uv):
        bin_dir = subprocess.run([uv, "tool", "dir", "--bin"], capture_output=True, text=True,
                                 creationflags=0x08000000).stdout.strip()
        log = os.path.join(DATA, "update.log")
        script = os.path.join(DATA, "update.ps1")
        q = lambda s: "'" + str(s).replace("'", "''") + "'"
        with open(script, "w", encoding="utf-8-sig") as f:
            f.write(f"""# written by Claude Buddy to update itself
$ErrorActionPreference = 'Continue'
& {{
  Write-Output ("--- " + (Get-Date) + "  updating Claude Buddy")
  Disable-ScheduledTask -TaskName 'Claude Buddy' -ErrorAction SilentlyContinue | Out-Null
  Stop-ScheduledTask -TaskName 'Claude Buddy' -ErrorAction SilentlyContinue
  Get-Process claude-buddy, claude-buddy-service -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Get-CimInstance Win32_Process -Filter "Name like 'python%'" | Where-Object {{ $_.CommandLine -match 'claude_bridge' }} |
    ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }}
  Start-Sleep 2
  & {q(uv)} tool install --force --reinstall --python 3.12 {q(APP_PACKAGE)}
  if ($LASTEXITCODE -eq 0) {{ & {q(os.path.join(bin_dir, 'claude-buddy-setup.exe'))} }}
  Enable-ScheduledTask -TaskName 'Claude Buddy' -ErrorAction SilentlyContinue | Out-Null
  Start-ScheduledTask -TaskName 'Claude Buddy' -ErrorAction SilentlyContinue
  if (-not $?) {{ Start-Process -FilePath {q(os.path.join(bin_dir, 'claude-buddy-service.exe'))} }}
  Write-Output ("--- " + (Get-Date) + "  done")
}} *>> {q(log)}
""")
        cmd = f'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{script}"'
        # started through WMI, so it isn't closed together with Claude Buddy (and the task) it stops
        ps = ("$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=" + q(cmd)
              + "}; exit $r.ReturnValue")
        r = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                           timeout=60, creationflags=0x08000000)
        if r.returncode:
            raise RuntimeError(f"WMI returned {r.returncode}")


class AwayWatcher:
    """'Oh, you're back!' - no keyboard or mouse for AWAY_S, then input again.
    Reads the system's own idle timer (Windows GetLastInputInfo, macOS
    IOHIDSystem); nothing is recorded."""

    AWAY_S = 600

    def __init__(self):
        self.ok = sys.platform == "win32"
        self.away_since = None
        self.next_check = 0.0
        if self.ok:
            import ctypes

            class LASTINPUTINFO(ctypes.Structure):
                _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]
            self.ct, self.info = ctypes, LASTINPUTINFO()
            self.info.cbSize = ctypes.sizeof(LASTINPUTINFO)

    def idle(self):
        if sys.platform == "darwin":
            try:
                out = subprocess.run(["ioreg", "-c", "IOHIDSystem"], capture_output=True,
                                     text=True, timeout=5).stdout
                m = re.search(r'"HIDIdleTime" = (\d+)', out)
                return int(m.group(1)) / 1e9 if m else 0.0
            except (OSError, subprocess.SubprocessError):
                return 0.0
        if not self.ok or not self.ct.windll.user32.GetLastInputInfo(self.ct.byref(self.info)):
            return 0.0
        return ((self.ct.windll.kernel32.GetTickCount() - self.info.dwTime) & 0xFFFFFFFF) / 1000.0

    def poll(self):
        """Minutes you were away - once, the moment you're back. Else None."""
        now = time.time()
        if now < self.next_check:
            return None
        self.next_check = now + 1
        idle = self.idle()
        if idle >= self.AWAY_S:
            if self.away_since is None:
                self.away_since = now - idle
            return None
        if self.away_since is not None and idle < 5:
            mins, self.away_since = int((now - self.away_since) // 60), None
            return max(1, mins)
        return None


class CalendarWatcher:
    """Your next meetings, from the Windows calendar (accounts added to Windows'
    Calendar/Outlook app) and any iCal links (--ics: Google Calendar > settings >
    'Secret address in iCal format', or Outlook > publish calendar)."""

    EVERY_S = 120
    ALERTS = (10, 5, 1, 0)                               # minutes before: alert the board

    def __init__(self, ics_urls):
        self.ics = list(ics_urls)
        self.events = []                                 # [(start epoch, title, key)]
        self.alerted = {}                                # key -> last alert threshold
        self.err = {}
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        import asyncio
        loop = asyncio.new_event_loop()
        while True:
            evs = []
            if sys.platform == "win32":
                try:
                    evs += loop.run_until_complete(self._windows())
                except Exception as e:
                    self._warn("Windows calendar", e)
            for url in self.ics:
                try:
                    evs += self._ics(url)
                except Exception as e:
                    self._warn("iCal link", e)
            self.events = sorted(evs)
            time.sleep(self.EVERY_S)

    def _warn(self, src, e):
        msg = str(e) or type(e).__name__
        if self.err.get(src) != msg:
            self.err[src] = msg
            say(f"calendar: {src} unavailable ({msg})", "warn")

    async def _windows(self):
        import datetime
        from winrt.windows.applicationmodel.appointments import (
            AppointmentManager, AppointmentStoreAccessType)
        st = await AppointmentManager.request_store_async(AppointmentStoreAccessType.ALL_CALENDARS_READ_ONLY)
        now = datetime.datetime.now(datetime.timezone.utc)
        out = []
        for a in await st.find_appointments_async(now - datetime.timedelta(minutes=5),
                                                   datetime.timedelta(hours=12)):
            if a.all_day:
                continue
            start = a.start_time.timestamp()
            out.append((start, a.subject or "meeting", f"w:{a.local_id}:{int(start)}"))
        return out

    def _ics(self, url):
        import datetime
        import urllib.request
        try:
            import icalendar
            import recurring_ical_events
        except ImportError:
            raise RuntimeError("pip install icalendar recurring-ical-events")
        url = url.replace("webcal://", "https://", 1)
        req = urllib.request.Request(url, headers={"User-Agent": "claude-buddy-bridge"})
        with urllib.request.urlopen(req, timeout=20) as r:
            cal = icalendar.Calendar.from_ical(r.read())
        now = datetime.datetime.now().astimezone()
        out = []
        for ev in recurring_ical_events.of(cal).between(now - datetime.timedelta(minutes=5),
                                                        now + datetime.timedelta(hours=12)):
            s = ev.get("DTSTART").dt
            if not isinstance(s, datetime.datetime) or str(ev.get("STATUS", "")).upper() == "CANCELLED":
                continue                                 # all-day or cancelled
            if s.tzinfo is None:
                s = s.astimezone()
            out.append((s.timestamp(), str(ev.get("SUMMARY", "meeting")),
                        f"i:{ev.get('UID', '')}:{int(s.timestamp())}"))
        return out

    def due(self):
        """[(minutes, title)] for meetings crossing 10 / 5 / 1 / 0 minutes away."""
        now, out = time.time(), []
        for start, title, key in self.events:
            left = start - now
            if left < -60 or left > 10 * 60 + 30:
                continue
            mins = max(0, math.ceil(left / 60))
            stage = min(t for t in self.ALERTS if mins <= t) if mins <= self.ALERTS[0] else None
            if stage is not None and stage < self.alerted.get(key, 99):
                self.alerted[key] = stage
                out.append((mins, title))
        return out

    def next(self):
        now = time.time()
        return next(((s, t) for s, t, _ in self.events if s > now - 60), None)


class NotifyWatcher:
    """New GitHub notifications and your PRs' reviews (gh CLI), plus messages
    from chat apps (Slack, Teams, Discord...) via Windows' notification
    listener. Queued for the board's banner, one at a time."""

    GH_EVERY_S = 90
    REASONS = {"review_requested": "review requested", "mention": "mentioned you",
               "team_mention": "team mentioned", "assign": "assigned to you",
               "comment": "new comment", "ci_activity": "CI finished", "author": "update",
               "state_change": "state changed", "approval_requested": "approval requested",
               "security_alert": "security alert", "subscribed": "update", "manual": "update"}
    TYPES = {"PullRequest": "PR", "Issue": "issue", "Release": "release",
             "CheckSuite": "CI", "Discussion": "discussion"}
    PR_QUERY = ("query={viewer{pullRequests(states:OPEN,first:30,orderBy:{field:UPDATED_AT,direction:DESC})"
                "{nodes{number title reviewDecision repository{name}}}}}")

    def __init__(self, apps, github=True, toasts=True):
        self.queue = deque(maxlen=12)
        self.apps = [x.strip().lower() for x in apps.split(",") if x.strip()]
        self.err = {}
        if github:
            threading.Thread(target=self._github, daemon=True).start()
        if toasts and sys.platform == "win32" and self.apps:
            threading.Thread(target=self._toasts, daemon=True).start()

    def _push(self, icon, text):
        self.queue.append((icon, text))
        say("◆ " + text, "notify")

    def _warn(self, src, e):
        msg = str(e) or type(e).__name__
        if self.err.get(src) != msg:
            self.err[src] = msg
            say(f"notify: {src} unavailable ({msg})", "warn")

    @staticmethod
    def _gh(*args):
        r = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8", timeout=30,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r.returncode:
            raise RuntimeError(((r.stderr or "").strip().splitlines() or ["gh failed"])[-1])
        return json.loads(r.stdout or "null")

    def _github(self):
        seen, reviews, first = set(), {}, True
        while True:
            try:
                for n in self._gh("api", "notifications") or []:
                    nid = n["id"] + n.get("updated_at", "")
                    if nid in seen:
                        continue
                    seen.add(nid)
                    if first:                               # what's already there isn't news
                        continue
                    subj = n.get("subject") or {}
                    url = subj.get("url") or ""
                    num = " #" + url.rsplit("/", 1)[-1] if ("/pulls/" in url or "/issues/" in url) else ""
                    what = (self.TYPES.get(subj.get("type"), "") + num).strip()
                    reason = self.REASONS.get(n.get("reason"), n.get("reason", ""))
                    self._push(1, f"{n['repository']['name']}: {what} {reason} - {subj.get('title', '')}")
                data = self._gh("api", "graphql", "-f", self.PR_QUERY)
                for pr in data["data"]["viewer"]["pullRequests"]["nodes"]:
                    key, dec = f"{pr['repository']['name']}#{pr['number']}", pr.get("reviewDecision")
                    if key in reviews and dec != reviews[key]:
                        if dec == "APPROVED":
                            self._push(3, f"PR #{pr['number']} approved! {pr['title']}")
                        elif dec == "CHANGES_REQUESTED":
                            self._push(1, f"PR #{pr['number']}: changes requested - {pr['title']}")
                    reviews[key] = dec
                first = False
                if len(seen) > 3000:
                    seen = set(list(seen)[-1000:])
                self.err.pop("GitHub", None)
            except FileNotFoundError:
                say("notify: GitHub CLI (gh) not installed - GitHub alerts off", "warn")
                return
            except Exception as e:
                self._warn("GitHub", e)
            time.sleep(self.GH_EVERY_S)

    def _toasts(self):
        import asyncio
        try:
            from winrt.windows.ui.notifications import KnownNotificationBindings, NotificationKinds
            from winrt.windows.ui.notifications.management import UserNotificationListener
        except ImportError:
            say("notify: pip install winrt-Windows.UI.Notifications.Management "
                "winrt-Windows.UI.Notifications  (for Slack / Teams messages)", "warn")
            return

        async def go():
            lst = UserNotificationListener.current
            if int(await lst.request_access_async()) != 1:
                say("notify: allow notification access in Windows Settings > Privacy > Notifications", "warn")
                return
            seen, first = set(), True
            while True:
                try:
                    for n in await lst.get_notifications_async(NotificationKinds.TOAST):
                        if n.id in seen:
                            continue
                        seen.add(n.id)
                        if first:
                            continue
                        try:
                            app = n.app_info.display_info.display_name
                        except Exception:
                            continue
                        if not any(x in app.lower() for x in self.apps):
                            continue
                        texts = []
                        b = n.notification.visual.get_binding(KnownNotificationBindings.toast_generic)
                        if b is not None:
                            texts = [t.text for t in b.get_text_elements() if t.text]
                        self._push(2, f"{app}: " + " - ".join(texts[:2]))
                    first = False
                except Exception as e:
                    self._warn("Windows notifications", e)
                await asyncio.sleep(3)

        asyncio.new_event_loop().run_until_complete(go())


class Todos:
    """Claude's to-do list (from the hook) -> Y:<done>,<total>,<current item>."""

    def __init__(self):
        self.items, self.session, self.next_id = [], None, 1

    def apply(self, msg):
        """Returns how many items just got ticked off, or None if nothing changed."""
        before, done_before = self.line(), self.done()
        if msg.get("s") and msg["s"] != self.session:   # a new Claude session: a new list
            self.items, self.session, self.next_id = [], msg["s"], 1
        op = msg.get("op")
        if op == "write":
            self.items = [{"id": str(i + 1), "text": t, "status": s}
                          for i, (s, t) in enumerate(msg.get("todos") or [])]
        elif op == "create":
            self.items.append({"id": str(self.next_id), "text": msg.get("text", ""), "status": "pending"})
            self.next_id += 1
        elif op == "update":
            it = next((x for x in self.items if x["id"] == msg.get("id")), None)
            if it is not None:
                if msg.get("status") == "deleted":
                    self.items.remove(it)
                elif msg.get("status"):
                    it["status"] = msg["status"]
                if msg.get("text"):
                    it["text"] = msg["text"]
        if self.line() == before:
            return None
        return max(0, self.done() - done_before)

    def done(self):
        return sum(x["status"] == "completed" for x in self.items)

    def current(self):
        it = (next((x for x in self.items if x["status"] == "in_progress"), None)
              or next((x for x in self.items if x["status"] == "pending"), None))
        return it["text"] if it else ""

    def line(self):
        return "Y:%d,%d,%s" % (self.done(), len(self.items), oled_text(self.current(), 30))


class Progress:
    """Streaks and achievements: which days you shipped something and which
    badges you've unlocked, kept in progress.json. record() returns V: lines."""

    # id, title, icon (0 trophy, 1 flame, 2 star)
    BADGES = [
        ("first_commit", "First commit!", 0),
        ("commits_10", "10 commits in a day", 0),
        ("commits_100", "100 commits!", 0),
        ("tasks_5", "5 tasks in a day", 2),
        ("tasks_50", "50 tasks done", 0),
        ("tests_10", "Bug hunter: 10 passes", 2),
        ("todos_25", "Checklist hero", 2),
        ("lines_1000", "1000 lines session", 2),
        ("night_owl", "Night owl", 2),
        ("early_bird", "Early bird", 2),
        ("weekend", "Weekend warrior", 0),
        ("streak_3", "3 day streak", 1),
        ("streak_7", "7 day streak", 1),
        ("streak_30", "30 day streak!", 1),
    ]

    def __init__(self):
        d = load_json(PROGRESS_FILE, {})
        self.days = d.get("days", {})
        self.unlocked = d.get("unlocked", {})
        self.totals = {"commits": 0, "tasks": 0, "tests": 0, "todos": 0, **d.get("totals", {})}
        self.streak_shown = d.get("streak_shown", "")

    def save(self):
        save_json(PROGRESS_FILE, {"days": self.days, "unlocked": self.unlocked,
                                  "totals": self.totals, "streak_shown": self.streak_shown})

    def _active(self, day):
        x = self.days.get(day) or {}
        return bool(x.get("commits") or x.get("tasks"))

    def streak(self):
        import datetime
        d = datetime.date.today()
        if not self._active(d.isoformat()):             # today hasn't started: still alive
            d -= datetime.timedelta(days=1)
        n = 0
        while self._active(d.isoformat()):
            n += 1
            d -= datetime.timedelta(days=1)
        return n

    def _unlock(self, checks):
        out, day = [], time.strftime("%Y-%m-%d")
        for bid, title, icon in self.BADGES:
            if checks.get(bid) and bid not in self.unlocked:
                self.unlocked[bid] = day
                out.append(f"V:{icon},{title}")
        return out

    def record(self, key, n=1):
        if n <= 0:
            return []
        day = time.strftime("%Y-%m-%d")
        rec = self.days.setdefault(day, {"commits": 0, "tasks": 0, "tests": 0, "todos": 0})
        rec[key] = rec.get(key, 0) + n
        self.totals[key] = self.totals.get(key, 0) + n
        lt, s = time.localtime(), self.streak()
        out = self._unlock({
            "first_commit": self.totals["commits"] >= 1,
            "commits_10": rec["commits"] >= 10,
            "commits_100": self.totals["commits"] >= 100,
            "tasks_5": rec["tasks"] >= 5,
            "tasks_50": self.totals["tasks"] >= 50,
            "tests_10": rec["tests"] >= 10,
            "todos_25": self.totals["todos"] >= 25,
            "night_owl": key == "commits" and lt.tm_hour < 4,
            "early_bird": key == "commits" and 5 <= lt.tm_hour < 7,
            "weekend": key == "commits" and lt.tm_wday >= 5,
            "streak_3": s >= 3, "streak_7": s >= 7, "streak_30": s >= 30,
        })
        if s >= 2 and self.streak_shown != day and self._active(day):
            self.streak_shown = day                     # first work of the day: keep it going!
            if not any(v.startswith("V:1,") for v in out):   # (a streak badge already says it)
                out.insert(0, f"V:1,{s} day streak!")
        if len(self.days) > 400:                        # keep about a year
            for k in sorted(self.days)[:-400]:
                del self.days[k]
        self.save()
        return out

    def seed(self, day_stats):
        """Count today's work from before the streak tracker ran (stats.json);
        badges that earns unlock quietly."""
        day = time.strftime("%Y-%m-%d")
        if day_stats.get("date") != day:
            return
        rec = self.days.setdefault(day, {"commits": 0, "tasks": 0, "tests": 0, "todos": 0})
        for k in ("commits", "tasks", "tests"):
            extra = day_stats.get(k, 0) - rec.get(k, 0)
            if extra > 0:
                rec[k] = rec.get(k, 0) + extra
                self.totals[k] = self.totals.get(k, 0) + extra
        self._unlock({"first_commit": self.totals["commits"] >= 1, "commits_10": rec["commits"] >= 10,
                      "tasks_5": rec["tasks"] >= 5, "tests_10": rec["tests"] >= 10,
                      "tasks_50": self.totals["tasks"] >= 50, "commits_100": self.totals["commits"] >= 100})
        self.save()

    def session_lines(self, added):
        out = self._unlock({"lines_1000": added >= 1000})
        if out:
            self.save()
        return out


# ------------------------------------------------------------------ dashboard
# A live, full-screen view of what the bridge is doing. Plain ANSI escapes, no
# extra packages. Falls back to ordinary log lines when the output isn't a
# terminal (or with --plain).

ESC = "\x1b["
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

CORAL, PEACH = "38;2;217;119;87", "38;2;240;175;145"
GREEN, YELLOW, RED = "38;2;125;205;135", "38;2;235;195;95", "38;2;240;105;95"
CYAN, VIOLET = "38;2;110;190;230", "38;2;185;145;240"
GREY, FAINT = "38;2;140;140;150", "38;2;85;85;95"

# board event -> (what it means, colour, eyes, mouth, extra) for Claude's face
EVENTS = {
    "think":   ("thinking",            VIOLET, "o", ".", " ?"),
    "read":    ("reading files",       CYAN,   "o", "_", ""),
    "type":    ("editing code",        CORAL,  ">", "_", " ~"),
    "write":   ("creating a file",     CORAL,  "^", "o", " +"),
    "search":  ("searching",           CYAN,   "o", "_", " >>"),
    "web":     ("browsing the web",    CYAN,   "@", "_", ""),
    "bash":    ("running a command",   YELLOW, "-", "_", " $"),
    "ps":      ("running a command",   YELLOW, "-", "_", " $"),
    "build":   ("building",            YELLOW, "-", "_", " #"),
    "git":     ("committing",          GREEN,  "^", "_", " *"),
    "plane":   ("pushing to remote",   GREEN,  "^", "o", " >>"),
    "merge":   ("merging",             YELLOW, "o", "_", " <>"),
    "test":    ("running tests",       YELLOW, "o", "_", " ..."),
    "bug":     ("tests passed!",       GREEN,  "^", "v", " !"),
    "fail":    ("something failed",    RED,    "x", "_", " !"),
    "limit":   ("out of usage",        RED,    "x", "_", " !"),
    "pkg":     ("installing packages", YELLOW, "o", "_", " []"),
    "need":    ("needs you!",          RED,    "O", "o", " !!"),
    "done":    ("all done",            GREEN,  "^", "v", " *"),
    "compact": ("compacting memory",   VIOLET, "-", "_", " ..."),
    "idle":    ("waiting for you",     GREY,   "o", "_", ""),
    "key":     ("you're typing",       PEACH,  "o", "_", " ..."),
}

UI = None   # the Dashboard, once main() starts it

_hooked = [0.0, None]


def claude_hooked():
    """Are our hooks in Claude Code's settings? (checked at most every 10 s)"""
    if time.time() - _hooked[0] > 10:
        try:
            with open(os.path.join(os.path.expanduser("~"), ".claude", "settings.json"), encoding="utf-8-sig") as f:
                _hooked[1] = "claude_hook" in f.read()
        except OSError:
            _hooked[1] = False
        _hooked[0] = time.time()
    return _hooked[1]


def say(msg, kind="info"):
    """Log a line: into the dashboard's activity feed, or plain stdout."""
    if UI is not None and UI.live:
        UI.log(msg, kind)
    else:
        print(msg)
        if UI is not None:                    # still kept for the settings page's activity list
            UI.events.append((time.strftime("%H:%M:%S"), kind, str(msg)))


def paint(code, s):
    return f"{ESC}{code}m{s}{ESC}0m" if code else str(s)


def vis(s):
    return len(ANSI_RE.sub("", s))


def clip(s, n):
    s = str(s)
    return s if len(s) <= n else s[:max(0, n - 1)] + "…"


def mmss(sec):
    sec = int(sec)
    return f"{sec // 60}:{sec % 60:02}"


def bar(pct, w):
    """A smooth meter: green -> yellow -> red as it fills."""
    if pct is None or pct < 0:
        return paint(FAINT, "·" * w) + paint(FAINT, "    –")
    p = max(0, min(100, pct))
    col = GREEN if p < 50 else YELLOW if p < 80 else RED
    fill = p * w / 100
    n, frac = int(fill), int((fill - int(fill)) * 8)
    s = "█" * n + ("▏▎▍▌▋▊▉"[frac - 1] if frac and n < w else "")
    return paint(col, s) + paint(FAINT, "░" * (w - len(s))) + paint("1;" + col, f" {p:3d}%")


class Dashboard:
    SPIN = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
    KIND = {"info": GREY, "ok": GREEN, "warn": YELLOW, "err": RED,
            "tx": FAINT, "commit": GREEN, "music": PEACH, "notify": CYAN, "ach": YELLOW}

    def __init__(self, live):
        self.live = bool(live and sys.stdout.isatty() and self._vt())
        self.events = deque(maxlen=80)
        self.link, self.port = "search", ""
        self.rssi = None
        self.started = time.time()
        self.tx = 0
        self.ev, self.ev_at = "idle", time.time()
        self.doing, self.doing_at = "", 0.0
        self.typing = False
        self.levels = deque([0] * 20, maxlen=20)
        self.stats, self.music = None, None
        self.lyric = ""
        self.calendar = self.todos = self.progress = None
        self.sticky, self.git_lines = {}, []
        self.next_draw = 0.0
        if self.live:
            try:
                sys.stdout.reconfigure(errors="replace")
            except Exception:
                pass
            # alternate screen, hidden cursor, no line wrap (narrow windows clip)
            sys.stdout.write(ESC + "?1049h" + ESC + "?25l" + ESC + "?7l" + ESC + "2J")
            sys.stdout.flush()
            atexit.register(self.close)

    @staticmethod
    def _vt():
        """Turn on ANSI escapes in the Windows console."""
        if sys.platform != "win32":
            return True
        try:
            import ctypes
            k = ctypes.windll.kernel32
            h = k.GetStdHandle(-11)
            mode = ctypes.c_uint32()
            if not k.GetConsoleMode(h, ctypes.byref(mode)):
                return False
            return bool(k.SetConsoleMode(h, mode.value | 0x0004))
        except Exception:
            return False

    def close(self):
        if not self.live:
            return
        self.live = False
        sys.stdout.write(ESC + "?7h" + ESC + "?25h" + ESC + "?1049l")
        sys.stdout.flush()
        for t, _kind, msg in list(self.events)[-8:]:    # leave the tail of the log behind
            print(f"{t}  {msg}")
        print("bridge stopped.")

    # -- feeding it ------------------------------------------------------
    def log(self, msg, kind="info"):
        msg = str(msg)
        if self.events and self.events[-1][2] == msg:   # don't repeat the same line
            return
        self.events.append((time.strftime("%H:%M:%S"), kind, msg))
        self.next_draw = 0.0

    def event(self, ev):
        self.ev, self.ev_at = ev, time.time()
        if ev != "key":
            label, col = EVENTS.get(ev, (ev, GREY))[:2]
            say("● " + label, col)

    def activity(self, text):
        self.doing, self.doing_at = text, time.time()

    # -- drawing ---------------------------------------------------------
    def face(self, now):
        ev = "key" if self.typing else self.ev
        if ev in ("idle", "done") and now - self.ev_at > 300:
            return "(-_-) zZ", FAINT, "dozing off"
        label, col, eye, mouth, extra = EVENTS.get(ev, (ev, GREY, "o", "_", ""))
        if now % 4.0 < 0.2 and eye not in "-x":          # blink
            eye = "-"
        return f"({eye}{mouth}{eye}){extra}", col, label

    @staticmethod
    def _nums(line, n):
        try:
            v = [int(x) for x in line[2:].split(",")][:n]
            return v if len(v) == n else None
        except (TypeError, ValueError):
            return None

    def draw(self, force=False):
        now = time.time()
        if not self.live or (now < self.next_draw and not force):
            return
        self.next_draw = now + 0.2
        cols, rows = shutil.get_terminal_size((80, 24))
        W = max(44, min(cols - 2, 78))
        L = []

        def lab(s):
            return "  " + paint(FAINT, s.ljust(9))

        # header
        if self.link == "usb":
            link = paint(GREEN, "● USB ") + paint(GREY, self.port)
        elif self.link == "ble":
            r = self.rssi
            n = 0 if r is None else 4 if r > -60 else 3 if r > -70 else 2 if r > -80 else 1
            link = (paint(CYAN, "● Bluetooth ") + paint(CYAN, "▂▄▆█"[:n])
                    + paint(FAINT, "▂▄▆█"[n:] if n else ""))
        else:
            spin = self.SPIN[int(now * 10) % len(self.SPIN)]
            link = paint(YELLOW, spin + (" scanning bluetooth" if self.link == "scan" else " looking for board"))
        title = paint("1;" + CORAL, "✻ Claude Buddy") + paint(GREY, "  bridge")
        L.append(paint(CORAL, "╭" + "─" * (W - 2) + "╮"))
        L.append(paint(CORAL, "│ ") + title + " " * max(1, W - 4 - vis(title) - vis(link))
                 + link + paint(CORAL, " │"))
        L.append(paint(CORAL, "╰" + "─" * (W - 2) + "╯"))

        # Claude's face + what it's doing
        face, col, label = self.face(now)
        up = int(now - self.started)
        right = paint(FAINT, f"up {up // 3600}:{up // 60 % 60:02}:{up % 60:02} · {self.tx} sent")
        left = "  " + paint("1;" + col, face.ljust(11)) + paint(col, label)
        L.append(left + " " * max(2, W - vis(left) - vis(right)) + right)
        doing = self.doing if self.doing and now - self.doing_at < 120 else ""
        L.append(" " * 13 + paint(GREY, clip(doing, W - 14) if doing else "·"))
        L.append("")

        # today / session
        if self.stats is not None:
            d, s = self.stats.day, self.stats.session

            def num(n, c):
                return paint("1;" + c, n)
            L.append(lab("today") + f"{num(d['commits'], GREEN)} commits   "
                     f"{num(d['tasks'], CORAL)} tasks   {num(d['tests'], CYAN)} tests passed")
            if self.progress is not None:
                st, nb = self.progress.streak(), len(self.progress.unlocked)
                L.append(lab("streak") + (paint("1;" + CORAL, f"{st} day{'s' if st != 1 else ''}") if st
                                          else paint(FAINT, "ship something today"))
                         + paint(GREY, f"   {nb}/{len(Progress.BADGES)} badges"))
            if any(s):
                L.append(lab("session") + paint("1", f"${s[0] / 100:.2f}") + "   "
                         + paint(GREEN, f"+{s[1]}") + " " + paint(RED, f"-{s[2]}")
                         + paint(GREY, f" lines   {s[3]} min"))

        # Claude's to-do list
        t = self.todos
        if t is not None and t.items:
            n, d = len(t.items), t.done()
            dots = "".join(paint(GREEN, "●") if x["status"] == "completed" else
                           paint(CORAL, "◉") if x["status"] == "in_progress" else paint(FAINT, "○")
                           for x in t.items[:24])
            L.append(lab("todo") + dots + paint("1", f"  {d}/{n}"))
            if t.current():
                L.append(" " * 11 + paint(GREY, clip(t.current(), W - 12)))

        # next meeting
        if self.calendar is not None:
            nx = self.calendar.next()
            if nx:
                m = max(0, int((nx[0] - now) // 60))
                when = "now" if m == 0 else f"in {m} min" if m < 60 else time.strftime("at %H:%M", time.localtime(nx[0]))
                L.append(lab("next") + paint("1", clip(nx[1], W - 30)) + paint(YELLOW if m <= 10 else GREY, "  " + when))

        # usage + context meters
        bw = max(10, min(26, W - 38))
        u = self._nums(self.sticky.get("U"), 3)
        x = self._nums(self.sticky.get("X"), 1)
        if u or x:
            L.append("")
        if u:
            reset = f"resets in {u[2] // 60}h {u[2] % 60:02}m" if u[2] >= 0 else ""
            L.append(lab("5-hour") + bar(u[0], bw) + "  " + paint(FAINT, reset))
            L.append(lab("weekly") + bar(u[1], bw))
        if x:
            L.append(lab("context") + bar(x[0], bw))

        # music: visualizer + progress
        m = self.music
        if m is not None:
            self.levels.append(m.level() if m.state == 1 else 0)
            L.append("")
            if m.state:
                icon = paint(PEACH, "▶ ") if m.state == 1 else paint(GREY, "❚❚ ")
                L.append(lab("music") + icon + paint("1", clip(m.title, W - 15)))
                if self.lyric and m.state == 1:
                    L.append(" " * 11 + paint("3;" + PEACH, "♪ " + clip(self.lyric, W - 14)))
                spark = "".join("▁▂▃▄▅▆▇█"[min(7, v * 8 // 100)] for v in self.levels)
                line = " " * 11 + paint(PEACH if m.state == 1 else FAINT, spark)
                if m.dur:
                    pw = max(6, W - 11 - len(spark) - 16)
                    k = min(pw - 1, int(pw * m.pos / m.dur))
                    line += ("  " + paint(GREY, mmss(m.pos)) + " " + paint(CORAL, "━" * k + "●")
                             + paint(FAINT, "─" * (pw - k - 1)) + " " + paint(GREY, mmss(m.dur)))
                L.append(line)
            else:
                L.append(lab("music") + paint(FAINT, "nothing playing"))

        # git graph
        if self.git_lines:
            L.append("")
        for gl in self.git_lines:
            if gl.startswith("R:"):
                br, _, repo = gl[2:].partition(",")
                L.append(lab("git") + paint("1;" + GREEN, "⎇ " + br) + paint(GREY, "  " + repo))
            elif gl.startswith("L:"):
                p = gl[2:].split(",", 2)
                if len(p) == 3 and p[1] and p[0].isdigit() and int(p[0]) < 3:
                    dot = paint(GREEN, "●") if p[0] == "0" else paint(FAINT, "○")
                    L.append(" " * 11 + dot + " " + paint(YELLOW, p[1]) + " " + paint(GREY, p[2]))

        # activity feed fills the rest of the window
        L.append("")
        L.append(paint(FAINT, "  ── activity " + "─" * max(0, W - 15)))
        room = max(3, rows - len(L) - 1)
        for t, kind, msg in list(self.events)[-room:]:
            L.append("  " + paint(FAINT, t) + "  " + paint(self.KIND.get(kind, kind), clip(msg, W - 14)))

        L = L[:max(1, rows - 1)]
        sys.stdout.write(ESC + "H" + "".join(ln + ESC + "K\n" for ln in L) + ESC + "J")
        sys.stdout.flush()


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
    global UI
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
    ap.add_argument("--no-lyrics", action="store_true", help="don't show song lyrics on the board")
    ap.add_argument("--lyrics-offset", type=float, metavar="SEC",
                    help="show lyric lines this many seconds earlier (negative = later; remembered)")
    ap.add_argument("--no-usage", action="store_true",
                    help="don't fetch your Claude usage (the usage screen then relies on the status line)")
    ap.add_argument("--ble", action="store_true", help="always use Bluetooth, even if USB is plugged in")
    ap.add_argument("--usb", action="store_true", help="USB cable only, never Bluetooth")
    ap.add_argument("--plain", action="store_true", help="plain log lines instead of the live dashboard")
    ap.add_argument("--ics", action="append", default=[], metavar="URL",
                    help="an iCal calendar link for meeting alerts (repeatable; remembered)")
    ap.add_argument("--no-calendar", action="store_true", help="no meeting alerts")
    ap.add_argument("--no-notify", action="store_true", help="no GitHub / chat notifications")
    ap.add_argument("--notify-apps", metavar="APPS",
                    help="comma-separated app names whose Windows notifications go to the board (remembered)")
    ap.add_argument("--no-web", action="store_true", help="don't serve the settings page")
    ap.add_argument("--version", action="version", version=f"claude-buddy {VERSION}")
    ap.add_argument("--no-away", action="store_true", help="don't say 'oh, you're back!'")
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
        print("Claude Buddy is already running on this computer (it starts at login).\n"
              "Settings and status: http://localhost:47801", file=sys.stderr)
        raise SystemExit(ALREADY_RUNNING)
    sock.settimeout(0.1)

    ui = UI = Dashboard(live=not a.plain)
    stats = Stats()
    sticky = {"S": stats.line()}   # last U:/X:/S: line per kind, replayed after a reconnect
    watcher = CommitWatcher(a.repo)
    seen_commits = []              # short hashes already sent (hook + watcher both report)
    git_lines = []                 # last R:/L: history sent, replayed after a reconnect
    last_event = ("", 0.0)
    ser = None
    ble = None                     # BleLink, created the first time USB isn't there
    music = None if a.no_music else (MusicWatcher() if sys.platform == "win32" else
                                     MacMusicWatcher() if sys.platform == "darwin" else None)
    usage = None if a.no_usage else UsageFetcher()
    music_state, music_sent = (0, ""), 0.0
    cfg0 = load_json(CONFIG_FILE, {})
    if a.lyrics_offset is not None:
        cfg0["lyrics_offset"] = a.lyrics_offset
        save_json(CONFIG_FILE, cfg0)
    lyrics = None if (a.no_music or a.no_lyrics) else Lyrics(0.3 + float(cfg0.get("lyrics_offset", 0.0)))
    lyric_sent = None              # last lyric line sent (index), None = cleared
    rx_buf = b""                   # partial line from the board
    date_sent = 0.0                # last time the board got the date & time
    level_sent, level_at = -1, 0.0
    keys = None if a.no_typing else KeyWatcher(a.type_windows)
    typing, last_key, last_typing_send = False, 0.0, 0.0
    announced = False              # "no board found" said once per outage, not every retry
    cfg = load_json(CONFIG_FILE, {})
    for u in a.ics:
        if u not in cfg.setdefault("ics", []):
            cfg["ics"].append(u)
    if a.notify_apps:
        cfg["notify_apps"] = a.notify_apps
    save_json(CONFIG_FILE, cfg)
    S = {**DEFAULT_SETTINGS, **{k: cfg[k] for k in DEFAULT_SETTINGS if k in cfg}}
    calendar = None if a.no_calendar else CalendarWatcher(S["ics"])
    notifier = None if a.no_notify else NotifyWatcher(S["notify_apps"])
    away = None if a.no_away else AwayWatcher()
    todos, progress = Todos(), Progress()
    progress.seed(stats.day)       # today's commits/tasks from before streaks existed
    notify_at = 0.0                # next time the board's banner is free
    badges, fresh = [], []         # V: lines waiting for the board / just earned
    ui.stats, ui.sticky, ui.music = stats, sticky, music
    ui.calendar, ui.todos, ui.progress = calendar, todos, progress
    say(f"bridge listening on udp://{UDP_HOST}:{UDP_PORT}", "ok")
    locked = [k for k, obj in (("music", music), ("lyrics", lyrics), ("notify", notifier),
                               ("calendar", calendar), ("away", away), ("typing", keys),
                               ("usage", usage)) if obj is None] + (["link"] if a.ble or a.usb else [])
    web = None if a.no_web else WebApp()
    if web is not None:
        if web.ok:
            say(f"settings page: {web.url}", "ok")
        else:
            say(f"settings page unavailable ({web.err})", "warn")
            web = None
    web_at = 0.0
    board_cfg = None               # the board's brightness / volume / sound, as it reports them

    relink = False                 # the connection setting changed: connect again its way

    def apply_settings(change):
        """A change from the settings page: check it, keep it, use it now."""
        nonlocal relink
        for k, v in change.items():
            if k not in DEFAULT_SETTINGS:
                continue
            if k == "link":
                if v in ("auto", "usb", "ble") and v != S[k] and k not in locked:
                    S[k], relink = v, True
            elif isinstance(DEFAULT_SETTINGS[k], bool) and k in S:
                S[k] = bool(v)
            elif k == "lyrics_offset":
                try:
                    S[k] = round(max(-3.0, min(3.0, float(v))), 2)
                except (TypeError, ValueError):
                    pass
            elif k == "notify_apps":
                S[k] = str(v)[:200]
                if notifier is not None:
                    notifier.apps = [x.strip().lower() for x in S[k].split(",") if x.strip()]
            elif k == "ics" and isinstance(v, list):
                S[k] = [str(u)[:500] for u in v if str(u).lower().startswith(("http://", "https://", "webcal://"))][:10]
                if calendar is not None:
                    calendar.ics = list(S[k])
        c = load_json(CONFIG_FILE, {})
        c.update({k: S[k] for k in DEFAULT_SETTINGS})
        save_json(CONFIG_FILE, c)
        say("settings changed: " + ", ".join(change), "info")

    def out(line):
        if ser is None:                    # board away: state is kept and replayed on reconnect
            return
        ser.write((line + "\n").encode())
        ui.tx += 1

    fw = Firmware()
    app = AppUpdates()
    next_try = 0.0                 # next attempt to find the board
    beat_at = 0.0                  # last heartbeat to the board
    busy_said = None               # a USB port we said is busy (said once, not on every retry)
    usb_probe = 0.0                # on Bluetooth in auto mode: next look for a USB cable
    usb_seen = None                # ...and the port it saw last time (a fresh cable needs a moment)
    while True:
        mode = "ble" if a.ble else "usb" if a.usb else S["link"]
        if relink:                         # the connection setting changed
            relink = False
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
            say({"auto": "connection: automatic (USB, else Bluetooth)",
                 "usb": "connection: USB only", "ble": "connection: Bluetooth only"}[mode], "info")
            next_try, announced = 0.0, False
        if (app.available and app.state != "updating" and app.can and S["auto_update"]
                and fw.state != "flashing" and time.time() - ui.ev_at > app.QUIET_FOR):
            app.start()                    # a new Claude Buddy: install it and come back
        if fw.finished:                    # a firmware update ended: the board restarts, reconnect
            fw.finished, next_try = False, time.time() + 2.5
        linked_now = False
        if ser is None and fw.state != "flashing" and time.time() >= next_try:
            ui.link = "search"
            port = None if mode == "ble" else (a.port or find_port())
            if port:
                try:
                    ser = open_serial(port, a.baud)
                    ui.link, ui.port, busy_said = "usb", port, None
                    say(f"connected on {port} (USB)", "ok")
                    linked_now = True
                except (serial.SerialException, OSError) as e:
                    if busy_said != port:
                        busy_said = port
                        err = str(e).lower()
                        why = ("another program is using it (the Arduino IDE, a firmware update...)"
                               if any(w in err for w in ("denied", "permission", "busy", "in use")) else str(e))
                        say(f"{port} is busy: {why}. "
                            + ("Using Bluetooth meanwhile." if mode == "auto" else "Trying again..."), "warn")
            if ser is None and mode != "usb" and (not port or mode == "auto"):
                if not announced:
                    say("looking for 'Claude Buddy' over Bluetooth...")
                ui.link = "scan"
                ui.draw(force=True)
                if ble is None:
                    ble = BleLink()                # one bluetooth thread, reused
                if ble.connect():
                    ser = ble
                    ui.link, ui.rssi = "ble", ble.rssi
                    sig = f", signal {ble.rssi} dBm" if ble.rssi is not None else ""
                    say(f"connected over Bluetooth{sig}, {ble.chunk}-byte packets"
                        + ("" if ble.buttons else ", buttons off"), "ok")
                    linked_now = True
            if ser is None:
                # no board: keep going offline - Claude's events, commits, to-dos and
                # streaks still count, and the board catches up when it's back
                ui.link = "search"
                if not announced:
                    say("no board found - working offline (stats, to-dos and streaks still count). "
                        "retrying...", "warn")
                    announced = True
                next_try = time.time() + 3
        # on Bluetooth, automatic mode, and the cable goes in: USB is quicker and steadier
        if ser is not None and ser is ble and mode == "auto" and not a.port and time.time() >= usb_probe:
            usb_probe = time.time() + 5
            port = find_port()
            usb = None
            settled, usb_seen = port is not None and port == usb_seen, port   # seen twice, 5 s apart
            if settled:
                try:
                    usb = open_serial(port, a.baud)
                except (serial.SerialException, OSError):
                    usb = None
            if usb is not None:
                ble.close()
                ser = usb
                ui.link, ui.port = "usb", port
                say(f"USB cable plugged in - switched from Bluetooth to USB ({port})", "ok")
                linked_now = True
        if linked_now:
            announced = False
            board_cfg, fw.board = None, None
            try:
                time.sleep(0.3)
                date_sent = beat_at = 0.0          # fresh time + heartbeat right away
                lyric_sent = -1                    # resend the current lyric line
                out("H:1," + ("usb" if ui.link == "usb" else "ble"))   # hello intro, then a "connected" card
                out("F:?")                         # ...and its brightness / volume / sound / firmware
                for line in list(sticky.values()) + git_lines:
                    out(line)
            except (serial.SerialException, OSError) as e:
                say(f"link lost while syncing: {e}", "err")
                ser = None
                next_try = time.time() + 3

        try:
            data = sock.recvfrom(65535)[0].decode(errors="ignore")
        except socket.timeout:
            data = ""

        try:
            incoming = data.splitlines()
            for h, subj, repo in watcher.poll():   # commits from any editor or tool
                say(f"✓ commit {h} in {repo}: {subj}", "commit")
                incoming += ["T:" + oled_text("commit: " + subj), "C:" + h]
            for line in incoming:
                line = line.strip()
                if len(line) < 3 or line[1] != ":":
                    continue
                if line[0] == "W":                 # a folder Claude Code works in
                    watcher.add(line[2:])
                    continue
                if line[0] == "J":                 # Claude's to-do list changed (hook)
                    try:
                        ticked = todos.apply(json.loads(line[2:]))
                    except (ValueError, TypeError, AttributeError):
                        continue
                    if ticked is None:
                        continue
                    if ticked:
                        fresh += progress.record("todos", ticked)
                        if todos.done() == len(todos.items):
                            say(f"✓ all {len(todos.items)} to-dos done!", "ok")
                    line = sticky["Y"] = todos.line()
                if line[0] in "UX":
                    sticky[line[0]] = line
                if line[0] == "T":
                    ui.activity(line[2:])
                stats_changed = False
                if line[0] == "K":                 # session cost/lines/time -> S: line
                    try:
                        stats.session = [int(x) for x in line[2:].split(",")][:4]
                    except ValueError:
                        continue
                    line, stats_changed = stats.line(), True
                    fresh += progress.session_lines(stats.session[1])
                elif line[0] == "C":
                    h = line[2:9]
                    if h in seen_commits:          # already counted (hook + watcher)
                        continue
                    seen_commits = (seen_commits + [h])[-50:]
                    line = "C:" + h
                    stats.bump("commits")
                    fresh += progress.record("commits")
                    stats_changed = True
                elif line[0] == "E":
                    ev = line[2:]
                    # drop identical events fired back-to-back
                    now = time.time()
                    if line == last_event[0] and now - last_event[1] < 0.4:
                        continue
                    last_event = (line, now)
                    ui.event(ev)
                    if ev in ("done", "bug"):
                        stats.bump("tasks" if ev == "done" else "tests")
                        fresh += progress.record("tasks" if ev == "done" else "tests")
                        stats_changed = True
                if line == "E:limit":              # usage limit hit: the board's "limit hit" face
                    try:                           # (idle ends the turn; a full 5-hour bar shows it)
                        _, wk, mins = (sticky.get("U") or "U:0,-1,-1")[2:].split(",")[:3]
                    except ValueError:
                        wk, mins = "-1", "-1"
                    out("E:idle")
                    line = sticky["U"] = f"U:100,{wk},{mins}"
                if a.verbose:
                    say("> " + line, "tx")
                out(line)
                if stats_changed:
                    sticky["S"] = stats.line()
                    if line != sticky["S"]:
                        out(sticky["S"])
                if line[0] == "C":
                    watcher.changed = True         # resend the history with this commit
            if ser is not None and time.time() - beat_at > 5:
                beat_at = time.time()              # "still here": the board knows the bridge is alive
                out("I:1")
            for b in fresh:                        # streaks & achievements: say it now...
                say("★ " + b.split(",", 1)[1], "ach")
            badges, fresh = badges + fresh, []
            if ser is not None:                    # ...show the trophy when the board is here
                for b in badges[-4:]:
                    out(oled_text(b, 40))
                badges = []
            if web is not None:                    # the settings page
                while not web.changes.empty():
                    apply_settings(web.changes.get())
                while not web.outbox.empty():
                    line = web.outbox.get()
                    if ser is not None:
                        out(line)
                while not web.actions.empty():
                    act = web.actions.get()
                    if act == "update_app":
                        if app.available and app.can and fw.state != "flashing":
                            app.start()
                        continue
                    if act != "flash" or fw.state == "flashing":
                        continue
                    if ser is None or ui.link != "usb":
                        fw.state, fw.msg = "error", "Connect the board with a USB cable to update it."
                    elif fw.best is None:
                        fw.state, fw.msg = "error", "No firmware to install was found."
                    else:
                        say(f"updating the board to firmware {fw.best[0]}...", "info")
                        port = ui.port
                        try:
                            ser.close()
                        except Exception:
                            pass
                        ser, board_cfg = None, None
                        ui.link = "flash"
                        fw.flash(port)
                if time.time() >= web_at:
                    web_at = time.time() + 0.5
                    u = ui._nums(sticky.get("U"), 3)
                    web.state = {
                        "version": VERSION, "link": ui.link, "port": ui.port, "rssi": ui.rssi,
                        "linkMode": mode, "hooks": claude_hooked(),
                        "claude": {"ev": ui.ev, "label": EVENTS.get(ui.ev, (ui.ev,))[0],
                                   "ago": int(time.time() - ui.ev_at),
                                   "doing": getattr(ui, "doing", "") or "",
                                   "doingAgo": int(time.time() - (getattr(ui, "doing_at", 0) or 0))},
                        "app": {"latest": app.latest, "update": app.available, "state": app.state,
                                "msg": app.msg, "can": app.can},
                        "firmware": {"board": fw.board, "latest": fw.best[0] if fw.best else None,
                                     "update": fw.update, "state": fw.state, "pct": fw.pct, "msg": fw.msg},
                        "board": board_cfg if ser is not None else None,
                        "music": ({"state": music.state, "title": music.title, "lyric": ui.lyric,
                                   "pos": int(music.pos), "dur": int(music.dur)}
                                  if music is not None and S["music"] else None),
                        "usage": {"five": u[0], "week": u[1], "reset": u[2]} if u and S["usage"] else None,
                        "today": {k: stats.day.get(k, 0) for k in ("commits", "tasks", "tests")},
                        "streak": progress.streak(), "badges": len(progress.unlocked),
                        "badgesTotal": len(Progress.BADGES),
                        "todos": {"done": todos.done(), "total": len(todos.items), "current": todos.current()},
                        "settings": S, "locked": locked, "faces": FACES, "data": DATA,
                        "events": [[t, m] for t, _k, m in list(ui.events)[-12:]],
                    }
            if notifier is not None and not S["notify"]:
                notifier.queue.clear()             # switched off: nothing piles up
            if away is not None and ser is not None and S["away"]:
                mins = away.poll()
                if mins is not None:               # you came back to the laptop
                    say(f"welcome back! (away {mins} min)", "ok")
                    out(f"B:{mins}")
            if calendar is not None and ser is not None and S["calendar"]:
                for mins, title in calendar.due():  # meeting coming up -> alarm clock panic
                    say(f"meeting: {title} " + ("starting now" if mins <= 0 else f"in {mins} min"), "warn")
                    out("N:%d,%s" % (mins, oled_text(title, 33)))
            if ser is not None and notifier is not None and notifier.queue and time.time() >= notify_at:
                icon, text = notifier.queue.popleft()
                text = oled_text(text, 64)
                out(f"O:{icon},{text}")            # banner across the bottom of the screen
                notify_at = time.time() + max(6.0, len(text) * 0.14 + 4)
            if time.time() - date_sent > 60:       # the board's clock: local time every minute
                date_sent = time.time()
                out("D:%d" % (int(date_sent) + time.localtime().tm_gmtoff))
            if usage is not None and S["usage"] and usage.line and usage.line != sticky.get("U"):
                sticky["U"] = usage.line           # your 5-hour / weekly limits -> usage screen
                out(usage.line)
                if a.verbose:
                    say("> " + usage.line, "tx")
            if music is not None and not S["music"] and music_state != (0, ""):
                music_state, lyric_sent = (0, ""), None    # switched off: the board stops the music faces
                out("M:0,")
                out("Z:0,")
            if music is not None and S["music"]:   # music on this laptop -> music faces
                music.tick()
                now = time.time()
                state = (music.state, music.title)
                if state != music_state or (state[0] and now - music_sent > 2):
                    if state != music_state:
                        say(("♪ " + ("now playing: ", "paused: ")[state[0] - 1] + state[1])
                            if state[0] else "♪ music stopped", "music")
                    music_state, music_sent = state, now
                    out("M:%d,%s" % (state[0], oled_text(state[1], 40)))
                    if state[0] and music.dur:     # song position for the progress bar
                        out("Q:%d,%d" % (music.pos, music.dur))
                if state[0] == 1 and now - level_at >= 0.08:
                    lvl = music.level()
                    if abs(lvl - level_sent) >= 2 or now - level_at > 0.5:
                        out(f"A:{lvl}")
                        level_sent, level_at = lvl, now
                if lyrics is not None and not S["lyrics"] and lyric_sent is not None:
                    lyric_sent, ui.lyric = None, ""
                    out("Z:0,")
                if lyrics is not None and S["lyrics"]:   # karaoke: the line under the playhead
                    lyrics.lead = 0.3 + float(S["lyrics_offset"])
                    if music.state:
                        lyrics.want(music.track, music.artist, music.dur)
                    cur = lyrics.at(music.position()) if music.state == 1 and lyrics.texts and music.dur else None
                    key = cur[0] if cur else None
                    if key != lyric_sent:
                        lyric_sent = key
                        if cur:
                            text = lyric_text(cur[1])
                            out(f"Z:{int(cur[2] * 1000)},{text}")
                            ui.lyric = text
                        else:
                            out("Z:0,")
                            ui.lyric = ""
                sock.settimeout(0.03 if state[0] == 1 else 0.1)   # faster loop while music plays
            if watcher.changed:                    # board's git graph = your real history
                watcher.changed = False
                git_lines = watcher.snapshot()
                ui.git_lines = git_lines
                for gl in git_lines:
                    out(gl)
                if a.verbose and git_lines:
                    say("> git history: " + git_lines[0], "tx")
            if keys is not None and S["typing"]:
                now = time.time()
                if keys.active():
                    last_key = now
                    if not typing or now - last_typing_send > 1.0:
                        if a.verbose and not typing:
                            say("> typing", "tx")
                        typing, last_typing_send = True, now
                        out("E:key")
                elif typing and now - last_key > 2.0:
                    typing = False     # the board decides how (and whether) to react
                ui.typing = typing
            # lines from the board: its debug prints, and P:<cmd> from the music buttons
            if ser is not None:
                rx_buf += ser.read(ser.in_waiting or 1)
            while b"\n" in rx_buf:
                raw, rx_buf = rx_buf.split(b"\n", 1)
                msg = raw.decode(errors="ignore").strip()
                if msg.startswith("P:") and music is not None:
                    say("◆ board button: " + msg[2:], "music")
                    music.control(msg[2:])
                elif msg.startswith("F:"):         # the board's settings (and firmware version)
                    try:
                        parts = msg[2:].split(",")
                        b, bn, v, vn, snd = (int(x) for x in parts[:5])
                        board_cfg = {"bri": b, "briCount": bn, "vol": v, "volCount": vn, "snd": snd}
                        fw.board = parts[5].strip()[:16] if len(parts) > 5 else ""
                    except ValueError:
                        pass
            rx_buf = rx_buf[-200:]
            ui.draw()
        except (serial.SerialException, OSError) as e:
            if isinstance(e, serial.SerialTimeoutException):
                say("board is not reading the port (write timed out).", "err")
                say("  -> Arduino IDE: Tools > USB CDC On Boot > Enabled, re-upload the sketch,", "warn")
                say("     and close the Arduino Serial Monitor.", "warn")
            else:
                say(f"serial lost ({e}) - reconnecting...", "err")
            try:
                ser.close()
            except Exception:
                pass
            ser, board_cfg, fw.board = None, None, None
            next_try = time.time() + 1


def run():
    """The claude-buddy command."""
    try:
        main()
    except KeyboardInterrupt:
        pass
    finally:
        if UI is not None:
            UI.close()


def service():
    """The claude-buddy-service command: Claude Buddy in the background - no window,
    started at login, and started again if it ever stops. Its log goes to
    claude-buddy.log in the data folder."""
    os.makedirs(DATA, exist_ok=True)
    path = os.path.join(DATA, "claude-buddy.log")
    try:
        if os.path.getsize(path) > 2_000_000:          # keep the log small
            os.replace(path, path + ".old")
    except OSError:
        pass
    cmd = [sys.executable, "-m", "claude_buddy.claude_bridge", "--plain"] + sys.argv[1:]
    wait = 5
    while True:
        started = time.time()
        with open(path, "a", encoding="utf-8", errors="replace") as log:
            log.write(time.strftime("\n--- %Y-%m-%d %H:%M:%S  Claude Buddy starting\n"))
            log.flush()
            env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
            code = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                   creationflags=0x08000000 if sys.platform == "win32" else 0)
            log.write(time.strftime("--- %H:%M:%S  stopped") + f" (exit code {code})\n")
        if code == ALREADY_RUNNING:
            return                                     # another one is already running
        wait = 5 if time.time() - started > 120 else min(wait * 2, 120)   # back off if it keeps failing
        time.sleep(wait)


if __name__ == "__main__":
    run()
