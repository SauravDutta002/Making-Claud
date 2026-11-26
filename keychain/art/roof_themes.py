"""
roof_themes.py - more nights for the starry-roof scene (K7, art/doghouse.png).
Same Snoopy on his roof, a different sky each time:

    meteors    a meteor shower
    snow       snow drifting down
    fireflies  a summer night, fireflies blinking
    aurora     northern lights rippling
    stars      constellations joining up, then fading
    rain       rain, and lightning far away

Used by make_candidates.py; 96 frames that loop seamlessly.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import doghouse_anim as dh  # noqa: E402
import scene               # noqa: E402

W, H, N = 128, 64, 96
BT = scene.BT
YS, XS = np.mgrid[0:H, 0:W]


def masks(parts):
    snoopy, moon, roof, big, stars = parts
    keep = np.zeros((H, W), bool)
    for x, y in list(snoopy) + list(roof):
        if 0 <= x < W and 0 <= y < H:
            keep[y, x] = True
    return scene.grow(keep, 2)                    # sky effects stay off Snoopy and his roof


def frame(f, parts, theme, rng_seed=1):
    base = np.asarray(dh.frame(f, *parts).convert("L")) > 127
    solid = masks(parts)
    sky = ~solid
    fx = np.zeros((H, W), bool)
    rng = np.random.default_rng(rng_seed)

    if theme == "meteors":                         # lots of shooting stars
        d = Image.new("1", (W, H), 0)
        dr = ImageDraw.Draw(d)
        for i in range(10):
            start = i * N // 10 + int(rng.integers(0, 6))
            t = (f - start) % N
            if t < 14:
                x0, y0 = int(rng.integers(30, W + 20)), int(rng.integers(-4, 20))
                hx, hy = x0 - t * 6, y0 + t * 3
                dr.line([hx, hy, hx + 8, hy - 4], fill=1)
                dr.point((hx + 11, hy - 6), fill=1)
        fx |= np.asarray(d).astype(bool)

    elif theme == "snow":                          # snow drifting down
        for i in range(45):
            x0, y0 = float(rng.uniform(0, W)), float(rng.uniform(0, H))
            laps = 1 + i % 2
            y = int((y0 + H * laps * f / N) % H)
            x = int(x0 + 2 * math.sin(2 * math.pi * f / N * 3 + i)) % W
            fx[y, x] = True
            if i % 5 == 0 and x + 1 < W:
                fx[y, x + 1] = True

    elif theme == "fireflies":                     # fireflies drifting and blinking
        for i in range(10):
            cx, cy = float(rng.uniform(5, W - 5)), float(rng.uniform(8, 54))
            ph = float(rng.uniform(0, 6.28))
            x = int(cx + 6 * math.sin(2 * math.pi * f / N * 2 + ph))
            y = int(cy + 4 * math.cos(2 * math.pi * f / N * 3 + ph))
            glow = (f * 2 + i * 13) % 32
            if glow < 20 and 0 <= x < W - 1 and 0 <= y < H:
                fx[y, x] = fx[y, x + 1] = True          # a little 2-pixel glow
                if 6 <= glow < 14:                 # brightest: a little halo
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        if 0 <= x + dx < W and 0 <= y + dy < H:
                            fx[y + dy, x + dx] = True

    elif theme == "aurora":                        # northern lights, rippling
        t = 2 * math.pi * f / N
        top = 6 + 4 * np.sin(XS / 14.0 + t * 2) + 3 * np.sin(XS / 7.0 - t * 3)
        band = (YS > top) & (YS < top + 9 + 3 * np.sin(XS / 9.0 + t))
        depth = np.clip(1 - (YS - top) / 12.0, 0, 1)
        streak = (np.sin(XS * 0.9 + t * 4) > 0.2)
        fx |= band & (BT < depth * 0.55) & streak
        edge = np.abs(YS - np.round(top)) < 0.5                  # the curtain's bright top edge
        fx |= edge & ((XS + f) % 3 != 0)

    elif theme == "stars":                         # little constellations drawing themselves in
        shapes = [
            [(76, 5), (83, 4), (90, 6), (96, 9), (98, 15), (106, 15), (106, 9)],   # a dipper
            [(14, 20), (19, 26), (24, 21), (29, 27), (34, 22)],                     # a W
        ]
        d = Image.new("1", (W, H), 0)
        dr = ImageDraw.Draw(d)
        for g, pts in enumerate(shapes):
            t = (f + g * 48) % N
            shown = min(len(pts) - 1, t // 6) if t < 80 else -1
            for k, (x, y) in enumerate(pts):
                if t < 86:
                    dr.point((x, y), fill=1)
                    if k <= shown + 1:
                        dr.point((x + 1, y), fill=1)
            for k in range(max(0, shown)):
                (x0, y0), (x1, y1) = pts[k], pts[k + 1]
                steps = max(abs(x1 - x0), abs(y1 - y0), 1)
                for s_ in range(1, steps, 2):
                    dr.point((x0 + (x1 - x0) * s_ // steps, y0 + (y1 - y0) * s_ // steps), fill=1)
        fx |= np.asarray(d).astype(bool)

    elif theme == "rain":                          # rain, and lightning far away
        for i in range(36):
            x0, y0 = float(rng.uniform(0, W + 20)), float(rng.uniform(0, H))
            y = int((y0 + H * 3 * f / N) % H)
            x = int(x0 - y * 0.25) % W
            for k in range(3):
                if 0 <= y + k < H and 0 <= x - k // 2 < W:
                    fx[y + k, x - k // 2] = True
        if f % 48 in (20, 21, 23):                 # lightning far away: a forked bolt
            d = Image.new("1", (W, H), 0)
            dr = ImageDraw.Draw(d)
            dr.line([(104, 0), (100, 7), (105, 11), (99, 20), (103, 24), (98, 32)], fill=1)
            dr.line([(105, 11), (111, 17)], fill=1)
            fx |= np.asarray(d).astype(bool)

    out = base | (fx & sky)
    return Image.fromarray((out * 255).astype(np.uint8)).convert("1")


def make(theme):
    parts = dh.load()
    return [frame(f, parts, theme) for f in range(N)]


if __name__ == "__main__":
    theme = sys.argv[1] if len(sys.argv) > 1 else "meteors"
    fr = make(theme)
    fr[0].save(os.path.join(HERE, f"roof_{theme}.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
