"""
stand_anim.py - Snoopy standing (art/stand.png) in a few little scenes.
He is drawn white with his own black lines on a black screen; he blinks now
and then and his ear flops. Themes:

    hello      hearts float up
    stars      moon, twinkling stars, a shooting star
    snow       snow falling, settling on the ground
    music      notes drifting by, he bobs to the beat
    autumn     leaves tumbling down
    fireflies  a summer night

96 frames, loops seamlessly.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402

W, H, N = 128, 64, 96
GROUND = 62
LEFT = 38
BT = scene.BT
YS, XS = np.mgrid[0:H, 0:W]

HEART = ["01010", "11111", "11111", "01110", "00100"]
NOTE = ["0110", "0101", "0100", "1100", "1100"]
LEAF = ["0110", "1111", "1110", "0100"]


def figure():
    """(white, black, silhouette, eye, ear) at screen size."""
    im = Image.open(os.path.join(HERE, "stand.png")).convert("L")
    im = im.crop((4, 6, im.width - 4, im.height - 6))      # the picture's frame
    a = np.asarray(im).astype(float)
    ys, xs = np.nonzero(a < 120)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    tall = GROUND - 3
    w = round(a.shape[1] * tall / a.shape[0])
    S = 4
    big = np.asarray(Image.fromarray(a.astype(np.uint8)).resize((w * S, tall * S), Image.LANCZOS)).astype(float)
    ink = big < 130
    paper = big > 200
    # outside him = paper reachable from the edges
    out = np.zeros_like(paper)
    out[0, :], out[-1, :], out[:, 0], out[:, -1] = paper[0, :], paper[-1, :], paper[:, 0], paper[:, -1]
    walls = scene.grow(~paper, 8)                           # close small gaps in his outline
    out &= ~walls
    for _ in range(600):
        nxt = scene.grow(out, 2) & ~walls
        if (nxt == out).all():
            break
        out = nxt
    sil = ~scene.grow(out, 7)                               # give back the rim the walls took
    ear = scene.grow(scene.shrink(ink, 5), 5) & ink         # his solid black ear (and nose)
    small = lambda m, t: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((w, tall), Image.BOX)) > t
    S_ = small(sil, 110)
    B_ = small(ink, 45) & S_                                # thin lines survive the shrink
    E_ = small(ear, 90) & S_
    return S_, B_, E_, w, tall


FIG = None


def place(f, bob=0, flop=0):
    global FIG
    if FIG is None:
        FIG = figure()
    S_, B_, E_, w, tall = FIG
    sil = np.zeros((H, W), bool); ink = np.zeros((H, W), bool); ear = np.zeros((H, W), bool)
    oy = GROUND - tall - bob
    sil[oy:oy + tall, LEFT:LEFT + w] = S_
    ink[oy:oy + tall, LEFT:LEFT + w] = B_
    ear[oy:oy + tall, LEFT:LEFT + w] = E_
    if flop:                                                # the ear flops a pixel
        moved = np.roll(ear, flop, axis=0) & scene.grow(sil, 1)
        ink = (ink & ~ear) | moved
        sil |= moved
    # a blink: his eye (the small black spot high in his head) closes to a line
    if f % 48 in (36, 37):
        head = (YS < oy + tall * 0.35) & (XS > LEFT + w * 0.35) & (XS < LEFT + w * 0.62)
        eye = ink & head & ~scene.edges(sil)
        ink &= ~eye
        rows = np.nonzero(eye.any(axis=1))[0]
        if len(rows):
            r = rows.max()
            ink[r] |= eye.any(axis=0)
    return sil, ink


def sprite(img, pat, x, y):
    for r, row in enumerate(pat):
        for c, v in enumerate(row):
            if v == "1" and 0 <= x + c < W and 0 <= y + r < H:
                img[y + r, x + c] = True


def frame(f, theme):
    rng = np.random.default_rng(4)
    img = np.zeros((H, W), bool)
    img[GROUND, :] = True                                   # the ground
    bob = 0
    if theme == "music":
        bob = 1 if (f // 6) % 2 else 0
    flop = (0, 1, 0, -1)[(f // 6) % 4] if theme in ("hello", "music") else (1 if (f // 12) % 2 else 0)

    if theme == "hello":
        for k in range(3):
            t = ((f + k * 32) % N) / N
            x = int(LEFT + 44 + k * 10 + 3 * math.sin(t * 9 + k))
            y = int(48 - t * 52)
            if t < 0.9:
                sprite(img, HEART, x, y)
    if theme == "stars":
        d = Image.new("1", (W, H), 0)
        dr = ImageDraw.Draw(d)
        dr.ellipse([100, 6, 114, 20], fill=1)
        dr.ellipse([96, 3, 110, 17], fill=0)                # a crescent moon
        img |= np.asarray(d).astype(bool)
        for i in range(22):
            x, y = int(rng.integers(0, W)), int(rng.integers(0, 48))
            if (f // 3 + i * 5) % 17 and not (LEFT - 2 < x < LEFT + 52):
                img[y, x] = True
        t = f - 50
        if 0 <= t < 12:
            hx, hy = 126 - t * 6, 2 + t * 2
            for k in range(6):
                if 0 <= hx + k * 2 < W and 0 <= hy - k < H:
                    img[hy - k, hx + k * 2] = True
    if theme == "snow":
        for i in range(50):
            x0, y0 = float(rng.uniform(0, W)), float(rng.uniform(0, H))
            laps = 1 + i % 2
            y = int((y0 + H * laps * f / N) % H)
            x = int(x0 + 2 * math.sin(2 * math.pi * f / N * 3 + i)) % W
            img[y, x] = True
        img[GROUND - 1, :] |= (XS[0] % 3 != 0)              # snow on the ground
    if theme == "music":
        for k in range(3):
            t = ((f + k * 32) % N) / N
            x = int(W - 6 - t * 70)
            y = int(20 + 10 * math.sin(t * 12 + k * 2))
            sprite(img, NOTE, x, y)
    if theme == "autumn":
        for i in range(10):
            x0, y0 = float(rng.uniform(0, W)), float(rng.uniform(0, H + 8))
            laps = 1 + i % 2
            y = int((y0 + (H + 8) * laps * f / N) % (H + 8)) - 4
            x = int(x0 + 6 * math.sin(2 * math.pi * f / N * 2 + i)) % W
            sprite(img, LEAF if (f // 4 + i) % 2 else ["0100", "1110", "0111", "0010"], x, y)
        for x in range(0, W, 5):                            # a little pile on the ground
            sprite(img, ["0110", "1111"], x, GROUND - 2)
    if theme == "fireflies":
        for i in range(10):
            cx, cy = float(rng.uniform(4, W - 4)), float(rng.uniform(6, 56))
            ph = float(rng.uniform(0, 6.28))
            x = int(cx + 6 * math.sin(2 * math.pi * f / N * 2 + ph))
            y = int(cy + 4 * math.cos(2 * math.pi * f / N * 3 + ph))
            glow = (f * 2 + i * 13) % 32
            if glow < 20 and 0 <= x < W - 1 and 0 <= y < H:
                img[y, x] = img[y, x + 1] = True

    sil, ink = place(f, bob, flop)
    img &= ~scene.grow(sil, 1)
    img |= sil & ~ink                                       # white Snoopy, his black lines
    img |= scene.edges(sil) & ink                           # black parts on his edge (ear, nose) get a white rim
    return Image.fromarray((img * 255).astype(np.uint8)).convert("1")


def make(theme):
    return [frame(f, theme) for f in range(N)]


if __name__ == "__main__":
    theme = sys.argv[1] if len(sys.argv) > 1 else "hello"
    fr = make(theme)
    fr[0].save(os.path.join(HERE, f"stand_{theme}.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
