"""
title_anim.py - the "SNOOPY" title card (art/title.png): the letters bounce one
after another in a wave, his head bobs gently, his nose twitches, and the blue
halftone background becomes a subtle dot pattern. 96 frames, seamless.
"""
import math
import os
import sys
from collections import deque

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402

W, H, N = 128, 64, 96
CROP = (-18, 32, 664, 373)
TEXT_BOTTOM = 135                 # in the picture: above this is the title


def comps(m):
    out, seen = [], np.zeros_like(m)
    for y, x in zip(*np.nonzero(m)):
        if seen[y, x]:
            continue
        pts, q = [], deque([(y, x)])
        seen[y, x] = True
        while q:
            cy, cx = q.popleft()
            pts.append((cy, cx))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < m.shape[0] and 0 <= nx < m.shape[1] and m[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
        out.append(pts)
    return out


def parts():
    src = Image.open(os.path.join(HERE, "title.png")).convert("RGB")
    src = src.crop((10, 10, src.width - 8, src.height - 4))            # inside the screenshot's border
    x0, y0, x1, y1 = CROP
    im = Image.new("RGB", (x1 - x0, y1 - y0), (52, 152, 219))         # the picture's blue round the edges
    im.paste(src, (10 - x0, 10 - y0))
    S = 4
    a = np.asarray(im.resize((W * S, H * S), Image.LANCZOS)).astype(float)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mn, mx = np.minimum(np.minimum(r, g), b), np.maximum(np.maximum(r, g), b)
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    white = (mn > 215) & (mx - mn < 40)
    ink = lum < 60
    small = lambda m, t: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.BOX)) > t
    Wm, Im = small(white, 110), small(ink, 40)
    ty = int((TEXT_BOTTOM - CROP[1]) * H / (CROP[3] - CROP[1]))
    text = Wm.copy(); text[ty:] = False
    head_white = Wm.copy(); head_white[:ty] = False
    head_ink = Im.copy(); head_ink[:ty] = False
    letters = sorted(comps(scene.drop_small(text, 4)), key=lambda p: min(x for _, x in p))
    # the nose: the biggest solid black blob in his head
    solid = scene.drop_small(head_ink & scene.grow(scene.shrink(head_ink, 1), 1), 4)
    blobs = sorted(comps(solid), key=len, reverse=True)
    nose = np.zeros((H, W), bool)
    for y, x in (blobs[0] if blobs else []):
        nose[y, x] = True
    return letters, head_white, head_ink, nose


def make():
    letters, hw, hi, nose = parts()
    ys, xs = np.mgrid[0:H, 0:W]
    halftone = (xs % 4 == 1) & (ys % 4 == 1)
    frames = []
    for f in range(N):
        img = halftone.copy()
        # letters bounce in a wave
        for i, pts in enumerate(letters):
            ph = (f * 4 / N - i * 0.14) % 1.0
            up = int(round(max(0.0, math.sin(ph * 2 * math.pi)) * 3))
            for y, x in pts:
                if 0 <= y - up < H:
                    img[y - up, x] = True
            for y, x in pts:                              # a dark gap round each letter
                for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                    yy, xx = y - up + dy, x + dx
                    if 0 <= yy < H and 0 <= xx < W and img[yy, xx] and (yy + up, xx) not in pts:
                        pass
        # his head bobs a pixel, the nose twitches
        bob = 1 if (f // 12) % 2 else 0
        white, ink, nz = (np.roll(m, -bob, axis=0) for m in (hw, hi, nose))
        if f % 32 in (8, 9, 11):
            ink = (ink & ~nz) | np.roll(nz, -1, axis=0)
        body = white | ink
        img &= ~scene.grow(body, 1)
        img |= white & ~ink
        img |= scene.edges(body) & ink                    # his black ear gets a white rim
        frames.append(Image.fromarray((img * 255).astype(np.uint8)).convert("1"))
    return frames


if __name__ == "__main__":
    fr = make()
    fr[0].save(os.path.join(HERE, "title_anim.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
