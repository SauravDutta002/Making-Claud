"""
bed_anim.py - brings art/bed.png to life: Snoopy in bed, phone in hand, charging.

    python art/bed_anim.py      -> art/bed_anim.gif (+ art/bed_anim_big.gif)
    python tools/make_frames.py "art/bed_anim.gif=Snoopy in bed" --dither none --contrast 1.0

The cartoon's black outlines become the screen's white lines (big black areas
like his ear become outlines). Then: z's float up, the phone's light flickers
on his face, and a little status bar charges its battery.
96 frames, loops seamlessly.
"""
import math
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N, MS = 96, 70
CROP = (0, 108, 238, 227)        # the part of the picture with Snoopy (2:1)
GLOW = (44, 24, 64, 46)          # where the phone lights up his face
PHONE = (61, 24, 74, 45)
Z_FROM = (40, 22)

Z = [["1111", "0010", "0100", "1111"],
     ["11111", "00010", "00100", "01000", "11111"],
     ["111111", "000010", "000100", "001000", "010000", "111111"]]


def font():
    """The keychain's own 5x7 font, taken from the preview page."""
    src = open(os.path.join(HERE, "..", "tools", "preview.py"), encoding="utf-8").read()
    hexs = re.search(r'const FONT="([0-9a-f]+)"', src).group(1)
    return bytes.fromhex(hexs)


FONT = font()


def text(px, s, x, y):
    for ch in s:
        c = ord(ch) if 32 <= ord(ch) <= 126 else 63
        for i in range(5):
            col = FONT[(c - 32) * 5 + i]
            for j in range(8):
                if col >> j & 1 and 0 <= x + i < W and 0 <= y + j < H:
                    px[x + i, y + j] = 255
        x += 6


def base():
    im = Image.open(os.path.join(HERE, "bed.png")).convert("RGB")
    sm = im.crop(CROP).resize((W, H), Image.LANCZOS)
    L = sm.convert("L").load()
    dark = [[L[x, y] < 95 and x < 124 for x in range(W)] for y in range(H)]
    out = Image.new("L", (W, H), 0)
    p = out.load()
    for y in range(H):
        for x in range(W):
            if not dark[y][x]:
                continue
            # big solid areas (his ear) become an outline; lines stay lines
            solid = all(dark[yy][xx] for yy in range(max(0, y - 2), min(H, y + 3))
                        for xx in range(max(0, x - 2), min(W, x + 3)))
            if not solid:
                p[x, y] = 255
    # the charger: a clean wall socket, the plug, and its cable down to the phone
    for x in range(108, 119):
        p[x, 9] = p[x, 22] = 255
    for y in range(9, 23):
        p[108, y] = p[118, y] = 255
    for y in range(12, 16):
        p[111, y] = p[115, y] = 255                      # the socket's slots
    for x in range(110, 117):
        for y in range(15, 20):
            p[x, y] = 255                                # the plug
    cable = [(113, 20 + t) for t in range(4)]
    for t in range(60):                                  # a soft curve down and left
        u = t / 59
        cx = (1 - u) ** 2 * 113 + 2 * (1 - u) * u * 112 + u * u * 96
        cy = (1 - u) ** 2 * 23 + 2 * (1 - u) * u * 42 + u * u * 44
        cable.append((round(cx), round(cy)))
    for x, y in cable:
        if 0 <= x < W and 0 <= y < H and not any(p[x + dx, y + dy] for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                                               if (x + dx, y + dy) not in cable and 0 <= x + dx < W and 0 <= y + dy < H):
            p[x, y] = 255
    return out


def base_filled():
    """Exactly the clean line drawing (as in the light version), but only Snoopy
    is white: everything outside his outline - reached from the top and sides
    without crossing a line - turns black."""
    import numpy as np
    import sys
    sys.path.insert(0, HERE)
    import scene
    lines = np.asarray(base()).astype(bool)                  # the clean white-line drawing
    lines[:26, 100:] = False                                 # (the socket is added back below)
    walls = scene.grow(lines, 2)                             # small gaps in his outline don't let the dark in
    open_ = ~walls
    out = np.zeros_like(open_)
    out[0, :] = open_[0, :]
    out[:48, 0] = open_[:48, 0]
    out[:48, -1] = open_[:48, -1]
    for _ in range(300):
        nxt = scene.grow(out, 1) & open_
        if (nxt == out).all():
            break
        out = nxt
    outside = scene.grow(out, 2) & ~lines                    # the black room
    white = ~outside & ~lines
    white = scene.drop_small(white, 10)
    img = Image.fromarray((white * 255).astype(np.uint8)).copy()
    p = img.load()
    ref = base().load()
    for y in range(H):
        for x in range(94, W):
            if ref[x, y] and y < 46:                         # socket, plug and cable, in white
                p[x, y] = 255
    return img


def frame(f, bw):
    im = bw.copy()
    px = im.load()

    # the phone's light flickering on his face
    x0, y0, x1, y1 = GLOW
    bright = 0.5 + 0.5 * math.sin(2 * math.pi * 4 * f / N) * (0.7 + 0.3 * math.sin(2 * math.pi * 11 * f / N))
    for y in range(y0, y1):
        for x in range(x0, x1):
            d = math.hypot((x - x1) / 15.0, (y - (y0 + y1) / 2) / 10.0)
            near_line = any(bw.getpixel((xx, yy)) for xx in (x - 1, x, x + 1) for yy in (y - 1, y, y + 1)
                            if 0 <= xx < W and 0 <= yy < H)
            if d < 1 and not near_line and ((x + y) % 2 == 0) and (1 - d) * bright > 0.12 + ((x * 7 + y * 3 + f) % 5) * 0.06:
                px[x, y] = 255
    # the phone screen: a little light scrolling (he's reading)
    sx, sy = 66, 30
    for k in range(3):
        yy = sy + (k * 4 + f // 3) % 10
        for xx in range(sx, sx + 3):
            if px[xx, yy] == 0 and (f // 3 + k) % 4:
                px[xx, yy] = 255

    # z z Z
    for k in range(3):
        t = ((f + k * 11) % 32) / 32.0
        if t > 0.92:
            continue
        g = Z[min(2, int(t * 3))]
        zx = int(Z_FROM[0] + t * 12 + math.sin(t * 6.3) * 2)
        zy = int(Z_FROM[1] - t * 18)
        for r, row in enumerate(g):
            for c, v in enumerate(row):
                if v == "1":
                    px[zx + c, zy + r] = 255

    # status bar, top right: signal, wifi, the battery charging
    for i in range(4):                                     # signal bars
        h = 2 + i
        for y in range(7 - h, 7):
            px[70 + i * 2, y] = 255
    for i, rr in enumerate((1, 3, 5)):                     # wifi arcs
        for a in range(-40, 41, 8):
            ang = math.radians(a - 90)
            x, y = 84 + round(math.cos(ang) * rr), 6 + round(math.sin(ang) * rr)
            if (f // 6 + i) % 4 != 3:
                px[x, y] = 255
    text(px, "28%", 93, 0)                                 # what the picture says
    bx, by = 112, 1                                        # battery
    for x in range(bx, bx + 13):
        px[x, by] = px[x, by + 6] = 255
    for y in range(by, by + 7):
        px[bx, y] = px[bx + 12, y] = 255
    px[bx + 13, by + 2] = px[bx + 13, by + 3] = px[bx + 13, by + 4] = 255
    level = 3 + (f // 12) % 8                              # fills up, again and again
    for x in range(bx + 2, min(bx + 2 + level, bx + 11)):
        for y in range(by + 2, by + 5):
            px[x, y] = 255
    return im.convert("1")


def main():
    bw = base()
    frames = [frame(f, bw) for f in range(N)]
    out = os.path.join(HERE, "bed_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [f.convert("L").resize((W * 4, H * 4), Image.NEAREST) for f in frames]
    big[0].save(os.path.join(HERE, "bed_anim_big.gif"), save_all=True, append_images=big[1:], duration=MS, loop=0)
    print(f"wrote {out}: {N} frames")


if __name__ == "__main__":
    main()
