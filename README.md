# Claude Buddy

A little desk screen that reacts to [Claude Code](https://claude.com/claude-code).
It thinks when Claude thinks, cheers when your tests pass, ticks off Claude's
to-do list, shows your usage, git history and streaks, sings along to your
music, and lives its own life when you're away.

It has two parts:

- **The board**: an ESP32-C3 with a small OLED screen, a touch pad and a buzzer.
- **The bridge**: a small program on your computer. It passes what Claude Code
  is doing to the board, over USB or Bluetooth.

---

## Set it up on a computer

You need Claude Code installed, and Windows 10/11 or macOS.

**Windows**: open PowerShell and run:

```powershell
irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.ps1 | iex
```

**macOS**: open Terminal and run:

```sh
curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.sh | sh
```

Then **restart Claude Code** (close and reopen any open sessions).

The command does everything:

1. Installs [uv](https://docs.astral.sh/uv/), which brings its own Python, so you don't need Python already installed.
2. Installs the Claude Buddy commands.
3. Connects Claude Code to the bridge: it adds hooks and a status line to `~/.claude/settings.json` and backs up the old file first.
4. If the board is plugged in over USB, it offers to install the latest firmware.
5. Starts the bridge now and at every login.

### Connecting the board

- **USB**: plug the board into the computer. It's found automatically.
- **Bluetooth**: power the board from anywhere. The bridge finds "Claude Buddy" by
  itself and reconnects if the link drops. Don't pair it in Windows or macOS
  Bluetooth settings, because the bridge connects to it directly.

Without the bridge the board still works on its own: it plays, naps and runs its
games, and catches up as soon as the bridge is back.

---

## The settings page

While the bridge runs, open **http://localhost:47801**. On Windows you can also use
Start menu > *Claude Buddy Settings*, or run `claude-buddy-settings` on either system.

- **Right now**: what's playing (with the lyric line), your Claude usage, today's
  commits and tasks, your streak and badges, and Claude's to-dos.
- **The screen**: brightness, volume and sound (saved on the board). You can also
  put any face on the screen, and test the hello intro, a game, a notification,
  a meeting alert or a badge.
- **What the board reacts to**: music, lyrics, notifications, meeting alerts,
  "you're back", the typing face and Claude usage. Each one switches on or off instantly.
- **Lyrics timing**: move lines earlier or later if they don't match a song.
- **Chat notifications**: which apps' Windows notifications go to the board.
- **Calendar links**: iCal links for meeting alerts.

Only this computer can open the page. It is not reachable from the network.

---

## Commands

| Command | What it does |
|---|---|
| `claude-buddy` | The bridge, with its live dashboard (started at login for you) |
| `claude-buddy-settings` | Opens the settings page |
| `claude-buddy-flash` | Installs the latest firmware on a board plugged in over USB |
| `claude-buddy-setup` | Connects Claude Code to the bridge again (`--remove` disconnects it) |

Useful `claude-buddy` options: `--plain` (plain log lines instead of the
dashboard), `--usb` / `--ble` (force one link), `--lyrics-offset 0.5`,
`--ics <link>`, `--no-music`, `--no-notify`, `--version`. Run
`claude-buddy --help` for all of them.

**Touch controls on the board:**

| Touch | What it does |
|---|---|
| Tap | Next face |
| Double tap | Settings |
| Triple tap | Game |
| Four taps | Mute |
| Hold | Pet Claude |

On the record player screen, the taps control the music instead: 2 taps play or
pause, 3 taps skip to the next song, 4 taps go back.

---

## What works where

| | Windows | macOS |
|---|---|---|
| Claude Code reactions, usage, git, to-dos, streaks, badges | ✓ | ✓ |
| USB and Bluetooth | ✓ | ✓ |
| Now playing and synced lyrics | Spotify, YouTube in Chrome/Edge, Apple Music, any app with media controls | Spotify, Apple Music |
| Equalizer follows the real volume | ✓ | moves on its own |
| "Oh, you're back!" | ✓ | ✓ |
| GitHub notifications (needs `gh auth login`) | ✓ | ✓ |
| Slack / Teams / Discord messages | ✓ | – |
| Meeting alerts | Windows calendar and iCal links | iCal links |
| Typing face | ✓ | – |

---

## Updating

**On any computer that already has Claude Buddy**, run the same install command
again. It updates the bridge and keeps your stats, streaks and settings. If the
board is plugged in over USB, it also offers to update the firmware.

To update only the board's firmware, plug it in over USB, close the bridge, and run:

```sh
claude-buddy-flash
```

The board keeps its brightness, volume, XP and level.

### Publishing a new version (for the maintainer)

Everyone installs from the `main` branch of this repository, so an update is
just a push:

1. **Make your changes.** Faces and animations are in `claude.ino`; the bridge
   and the settings page are in `bridge/`.
2. **Rebuild the firmware the installer ships.** Optionally flash your own board
   at the same time:
   ```sh
   python tools/build_firmware.py               # refreshes bridge/firmware/*.bin
   python tools/build_firmware.py --upload COM25
   ```
   This needs the Arduino IDE 2 (it includes `arduino-cli`), the ESP32 core 3.x
   and the *Adafruit GFX* and *Adafruit SH110X* libraries.
3. **Bump the version** in `pyproject.toml` and `VERSION` in `bridge/claude_bridge.py`.
4. **Commit and push to `main`.**
5. Users run the install command again to get it.

To try bridge changes on your own machine before pushing, run it from the folder:
`python bridge/claude_bridge.py`. Close the installed one first, because only one
bridge can run at a time.

---

## Building it

| Part | Notes |
|---|---|
| ESP32-C3 Super Mini | |
| 1.3" OLED 128×64, SH1106, I²C | address 0x3C |
| TTP223 touch module | |
| Passive buzzer | optional |

| From | To |
|---|---|
| OLED VCC / GND | 3V3 / GND |
| OLED SDA / SCL | GPIO5 / GPIO4 |
| TTP223 VCC / GND / OUT | 3V3 / GND / GPIO3 |
| Buzzer + / − | GPIO6 / GND |

**Battery (optional).** This setup still uses the board's single USB-C port for
power, charging and data.

| Part | Notes |
|---|---|
| TP4056 charger | the 6-pad version, with protection |
| 3.7 V LiPo | 500–1000 mAh |
| Schottky diode | SS14 or 1N5817 |
| Slide switch | |

| From | To |
|---|---|
| TP4056 IN+ / IN− | board 5V / GND |
| TP4056 B+ / B− | battery |
| TP4056 OUT+ | switch → diode → board 5V (striped end of the diode at 5V) |

Never connect the battery straight to 5V. Also change the TP4056's 1.2 kΩ R_PROG
resistor to about 3 kΩ, which charges at roughly 400 mA.

**Firmware from source:** open `claude.ino` in the Arduino IDE 2 with the ESP32
core 3.x and the *Adafruit GFX* and *Adafruit SH110X* libraries. Choose
**ESP32C3 Dev Module**, set **USB CDC On Boot: Enabled**, and upload. Or run
`python tools/build_firmware.py --upload <port>`.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| "Port busy" / "Access is denied" when flashing | Close the bridge (and the Arduino Serial Monitor) first. Only one program can use the port. |
| "Claude Buddy is already running" | It started at login. Close that window to run another one. |
| The board isn't found over Bluetooth | Make sure Bluetooth is on. If "Claude Buddy" is paired in your system's Bluetooth settings, remove it there. |
| Nothing happens when Claude works | Restart Claude Code after installing. Run `claude-buddy-setup` if you changed your settings file. |
| Lyrics are early or late | Settings page > Lyrics timing. Some songs only have lyrics for a different version. |
| No Slack/Teams cards (Windows) | Allow notification access: Settings > Privacy > Notifications. |
| The settings page won't open | The bridge must be running: run `claude-buddy`. |

---

## Privacy

- **Claude usage**: fetched from Anthropic with your own Claude Code login, as
  `/usage` does. The token is read on your machine and never printed or sent
  anywhere else.
- **Lyrics**: the song name, artist and length are sent to [lrclib.net](https://lrclib.net).
- **GitHub**: read through your own `gh` login.
- **Calendar**: only the iCal links you add are fetched.
- **Your stats, streaks and settings** stay on your computer, in
  `%LOCALAPPDATA%\ClaudeBuddy` (Windows) or `~/Library/Application Support/ClaudeBuddy` (macOS).

---

## Uninstall

```powershell
irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.ps1 | iex
```

```sh
curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.sh | sh
```

This disconnects Claude Code and removes the commands, the autostart and the
shortcuts. The board keeps its firmware, and your stats folder stays until you
delete it.

---

## What's in this repository

| Path | What it is |
|---|---|
| `claude.ino` | The board's firmware: every face, the touch controls and the sounds |
| `bridge/claude_bridge.py` | The bridge: USB/Bluetooth link, music and lyrics, notifications, streaks, the dashboard |
| `bridge/claude_hook.py` | The Claude Code hook: turns Claude's activity into messages for the bridge |
| `bridge/claude_statusline.py` | The Claude Code status line: usage and context |
| `bridge/webapp.py`, `bridge/web/` | The settings page |
| `bridge/setup_claude.py` | Connects Claude Code (hooks and status line) |
| `bridge/flash.py`, `bridge/firmware/` | `claude-buddy-flash` and the ready-built firmware |
| `tools/build_firmware.py` | Rebuilds `bridge/firmware/` from `claude.ino` |
| `install.ps1`, `install.sh`, `uninstall.*` | The one-command installers |
| `pyproject.toml` | Python package definition (`uv tool install`) |
