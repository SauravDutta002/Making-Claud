"""
blackhole_anim.py - Gargantua, ray traced, for the keychain.

    python art/blackhole_anim.py [grain|ascii]   -> art/blackhole_anim.gif (+ _big preview)
    python tools/make_frames.py "art/blackhole_anim.gif=Black hole" --dither none --contrast 1.0

Every pixel's light is traced backwards around a real (Schwarzschild) black hole,
so the picture comes out like the one in Interstellar by itself: the far side of
the disk bends up over the top, a thin copy wraps underneath, the shadow is the
true size, the side spinning towards you is brighter and the stars behind are
lensed. The geometry is traced once; the animation is the disk's texture
turning (inner rings faster, like real orbits). 96 frames, seamless.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 128, 64
N, MS = 96, 70
SS = 3                          # rays per pixel, per side
DIST = 70.0                     # camera distance (in units of the hole's mass)
ELEV = math.radians(15.0)       # camera height above the disk plane
ROLL = math.radians(-12)        # the picture is tilted: left side higher
SCALE = 0.0049                  # radians per pixel (sets the zoom)
R_IN, R_OUT = 6.0, 24.0         # the disk: from the innermost stable orbit outwards

BAYER = np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                  [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                  [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                  [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]) / 64.0

CHARS = {
    ".": ["000", "000", "000", "010"], ":": ["000", "010", "000", "010"],
    "-": ["000", "000", "111", "000"], "'": ["010", "010", "000", "000"],
    "i": ["010", "000", "010", "010"], "r": ["000", "101", "110", "100"],
    "c": ["000", "011", "100", "011"], "=": ["000", "111", "000", "111"],
    "+": ["000", "010", "111", "010"], "t": ["010", "111", "010", "011"],
    "x": ["000", "101", "010", "101"], "f": ["011", "010", "111", "010"],
    "z": ["000", "111", "010", "111"], "o": ["000", "111", "101", "111"],
    "[": ["011", "010", "010", "011"], "%": ["101", "001", "010", "101"],
    "e": ["111", "101", "110", "011"], "a": ["110", "011", "101", "111"],
    "0": ["111", "101", "101", "111"], "&": ["010", "101", "010", "111"],
    "$": ["111", "110", "011", "111"], "8": ["111", "111", "101", "111"],
    "#": ["101", "111", "101", "111"], "@": ["111", "101", "111", "111"],
}
GLYPHS = sorted((sum(r.count("1") for r in g), k) for k, g in CHARS.items())
MAXINK = GLYPHS[-1][0]


def trace():
    """Shoot one ray per sub-pixel back from the camera; return, per ray, where it
    hit the disk (radius, angle, Doppler boost), whether it fell in, and where it
    ended up in the sky."""
    cam = np.array([0.0, DIST * math.sin(ELEV), -DIST * math.cos(ELEV)])
    fwd = -cam / np.linalg.norm(cam)
    right = np.cross([0.0, 1.0, 0.0], fwd)
    right /= np.linalg.norm(right)
    up = np.cross(fwd, right)

    ys, xs = np.mgrid[0:H * SS, 0:W * SS]
    px = ((xs + 0.5) / SS - W / 2) * SCALE
    py = -((ys + 0.5) / SS - H / 2) * SCALE
    u = px * math.cos(ROLL) - py * math.sin(ROLL)
    v = px * math.sin(ROLL) + py * math.cos(ROLL)
    d = fwd[None, None] + np.tan(u)[..., None] * right + np.tan(v)[..., None] * up
    d = (d / np.linalg.norm(d, axis=-1, keepdims=True)).reshape(-1, 3)
    n = d.shape[0]

    pos = np.tile(cam, (n, 1))
    vel = d.copy()
    h2 = np.sum(np.cross(pos, vel) ** 2, axis=1)            # conserved: sets the bending
    alive = np.ones(n, bool)
    fell = np.zeros(n, bool)
    hit_r = np.full(n, np.nan)
    hit_phi = np.zeros(n)
    hit_dop = np.ones(n)

    for _ in range(5000):
        if not alive.any():
            break
        idx = np.nonzero(alive)[0]
        p, vv, hh = pos[idx], vel[idx], h2[idx]
        r = np.linalg.norm(p, axis=1)
        dt = np.clip(0.04 * r, 0.02, 1.2)[:, None]

        def a(q):
            rq = np.linalg.norm(q, axis=1)
            return -1.5 * hh[:, None] * q / rq[:, None] ** 5
        k1v, k1x = a(p), vv                                  # RK4 step
        k2v, k2x = a(p + 0.5 * dt * k1x), vv + 0.5 * dt * k1v
        k3v, k3x = a(p + 0.5 * dt * k2x), vv + 0.5 * dt * k2v
        k4v, k4x = a(p + dt * k3x), vv + dt * k3v
        np_ = p + dt / 6 * (k1x + 2 * k2x + 2 * k3x + k4x)
        nv = vv + dt / 6 * (k1v + 2 * k2v + 2 * k3v + k4v)

        # crossed the disk plane (y = 0) between the two points?
        cross = (p[:, 1] * np_[:, 1] <= 0) & np.isnan(hit_r[idx])
        if cross.any():
            t = p[cross, 1] / (p[cross, 1] - np_[cross, 1] + 1e-12)
            c = p[cross] + (np_[cross] - p[cross]) * t[:, None]
            rc = np.hypot(c[:, 0], c[:, 2])
            ok = (rc >= R_IN) & (rc <= R_OUT)
            j = idx[cross][ok]
            cc, rk = c[ok], rc[ok]
            hit_r[j] = rk
            hit_phi[j] = np.arctan2(cc[:, 2], cc[:, 0])
            # Doppler: light from the side of the disk coming towards us is boosted
            beta = np.sqrt(1.0 / rk) * 1.15
            tang = np.stack([-cc[:, 2], np.zeros(len(rk)), cc[:, 0]], 1) / rk[:, None]
            dirn = nv[cross][ok]
            to_cam = -dirn / np.linalg.norm(dirn, axis=1)[:, None]
            cos = np.sum(tang * to_cam, axis=1)
            gamma = 1 / np.sqrt(1 - beta ** 2)
            hit_dop[j] = 1 / (gamma * (1 - beta * cos))
            alive[j] = False                                 # the disk is opaque

        pos[idx], vel[idx] = np_, nv
        rn = np.linalg.norm(np_, axis=1)
        gone_in = rn < 2.05
        fell[idx[gone_in]] = True
        alive[idx[gone_in]] = False
        away = (rn > DIST * 2.2) & (np.sum(np_ * nv, axis=1) > 0)
        alive[idx[away]] = False

    sky = vel / np.linalg.norm(vel, axis=1, keepdims=True)
    shape = (H * SS, W * SS)
    return (hit_r.reshape(shape), hit_phi.reshape(shape), hit_dop.reshape(shape),
            fell.reshape(shape), sky.reshape(shape + (3,)))


def disk_image(hit_r, hit_phi, hit_dop, f):
    on = ~np.isnan(hit_r)
    r = np.where(on, hit_r, R_OUT * 2)
    turns = np.clip(np.round(7.0 * (R_IN / np.maximum(r, R_IN)) ** 1.5), 1, 7)
    a = hit_phi - 2 * math.pi * turns * f / N
    tex = (0.66 + 0.17 * np.sin(3 * a + r * 0.9) + 0.12 * np.sin(8 * a - r * 1.7 + 1.3)
           + 0.09 * np.sin(15 * a + r * 3.1 + 0.4))
    prof = np.clip(1 - np.sqrt(R_IN / np.maximum(r, R_IN)), 0, 1) ** 0.35 * (R_IN / np.maximum(r, R_IN)) ** 1.6
    fade = np.clip((R_OUT - r) / 10.0, 0, 1)
    light = prof * fade * tex * hit_dop ** 2.4 * 4.0
    return np.where(on, light, 0.0)


def stars(sky, fell, f):
    """Stars fixed on the sky sphere, so they're lensed around the hole too."""
    az = np.arctan2(sky[..., 0], sky[..., 2])
    el = np.arcsin(np.clip(sky[..., 1], -1, 1))
    gx = np.floor(az / 0.045).astype(np.int64)
    gy = np.floor(el / 0.045).astype(np.int64)
    hsh = (gx * 73856093 ^ gy * 19349663) & 0xFFFF
    star = (hsh % 97 == 0)
    tw = ((hsh // 97 + f) % 24) < 18
    return star & tw & ~fell


def to_screen(img):
    return img.reshape(H, SS, W, SS).mean(axis=(1, 3))


def grain(img):
    return img > np.tile(BAYER, (H // 8, W // 8)) * 0.92 + 0.04


def ascii_look(img, hot, f):
    """The light written in tiny characters (one pixel apart); the hottest light
    stays solid so the shapes read."""
    out = np.zeros((H, W), bool)
    soft = np.where(hot, 0.0, img)
    for cy in range(0, H, 5):
        for cx in range(0, W, 4):
            b = float(soft[cy:cy + 5, cx:cx + 4].mean()) ** 0.8
            if b < 0.07:
                continue
            ink = b * MAXINK
            close = [k for nk, k in GLYPHS if abs(nk - ink) <= 1.2] or \
                    [min(GLYPHS, key=lambda g: abs(g[0] - ink))[1]]
            seed = (cx * 73856093 ^ cy * 19349663 ^ ((f // 3) * 83492791)) & 0xFFFFFFFF
            g = CHARS[close[seed % len(close)]]
            for rr in range(4):
                for cc in range(3):
                    if cy + rr < H and cx + cc < W and g[rr][cc] == "1":
                        out[cy + rr, cx + cc] = True
    return out | hot


def main():
    look = sys.argv[1] if len(sys.argv) > 1 else "grain"
    print("tracing light around the black hole...")
    hit_r, hit_phi, hit_dop, fell, sky = trace()
    fell_px = to_screen(fell.astype(float)) > 0.5
    frames = []
    for f in range(N):
        img = np.clip(to_screen(disk_image(hit_r, hit_phi, hit_dop, f)), 0, 1) ** 0.8
        if look == "ascii":
            hot = (img > 0.6) & grain(np.clip((img - 0.4) * 1.7, 0, 1))
            bw = ascii_look(img, hot, f)
        else:
            bw = grain(img)
        st = to_screen(stars(sky, fell, f).astype(float)) > 0.2
        bw |= st & (img < 0.04)
        bw &= ~fell_px
        frames.append(Image.fromarray((bw * 255).astype(np.uint8)).convert("1"))
    out = os.path.join(HERE, "blackhole_anim.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    big = [im.convert("L").resize((W * 4, H * 4), Image.NEAREST) for im in frames]
    big[0].save(os.path.join(HERE, "blackhole_anim_big.gif"), save_all=True, append_images=big[1:],
                duration=MS, loop=0)
    grey = np.clip(to_screen(disk_image(hit_r, hit_phi, hit_dop, 0)), 0, 1) ** 0.8
    Image.fromarray((grey * 255).astype(np.uint8)).resize((W * 4, H * 4), Image.LANCZOS) \
        .save(os.path.join(HERE, "blackhole_grey.png"))
    print(f"wrote {out}: {N} frames ({look})")


if __name__ == "__main__":
    main()
