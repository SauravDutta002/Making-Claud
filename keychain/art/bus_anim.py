"""
bus_anim.py - Snoopy dozing on the night bus (from art/bus.png).

    python art/bus_anim.py      -> art/bus_anim.gif (+ art/bus_anim_big.gif)
    python tools/make_frames.py "art/bus_anim.gif=Snoopy night bus" --dither none --contrast 1.0

Snoopy's exact shape comes from the picture (his white, his black ear, nose and
eye). The bus around him is redrawn with clean lines: the big window with the
night outside, the grab pole, his seat and the seats around him. City lights
and street lamps stream past, stars twinkle, the bus bumps, Snoopy breathes
and z's float up. 96 frames, loops seamlessly.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import make_frames as mf  # noqa: E402

W, H = 128, 64
N, MS = 96, 70
WIN = (0, 4, 127, 30)          # the window: left, top, right, bottom (the sill)
POLE_X = 11

Z = [["1111", "0010", "0100", "1111"],
     ["11111", "00010", "00100", "01000", "11111"],
     ["111111", "000010", "000100", "001000", "010000", "111111"]]


def rnd(n):
    n = (n << 13 ^ n) & 0xFFFFFFFF
    return ((n * (n * n * 15731 + 789221) + 1376312589) & 0xFFFFFFFF) >> 16 & 0xFF


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


def blobs(mask, keep_min):
    """Connected pieces of a mask bigger than keep_min pixels."""
    seen = np.zeros_like(mask)
    out = np.zeros_like(mask)
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        comp, st = [], [(y, x)]
        seen[y, x] = True
        while st:
            cy, cx = st.pop()
            comp.append((cy, cx))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    st.append((ny, nx))
        if len(comp) >= keep_min:
            for cy, cx in comp:
                out[cy, cx] = True
    return out


def snoopy():
    """His white (head + body), his black ear, from the picture."""
    rgb = np.asarray(mf.fit(Image.open(os.path.join(HERE, "bus.png")).convert("RGBA"), "cover")).astype(int)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mn = np.minimum(np.minimum(r, g), b)
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    ys, xs = np.mgrid[0:H, 0:W]
    zone = (xs > 38) & (xs < 104) & (ys > 6)
    white = blobs((mn > 95) & zone, 40)
    # fill small specks inside him, but keep his nose and eye (dark, inside the head)
    filled = ~blobs(~grow(white, 1), 30) | white
    ear = blobs((lum < 40) & (xs > 40) & (xs < 64) & (ys > 12) & (ys < 42) & grow(white, 3), 15)
    return white, filled, ear


def background(f, sy):
    """The bus interior, drawn clean; sy = how far the bus has bumped."""
    img = np.zeros((H, W), bool)
    x0, y0, x1, y1 = WIN
    y0s, y1s = y0 + sy, y1 + sy

    def win(x, y):
        return x0 < x < x1 and y0s < y < y1s

    # outside: stars
    for i in range(18):
        x, y = 2 + rnd(i * 5) % 124, y0s + 2 + rnd(i * 5 + 1) % 8
        if (f // 5 + i * 3) % 13 and win(x, y):
            img[y, x] = True
    # outside: a far skyline drifting by, with lit windows
    far = f * 0.5
    base_y = y1s - 1
    for i in range(int(far // 12) - 1, int(far // 12) + 13):
        bx = i * 12 - int(far)
        h = 4 + rnd(i + 7) % 8
        w = 7 + rnd(i + 70) % 4
        for x in range(bx, bx + w):
            for y in (base_y - h,):
                if win(x, y):
                    img[y, x] = True
        for y in range(base_y - h, base_y + 1):
            for x in (bx, bx + w - 1):
                if win(x, y):
                    img[y, x] = True
        for y in range(base_y - h + 2, base_y - 1, 2):
            for x in range(bx + 2, bx + w - 2, 2):
                if rnd(i * 37 + y * 5 + x + (f // 24)) > 165 and win(x, y):
                    img[y, x] = True
    # outside: street lamps rushing past (bright streaks)
    near = f * 4.0
    for i in range(int(near // 48) - 1, int(near // 48) + 4):
        lx = i * 48 - int(near) + 20
        for y in range(y0s + 4, y1s):
            if win(lx, y):
                img[y, lx] = True
        for k in range(4):
            if win(lx - k, y0s + 4):
                img[y0s + 4, lx - k] = True
        for dx in (-4, -3):
            for dy in (5, 6):
                if win(lx + dx, y0s + dy):
                    img[y0s + dy, lx + dx] = True

    # the window frame (double line) and its sill
    for x in range(W):
        for y in (y0s, y0s - 1, y1s, y1s + 2):
            if 0 <= y < H:
                img[y, x] = True
    # the grab pole on the left, top to bottom, with a little ring
    img[:, POLE_X] = True
    img[:, POLE_X + 1] = True
    # seats: HIS seat (a tall backrest behind his back + the cushion he sits in),
    # the seat across the aisle on the right, one more on the left
    for (sx0, sy0, sx1, sy1, rad) in ((0, 38, 24, 64, 4), (108, 34, 127, 64, 4),
                                       (32, 12, 60, 64, 7), (32, 52, 104, 72, 4)):
        yy, xx = np.mgrid[0:H, 0:W]
        cx = np.clip(xx, sx0 + rad, sx1 - rad)
        cy = np.clip(yy, sy0 + sy + rad, sy1 - rad + 8)
        inside = (xx >= sx0) & (xx <= sx1) & (yy >= sy0 + sy) & ((xx - cx) ** 2 + (yy - cy) ** 2 <= rad * rad + rad)
        img &= ~inside
        img |= edges(inside)
        stitch = inside & (yy == sy0 + sy + 5) & (xx > sx0 + 3) & (xx < sx1 - 3) & (xx % 2 == 0)
        img |= stitch
    return img


def frame(f, white, filled, ear):
    bump = 1 if f % 48 in (30, 31, 33) else 0
    img = background(f, bump)

    body = filled | ear
    breathe = math.sin(2 * math.pi * f / 48) > 0.35
    def lift(m):
        if not breathe:
            return m
        out = m.copy()
        head = np.zeros_like(m)
        head[:40, :] = m[:40, :]
        out[:40, :] = False
        out[:39, :] |= head[1:40, :]
        return out
    wh, fl, er = lift(white), lift(filled), lift(ear)
    halo = grow(fl | er, 1)
    img &= ~halo
    img |= wh
    img |= edges(er)                                 # his black ear, outlined

    # z z Z
    for k in range(3):
        t = ((f + k * 11) % 32) / 32.0
        if t > 0.92:
            continue
        g = Z[min(2, int(t * 3))]
        zx = int(90 + t * 8 + math.sin(t * 6.3) * 2)
        zy = int(16 - t * 13)
        for rr in range(-1, len(g) + 1):
            for cc in range(-1, len(g[0]) + 1):
                if 0 <= zx + cc < W and 0 <= zy + rr < H:
                    img[zy + rr, zx + cc] = False
        for rr, row in enumerate(g):
            for cc, v in enumerate(row):
                if v == "1" and 0 <= zx + cc < W and 0 <= zy + rr < H:
                    img[zy + rr, zx + cc] = True
    return Image.fromarray((img * 255).astype(np.uint8)).convert("1")


def main():
    white, filled, ear = snoopy()
    frames = [frame(f, white, filled, ear) for f in range(N)]
    out = os.path.join(HERE, "bus_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [im.convert("L").resize((W * 4, H * 4), Image.NEAREST) for im in frames]
    big[0].save(os.path.join(HERE, "bus_anim_big.gif"), save_all=True, append_images=big[1:], duration=MS, loop=0)
    print(f"wrote {out}: {N} frames")


if __name__ == "__main__":
    main()
