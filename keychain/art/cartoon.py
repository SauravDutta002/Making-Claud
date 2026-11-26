"""
cartoon.py - flat-colour Snoopy cartoons (clean colours, black outlines) turned
into keychain animations. Each colour becomes its own look on the 1-bit screen:
Snoopy's white stays white, outlines black, and the rest gets a pattern
(planks for the red doghouse, a checker for the green helmet, dots for
Woodstock...). Paper / sky becomes the black screen, and each scene gets its
own motion.

    ace      the flying ace on his doghouse: wind streaming, scarf flapping, Woodstock bobbing
    houseday Snoopy on his doghouse: clouds drifting, grass swaying, birds passing
    heart    Snoopy hugging a big heart: it beats, little hearts float up
    car      Snoopy driving at night: moon and stars, road rushing by, wheels turning

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
BT = scene.BT
YS, XS = np.mgrid[0:H, 0:W]
HEART = ["01010", "11111", "11111", "01110", "00100"]

# colour tests on (r, g, b, lum, mn, mx)
WHITE = lambda r, g, b, l, mn, mx: (mn > 246) & (mx - mn < 10)
INK = lambda r, g, b, l, mn, mx: l < 55
RED = lambda r, g, b, l, mn, mx: (r > 140) & (g < 95) & (b < 95)
GREEN = lambda r, g, b, l, mn, mx: (g > r + 15) & (g > b + 15) & (l < 170)
YELLOW = lambda r, g, b, l, mn, mx: (r > 200) & (g > 170) & (b < 130)
BROWN = lambda r, g, b, l, mn, mx: (r > g + 25) & (r > b + 40) & (l > 60) & (l < 175) & ~((g < 95) & (b < 95))

SCENES = {
    "ace": dict(picture="ace.png", crop=(-24, 14, 936, 494), pad=(121, 161, 163), card=(14, 4, 904, 600),
                classes=[("white", WHITE, "white"), ("ink", INK, "ink"), ("green", GREEN, "checker"),
                         ("brown", BROWN, "light"), ("red", RED, "stripes"), ("yellow", YELLOW, "dots")]),
    "houseday": dict(picture="house_day.png", crop=(-109, 0, 763, 436), pad=(158, 194, 249),
                     classes=[("white", WHITE, "white"), ("ink", INK, "ink"), ("red", RED, "plank"),
                              ("grass", lambda r, g, b, l, mn, mx: (g > r + 20) & (g > b + 5), "none"),
                              ("cloud", lambda r, g, b, l, mn, mx: (mn > 205) & (b > r + 8), "none")]),
    "heart": dict(picture="heart.png", crop=(-140, 112, 560, 462), pad=(242, 237, 233), card=(30, 30, 410, 553),
                  classes=[("white", WHITE, "white"), ("ink", INK, "ink"), ("red", RED, "checker")]),
    "nap": dict(picture="nap.png", crop=(-205, 48, 775, 538), pad=(200, 225, 250), card=(8, 6, 546, 548),
                classes=[("white", WHITE, "white"), ("ink", INK, "ink"),
                         ("orange", lambda r, g, b, l, mn, mx: (r > 200) & (g > 70) & (g < 160) & (b < 80), "white"),
                         ("yellow", YELLOW, "dots")]),
    "coffee": dict(picture="coffee.png", crop=(-170, 6, 560, 371), pad=(201, 165, 91), card=(2, 2, 388, 379),
                   classes=[("white", WHITE, "white"), ("ink", INK, "ink"),
                            ("red", lambda r, g, b, l, mn, mx: (r > 150) & (g < 90) & (b < 90), "checker")]),
    "moon": dict(picture="moon.png", crop=(-105, 28, 435, 298), pad=(174, 217, 226), card=(4, 4, 327, 342),
                 classes=[("white", lambda r, g, b, l, mn, mx: (mn > 232) & (mx - mn < 14), "white"),
                          ("ink", INK, "ink"),
                          ("moonc", lambda r, g, b, l, mn, mx: (r > 220) & (g > 205) & (b < 210) & (r - b > 30), "light")]),
    "phones": dict(picture="phones.png", crop=(-112, 4, 312, 216), pad=(247, 195, 187), card=(3, 3, 196, 222),
                   classes=[("white", lambda r, g, b, l, mn, mx: (mn > 236) & (mx - mn < 14), "white"),
                            ("ink", lambda r, g, b, l, mn, mx: l < 100, "ink"),
                            ("phone", lambda r, g, b, l, mn, mx: (g > r + 10) & (l > 140), "dots")]),
    "tree": dict(picture="tree.png", crop=(-2, 282, 306, 436), pad=(250, 240, 225), card=(2, 2, 302, 438),
                 classes=[("white", lambda r, g, b, l, mn, mx: (mn > 236) & (mx - mn < 18), "white"),
                          ("ink", lambda r, g, b, l, mn, mx: l < 70, "ink"),
                          ("red", lambda r, g, b, l, mn, mx: (r > 170) & (g < 90) & (b < 90), "white"),
                          ("yellow", YELLOW, "dots"),
                          ("trunk", lambda r, g, b, l, mn, mx: (r > g + 15) & (r > b + 35) & (l < 160) & ~((g < 90) & (b < 90)), "outline"),
                          ("leafg", lambda r, g, b, l, mn, mx: (g > r + 5) & (g > b + 20), "dots")]),
    "hearts": dict(picture="hearts.png", crop=(-103, 28, 497, 328), pad=(245, 245, 245), card=(2, 2, 384, 442),
                   classes=[("white", lambda r, g, b, l, mn, mx: (mn > 205) & (r - b > 12) & (r - b < 45) & (g > 225), "white"),
                            ("ink", INK, "ink"), ("yellow", YELLOW, "dots"),
                            ("scarf", lambda r, g, b, l, mn, mx: (b > r + 40) & (l < 120), "checker")]),
    "car": dict(picture="car.png", crop=(18, 12, 588, 297), pad=(242, 238, 235),
                classes=[("white", WHITE, "white"), ("ink", INK, "ink"),
                         ("blue", lambda r, g, b, l, mn, mx: (b > r + 18) & (l > 160) & (l < 245), "white"),
                         ("grey", lambda r, g, b, l, mn, mx: (mx - mn < 16) & (l > 140) & (l < 215), "white"),
                         ("yellow", YELLOW, "dots"),
                         ("board", lambda r, g, b, l, mn, mx: (l > 70) & (l < 225) & (mx - mn > 18)
                          & ~((b > r + 18) & (l > 160)), "outline")]),
}


def classify(cfg):
    im = Image.open(os.path.join(HERE, cfg["picture"])).convert("RGB")
    x0, y0, x1, y1 = cfg["crop"]
    canvas = Image.new("RGB", (x1 - x0, y1 - y0), cfg["pad"])
    if "card" in cfg:                                         # only the picture card, not the page around it
        cx0, cy0, cx1, cy1 = cfg["card"]
        canvas.paste(im.crop(cfg["card"]), (cx0 - x0, cy0 - y0))
    else:
        canvas.paste(im, (-x0, -y0))
    S = 4
    a = np.asarray(canvas.resize((W * S, H * S), Image.LANCZOS)).astype(float)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    mn, mx = np.minimum(np.minimum(r, g), b), np.maximum(np.maximum(r, g), b)
    taken = np.zeros(lum.shape, bool)
    fracs = []
    for name, test, style in cfg["classes"]:
        m = test(r, g, b, lum, mn, mx) & ~taken
        taken |= m
        fracs.append(m.reshape(H, S, W, S).mean(axis=(1, 3)))
    fr = np.stack(fracs)
    best = fr.argmax(0)
    ok = fr.max(0) > 0.3
    # thin black lines: keep them even when they are a minority of the pixel
    ink_i = [c[0] for c in cfg["classes"]].index("ink")
    ink = fr[ink_i] > 0.22
    labels = {}
    for i, (name, _, style) in enumerate(cfg["classes"]):
        labels[name] = (best == i) & ok
    labels["ink"] = ink | labels["ink"]
    for name in labels:
        if name != "ink":
            labels[name] &= ~labels["ink"]
    return labels


def draw_classes(cfg, L, shift=None):
    img = np.zeros((H, W), bool)
    fig = np.zeros((H, W), bool)
    for name, _, style in cfg["classes"]:
        m = L[name]
        if shift and name in shift:
            m = shift[name](m)
        if style == "none":
            continue
        fig |= m
        if style == "white":
            img |= m
        elif style == "checker":
            img |= m & ((XS + YS) % 2 == 0)
        elif style == "dots":
            img |= (m & (BT < 0.62)) | scene.edges(m)
        elif style == "light":
            img |= (m & (BT < 0.2)) | scene.edges(m)
        elif style == "stripes":
            img |= (m & (YS % 2 == 0)) | scene.edges(m)
        elif style == "plank":
            img |= scene.edges(m) | (m & (YS % 5 == 0))
        elif style == "outline":
            img |= scene.edges(m)
    ink = L["ink"]
    if shift and "ink" in shift:                              # black lines move with their part too
        ink = shift["ink"](ink)
    img &= ~ink
    img |= scene.edges(fig | ink) & ink                       # black parts on the outer edge get a white rim
    return img, fig | ink


def roll_wrap(m, dx):
    return np.roll(m, dx, axis=1)


def ridge(period, seed, lo, hi, jag):
    """A periodic skyline (heights for x = 0..period-1) of peaks."""
    rng = np.random.default_rng(seed)
    hgt = np.zeros(period)
    x = 0
    while x < period:
        w = int(rng.integers(14, 30))
        peak = float(rng.uniform(lo, hi))
        for i in range(w):
            u = abs(i - w / 2) / (w / 2)
            hgt[(x + i) % period] = max(hgt[(x + i) % period], peak * (1 - u) + rng.uniform(-jag, jag))
        x += int(w * 0.6)
    return hgt


MOUNTAINS = ridge(128, 11, 14, 26, 0.6)          # far range: tall, jagged
HILLS = ridge(64, 5, 4, 9, 0.3)                  # nearer: low hills with trees
HORIZON = 50                                     # where the mountains stand
ROAD = 59


def night_drive(f, rng):
    """Everything behind the car, in layers that move at different speeds."""
    out = np.zeros((H, W), bool)
    d = Image.new("1", (W, H), 0)
    dr = ImageDraw.Draw(d)
    dr.ellipse([104, 2, 117, 15], fill=1)                     # the moon
    dr.ellipse([100, 0, 113, 13], fill=0)
    out |= np.asarray(d).astype(bool)
    # far mountains: 128 px a loop (slow), outline + snowy tops
    sky_cut = np.full(W, H)
    off = int(round(f * 128 / N))
    for x in range(W):
        h = MOUNTAINS[(x - off) % 128]
        top = int(round(HORIZON - h))
        sky_cut[x] = top
        out[top, x] = True
        if h > 19 and (x + top) % 2 == 0:
            out[top + 1, x] = True                             # snow on the high peaks
    # stars only in the sky above the mountains, twinkling
    for i in range(30):
        x, y = int(rng.integers(0, W)), int(rng.integers(0, 34))
        if y < sky_cut[x] - 1 and (f // 3 + i * 5) % 17:
            out[y, x] = True
    # nearer hills with little pines: 256 px a loop (twice as fast)
    off2 = int(round(f * 256 / N))
    for x in range(W):
        h = HILLS[(x - off2) % 64]
        top = int(round(ROAD - 3 - h))
        out[top, x] = True
        out[top + 1:ROAD - 1, x] = False                       # hills hide the mountains behind
        if (x - off2) % 16 == 3:                               # a pine tree
            for k in range(5):
                for dx in range(-(k // 2 + 1) + 1, k // 2 + 1):
                    yy = top - 5 + k
                    if 0 <= x + dx < W and 0 <= yy < H:
                        out[yy, x + dx] = True
    # the road: an edge line, dashes and reflector posts rushing past (384 px a loop)
    out[ROAD, :] = True
    off3 = int(round(f * 384 / N))
    for k in range(-1, 10):
        x = k * 16 + off3 % 16 - 16
        out[ROAD + 3, max(0, x):max(0, min(W, x + 8))] = True
    for k in range(-1, 4):
        x = k * 48 + off3 % 48 - 28
        if 0 <= x < W:
            out[ROAD - 5:ROAD, x] = True
            out[ROAD - 5, x] = (f // 2) % 2 == 0 or True
    # exhaust puffs drifting back from the car (it faces left, so behind is to the right)
    for k in range(3):
        t = ((f + k * 8) % 24) / 24.0
        x = int(114 + t * 14)
        y = int(51 - t * 4)
        rad = 1 + int(t * 2)
        for dy in range(-rad, rad + 1):
            for dx in range(-rad, rad + 1):
                if dx * dx + dy * dy <= rad * rad and 0 <= x + dx < W and 0 <= y + dy < H and (dx + dy + k) % 2 == 0:
                    out[y + dy, x + dx] = True
    return out


def sip_offset(f):
    """The cup stays where it is in the picture (moving a cut-out looked broken)."""
    return 0, 0
    t = f % N
    lift = (3, -8)
    if t < 34:
        p = 0.0
    elif t < 46:
        p = (t - 34) / 12.0
    elif t < 62:
        p = 1.0
    elif t < 74:
        p = 1 - (t - 62) / 12.0
    else:
        p = 0.0
    p = p * p * (3 - 2 * p)                                   # smooth start and stop
    return int(round(lift[0] * p)), int(round(lift[1] * p))


def frame(name, f, L):
    cfg = SCENES[name]
    rng = np.random.default_rng(3)
    bg = np.zeros((H, W), bool)
    shift = {}

    if name == "ace":
        for i in range(12):                                   # wind streaming past
            y = int(rng.integers(2, H - 10))
            x0 = float(rng.uniform(0, W))
            x = int((x0 - f * 4 * W / N * 2) % (W + 20)) - 10
            ln = 4 + i % 5
            bg[y, max(0, x):max(0, min(W, x + ln))] = True
        flap = (0, 1, 0, -1)[(f // 3) % 4]
        top = YS < H * 0.78                                   # the scarf (not the doghouse below)
        shift["red"] = lambda m: (m & ~top) | (np.roll(m & top, flap, axis=0) & top)
        bob = 1 if (f // 6) % 2 else 0
        right = XS > W * 0.66
        shift["yellow"] = lambda m: (m & ~right) | np.roll(m & right, -bob, axis=0)

    if name == "houseday":
        t = f * W / N
        clouds = L["cloud"]
        moved = roll_wrap(clouds, int(t) % W)
        bg |= scene.edges(moved) & ((XS + YS) % 2 == 0)       # clouds drifting by (dotted outlines)
        grass = L["grass"]
        sway = (0, 1, 0, -1)[(f // 5) % 4]
        top_of_grass = grass & ~np.roll(grass, 1, axis=0)
        blades = np.roll(top_of_grass, sway, axis=1) & ((XS * 7 + 3) % 3 == 0)
        bg |= blades | np.roll(blades, -1, axis=0)
        bg[np.nonzero(grass.any(axis=1))[0].min() if grass.any() else H - 1, :] |= True
        for k in range(2):                                    # birds crossing the sky
            bx = int((f * 2 * W / N * 1.0 + k * 64) % (W + 20)) - 10
            by = 10 + k * 7 + int(2 * math.sin(f * 0.4 + k))
            wing = (f // 3 + k) % 2
            for dx, dy in ((0, 0), (1, -1 + wing), (-1, -1 + wing), (2, -wing), (-2, -wing)):
                if 0 <= bx + dx < W and 0 <= by + dy < H:
                    bg[by + dy, bx + dx] = True

    if name == "heart":
        beat = 1 if (f % 24) in (0, 1, 2, 6, 7) else 0       # lub-dub
        if beat:
            shift["red"] = lambda m: scene.grow(m, 1) & ~scene.grow(L["white"] & ~scene.grow(m, 2), 0)
        for k in range(4):                                    # little hearts floating up
            t = ((f + k * 24) % N) / N
            x = int(W * 0.68 + k * 9 + 3 * math.sin(t * 9 + k)) if k % 2 else int(W * 0.2 + k * 5 + 3 * math.sin(t * 9 + k))
            y = int(54 - t * 60)
            if t < 0.9:
                for rr, row in enumerate(HEART):
                    for cc, v in enumerate(row):
                        if v == "1" and 0 <= x + cc < W and 0 <= y + rr < H:
                            bg[y + rr, x + cc] = True

    if name == "nap":                                        # night: stars, a moon, z's, Woodstock breathing
        d = Image.new("1", (W, H), 0)
        dr = ImageDraw.Draw(d)
        dr.ellipse([104, 3, 117, 16], fill=1)
        dr.ellipse([100, 1, 113, 14], fill=0)
        bg |= np.asarray(d).astype(bool)
        for i in range(26):
            x, y = int(rng.integers(0, W)), int(rng.integers(0, 40))
            if (f // 3 + i * 5) % 17:
                bg[y, x] = True
        Z = [["1111", "0010", "0100", "1111"], ["11111", "00010", "00100", "01000", "11111"],
             ["111111", "000010", "000100", "001000", "010000", "111111"]]
        for k in range(3):
            t = ((f + k * 11) % 32) / 32.0
            if t > 0.92:
                continue
            g = Z[min(2, int(t * 3))]
            zx, zy = int(16 + t * 10 + math.sin(t * 6.3) * 2), int(12 - t * 12)
            for rr, row in enumerate(g):
                for cc, v in enumerate(row):
                    if v == "1" and 0 <= zx + cc < W and 0 <= zy + rr < H:
                        bg[zy + rr, zx + cc] = True
        bob = 1 if (f // 12) % 2 else 0
        shift["yellow"] = lambda m: np.roll(m, -bob, axis=0)

    if name in ("coffee", "moon"):                            # a starry night behind (slow, calm twinkle)
        for i in range(18):
            x, y = int(rng.integers(0, W)), int(rng.integers(0, H))
            if (f // 8 + i * 5) % 12:
                bg[y, x] = True
    if name == "coffee":                                      # he lifts the cup, sips, lowers it
        dx, dy = sip_offset(f)
        cup = (XS >= 28) & (XS <= 57) & (YS >= 43) & (YS <= 61)
        steam_box = (XS >= 32) & (XS <= 47) & (YS >= 34) & (YS <= 42)

        def move(m, dx=dx, dy=dy):
            m = m & ~steam_box                               # the drawn steam: animated instead
            return (m & ~cup) | np.roll(np.roll(m & cup, dy, axis=0), dx, axis=1)
        for key in ("white", "ink", "red"):
            shift[key] = move
        # steam: three soft wavy wisps curling up from the cup, drifting upward, fading at the top
        top = 41
        for k in range(3):
            bx = 34 + k * 4
            for y in range(top - 18, top):
                u = (top - y) / 18.0                              # 0 at the cup .. 1 at the top
                x = bx - u * 14 + 2.2 * math.sin(y * 0.45 + 2 * math.pi * f / N * 3 + k * 2.1) * (0.4 + u)
                drift = (y + int(f * 18 / N * 3)) % 7              # dashes travelling up
                keep = drift < 5 and (u < 0.6 or drift < 3 - int(u * 3) + 2)
                if keep and 0 <= int(round(x)) < W and 0 <= y < H:
                    bg[y, int(round(x))] = True
        t = (f % N) / N                                           # now and then, a little heart rises out of it
        if 0.15 < t < 0.75:
            v = (t - 0.15) / 0.6
            hx, hy = int(round(14 - v * 6 + 2 * math.sin(v * 6))), int(round(top - 16 - v * 18))
            for rr, row in enumerate(HEART):
                for cc, val in enumerate(row):
                    if val == "1" and 0 <= hx + cc < W and 0 <= hy + rr < H:
                        bg[hy + rr, hx + cc] = True

    if name == "moon":                                        # rocking gently, z's floating up
        rock = (0, 1, 0, -1)[(f // 12) % 4]
        for key in ("white", "ink", "moonc"):
            shift[key] = (lambda m, r=rock: np.roll(m, r, axis=0))
        Z = [["1111", "0010", "0100", "1111"], ["11111", "00010", "00100", "01000", "11111"],
             ["111111", "000010", "000100", "001000", "010000", "111111"]]
        for k in range(3):
            t = ((f + k * 11) % 32) / 32.0
            if t > 0.92:
                continue
            g = Z[min(2, int(t * 3))]
            zx, zy = int(76 + t * 12 + math.sin(t * 6.3) * 2), int(18 - t * 16)
            for rr, row in enumerate(g):
                for cc, v in enumerate(row):
                    if v == "1" and 0 <= zx + cc < W and 0 <= zy + rr < H:
                        bg[zy + rr, zx + cc] = True
    if name == "phones":                                      # calm: notes drifting up, a smooth equalizer
        NOTE = ["0110", "0101", "0100", "1100", "1100"]
        NOTE2 = ["0111", "0101", "0101", "1101", "1101"]
        for k in range(4):
            t = ((f + k * 24) % N) / N                        # each note: a smooth path up and out
            side = k % 2
            x = (86 + t * 24) if side else (38 - t * 24)
            x += 2 * math.sin(t * 2 * math.pi * 1.5 + k)
            y = 30 - t * 30
            pat = NOTE2 if k >= 2 else NOTE
            if t < 0.88:
                for rr, row in enumerate(pat):
                    for cc, v in enumerate(row):
                        xx, yy = int(round(x)) + cc, int(round(y)) + rr
                        if v == "1" and 0 <= xx < W and 0 <= yy < H:
                            bg[yy, xx] = True
        for side, x0 in ((0, 6), (1, 98)):                    # equalizer bars, rising and falling smoothly
            for i in range(6):
                ph = 2 * math.pi * (f / N) * (2 + (i % 3)) + i * 1.3 + side * 0.7
                h = int(round(3 + 4 * (0.5 + 0.5 * math.sin(ph))))
                bg[H - 1 - h:H - 1, x0 + i * 4:x0 + i * 4 + 2] = True

    if name == "hearts":                                      # hearts floating up, Woodstock bobbing
        for k in range(6):
            t = ((f + k * 16) % N) / N
            x = int((8 + k * 9) if k % 2 else (100 + k * 4)) + int(3 * math.sin(t * 9 + k))
            y = int(62 - t * 70)
            for rr, row in enumerate(HEART):
                for cc, v in enumerate(row):
                    if v == "1" and 0 <= x + cc < W and 0 <= y + rr < H:
                        bg[y + rr, x + cc] = True
        bob = 1 if (f // 12) % 2 else 0
        for key in ("yellow", "scarf"):
            shift[key] = (lambda m, b=bob: np.roll(m, -b, axis=0))

    if name == "tree":                                        # leaves drifting down, clouds floating by
        LEAF = ["0110", "1111", "0110"]
        for i in range(8):
            x0, y0 = float(rng.uniform(0, 70)), float(rng.uniform(0, H))
            y = int((y0 + H * f / N) % H)
            x = int(x0 + 8 * math.sin(2 * math.pi * f / N * 2 + i) + y * 0.2)
            for rr, row in enumerate(LEAF if (f // 4 + i) % 2 else ["010", "111", "010"]):
                for cc, v in enumerate(row):
                    if v == "1" and 0 <= x + cc < W and 0 <= y + rr < H:
                        bg[y + rr, x + cc] = True
        d = Image.new("1", (W, H), 0)
        dr = ImageDraw.Draw(d)
        for k, (cy, w_) in enumerate(((6, 18), (16, 13))):
            cx = int((f * W / N * (1 + k) + k * 60) % (W + 30)) - 20
            dr.ellipse([cx, cy, cx + w_, cy + 7], outline=1)
            dr.ellipse([cx + w_ // 3, cy - 4, cx + w_ // 3 + 9, cy + 4], outline=1)
            dr.rectangle([cx + 1, cy + 3, cx + w_ - 1, cy + 6], fill=0)
            dr.line([cx + 1, cy + 7, cx + w_ - 1, cy + 7], fill=1)
        bg |= np.asarray(d).astype(bool)

    if name == "car":
        bg |= night_drive(f, rng)
        bump = 0                                              # the car rides steady (no jolts)
        for key in ("white", "ink", "blue", "grey", "yellow", "board"):
            shift[key] = (lambda m, b=bump: np.roll(m, -b, axis=0))

    img, fig = draw_classes(cfg, L, shift)

    if name == "car":                                         # wheels turning: spokes rotate in the hubs
        hubs = scene.drop_small(L["grey"] & (YS > H * 0.6), 4)
        comps = []
        seen = np.zeros_like(hubs)
        for y, x in zip(*np.nonzero(hubs)):
            if seen[y, x]:
                continue
            pts, st = [], [(y, x)]
            seen[y, x] = True
            while st:
                cy, cx = st.pop()
                pts.append((cy, cx))
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < H and 0 <= nx < W and hubs[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        st.append((ny, nx))
            comps.append(pts)
        bob = 0                                               # steady wheels (no flicker)
        for pts in comps:
            cy = sum(p[0] for p in pts) / len(pts) - bob
            cx = sum(p[1] for p in pts) / len(pts)
            rad = max(2.0, math.sqrt(len(pts) / math.pi))
            for k in range(3):
                a = 2 * math.pi * (k / 3 - f / 8)                # driving left: wheels turn anticlockwise
                for t in (0.4, 0.8, 1.1):
                    x, y = int(round(cx + math.cos(a) * rad * t)), int(round(cy + math.sin(a) * rad * t))
                    if 0 <= x < W and 0 <= y < H:
                        img[y, x] = False

    if name == "coffee":
        dx, dy = sip_offset(f)
        if dx or dy:
            # his arm: the paw swept along the path, so it stretches smoothly behind the cup
            paw = (L["white"] | L["ink"]) & (XS >= 46) & (XS <= 57) & (YS >= 48) & (YS <= 61)
            n = max(abs(dx), abs(dy))
            sweep = np.zeros((H, W), bool)
            for i in range(n + 1):
                sweep |= np.roll(np.roll(paw, round(dy * i / n), axis=0), round(dx * i / n), axis=1)
            sweep = scene.shrink(scene.grow(sweep, 1), 1)
            cupm = np.roll(np.roll((L["white"] | L["ink"]) & (XS >= 28) & (XS <= 47) & (YS >= 43) & (YS <= 61), dy, axis=0), dx, axis=1)
            arm = sweep & ~scene.grow(cupm, 1)
            img &= ~scene.grow(arm, 1) | fig & ~arm
            img |= arm & ~scene.edges(arm)
            img &= ~(scene.edges(arm) & ~scene.grow(cupm, 0))
            img |= scene.edges(scene.grow(arm, 1)) & ~scene.grow(fig, 0) & False

    if name == "car":                                         # his eye and nose, drawn in so they read
        x0, y0, x1, y1 = cfg["crop"]
        k = W / (x1 - x0)
        ex, ey = int(round((321 - x0) * k)), int(round((114 - y0) * k))
        img[ey - 1:ey + 2, ex - 1:ex + 2] = True                  # white round the eye
        img[ey - 1:ey + 2, ex] = False                            # the eye: a little upright oval
        nx, ny = int(round((268 - x0) * k)), int(round((133 - y0) * k))
        img[ny - 2:ny + 2, nx - 1:nx + 3] = True                  # a white rim, so the nose shows at the edge
        img[ny - 1:ny + 1, nx:nx + 2] = False                     # the nose: a 2x2 black dot
    out = (bg & ~scene.grow(fig, 1)) | img
    return Image.fromarray((out * 255).astype(np.uint8)).convert("1")


def make(name):
    L = classify(SCENES[name])
    return [frame(name, f, L) for f in range(N)]


if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "ace"
    fr = make(name)
    fr[0].save(os.path.join(HERE, f"cartoon_{name}.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
