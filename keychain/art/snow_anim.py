"""
snow_anim.py - brings art/snow.png to life: Snoopy in his winter hat, snow falling.

    python art/snow_anim.py     -> art/snow_anim.gif (+ art/snow_anim_big.gif)
    python tools/make_frames.py "art/snow_anim.gif=Snoopy in the snow" --dither none --contrast 1.0

The like / comment buttons on the right of the screenshot are cropped off.
Snoopy: face solid white, hat and coat as outlines, the red scarf and boots
in a checker pattern. The trees become a treeline, the snowy ground soft dots.
Snow falls in three layers (near flakes bigger and faster), his breath puffs
out in the cold. 96 frames, loops seamlessly.
"""
import math
import os

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N, MS = 96, 70
CROP = (0, 170, 650, 495)                 # 2:1, without the buttons on the right
SNOOPY = (360, 180, 590, 500)             # where he is, in the picture
BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64.0


def to_screen(x, y):
    x0, y0, x1, y1 = CROP
    return (x - x0) * W / (x1 - x0), (y - y0) * H / (y1 - y0)


def edges(mask):
    """Pixels of the shape that touch the outside on any side: its outline."""
    m = mask
    inner = m.copy()
    inner[1:, :] &= m[:-1, :]; inner[:-1, :] &= m[1:, :]
    inner[:, 1:] &= m[:, :-1]; inner[:, :-1] &= m[:, 1:]
    inner[0, :] = inner[-1, :] = False
    inner[:, 0] = inner[:, -1] = False
    return m & ~inner


def snoopy_masks():
    """Snoopy's parts, found in the full-size picture and then shrunk: everything
    connected to his face that is his white, his navy (darker than the trees) or
    his red."""
    im = Image.open(os.path.join(HERE, "snow.png")).convert("RGB").filter(ImageFilter.MedianFilter(5))
    a = np.asarray(im).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    x0, y0, x1, y1 = SNOOPY
    box = np.zeros(lum.shape, bool)
    box[y0:y1, x0:x1] = True
    white = box & (lum > 150) & (abs(r - b) < 30)
    navy = box & (lum < 56)
    red = box & (r > g + 60) & (r > b + 50)
    part = white | navy | red
    # flood from his face so trees and snow around him are left out
    seen = np.zeros_like(part)
    stack = [(300, 430)]
    while stack:
        y, x = stack.pop()
        if not (0 <= y < part.shape[0] and 0 <= x < part.shape[1]) or seen[y, x] or not part[y, x]:
            continue
        seen[y, x] = True
        stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    # the snow around his boots is white too: keep only white that's above his scarf
    white &= seen & (np.arange(lum.shape[0])[:, None] < 360)
    navy &= seen
    red &= seen

    def shrink(m):
        cx0, cy0, cx1, cy1 = CROP
        img = Image.fromarray((m[cy0:cy1, cx0:cx1] * 255).astype(np.uint8)).resize((W, H), Image.BOX)
        return np.asarray(img) > 110
    return shrink(white), shrink(navy), shrink(red)


def grow(m, n=1):
    out = m.copy()
    for _ in range(n):
        g = out.copy()
        g[1:, :] |= out[:-1, :]; g[:-1, :] |= out[1:, :]
        g[:, 1:] |= out[:, :-1]; g[:, :-1] |= out[:, 1:]
        out = g
    return out


def shrink_(m, n=1):
    return ~grow(~m, n)


def pines(ys, xs, horizon):
    """A calm treeline: pines with three tiers of branches, outlined, no clutter."""
    rng = np.random.default_rng(21)
    out = np.zeros((H, W), bool)
    trees, x = [], 2.0
    while x < W:
        trees.append((x, float(rng.uniform(17, 30))))
        x += float(rng.uniform(11, 16))
    trees.sort(key=lambda t: t[1])
    for tx, h in trees:
        top = horizon - h
        inside = np.zeros((H, W), bool)
        for k in range(3):
            ty0 = top + h * k * 0.28
            ty1 = top + h * (0.42 + k * 0.29)
            half = np.clip((ys - ty0) / (ty1 - ty0), 0, 1) * (3.5 + k * 2.8)
            inside |= (ys >= ty0) & (ys <= ty1) & (np.abs(xs - tx) <= half)
        inside &= ys < horizon
        out &= ~inside
        out |= edges(inside)
    return out


def base():
    im = Image.open(os.path.join(HERE, "snow.png")).convert("RGB")
    sm = im.crop(CROP).resize((W, H), Image.LANCZOS)
    L = np.asarray(sm.convert("L")).astype(float)
    ys, xs = np.mgrid[0:H, 0:W]
    light_rows = [(L[y] > 150).mean() for y in range(H)]
    horizon = next(y for y in range(10, H) if light_rows[y] > 0.6)

    white, navy, red = snoopy_masks()
    body = shrink_(grow(white | navy | red, 1), 1)                 # fill the speckles
    navy = body & ~white & ~red
    near = grow(body, 1)

    out = pines(ys, xs, horizon) & ~near
    rng = np.random.default_rng(4)
    out |= (ys > horizon + 1) & (rng.random((H, W)) < 0.025) & ~near
    out[horizon, :] |= ~near[horizon, :]

    fy, fx = np.argwhere(white).min(0)
    gy, gx = np.argwhere(white).max(0)
    in_face = (ys >= fy) & (ys <= gy) & (xs >= fx) & (xs <= gx)
    out |= white
    out |= edges(body)
    out |= navy & ~in_face & ((xs + ys) % 2 == 0) & (ys % 2 == 0)  # dark fabric
    out |= red
    face_pts = np.argwhere(white)
    nx = int(face_pts[:, 1].min())
    ny = int(np.median(face_pts[face_pts[:, 1] < nx + 4][:, 0]))
    return out, horizon, (nx, ny)


def frame(f, bw, horizon, nose):
    img = bw.copy()

    def flake(x, y, size):
        pts = [(0, 0)] if size == 1 else [(0, 0), (1, 0), (0, 1), (1, 1)] if size == 2 else \
            [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)]
        for dx, dy in pts:                                  # a black rim so it reads on anything
            for ex in (-1, 0, 1):
                for ey in (-1, 0, 1):
                    xx, yy = x + dx + ex, y + dy + ey
                    if 0 <= xx < W and 0 <= yy < H and (ex, ey) != (0, 0):
                        img[yy, xx] = False if (dx + ex, dy + ey) not in pts else img[yy, xx]
        for dx, dy in pts:
            if 0 <= x + dx < W and 0 <= y + dy < H:
                img[y + dy, x + dx] = True

    # three layers of snow; each flake falls a whole number of screens a loop
    rng = np.random.default_rng(3)
    for layer, (count, size, laps) in enumerate(((26, 1, 1), (14, 2, 2), (6, 3, 3))):
        for i in range(count):
            x0, y0 = float(rng.uniform(0, W)), float(rng.uniform(0, H + 8))
            ph = float(rng.uniform(0, 2 * math.pi))
            y = (y0 + (H + 8) * laps * f / N) % (H + 8) - 4
            x = (x0 + 3 * math.sin(2 * math.pi * (laps * f / N) * 2 + ph) - 6 * laps * f / N) % W
            flake(int(x), int(y), size)

    # breath: a little cloud puffs out of his nose, drifts and fades
    nx, ny = nose
    t = (f % 48) / 48.0
    if t < 0.55:
        k = t / 0.55
        cx, cy = nx - 3 - k * 9, ny - 1 - k * 4
        rad = 1.2 + k * 2.6
        dens = 1 - k
        for yy in range(int(cy - rad) - 1, int(cy + rad) + 2):
            for xx in range(int(cx - rad) - 1, int(cx + rad) + 2):
                if 0 <= xx < W and 0 <= yy < H and math.hypot(xx - cx, (yy - cy) * 1.3) <= rad:
                    if BAYER[yy % 8, xx % 8] < dens * 0.7:
                        img[yy, xx] = True
    return Image.fromarray((img * 255).astype(np.uint8)).convert("1")


def main():
    bw, horizon, nose = base()
    print("horizon at row", horizon, "- nose at", nose)
    frames = [frame(f, bw, horizon, nose) for f in range(N)]
    out = os.path.join(HERE, "snow_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [im.convert("L").resize((W * 4, H * 4), Image.NEAREST) for im in frames]
    big[0].save(os.path.join(HERE, "snow_anim_big.gif"), save_all=True, append_images=big[1:], duration=MS, loop=0)
    print(f"wrote {out}: {N} frames")


if __name__ == "__main__":
    main()
