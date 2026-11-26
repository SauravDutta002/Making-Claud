"""
office_anim.py - Snoopy off to work (from art/office.png): briefcase, tie,
coffee. He walks along a city pavement that scrolls by, with a step bob, and
steam curls up from his coffee.

    python art/office_anim.py [lines|solid] [dark|light]   -> art/office_anim.gif
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
GROUND = 59
BOX = (50, 0, 170, 122)          # Snoopy in the picture
TALL = 56                        # his height on the screen
LEFT = 40


def rnd(n):
    n = (n << 13 ^ n) & 0xFFFFFFFF
    return ((n * (n * n * 15731 + 789221) + 1376312589) & 0xFFFFFFFF) >> 16 & 0xFF


def city(d, f):
    """Far buildings drifting slowly, the pavement moving at walking pace, a lamp post now and then."""
    far = f * 64 / N
    for i in range(-1, 4):
        for j in range(4):
            x = i * 64 + j * 16 - int(far) % 64
            h = 12 + rnd(j * 7 + 3) % 14
            top = GROUND - 4 - h
            d.rectangle([x, top, x + 12, GROUND - 4], outline=1)
            for wy in range(top + 3, GROUND - 7, 4):
                for wx in range(x + 3, x + 10, 4):
                    if rnd(j * 131 + wy * 7 + wx % 64) > 150:
                        d.point((wx, wy), fill=1)
    d.line([0, GROUND, W, GROUND], fill=1)
    near = f * 128 / N
    for k in range(-1, 9):
        x = k * 16 - int(near) % 16
        d.line([x, GROUND + 1, x - 3, H - 1], fill=1)
    lx = 160 - int(near * 1.25) % 192
    d.line([lx, GROUND, lx, 20], fill=1)
    d.line([lx, 20, lx + 6, 20], fill=1)
    d.rectangle([lx + 5, 20, lx + 8, 22], fill=1)


def city_clean(d, f):
    """Like the clean bus: one continuous skyline outline drifting by, a single
    pavement line with a few marks, and a lamp post gliding past."""
    far = f * 64 / N                                        # the skyline repeats every 64 px
    pts = []
    for i in range(-1, 4):
        for j in range(4):
            x = i * 64 + j * 16 - int(far) % 64
            h = 10 + rnd(j * 7 + 3) % 16
            top = GROUND - 2 - h
            pts += [(x, top), (x + 16, top)]
    prev = None
    for x, y in pts:                                        # tops joined by steps: one outline
        if prev:
            d.line([prev, (prev[0], y), (x, y)], fill=1)
        prev = (x, y)
    d.line([0, GROUND, W, GROUND], fill=1)
    near = f * 128 / N
    for k in range(-1, 7):
        x = k * 24 - int(near) % 24
        d.line([x, GROUND + 3, x + 5, GROUND + 3], fill=1)
    lx = 160 - int(near * 1.25) % 192
    d.line([lx, GROUND - 1, lx, 18], fill=1)
    d.line([lx, 18, lx + 6, 18], fill=1)
    d.rectangle([lx + 5, 18, lx + 8, 20], fill=1)


def figure():
    im = Image.open(os.path.join(HERE, "office.png")).convert("RGB").crop(BOX)
    w = round(im.width * TALL / im.height)
    sm = im.resize((w * 4, TALL * 4), Image.LANCZOS)
    a = np.asarray(sm).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    dark = lum < 110
    red = (r > g + 60) & (r > b + 40)
    paper = (np.minimum(np.minimum(r, g), b) > 225) & ~red
    # his silhouette: everything not connected to the white paper around him
    bg = np.zeros_like(paper)
    bg[0, :], bg[-1, :], bg[:, 0], bg[:, -1] = paper[0, :], paper[-1, :], paper[:, 0], paper[:, -1]
    for _ in range(300):
        nb = scene.grow(bg, 2) & paper
        if (nb == bg).all():
            break
        bg = nb
    sil = ~bg
    small = lambda m, t: np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((w, TALL), Image.BOX)) > t
    solid = scene.grow(scene.shrink(dark, 3), 3) & dark   # only truly solid black (ear, briefcase, nose)
    # his eye(s) and nose: black spots inside his head, away from his outline
    inner = scene.shrink(sil, 6)
    spots = scene.drop_small(dark & inner & ~solid, 12)
    feat = small(spots, 40)
    feat = scene.drop_small(feat, 1)
    big_enough = np.zeros_like(feat)
    from collections import deque
    seen = np.zeros_like(feat)
    for y, x in zip(*np.nonzero(feat)):                    # every spot at least 2x2 on screen
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
                    if 0 <= ny < feat.shape[0] and 0 <= nx < feat.shape[1] and feat[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
        ys_, xs_ = zip(*comp)
        y0, x0 = min(ys_), min(xs_)
        big_enough[y0:max(ys_) + 1, x0:max(xs_) + 1] = True
        if len(comp) < 4:
            big_enough[y0:y0 + 2, x0:x0 + 2] = True
    figure.solid = small(solid, 100) | big_enough
    return small(sil, 100), small(dark, 90), small(red, 90), w


LEG_TOP = 0.80        # the bottom 20% of him is his legs and feet


def pose(m, phase, sil):
    """Walk cycle from one picture: 0 = legs as drawn, 1 = legs passing together,
    2 = legs swapped (mirrored about his middle), 3 = together again. The briefcase
    (big black shape on his left, lower half) swings the other way."""
    out = m.copy()
    h, w = m.shape
    top = int(h * LEG_TOP)
    cols = np.nonzero(sil[top:].any(axis=0))[0]
    if len(cols):
        c0, c1 = cols.min(), cols.max()
        mid = (c0 + c1) / 2.0
        legs = m[top:].copy()
        new = np.zeros_like(legs)
        for x in range(w):
            if phase == 0:
                src = x
            elif phase == 2:
                src = int(round(2 * mid - x))                     # mirrored: the other foot forward
            else:
                src = int(round(mid + (x - mid) * 1.8))           # squeezed: feet passing under him
            if 0 <= src < w and c0 - 2 <= src <= c1 + 2:
                new[:, x] = legs[:, src]
        out[top:] = new
    return out


def make(style="lines", look="dark"):
    sil, dark, red, w = figure()
    solid_src = figure.solid
    frames = []
    for f in range(N):
        im = Image.new("1", (W, H), 0)
        (city_clean if style == "clean" else city)(ImageDraw.Draw(im), f)
        img = np.asarray(im).astype(bool).copy()
        phase = (f // 4) % 4                                 # walk cycle: 4 poses
        step = 1 if phase in (0, 2) else 0                   # up while the feet pass, down as a step lands
        oy = GROUND - TALL + 1 - step
        S = np.zeros((H, W), bool); D = np.zeros((H, W), bool); R = np.zeros((H, W), bool)
        ys = slice(max(0, oy), oy + TALL)
        SD = np.zeros((H, W), bool)
        swing = (0, 0, 1, 0)[phase] - (1, 0, 0, 0)[phase]      # briefcase swings against the legs
        for src, dst in ((sil, S), (dark, D), (red, R), (solid_src, SD)):
            posed = src.copy() if style == "clean" else pose(src, phase, sil)
            if swing:
                h2 = posed.shape[0]
                case = np.zeros_like(posed)
                case[int(h2 * 0.45):int(h2 * LEG_TOP), :int(w * 0.42)] = True
                moved = np.roll(posed & case, swing, axis=1)
                posed = (posed & ~case) | moved
            dst[ys, LEFT:LEFT + w] = posed[max(0, -oy):, :]
        img &= ~scene.grow(S, 1)
        if style == "clean":
            # his legs: cut the drawn ones off and walk with real legs
            hip_y = oy + int(TALL * LEG_TOP) - 2
            right = np.zeros((H, W), bool)
            right[hip_y:, LEFT + int(w * 0.48):] = True
            for m in (S, D, R, SD):
                m &= ~right
            hip_x = LEFT + int(w * 0.66)
            legs = Image.new("1", (W, H), 0)
            ld = ImageDraw.Draw(legs)
            swing_leg = math.sin(2 * math.pi * f / 16)         # one stride every 16 frames
            length = GROUND - hip_y - 2
            feet = []
            for k, sgn in ((0, -1), (1, 1)):                    # back leg first, then the front one
                a = sgn * swing_leg * 0.55
                hx = hip_x + (k * 3 - 1)
                fx = hx + int(round(math.sin(a) * length))
                fy = hip_y + int(round(math.cos(a) * length))
                lift = 1 if (sgn * swing_leg) > 0.7 else 0     # the foot swinging forward lifts a little
                feet.append((hx, fx, fy - lift))
            img &= ~scene.grow(np.asarray(legs).astype(bool), 1)
            leg_masks = []
            for hx, fx, fy in feet:
                L = Image.new("1", (W, H), 0)
                d2 = ImageDraw.Draw(L)
                d2.line([hx, hip_y, fx, fy], fill=1, width=3)
                d2.ellipse([fx - 3, fy - 2, fx + 4, fy + 1], fill=1)   # his foot, pointing forward
                leg_masks.append(np.asarray(L).astype(bool))
            back, front = leg_masks
            img &= ~scene.grow(back | front, 1)
            img |= back & ~scene.grow(front, 1)                 # a thin dark gap where the legs cross
            img |= front
            S2 = S | ~scene.drop_small(~S, 12)              # fill pin holes in his shape
            img &= ~scene.grow(S2, 1)
            black = (SD | R) & S2                           # ear, eye, nose, briefcase, tie, sleeve
            img |= S2 & ~black
            big = scene.drop_small(black, 14)               # big black parts get a clean outline
            img |= scene.edges(big) & scene.edges(S2)
            img |= scene.edges(S2) & black & ~big
        elif style == "lines":
            solid = D & scene.shrink(D, 1)                   # big black areas (ear, briefcase) -> outlines
            img |= (D & ~scene.shrink(D, 2)) | (R & ((np.indices((H, W)).sum(0)) % 2 == 0))
            img |= scene.edges(S)
        else:
            img |= S & ~D & ~R
            img |= R & ((np.indices((H, W)).sum(0)) % 2 == 0)
            img |= scene.edges(scene.drop_small(D & S, 30)) & ~scene.edges(S)
            img |= scene.edges(S) & D
        # steam from the coffee (the cup is at his front, top right)
        cx = LEFT + int(w * 0.86)
        cy = oy + int(TALL * 0.42)
        for k in range(2):
            t = ((f + k * 12) % 24) / 24.0
            for j in range(4):
                u = t + j * 0.08
                x, y = int(cx + k * 2 + math.sin(u * 8 + k) * 1.5), int(cy - u * 11)
                if u <= 1 and (j + f // 2) % 2 == 0 and 0 <= x < W and 0 <= y < H:
                    img[y, x] = True
        if look == "light":
            img = ~img
        frames.append(Image.fromarray((img * 255).astype(np.uint8)).convert("1"))
    return frames


if __name__ == "__main__":
    style = sys.argv[1] if len(sys.argv) > 1 else "lines"
    look = sys.argv[2] if len(sys.argv) > 2 else "dark"
    fr = make(style, look)
    fr[0].save(os.path.join(HERE, "office_anim.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
