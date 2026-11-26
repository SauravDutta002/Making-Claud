"""
rose_anim.py - an original rose for the keychain, drawn from scratch in a grainy
"ascii art" style.

    python art/rose_anim.py     -> art/rose_anim.gif (+ art/rose_anim_big.gif)
    python tools/make_frames.py "art/rose_anim.gif=Rose" --dither none --contrast 1.0

The petals spiral in and slowly unfurl, the rose sways on its stem, sparkles
twinkle around it and a petal drifts down once a loop. 96 frames, seamless.
"""
import math
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N, MS = 96, 70
SS = 3
HX, HY, HR = 44.0, 20.0, 17.5       # the flower head: centre, size
BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64.0


def font():
    import re
    src = open(os.path.join(HERE, "..", "tools", "preview.py"), encoding="utf-8").read()
    return bytes.fromhex(re.search(r'const FONT="([0-9a-f]+)"', src).group(1))


FONT = font()


def text(img, s, x, y):
    for ch in s:
        c = ord(ch)
        for i in range(5):
            col = FONT[(c - 32) * 5 + i]
            for j in range(8):
                if col >> j & 1 and 0 <= x + i < W and 0 <= y + j < H:
                    img[y + j, x + i] = 1.0
        x += 6


def render(f):
    sway = math.sin(2 * math.pi * f / 48) * 1.6           # the head moves, the stem bends with it
    ys, xs = np.mgrid[0:H * SS, 0:W * SS]
    x = (xs + 0.5) / SS
    y = (ys + 0.5) / SS
    img = np.zeros_like(x)

    # stem: a gentle curve from the pot of the screen up to the head
    t = np.clip((y - HY) / (H - HY), 0, 1)
    stem_x = HX + sway * (1 - t) ** 2 + 4 * math.sin(1.3) * t * t - 2 * t
    stem = (np.abs(x - stem_x) < 1.1) & (y > HY + HR * 0.6)
    img = np.where(stem, 1.0, img)
    # a leaf on each side, with a vein
    for side, ly, ln in ((1, 44.0, 13.0), (-1, 52.0, 10.0)):
        sx = HX + 4 * math.sin(1.3) * ((ly - HY) / (H - HY)) ** 2 - 2 * (ly - HY) / (H - HY)
        lx = (x - sx) * side
        ang = 0.45
        u = lx * math.cos(ang) + (ly - y) * math.sin(ang)
        v = -lx * math.sin(ang) + (ly - y) * math.cos(ang)
        leaf = (u > 0) & (u < ln) & (np.abs(v) < 3.6 * np.sin(np.clip(u / ln, 0, 1) * math.pi))
        vein = leaf & (np.abs(v) < 0.45) & ~stem
        img = np.where(leaf, np.maximum(img, 0.62), img)
        img = np.where(vein, 0.0, img)

    # the head: a cup of petals with a scalloped top, petals spiralling in
    cx, cy = HX + sway, HY
    dx, dy = x - cx, (y - cy) * 1.15
    rho = np.hypot(dx, dy)
    th = np.arctan2(dy, dx)
    rim = HR * (1 + 0.10 * np.sin(5 * th + 0.6) + 0.05 * np.sin(9 * th))
    rim = np.where(dy > 2, rim * (1 - 0.18 * np.clip(dy / HR, 0, 1)), rim)   # narrower at the base
    inside = rho < rim
    unfurl = 2 * math.pi * f / N                                             # one turn of the spiral a loop
    s = rho / HR * 2.6 + (th + unfurl) / (2 * math.pi)
    fr = s - np.floor(s)
    petal = 0.62 + 0.38 * np.clip(fr * 1.25, 0, 1)                          # bright at each petal's edge
    shade = np.clip(1.25 - rho / rim * 0.45 + 0.2 * np.cos(th + 2.2), 0.55, 1)
    gap = fr > 0.86                                                           # dark line between petals
    head = np.where(gap, 0.08, petal * shade)
    edge = inside & (rho > rim - 1.0)
    img = np.where(inside, head, img)
    img = np.where(edge, 1.0, img)
    return img.reshape(H, SS, W, SS).mean(axis=(1, 3))


def finish(img, f):
    th = np.tile(BAYER, (H // 8, W // 8))
    bw = (img > th * 0.9 + 0.05).astype(float)

    # sparkles around it, each on its own beat
    rng = np.random.default_rng(11)
    for i in range(22):
        x, y = int(rng.integers(2, W - 2)), int(rng.integers(1, H - 1))
        if abs(x - HX) < 24 and y < 42 or abs(x - HX) < 16 or (x > 70 and y > 45):
            continue
        ph = (f + i * 9) % 32
        if ph < 20:
            bw[y, x] = 1
        if 6 <= ph < 11:
            for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                bw[y + ddy, x + ddx] = 1
            if ph == 8:
                for k in (2, -2):
                    bw[y, x + k] = bw[y + (1 if k > 0 else -1), x] = 1

    # a petal drifting down, once a loop
    t = (f - 20) / 50.0
    if 0 <= t < 1:
        px = int(HX + 10 + t * 26 + math.sin(t * 9) * 4)
        py = int(HY + 6 + t * 40)
        for ddx, ddy in ((0, 0), (1, 0), (0, 1), (1, 1), (2, 0) if int(t * 9) % 2 else (-1, 1)):
            if 0 <= px + ddx < W and 0 <= py + ddy < H:
                bw[py + ddy, px + ddx] = 1

    text(bw, "in bloom", 76, 50)
    return bw


def main():
    frames = []
    for f in range(N):
        bw = finish(render(f), f)
        frames.append(Image.fromarray((bw * 255).astype(np.uint8)).convert("1"))
    out = os.path.join(HERE, "rose_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [im.convert("L").resize((W * 4, H * 4), Image.NEAREST) for im in frames]
    big[0].save(os.path.join(HERE, "rose_anim_big.gif"), save_all=True, append_images=big[1:], duration=MS, loop=0)
    print(f"wrote {out}: {N} frames")


if __name__ == "__main__":
    main()
