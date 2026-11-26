"""
scene.py - turns a Snoopy picture (pixel-art Snoopy in a colourful room) into a
living keychain animation.

Snoopy is found by his cream colour and black outline inside a box, and drawn
crisp: solid white with his black details. From the room only a few big,
recognisable things are kept ("props", each found by colour inside a box) and
drawn cleanly: an armchair with upholstery stripes, window panes with snow
falling in them, a glowing lamp, a plaid couch, a flickering TV, blinds,
rustling leaves... Then Snoopy breathes and the little things move.

Looks: "dark" (white drawing on black) and "light" (inverted).
96 frames that loop seamlessly. Used by make_candidates.py.
"""
import math
import os
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N = 96
BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64.0
BT = np.tile(BAYER, (H // 8, W // 8))
YS, XS = np.mgrid[0:H, 0:W]
NOTE = ["0110", "0101", "0100", "1100", "1100"]


def grow(m, n=1):
    out = m.copy()
    for _ in range(n):
        g = out.copy()
        g[1:, :] |= out[:-1, :]; g[:-1, :] |= out[1:, :]
        g[:, 1:] |= out[:, :-1]; g[:, :-1] |= out[:, 1:]
        out = g
    return out


def shrink(m, n=1):
    return ~grow(~m, n)


def edges(m):
    return m & ~shrink(m, 1)


def drop_small(m, keep):
    seen = np.zeros_like(m)
    out = np.zeros_like(m)
    for y, x in zip(*np.nonzero(m)):
        if seen[y, x]:
            continue
        comp, q = [], deque([(y, x)])
        seen[y, x] = True
        while q:
            cy, cx = q.popleft()
            comp.append((cy, cx))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < m.shape[0] and 0 <= nx < m.shape[1] and m[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
        if len(comp) >= keep:
            for cy, cx in comp:
                out[cy, cx] = True
    return out


class Colours:
    def __init__(self, rgb):
        self.r, self.g, self.b = (rgb[..., i].astype(float) for i in range(3))
        self.lum = 0.3 * self.r + 0.59 * self.g + 0.11 * self.b
        self.mn = np.minimum(np.minimum(self.r, self.g), self.b)
        self.mx = np.maximum(np.maximum(self.r, self.g), self.b)


class Scene:
    def __init__(self, picture, crop, snoopy_box, props=(), effects=(), pad=(0, 0, 0), snoopy_colour=None):
        self.snoopy_colour = snoopy_colour
        self.im = Image.open(os.path.join(HERE, picture)).convert("RGB")
        x0, y0, x1, y1 = crop
        canvas = Image.new("RGB", (x1 - x0, y1 - y0), pad)
        canvas.paste(self.im, (-x0, -y0))
        self.src = canvas
        self.crop = crop
        self.fx = effects
        sm = canvas.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6))
        self.c = Colours(np.asarray(sm))
        self.find_snoopy(snoopy_box)
        self.props = [self.prop(**p) for p in props]

    def box_mask(self, box):
        if box is None:
            return np.ones((H, W), bool)
        ax, ay = self.to_screen(box[0], box[1])
        bx, by = self.to_screen(box[2], box[3])
        return (XS >= ax) & (XS < bx) & (YS >= ay) & (YS < by)

    def to_screen(self, x, y):
        x0, y0, x1, y1 = self.crop
        return int(round((x - x0) * W / (x1 - x0))), int(round((y - y0) * H / (y1 - y0)))

    def find_snoopy(self, box):
        S = 4
        big = self.src.resize((W * S, H * S), Image.LANCZOS).filter(ImageFilter.MedianFilter(3))
        c = Colours(np.asarray(big))
        inbox = np.repeat(np.repeat(self.box_mask(box), S, 0), S, 1)
        cream = inbox & (self.snoopy_colour(c) if self.snoopy_colour else (c.mn > 160) & (c.mx - c.mn < 70))
        # a plain light background touching the picture's edge is not Snoopy
        light = c.mn > 200
        bg = np.zeros_like(light)
        bg[0, :], bg[-1, :], bg[:, 0], bg[:, -1] = light[0, :], light[-1, :], light[:, 0], light[:, -1]
        for _ in range(400):
            nb = grow(bg, 2) & light
            if (nb == bg).all():
                break
            bg = nb
        cream &= ~bg
        ink = inbox & (c.lum < 75) & grow(cream, 5)
        # his black ear(s): follow the black outward from his outline, a limited way
        dark = inbox & (c.lum < 60)
        for _ in range(60):
            nxt = grow(ink, 1) & dark | ink
            if (nxt == ink).all():
                break
            ink = nxt
        body = grow(cream, 1) | ink
        small = lambda m: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.BOX))
        self.cream = drop_small(small(cream) > 115, 8)
        self.ink = (small(ink) > 90) & ~self.cream
        self.body = (small(body) > 70) | self.cream
        self.body |= ~drop_small(~self.body, 8)
        self.body = drop_small(self.body, 10)
        self.cream &= self.body
        self.ink &= self.body

    def prop(self, colour, style, box=None, keep=10, close=1, fill=False):
        m = colour(self.c) & self.box_mask(box)
        if close:
            m = shrink(grow(m, close), close)
        if fill:                                                     # one solid shape: no holes, no logo
            m |= ~drop_small(~m, 40)
        m = drop_small(m, keep) & ~grow(self.body, 1)
        return (m, style)

    def frame(self, f, look):
        img = np.zeros((H, W), bool)
        rng = np.random.default_rng(31)
        s2 = 0.5 + 0.5 * math.sin(2 * math.pi * 2 * f / N)
        for m, style in self.props:
            out = edges(m)
            inside = m & ~out
            if style == "stripes":                                   # upholstery
                img |= out | (inside & (XS % 3 == 0) & (YS % 2 == 0))
            elif style == "plaid":
                img |= out | (inside & (((XS % 5 == 0) & (YS % 2 == 0)) | ((YS % 5 == 0) & (XS % 2 == 0))))
            elif style == "outline":
                img |= out
            elif style == "solid":
                img |= m
            elif style == "blinds":
                img |= out | (inside & (YS % 3 == 0))
            elif style == "panes":                                   # window glass with snow falling
                img |= out
                for i in range(70):
                    x0, y0 = int(rng.integers(0, W)), float(rng.uniform(0, H))
                    laps = 1 + i % 2
                    y = int((y0 + H * laps * f / N) % H)
                    x = int(x0 + 1.5 * math.sin(2 * math.pi * f / N * 2 + i)) % W
                    if inside[y, x]:
                        img[y, x] = True
            elif style == "warm":                                    # lit glass, flickering softly
                img |= out
                s = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * f / N) * math.sin(2 * math.pi * 5 * f / N + 1)
                img |= inside & (BT < 0.10 + 0.22 * s)
            elif style == "glow":                                    # a lamp breathing light
                img |= m
                halo = grow(m, 2 + int(round(s2 * 2))) & ~m & ~grow(self.body, 1)
                img |= halo & (BT < 0.10 + 0.14 * s2)
            elif style == "leafy":                                   # leaves stirring
                flick = np.random.default_rng(f // 3 % 32).random((H, W))
                img |= m & (BT < 0.42) & (flick > 0.18)
            elif style == "screen":                                  # a TV flickering
                img |= edges(grow(m, 1))
                lvl = 0.22 + 0.16 * math.sin(2 * math.pi * 3 * f / N) + 0.08 * ((f * 7) % 3)
                noise = np.random.default_rng(f % 24).random((H, W)) < lvl
                roll = (YS - f) % 9 == 0
                img |= inside & (noise | roll)
            elif style == "night":                                   # dark windows: lights streaming past
                for i in range(9):
                    y = int(rng.integers(0, H))
                    x0 = float(rng.uniform(0, W))
                    ln = 2 + i % 4
                    x = int((x0 - f * (2 + i % 3) * W / N * 2) % (W + 10)) - 5
                    for dx in range(ln):
                        if 0 <= x + dx < W and inside[y, x + dx]:
                            img[y, x + dx] = True
                for i in range(16):
                    x, y = int(rng.integers(0, W)), int(rng.integers(0, H))
                    if inside[y, x] and (f // 4 + i * 5) % 11:
                        img[y, x] = True
            elif style == "folds":                                   # a blanket: outline + its fold lines
                img |= out
        for kind, arg in self.fx:
            if kind == "galaxy":                                     # a spiral galaxy turning slowly
                gx, gy = self.to_screen(arg[0], arg[1])
                zone = self.box_mask(arg[2]) & ~grow(self.body, 1)
                dx, dy = XS - gx, (YS - gy) / 0.35
                rr = np.hypot(dx, dy)
                th = np.arctan2(dy, dx) + 2 * math.pi * f / N
                arm = np.cos(2 * th - rr * 0.22)
                dens = np.clip(1 - rr / 34, 0, 1) * (0.5 + 0.5 * arm) + np.clip(1 - rr / 6, 0, 1)
                img |= zone & (dens * 0.75 > BT)
            if kind == "folds":                                      # dark fold lines inside a region
                img |= arg[1](self.c) & self.box_mask(arg[0]) & ~grow(self.body, 1)
            if kind == "steam":
                sx, sy = self.to_screen(*arg)
                for k in range(3):
                    t = ((f + k * 11) % 32) / 32.0
                    for j in range(5):
                        u = t + j * 0.06
                        if u > 1:
                            continue
                        x = int(sx + k * 2 - 2 + math.sin(u * 9 + k) * 2)
                        y = int(sy - u * 14)
                        if 0 <= x < W and 0 <= y < H and (j + f // 2) % 2 == 0:
                            img[y, x] = True
            if kind == "dust":
                for i in range(12):
                    x0, y0 = float(rng.uniform(arg[0], arg[2])), float(rng.uniform(arg[1], arg[3]))
                    x, y = self.to_screen(x0 + 18 * math.sin(2 * math.pi * f / N + i),
                                          y0 + 12 * math.cos(2 * math.pi * f / N * 2 + i * 1.7))
                    if 0 <= x < W and 0 <= y < H and (f // 3 + i) % 5:
                        img[y, x] = True
            if kind == "notes":                                      # music drifting from headphones
                sx, sy = self.to_screen(*arg)
                for k in range(2):
                    t = ((f + k * 24) % 48) / 48.0
                    x = int(sx + t * 14 + math.sin(t * 8 + k) * 2)
                    y = int(sy - t * 16)
                    if t < 0.85:
                        for rr, row in enumerate(NOTE):
                            for cc, v in enumerate(row):
                                if v == "1" and 0 <= x + cc < W and 0 <= y + rr < H:
                                    img[y + rr, x + cc] = True
            if kind == "glowface":                                   # screen light flickering on his face
                ax, ay = self.to_screen(arg[0], arg[1])
                bx, by = self.to_screen(arg[2], arg[3])
                s = 0.5 + 0.5 * math.sin(2 * math.pi * 4 * f / N) * (0.7 + 0.3 * math.sin(2 * math.pi * 11 * f / N))
                zone = (XS >= ax) & (XS < bx) & (YS >= ay) & (YS < by) & ~grow(self.body, 1)
                img |= zone & (BT < 0.08 + 0.2 * s) & ((XS + YS) % 2 == 0)

        lift = 1 if math.sin(2 * math.pi * f / 48) > 0.4 else 0       # Snoopy breathes
        cream, ink, body = self.cream, self.ink, self.body
        if lift:
            cream, ink, body = (np.roll(m, -1, 0) for m in (cream, ink, body))
            for m in (cream, ink, body):
                m[-1] = False
        img &= ~grow(body, 1)
        img |= cream & ~ink
        img |= edges(body) & ~cream
        img |= edges(drop_small(ink, 18)) & ~edges(body)               # his ear, outlined so it shows
        if look == "light":
            img = ~img
        return Image.fromarray((img * 255).astype(np.uint8)).convert("1")

    def frames(self, look):
        return [self.frame(f, look) for f in range(N)]


RED = lambda c: (c.r > c.g + 60) & (c.g < 105)
BLUE = lambda c: (c.b > c.r + 8) & (c.lum < 170)

SCENES = {
    "cozy": dict(
        picture="cozy.png", crop=(0, 45, 526, 308), snoopy_box=(80, 50, 310, 300),
        props=[
            dict(colour=RED, style="stripes", keep=30),
            dict(colour=BLUE, style="panes", box=(120, 45, 526, 230), keep=12),
            dict(colour=lambda c: c.lum > 165, style="glow", box=(325, 50, 430, 140), keep=6),
            dict(colour=lambda c: c.lum > 165, style="solid", box=(370, 195, 470, 235), keep=4),
        ],
        effects=[("steam", (228, 196))]),
    "couch": dict(
        picture="couch.png", crop=(0, 100, 634, 417), snoopy_box=(380, 160, 634, 340),
        props=[
            dict(colour=lambda c: (c.b > c.r - 12) & (c.lum > 50) & (c.lum < 175), style="plaid",
                 box=(355, 170, 634, 417), keep=40, close=2),
            dict(colour=lambda c: c.lum < 120, style="screen", box=(20, 122, 146, 198), keep=10, close=2),
            dict(colour=lambda c: c.lum > 175, style="blinds", box=(395, 100, 634, 195), keep=20, close=2),
            dict(colour=lambda c: (c.g > c.r + 6), style="leafy", box=(225, 100, 385, 290), keep=12, close=1),
            dict(colour=lambda c: (c.r > c.g + 18) & (c.lum > 55) & (c.lum < 160), style="outline",
                 box=(148, 262, 362, 410), keep=30, close=2),
            dict(colour=lambda c: (c.r > c.g + 12) & (c.lum < 110), style="outline",
                 box=(0, 200, 222, 330), keep=30, close=2),
        ],
        effects=[("dust", (420, 110, 600, 250))]),
    "window": dict(
        picture="window.png", crop=(0, 70, 528, 334), snoopy_box=(185, 130, 370, 300),
        props=[
            dict(colour=lambda c: c.lum < 105, style="warm", box=(55, 70, 528, 300), keep=20, close=1),
            dict(colour=lambda c: (c.g > c.r + 18) & (c.g > c.b + 10), style="leafy", keep=10, close=1),
        ]),
    "bus": dict(
        picture="bus_hd.png", crop=(0, 100, 802, 501), snoopy_box=(250, 150, 590, 501),
        snoopy_colour=lambda c: (c.b > c.r + 5) & (c.mn > 80) & (c.mx - c.mn < 120),
        props=[
            dict(colour=lambda c: c.lum < 40, style="night", box=(0, 120, 802, 330), keep=25, close=1),
            dict(colour=lambda c: (c.r > c.g + 80), style="outline", box=(0, 100, 802, 330), keep=80, close=2),
            dict(colour=lambda c: (c.r > c.g + 80), style="outline", box=(0, 330, 802, 501), keep=60, close=2),
        ],
        effects=[("galaxy", (330, 160, (0, 120, 620, 255)))]),
    "laptop": dict(
        picture="laptop.png", crop=(-6, -6, 296, 145), snoopy_box=(85, 0, 200, 75), pad=(255, 255, 255),
        props=[
            dict(colour=RED, style="outline", keep=30, close=1),
            dict(colour=lambda c: (c.mn > 140) & (c.mx - c.mn < 30) & (c.mn < 235), style="outline",
                 box=(110, 60, 215, 135), keep=10, close=2, fill=True),
        ],
        effects=[("folds", ((30, 10, 160, 140), lambda c: (c.lum < 70))),
                 ("notes", (95, 25)), ("glowface", (150, 45, 185, 75))]),
}


def make(name, look):
    return Scene(**SCENES[name]).frames(look)


def snoopy_hq(picture, box, crop, ink_max=70, reach=45, cream=lambda r, g, b, mn, mx: (mn > 145) & (mx - mn < 70)):
    """Snoopy at screen size, cut out carefully at full resolution:
    returns (white, black, silhouette) masks. His black outline is separated
    from darker background by brightness, and only what connects to his white
    counts, so the background never leaks in."""
    im = Image.open(os.path.join(HERE, picture)).convert("RGB")
    x0, y0, x1, y1 = crop
    full = Image.new("RGB", (x1 - x0, y1 - y0), (0, 0, 0))
    full.paste(im, (-x0, -y0))
    a = np.asarray(full).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    mn, mx = np.minimum(np.minimum(r, g), b), np.maximum(np.maximum(r, g), b)
    zone = np.zeros(lum.shape, bool)
    zone[max(0, box[1] - y0):box[3] - y0, max(0, box[0] - x0):box[2] - x0] = True
    wh = zone & cream(r, g, b, mn, mx)
    bk = zone & (lum < ink_max)
    wh = drop_small(wh, 400)                               # his white areas (not specks)
    sil = wh.copy()
    cand = wh | bk
    for _ in range(reach):                                 # his black parts, near his white
        nxt = grow(sil, 1) & cand | sil
        if (nxt == sil).all():
            break
        sil = nxt
    sil = shrink(grow(sil, 2), 2)
    sil |= ~drop_small(~sil, 300)                          # fill holes (nose, eyes stay as black)
    bk = sil & ~wh
    def small(m, t):
        return np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.BOX)) > t
    S = small(sil, 110)
    Wt = small(wh, 120) & S
    return Wt, S & ~Wt, S
