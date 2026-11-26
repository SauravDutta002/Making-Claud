"""
face_anim.py - Snoopy's face up close (art/face.png), as clean white lines on
the black screen. He blinks, glances left and right, and twitches his nose.
The title text in the picture is cropped away. 96 frames, loops seamlessly.
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402

W, H, N = 128, 64, 96
CROP = (-110, 70, 474, 362)       # 2:1, his face only (no title text)


def parts():
    im = Image.open(os.path.join(HERE, "face.png")).convert("L")
    x0, y0, x1, y1 = CROP
    canvas = Image.new("L", (x1 - x0, y1 - y0), 255)
    canvas.paste(im, (-x0, -y0))
    S = 4
    a = np.asarray(canvas.resize((W * S, H * S), Image.LANCZOS)).astype(float)
    dark = a < 110
    solid = scene.grow(scene.shrink(dark, 5), 5) & dark          # eyes and nose (solid black)
    small = lambda m, t: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((W, H), Image.BOX)) > t
    lines = small(dark & ~solid, 30)
    blobs = scene.drop_small(small(solid, 100), 3)
    # split the blobs: the nose is the biggest, the eyes are the two above it
    comps, seen = [], np.zeros_like(blobs)
    for y, x in zip(*np.nonzero(blobs)):
        if seen[y, x]:
            continue
        pts, st = [], [(y, x)]
        seen[y, x] = True
        while st:
            cy, cx = st.pop()
            pts.append((cy, cx))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < H and 0 <= nx < W and blobs[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    st.append((ny, nx))
        comps.append(pts)
    comps.sort(key=len, reverse=True)
    nose = comps[0] if comps else []
    eyes = [c for c in comps[1:3]]
    mask = lambda pts: np.array([[False]]) if not pts else None
    def to_mask(pts):
        m = np.zeros((H, W), bool)
        for y, x in pts:
            m[y, x] = True
        return m
    return lines, to_mask(nose), [to_mask(e) for e in eyes]


def make():
    lines, nose, eyes = parts()
    frames = []
    for f in range(N):
        img = lines.copy()
        # nose twitches up a pixel now and then
        twitch = 1 if f % 32 in (10, 11, 13) else 0
        img |= np.roll(nose, -twitch, axis=0)
        # eyes: glance left, back, right, back; blink twice a loop
        glance = (0, 0, -2, -2, 0, 0, 2, 2)[(f // 12) % 8]
        blink = f % 48 in (22, 23, 24)
        if blink:
            for e in eyes:
                img &= ~scene.grow(e, 1)                      # the eye's own outline goes too
        for e in eyes:
            if blink:
                rows = np.nonzero(e.any(axis=1))[0]
                cols = np.nonzero(e.any(axis=0))[0]
                if len(rows):
                    mid = (rows.min() + rows.max()) // 2
                    img[mid, cols.min():cols.max() + 1] = True
            else:
                img |= np.roll(e, glance, axis=1)
        frames.append(Image.fromarray((img * 255).astype(np.uint8)).convert("1"))
    return frames


if __name__ == "__main__":
    fr = make()
    fr[0].save(os.path.join(HERE, "face_anim.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
