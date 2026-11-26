"""
doghouse_anim.py - brings art/doghouse.png to life: Snoopy asleep on his roof
under a starry sky.

    python art/doghouse_anim.py     -> art/doghouse_anim.gif (+ art/doghouse_anim_big.gif)
    python tools/make_frames.py art/doghouse_anim.gif --dither none --contrast 1.0

Snoopy and the moon come from the picture. The roof gets a crisp outline and a
light texture (red would just be black on this screen). The stars are redrawn
as sharp sparkles where they are in the picture, and they twinkle; the big star
pulses, a shooting star crosses the sky, Snoopy breathes and z's float up.
96 frames, loops seamlessly.
"""
import math
import os

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N = 96
MS = 70
CROP = (-8, 12, 296, 164)     # a 2:1 window of the picture (outside it is night sky)

Z = [["1111", "0010", "0100", "1111"],
     ["11111", "00010", "00100", "01000", "11111"],
     ["111111", "000010", "000100", "001000", "010000", "111111"]]


def rnd(n):
    n = (n << 13 ^ n) & 0xFFFFFFFF
    return ((n * (n * n * 15731 + 789221) + 1376312589) & 0xFFFFFFFF) >> 16 & 0xFF


def load():
    src = Image.open(os.path.join(HERE, "doghouse.png")).convert("RGB")
    x0, y0, x1, y1 = CROP
    win = Image.new("RGB", (x1 - x0, y1 - y0), (0, 0, 0))
    win.paste(src.crop((max(0, x0), y0, min(src.width, x1), y1)), (max(0, -x0), 0))
    sm = win.resize((W, H), Image.LANCZOS)
    r, g, b = sm.split()
    pale = ImageChops.darker(ImageChops.darker(r, g), b).load()
    col = sm.load()
    snoopy, moon, roof = set(), set(), set()
    glow = []
    for y in range(H):
        for x in range(W):
            R, G, B = col[x, y]
            if pale[x, y] > 120 and 34 <= x <= 84 and y >= 30:
                snoopy.add((x, y))
            elif pale[x, y] > 110 and x < 50 and y < 22:
                moon.add((x, y))
            elif R > 110 and G < 85 and R > B + 40:
                roof.add((x, y))
            elif pale[x, y] > 70:
                glow.append((x, y, pale[x, y]))
    # the big star: the brightest spot on the right
    right = [(v, x, y) for x, y, v in glow if x > 92 and 20 < y < 48]
    big = max(right)[1:] if right else (109, 35)
    # small stars: bright specks away from everything else, one per neighbourhood
    stars = []
    for x, y, v in sorted(glow, key=lambda t: -t[2]):
        if abs(x - big[0]) < 12 and abs(y - big[1]) < 12:
            continue
        if any(abs(x - sx) <= 3 and abs(y - sy) <= 3 for sx, sy in stars):
            continue
        if any(abs(x - mx) <= 2 and abs(y - my) <= 2 for mx, my in moon | snoopy):
            continue
        if y < 58 and len(stars) < 16:
            stars.append((x, y))
    return snoopy, moon, roof, big, stars


def frame(f, snoopy, moon, roof, big, stars):
    im = Image.new("1", (W, H), 0)
    px = im.load()

    def put(x, y, v=1):
        if 0 <= x < W and 0 <= y < H:
            px[x, y] = v

    # stars: each twinkles on its own beat (dot, plus, dot, gone)
    for i, (x, y) in enumerate(stars):
        ph = (f + rnd(i) % 24) % 24
        if ph < 15:
            put(x, y)
        if 5 <= ph < 10:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                put(x + dx, y + dy)

    # the moon
    for x, y in moon:
        put(x, y)

    # the big star: a sparkle whose rays breathe in and out
    bx, by = big
    s = (math.sin(2 * math.pi * f / 32) + 1) / 2                 # 0..1, 3 pulses a loop
    long_r, diag_r = 6 + int(round(s * 5)), 2 + int(round(s * 2))
    for d in range(-1, 2):
        for e in range(-1, 2):
            put(bx + d, by + e)
    for k in range(2, long_r + 1):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if k < long_r - 1 or (f + k) % 2 == 0:
                put(bx + dx * k, by + dy * k)
    for k in range(2, diag_r + 1):
        for dx, dy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
            put(bx + dx * k, by + dy * k)
    if s > 0.85:                                                 # at its brightest: a ring of glints
        for a in range(8):
            ang = a * math.pi / 4 + math.pi / 8
            put(bx + int(round(math.cos(ang) * 8)), by + int(round(math.sin(ang) * 8)))

    # a shooting star, once a loop
    t = f - 56
    if 0 <= t < 14:
        hx, hy = 124 - t * 5, 2 + t * 2
        for k in range(7):
            if k < 4 or (t + k) % 2:
                put(hx + k * 5 // 2, hy - k)

    # the roof: crisp outline, a light diagonal texture inside, a gap around Snoopy
    near = {(x + dx, y + dy) for x, y in snoopy for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
    for x, y in roof:
        if (x, y) in near:
            continue
        edge = any((x + dx, y + dy) not in roof for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        if edge and y < H - 1:
            put(x, y)
        else:
            # shingles: rows of tiles, each row's joints offset from the last
            top = min(yy for xx, yy in roof if xx == x) if any(xx == x for xx, _ in roof) else y
            row = (y - top) // 3
            if (y - top) % 3 == 2 or ((x + row * 3) % 6 == 0 and (y - top) % 3 != 2):
                put(x, y)

    # Snoopy breathes: his tummy (the top of the body) rises a pixel and settles
    breathe = math.sin(2 * math.pi * f / 48) > 0.3
    for x, y in snoopy:
        if breathe and x < 58 and y < 47:
            put(x, y - 1)
        put(x, y)
    if breathe:
        for x, y in snoopy:
            if x < 58 and y < 47 and (x, y - 1) not in snoopy:
                put(x, y - 1)

    # z z Z from his head
    for k in range(3):
        tt = ((f + k * 11) % 32) / 32.0
        if tt > 0.92:
            continue
        g = Z[min(2, int(tt * 3))]
        zx = int(80 + tt * 12 + math.sin(tt * 6.3) * 2)
        zy = int(36 - tt * 32)
        for r_, row in enumerate(g):
            for c, v in enumerate(row):
                if v == "1":
                    put(zx + c, zy + r_)
    return im


def main():
    parts = load()
    print("big star at", parts[3], "-", len(parts[4]), "small stars,", len(parts[0]), "snoopy pixels")
    frames = [frame(f, *parts) for f in range(N)]
    out = os.path.join(HERE, "doghouse_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [f.convert("L").resize((W * 4, H * 4), Image.NEAREST) for f in frames]
    big[0].save(os.path.join(HERE, "doghouse_anim_big.gif"), save_all=True, append_images=big[1:],
                duration=MS, loop=0)
    print(f"wrote {out}: {N} frames")


if __name__ == "__main__":
    main()
