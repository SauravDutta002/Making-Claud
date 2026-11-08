# Claude Buddy

A little desk screen that reacts to [Claude Code](https://claude.com/claude-code).
It thinks when Claude thinks, cheers when your tests pass, shows your usage,
your git history and Claude's to-do list, sings along to your music, and lives
its own life when you're away.

## Install

One command. It installs everything, connects Claude Code, updates the board's
firmware if it's plugged in, and starts at every login.

**Windows** (PowerShell):

```powershell
irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.ps1 | iex
```

**macOS** (Terminal):

```sh
curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.sh | sh
```

Then restart Claude Code. Run the same command again any time to update.

The installer uses [uv](https://docs.astral.sh/uv/), which brings its own
Python, so you don't need Python installed. It adds three commands:

| Command | What it does |
|---|---|
| `claude-buddy` | The bridge between Claude Code and the board, with a live dashboard |
| `claude-buddy-flash` | Updates the board's firmware (board on USB) |
| `claude-buddy-setup` | Connects Claude Code (`--remove` disconnects it) |

## Connecting the board

- **USB:** plug it into the computer. It's found automatically.
- **Bluetooth:** power it from anywhere. The bridge finds "Claude Buddy" on
  its own and reconnects if the link drops. Don't pair it in your system's
  Bluetooth settings, because the bridge connects to it directly.

Without the bridge the board still works: it plays, naps and runs its games
on its own, and catches up as soon as the bridge is back.

## What works where

| | Windows | macOS |
|---|---|---|
| Claude Code reactions, usage, git, to-dos, streaks and badges | ✓ | ✓ |
| USB and Bluetooth | ✓ | ✓ |
| Now playing and synced lyrics | Spotify, YouTube in Chrome/Edge, Apple Music and any app showing media controls | Spotify, Apple Music |
| Music level (equalizer) | ✓ | dances on its own |
| "Oh, you're back!" | ✓ | ✓ |
| GitHub notifications (needs `gh` signed in) | ✓ | ✓ |
| Slack / Teams / Discord messages | ✓ | – |
| Meeting alerts | Windows calendar, plus iCal links | iCal links |
| "You're typing" face | ✓ | – |

Meeting alerts from Google Calendar: copy *Settings → your calendar → Secret
address in iCal format* and run `claude-buddy --ics "<that link>"` once. It's
remembered.

## Hardware

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

**Touch controls:** tap for the next face, double tap for settings, triple tap
for a game, four taps to mute, hold to pet.

## What goes online

- **Your Claude usage:** fetched from Anthropic with your own Claude Code
  login, as `/usage` does. The token is read from your machine and never
  printed or sent anywhere else.
- **Lyrics:** the song name, artist and length are sent to
  [lrclib.net](https://lrclib.net).
- **GitHub:** read through your own `gh` login.
- **iCal links:** only the links you add.

Your stats, streaks and settings stay on your machine:
`%LOCALAPPDATA%\ClaudeBuddy` on Windows, `~/Library/Application Support/ClaudeBuddy` on macOS.

## Uninstall

```powershell
irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.ps1 | iex
```

```sh
curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/uninstall.sh | sh
```

This disconnects Claude Code and removes the commands and autostart. The board
keeps its firmware.

## Building the firmware yourself

Open `claude.ino` in the Arduino IDE with the ESP32 core (3.x) and the
*Adafruit GFX* and *Adafruit SH110X* libraries. Choose **ESP32C3 Dev Module**,
set **USB CDC On Boot: Enabled**, and upload. The ready-built firmware that
`claude-buddy-flash` installs is in `bridge/firmware/`.
