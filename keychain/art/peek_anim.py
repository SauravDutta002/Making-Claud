"""
peek_anim.py - Snoopy peeking over Saturn with a star on his head (art/peek.png).
White lines on the black screen, the planet softly shaded with dots that drift
as it turns, the star on his head twinkling, stars all around, and Snoopy
peeking up and down over the planet. 96 frames, loops seamlessly.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402

W, H, N = 128, 64, 96
CARD = (0, 0, 388, 270)            # the drawing, without the dark border on the right
CROP = (-46, 2, 434, 242)          # 2:1 around it, a little closer
HEAD_BOTTOM = 98                   # in the picture: above this is Snoopy (his head and paws)
STAR = (244, 22)                   # the star on his head, in the picture


def to_screen(x, y):
    return (int(round((x - CROP[0]) * W / (CROP[2] - CROP[0]))),
            int(round((y - CROP[1]) * H / (CROP[3] - CROP[1]))))


def parts():
    src = Image.open(os.path.join(HERE, "peek.png")).convert("L").crop(CARD)
    canvas = Image.new("L", (CROP[2] - CROP[0], CROP[3] - CROP[1]), 255)
    canvas.paste(src, (CARD[0] - CROP[0], CARD[1] - CROP[1]))
    S = 4
    a = np.asarray(canvas.resize((W * S, H * S), Image.LANCZOS)).astype(float)
    small = lambda m, t: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.BOX)) > t
    lines = small(a < 150, 28)
    shade = small((a >= 150) & (a < 228), 120) & ~scene.grow(lines, 1)
    _, hb = to_screen(0, HEAD_BOTTOM)
    head = lines.copy()
    head[hb:] = False
    return lines & ~head, head, shade, hb


def make():
    body, head, shade, hb = parts()
    ys, xs = np.mgrid[0:H, 0:W]
    rng = np.random.default_rng(6)
    stars = [(int(rng.integers(0, W)), int(rng.integers(0, H)), int(rng.integers(0, 40))) for _ in range(30)]
    sx, sy = to_screen(*STAR)
    frames = []
    for f in range(N):
        img = body.copy()
        # the planet turning: its shading dots drift sideways (a whole pattern a loop)
        drift = (f * 8 // N) % 8
        img |= shade & (((xs + drift) % 4 == 0) & (ys % 3 == 0) | ((xs + drift + 2) % 4 == 0) & (ys % 3 == 1))
        # Snoopy peeks: up a pixel, then back down behind the rim
        peek = 1 if (f % 48) < 20 else 0
        h = np.roll(head, -peek, axis=0)
        h[hb - peek:hb] = head[hb - peek:hb] if peek else h[hb - peek:hb]
        img |= h
        # the star on his head twinkles: little rays come and go
        tw = (f % 16) / 16.0
        ray = int(round(2 + 2 * math.sin(tw * 2 * math.pi)))
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for k in range(3, 3 + max(0, ray)):
                x, y = sx + dx * k, sy - peek + dy * k
                if 0 <= x < W and 0 <= y < H and not img[y, x]:
                    img[y, x] = True
        # stars all around, twinkling (not on the drawing)
        near = scene.grow(body | head | shade, 2)
        for x, y, ph in stars:
            if not near[y, x] and (f + ph) % 40 < 30:
                img[y, x] = True
        frames.append(Image.fromarray((img * 255).astype(np.uint8)).convert("1"))
    return frames


if __name__ == "__main__":
    fr = make()
    fr[0].save(os.path.join(HERE, "peek_anim.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
