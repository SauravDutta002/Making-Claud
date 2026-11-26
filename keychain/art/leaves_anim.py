"""
leaves_anim.py - Snoopy jumping in autumn leaves (from art/leaves.png).

    python art/leaves_anim.py [lines|solid]   -> art/leaves_anim.gif

Snoopy is traced from the picture (his white, his outline, his big black ears);
the busy leaf background is dropped and redrawn: leaves tumble down around him,
a pile of leaves at the bottom, and he bounces now and then.
lines = white outlines like a drawing; solid = white Snoopy with black details.
96 frames, loops seamlessly.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N, MS = 96, 70
SX0 = 22                         # where Snoopy's picture sits on the screen (left edge)

LEAVES = [  # little leaf shapes, a few turns of the same leaf
    ["0110", "1111", "1110", "0100"],
    ["0010", "0111", "1110", "1100"],
    ["0100", "1110", "1111", "0110"],
    ["010", "111", "010"],
]


def grow(m, n=1):
    out = m.copy()
    for _ in range(n):
        g = out.copy()
        g[1:, :] |= out[:-1, :]; g[:-1, :] |= out[1:, :]
        g[:, 1:] |= out[:, :-1]; g[:, :-1] |= out[:, 1:]
        out = g
    return out


def edges(m):
    inner = m.copy()
    inner[1:, :] &= m[:-1, :]; inner[:-1, :] &= m[1:, :]
    inner[:, 1:] &= m[:, :-1]; inner[:, :-1] &= m[:, 1:]
    inner[0, :] = inner[-1, :] = inner[:, 0] = inner[:, -1] = False
    return m & ~inner


def snoopy():
    """(white, ink, body) masks at screen size: his white, his black lines and ears, all of him."""
    im = Image.open(os.path.join(HERE, "leaves.png")).convert("RGB")
    im = im.crop((2, 2, im.width - 2, im.height - 2))           # the screenshot's border
    h = H
    w = round(im.width * h / im.height)
    big = im.resize((w * 4, h * 4), Image.LANCZOS).filter(ImageFilter.MedianFilter(3))
    a = np.asarray(big).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mn = np.minimum(np.minimum(r, g), b)
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    white = (mn > 175)
    # keep the white that is Snoopy (the big connected piece), not white specks
    from collections import deque
    seen = np.zeros_like(white)
    best = None
    for sy, sx in zip(*np.nonzero(white[::6, ::6])):
        sy, sx = sy * 6, sx * 6
        if seen[sy, sx]:
            continue
        comp = []
        q = deque([(sy, sx)])
        seen[sy, sx] = True
        while q:
            y, x = q.popleft()
            comp.append((y, x))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < white.shape[0] and 0 <= nx < white.shape[1] and white[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    q.append((ny, nx))
        if len(comp) > white.size * 0.02:                    # his head, his body (not specks)
            best = (best or []) + comp
    sn = np.zeros_like(white)
    ys, xs = zip(*best)
    sn[list(ys), list(xs)] = True
    # his ink: dark pixels touching his white (outline, eyes, nose, collar) + the ears
    dark = lum < 70
    near = grow(sn, 10)
    ink = dark & near
    # the ears are big dark shapes reaching out from his head: take dark within reach of the head
    body = grow(sn, 2) | ink
    small = lambda m: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((w, h), Image.BOX)) > 100
    return small(sn), small(ink), small(body), w


def leaf(img, x, y, k, on=True):
    shape = LEAVES[k % len(LEAVES)]
    for r_, row in enumerate(shape):
        for c, v in enumerate(row):
            xx, yy = x + c, y + r_
            if 0 <= xx < W and 0 <= yy < H:
                if v == "1":
                    img[yy, xx] = on
    # a thin black rim so a leaf reads over anything
    return img


def frame(f, parts, style):
    sn, ink, body, w = parts
    img = np.zeros((H, W), bool)

    # a pile of leaves along the bottom (outlines), still
    rng = np.random.default_rng(12)
    for i in range(46):
        x = int(rng.integers(-2, W))
        y = 57 + int(rng.integers(0, 6))
        k = int(rng.integers(0, 3))
        sh = LEAVES[k]
        for r_, row in enumerate(sh):
            for c, v in enumerate(row):
                if v == "1" and 0 <= x + c < W and y + r_ < H:
                    img[y + r_, x + c] = ((x + c + y + r_) % 2 == 0) or r_ == 0

    # Snoopy, with a happy bounce twice a loop
    t = (f % 48) / 48.0
    lift = int(round(max(0.0, math.sin(t * 2 * math.pi)) * 3)) if t < 0.5 else 0
    canvas = np.zeros((H, W), bool)
    S = np.zeros((H, W), bool); I = np.zeros((H, W), bool); B = np.zeros((H, W), bool)
    for src, dst in ((sn, S), (ink, I), (body, B)):
        hh, ww = src.shape
        y0 = -lift
        ys = slice(max(0, y0), min(H, y0 + hh))
        sy = slice(max(0, -y0), max(0, -y0) + (ys.stop - ys.start))
        xs = slice(SX0, min(W, SX0 + ww))
        dst[ys, xs] = src[sy, : xs.stop - xs.start]
    img &= ~grow(B, 1)
    if style == "solid":
        canvas = S & ~I
        canvas |= edges(B)
    else:
        canvas = I.copy()
        ears = I & ~grow(S, 2)
        canvas &= ~(ears & ~edges(ears))                         # big black ears -> outlines
    img |= canvas

    # leaves tumbling down: each falls a whole number of screens a loop
    rng = np.random.default_rng(7)
    for i in range(14):
        x0, y0 = float(rng.uniform(0, W)), float(rng.uniform(0, H + 10))
        laps = 1 + i % 2
        ph = float(rng.uniform(0, 2 * math.pi))
        y = (y0 + (H + 10) * laps * f / N) % (H + 10) - 6
        x = x0 + 6 * math.sin(2 * math.pi * laps * f / N * 2 + ph)
        k = (f // 4 + i) % 3
        xi, yi = int(x) % W, int(y)
        sh = LEAVES[k if i % 4 else 3]
        for r_ in range(-1, len(sh) + 1):
            for c in range(-1, len(sh[0]) + 1):
                if 0 <= xi + c < W and 0 <= yi + r_ < H:
                    img[yi + r_, xi + c] = False
        for r_, row in enumerate(sh):
            for c, v in enumerate(row):
                if v == "1" and 0 <= xi + c < W and 0 <= yi + r_ < H:
                    img[yi + r_, xi + c] = True
    return Image.fromarray((img * 255).astype(np.uint8)).convert("1")


def main(style=None, out=None):
    style = style or (sys.argv[1] if len(sys.argv) > 1 else "lines")
    parts = snoopy()
    frames = [frame(f, parts, style) for f in range(N)]
    out = out or os.path.join(HERE, "leaves_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    print(f"wrote {out}: {N} frames ({style})")


if __name__ == "__main__":
    main()
