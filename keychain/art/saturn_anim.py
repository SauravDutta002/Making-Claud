"""
saturn_anim.py - Saturn and its rings, rendered in 3D for the keychain.

    python art/saturn_anim.py [classic|close]   -> art/saturn_anim.gif

Every pixel is worked out like a real picture: a lit sphere with soft cloud
bands and a storm drifting across, tilted rings with their gaps (the dark
Cassini division), the near side of the rings crossing in front of the planet
and the far side hidden behind it, the rings' shadow on the planet and the
planet's shadow across the rings. The rings shimmer as they turn (inner parts
faster), a little moon orbits and slips behind the planet, stars twinkle.
96 frames, loops seamlessly. Fine grain, like the black hole.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, N = 128, 64, 96
SS = 3
BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64.0

VIEWS = {
    #            centre x, y   radius px  tilt deg  roll deg
    "classic": dict(cx=64, cy=32, R=14.5, tilt=24, roll=-16),
    "close":   dict(cx=64, cy=32, R=21.0, tilt=20, roll=-12, moon=2.6),
}

RING_IN, RING_OUT = 1.24, 2.27          # in planet radii
LIGHT = np.array([-0.55, 0.45, 0.70])
LIGHT = LIGHT / np.linalg.norm(LIGHT)


def ring_density(r):
    """How bright the rings are at each radius: the C ring faint, the B ring
    bright, the Cassini gap dark, the A ring medium with the Encke gap."""
    d = np.zeros_like(r)
    d = np.where((r > 1.24) & (r < 1.52), 0.35, d)                      # C ring
    d = np.where((r >= 1.52) & (r < 1.95), 0.95, d)                     # B ring
    d = np.where((r >= 1.95) & (r < 2.03), 0.05, d)                     # Cassini division
    d = np.where((r >= 2.03) & (r < 2.27), 0.70, d)                     # A ring
    d = np.where((r > 2.20) & (r < 2.215), 0.15, d)                     # Encke gap
    fine = 0.85 + 0.15 * np.sin(r * 160.0)                              # many fine ringlets
    return d * fine


def render(view, f):
    v = VIEWS[view]
    R = v["R"]
    tilt, roll = math.radians(v["tilt"]), math.radians(v["roll"])
    ys, xs = np.mgrid[0:H * SS, 0:W * SS]
    X = ((xs + 0.5) / SS - v["cx"]) / R
    Y = -((ys + 0.5) / SS - v["cy"]) / R
    x = X * math.cos(roll) + Y * math.sin(roll)                       # undo the roll
    y = -X * math.sin(roll) + Y * math.cos(roll)

    n = np.array([0.0, math.cos(tilt), math.sin(tilt)])                # the ring plane's normal (planet's axis)
    # the planet
    rr = x * x + y * y
    on = rr < 1
    z = np.sqrt(np.clip(1 - rr, 0, 1))
    P = np.stack([x, y, z], -1)
    lam = np.clip(P @ LIGHT, 0, 1)
    lat = P @ n                                                        # -1..1 along the axis
    e1 = np.array([1.0, 0, 0])
    e2 = np.cross(n, e1)
    lon = np.arctan2(P @ e2, P @ e1)
    bands = 0.74 + 0.20 * np.sin(lat * 13.0) + 0.08 * np.sin(lat * 29.0 + 1.0)
    spin = 2 * math.pi * f / N                                         # one storm lap a loop
    storm = np.exp(-(((lat - 0.42) / 0.06) ** 2) - (((np.angle(np.exp(1j * (lon - spin)))) / 0.25) ** 2))
    planet = lam * (bands - 0.25 * storm) + 0.04
    # the rings' shadow on the planet: from the planet surface towards the sun, through the ring plane
    s = -(P @ n) / (LIGHT @ n)
    hit = P + s[..., None] * LIGHT
    hr = np.linalg.norm(hit, axis=-1)
    shade = np.where((s > 0) & (hr > RING_IN) & (hr < RING_OUT), 1 - 0.75 * ring_density(hr), 1.0)
    planet = planet * shade

    # the rings: where the view line crosses the ring plane
    t = -(y * n[1]) / n[2]
    Q = np.stack([x, y, t], -1)
    qr = np.linalg.norm(Q, axis=-1)
    ring_on = (qr > RING_IN) & (qr < RING_OUT)
    ang = np.arctan2(Q @ e2, Q @ e1)
    turns = np.clip(np.round(6 * (RING_IN / qr) ** 1.5), 1, 6)       # inner ring turns faster
    a = ang - 2 * math.pi * turns * f / N
    shimmer = 0.88 + 0.12 * np.sin(5 * a + qr * 40) * np.sin(3 * a - qr * 23)
    ring = ring_density(qr) * shimmer
    # the planet's shadow on the rings
    b = Q @ LIGHT
    c = np.sum(Q * Q, -1) - 1
    disc = b * b - c
    in_shadow = (disc > 0) & ((-b + np.sqrt(np.clip(disc, 0, None))) > 0)
    ring = np.where(in_shadow, ring * 0.12, ring)
    lit_side = 0.75 + 0.25 * np.clip(Q @ LIGHT / np.maximum(qr, 1e-6), -1, 1)
    ring = ring * lit_side

    img = np.zeros_like(x)
    behind = t < 0                                                      # far side of the rings
    img = np.where(ring_on & behind & ~on, ring, img)
    img = np.where(on, planet, img)
    front = ring_on & ~behind & (~on | (t > z))
    img = np.where(front, ring, img)

    # a little moon on its orbit, hidden when it goes behind the planet
    mo = 2 * math.pi * f / N
    mq = v.get("moon", 3.0) * (math.cos(mo) * e1 + math.sin(mo) * e2)
    mx, my, mz = mq
    mdist = np.hypot(x - mx, y - my)
    moon = mdist < 0.16
    hidden = on & (z > mz)
    img = np.where(moon & ~hidden, 0.9, img)
    return img.reshape(H, SS, W, SS).mean(axis=(1, 3))


def make(view="classic"):
    rng = np.random.default_rng(8)
    stars = [(int(rng.integers(0, W)), int(rng.integers(0, H)), int(rng.integers(0, 40))) for _ in range(34)]
    bt = np.tile(BAYER, (H // 8, W // 8))
    frames = []
    for f in range(N):
        img = np.clip((render(view, f) - 0.10) * 1.45, 0, 1) ** 1.1      # more contrast: gaps and dark side read black
        if view == "close":
            # snap to five clean, even patterns (solid, 3/4, 1/2, 1/4, black) so the ring zones and
            # the planet's bands read as clear areas instead of a fuzzy gradient
            lvl = np.clip(np.round(img * 4), 0, 4) / 4.0
            bw = lvl > bt * 0.999
        else:
            bw = img > bt * 0.92 + 0.04
        if view == "close":                                   # a faint edge on the planet's night side
            v = VIEWS[view]
            ys_, xs_ = np.mgrid[0:H, 0:W]
            disk = np.hypot(xs_ + 0.5 - v["cx"], ys_ + 0.5 - v["cy"]) < v["R"]
            inner = disk.copy()
            inner[1:, :] &= disk[:-1, :]; inner[:-1, :] &= disk[1:, :]
            inner[:, 1:] &= disk[:, :-1]; inner[:, :-1] &= disk[:, 1:]
            limb = disk & ~inner & (img < 0.25)
            bw |= limb & ((xs_ + ys_) % 2 == 0)
        for x, y, ph in stars:
            if img[y, x] < 0.03 and (f + ph) % 40 < 31:
                bw[y, x] = True
        frames.append(Image.fromarray((bw * 255).astype(np.uint8)).convert("1"))
    return frames


if __name__ == "__main__":
    view = sys.argv[1] if len(sys.argv) > 1 else "classic"
    fr = make(view)
    fr[0].save(os.path.join(HERE, f"saturn_{view}.gif"), save_all=True, append_images=fr[1:], duration=70, loop=0)
    print("ok")
