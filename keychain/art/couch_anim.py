"""
couch_anim.py - Snoopy napping on the plaid couch (art/couch.png), drawn to
match the picture: everything sits where it is in the picture.

    python art/couch_anim.py [dark|light]   -> art/couch_anim.gif

Snoopy and the plaid couch are taken from the picture; the rest of the room
(wall pictures, TV and its cabinet, side table, plant, two floor lamps, the
window with blinds, the coffee table, the carpet) is redrawn cleanly at the
picture's own positions. The TV flickers, dust floats in the window light,
the lamps glow softly, the plant stirs and Snoopy breathes. 96 frames, seamless.
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
CROP = (0, 75, 634, 392)
SX, SY = W / (CROP[2] - CROP[0]), H / (CROP[3] - CROP[1])


def P(x, y):
    """picture coordinates -> screen"""
    return (round((x - CROP[0]) * SX), round((y - CROP[1]) * SY))


def R(x0, y0, x1, y1):
    return [P(x0, y0), P(x1, y1)]


def room(draw, f):
    on = 1
    # the wall pictures
    for box in ((8, 20, 102, 95), (122, 42, 210, 70), (240, 22, 270, 65), (158, 95, 200, 130), (160, 175, 190, 200)):
        draw.rectangle(R(*box), outline=on)
    # the TV and its screen
    draw.rectangle(R(2, 112, 152, 208), outline=on)
    draw.rounded_rectangle(R(34, 124, 140, 196), radius=2, outline=on)
    # things on top of the TV: a pot and a little figure
    draw.rectangle(R(24, 92, 42, 112), outline=on)
    draw.ellipse(R(112, 88, 142, 112), outline=on)
    # the TV cabinet: top, two doors, the VCR shelves in the middle
    draw.rectangle(R(0, 205, 215, 312), outline=on)
    draw.rectangle(R(6, 218, 58, 300), outline=on)
    draw.rectangle(R(150, 218, 208, 300), outline=on)
    for y in (232, 248, 268, 286):
        draw.line([P(66, y), P(124, y)], fill=on)
    # side table with books
    draw.rectangle(R(212, 222, 276, 292), outline=on)
    for y in (206, 214):
        draw.line([P(214, y), P(252, y)], fill=on)
    # floor lamps: shade + pole
    draw.polygon([P(296, 48), P(322, 48), P(338, 95), P(282, 95)], outline=on)
    draw.line([P(310, 95), P(310, 245)], fill=on)
    draw.polygon([P(378, 95), P(408, 95), P(422, 133), P(364, 133)], outline=on)
    draw.line([P(392, 133), P(392, 190)], fill=on)
    # the plant: a pot
    draw.polygon([P(318, 245), P(368, 245), P(360, 292), P(326, 292)], outline=on)
    # the window: frame and blinds
    draw.rectangle(R(402, 0, 634, 186), outline=on)
    for x in (412, 506, 516):
        draw.line([P(x, 0), P(x, 186)], fill=on)
    for y in range(80, 186, 14):
        draw.line([P(416, y), P(502, y)], fill=on)
        draw.line([P(520, y), P(630, y)], fill=on)
    # the carpet starts here
    draw.line([P(0, 312), P(212, 312)], fill=on)
    draw.line([P(276, 296), P(372, 296)], fill=on)
    # the coffee table: top, front edge, legs, lower shelf
    draw.polygon([P(150, 285), P(298, 268), P(360, 290), P(205, 305)], outline=on)
    draw.line([P(205, 305), P(205, 318)], fill=on)
    draw.line([P(205, 318), P(360, 300)], fill=on)
    draw.line([P(150, 285), P(150, 297)], fill=on)
    draw.line([P(150, 297), P(205, 318)], fill=on)
    for x0, y0, y1 in ((158, 297, 372), (215, 318, 392), (338, 304, 380), (350, 300, 365)):
        draw.line([P(x0, y0), P(x0, y1)], fill=on)
    draw.line([P(170, 350), P(330, 340)], fill=on)
    # bits on the table
    for x in (176, 190, 204, 236):
        draw.point(P(x, 282), fill=on)


def room_clean(draw, f):
    """Only the main shapes, like the clean bus."""
    on = 1
    draw.rectangle(R(8, 20, 102, 95), outline=on)                      # the big wall picture
    draw.rectangle(R(2, 112, 152, 208), outline=on)                    # TV
    draw.rounded_rectangle(R(34, 124, 140, 196), radius=2, outline=on)
    draw.rectangle(R(0, 208, 215, 312), outline=on)                    # its cabinet
    draw.line([P(107, 208), P(107, 312)], fill=on)
    draw.polygon([P(378, 95), P(408, 95), P(422, 133), P(364, 133)], outline=on)   # lamp
    draw.line([P(392, 133), P(392, 190)], fill=on)
    draw.rectangle(R(402, 0, 634, 186), outline=on)                   # window + a few blinds
    draw.line([P(511, 0), P(511, 186)], fill=on)
    for y in (90, 120, 150):
        draw.line([P(416, y), P(500, y)], fill=on)
        draw.line([P(522, y), P(628, y)], fill=on)
    draw.polygon([P(318, 245), P(368, 245), P(360, 292), P(326, 292)], outline=on)  # plant pot
    draw.polygon([P(150, 285), P(298, 268), P(360, 290), P(205, 305)], outline=on)  # coffee table
    for x0, y0, y1 in ((158, 292, 372), (215, 306, 392), (350, 292, 368)):
        draw.line([P(x0, y0), P(x0, y1)], fill=on)
    draw.line([P(0, 312), P(150, 312)], fill=on)                       # where the carpet starts


CLOSE = (250, 150, 634, 342)         # a closer view: Snoopy and the couch, bigger


def make(look="dark", crop=None, clean=False):
    global CROP, SX, SY
    CROP = crop or (0, 75, 634, 392)
    SX, SY = W / (CROP[2] - CROP[0]), H / (CROP[3] - CROP[1])
    sc = scene.Scene(picture="couch.png", crop=CROP, snoopy_box=(380, 165, 634, 340),
                     props=[dict(colour=lambda c: (c.b > c.r - 12) & (c.lum > 50) & (c.lum < 175),
                                 style="plaid", box=(368, 178, 634, 392), keep=80, close=4, fill=True)])
    hq = scene.snoopy_hq("couch.png", (380, 165, 634, 345), CROP)
    plaid = sc.props[0][0] & ~scene.grow(hq[2], 1)
    ys, xs = np.mgrid[0:H, 0:W]
    bt = scene.BT
    rng = np.random.default_rng(5)
    frames = []
    for f in range(N):
        im = Image.new("1", (W, H), 0)
        (room_clean if clean else room)(ImageDraw.Draw(im), f)
        img = np.asarray(im).astype(bool)
        # the plant: leafy, stirring
        cx, cy = P(306, 160)
        rr = np.hypot((xs - cx) / 9.0, (ys - cy) / 9.5)
        if clean:
            bush = rr < 1
            img |= scene.edges(bush) & (np.random.default_rng(f // 6 % 16).random((H, W)) > 0.15)
            img |= bush & (bt < 0.12)
        else:
            img |= (rr < 1) & (np.random.default_rng(f // 4 % 24).random((H, W)) > 0.2) & (bt < 0.42)
        # the TV screen flickering
        ax, ay = P(36, 126)
        bx, by = P(138, 194)
        scr = (xs > ax) & (xs < bx) & (ys > ay) & (ys < by)
        lvl = 0.2 + 0.15 * math.sin(2 * math.pi * 3 * f / N) + 0.08 * ((f * 7) % 3)
        img |= scr & ((np.random.default_rng(f % 24).random((H, W)) < lvl) | ((ys - f) % 6 == 0))
        # lamps glowing
        s = 0.5 + 0.5 * math.sin(2 * math.pi * 2 * f / N)
        for (lx, ly) in (((393, 115),) if clean else ((310, 72), (393, 115))):
            gx, gy = P(lx, ly)
            g = np.hypot(xs - gx, (ys - gy) * 1.3)
            img |= (g < 3 + s * 2) & (g > 2) & (bt < 0.12 + 0.12 * s)
        # dust floating in the window light
        rng = np.random.default_rng(5)
        for i in range(6 if clean else 14):
            x0, y0 = float(rng.uniform(420, 620)), float(rng.uniform(40, 175))
            x, y = P(x0 + 16 * math.sin(2 * math.pi * f / N + i), y0 + 10 * math.cos(2 * math.pi * 2 * f / N + i * 1.7))
            if 0 <= x < W and 0 <= y < H and (f // 3 + i) % 5:
                img[y, x] = True
        # carpet: a few soft dots
        floor = (ys > P(0, 314)[1] + 1) & (xs < P(370, 0)[0])
        if not clean:
            img |= floor & (bt < 0.03)
        # the couch (plaid) and Snoopy
        img &= ~grow(plaid, 0)
        out = plaid & ~scene.shrink(plaid, 1)
        inside = plaid & ~out
        if clean:
            img |= out | (inside & (xs % 10 == 0) & (ys % 3 == 0))
        else:
            img |= out | (inside & (((xs % 8 == 0) & (ys % 2 == 0)) | ((ys % 6 == 0) & (xs % 2 == 0))))
        lift = 1 if math.sin(2 * math.pi * f / 48) > 0.4 else 0
        cream, ink, body = hq
        if lift:
            cream, ink, body = (np.roll(m, -1, 0) for m in (cream, ink, body))
        img &= ~scene.grow(body, 2)
        img |= cream & ~ink
        img |= scene.edges(body) & ~cream
        img |= scene.edges(scene.drop_small(ink, 18)) & ~scene.edges(body)   # his ear, outlined
        if look == "light":
            img = ~img
        frames.append(Image.fromarray((img * 255).astype(np.uint8)).convert("1"))
    return frames


def grow(m, n):
    return scene.grow(m, n) if n else m


if __name__ == "__main__":
    look = sys.argv[1] if len(sys.argv) > 1 else "dark"
    fr = make(look)
    out = os.path.join(HERE, "couch_anim.gif")
    fr[0].save(out, save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("wrote", out)
