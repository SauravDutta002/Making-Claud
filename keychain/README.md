# Keychain

A tiny screen that plays your own animations. Same board and wiring as Claude Buddy (ESP32-C3 Super Mini, 1.3" SH1106 OLED, TTP223 touch, optional buzzer). Nothing here touches `claude.ino`.

## Put your pictures on it

```
pip install pillow
python tools/make_frames.py art.gif                 # one animation
python tools/make_frames.py ride.gif rain.gif logo.png --preview
```

Every file becomes one animation. You can use a GIF, an animated WebP or PNG, a still image, or a folder of numbered frames. The script writes `frames.h`; open `keychain.ino` in the Arduino IDE and upload it. Check `preview.png` first to see exactly what the screen will show.

The screen is 128×64 and black & white, so pictures turn into dots:
- **Pictures with clear shapes and strong contrast work best.**
- **Light background?** Use `--invert`.
- **Edges cut off?** Use `--fit contain` to show the whole picture with bars, instead of filling the screen and cropping.
- **Grey areas look muddy?** Try `--dither floyd` for finer dots on stills, or `--dither none --threshold 120` for pure black & white.

Each frame takes 1 KB. With the default settings there's room for about 850 frames. For more, choose **Tools › Partition Scheme › Huge APP (3MB No OTA)** in the Arduino IDE, which fits about 2,700.

Only use art you have the rights to, especially for orders you sell.

## Settings (top of `keychain.ino`)

| Setting | What it does |
|---|---|
| `CAPTION` | a small name in the corner, e.g. `"NNAN"` |
| `SLEEP_MIN` | minutes untouched before it sleeps (0 = never) |
| `FLIP` | 1 if the screen is mounted upside down |
| `BUILTIN_RIDE` | 0 hides the built-in night-ride animation |

## Touch

- **Tap:** next animation
- **Double tap:** sound on/off
- **Hold:** brightness. Keep holding to step through the levels.
- **Hold for 3 seconds:** sleep. Tap to wake it.

It also sleeps by itself after `SLEEP_MIN` minutes: the screen turns off and it uses almost no battery. It remembers the animation, brightness and sound setting between sleeps.

Arduino IDE board: **ESP32C3 Dev Module**, with **USB CDC On Boot: Enabled**. Libraries: Adafruit GFX and Adafruit SH110X.
