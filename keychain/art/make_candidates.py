"""
make_candidates.py - builds every option for the keychain as a numbered GIF in
art/candidates/, plus index.json. The preview page (tools/preview.py) shows
them under "Pick your favourites"; tell Claude the numbers you like.

    python art/make_candidates.py
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "candidates")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

N, MS = 96, 70


def save(frames, key):
    path = os.path.join(OUT, key + ".gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=MS, loop=0)
    return key + ".gif"


def invert(frames):
    return [ImageOps.invert(f.convert("L")).convert("1") for f in frames]


def blackhole(elev, scale, look):
    import blackhole_anim as bh
    bh.ELEV, bh.SCALE = math.radians(elev), scale
    hit_r, hit_phi, hit_dop, fell, sky = bh.trace()
    fell_px = bh.to_screen(fell.astype(float)) > 0.5
    frames = []
    for f in range(N):
        img = np.clip(bh.to_screen(bh.disk_image(hit_r, hit_phi, hit_dop, f)), 0, 1) ** 0.8
        if look == "ascii":
            hot = (img > 0.6) & bh.grain(np.clip((img - 0.4) * 1.7, 0, 1))
            bw = bh.ascii_look(img, hot, f)
        else:
            bw = bh.grain(img)
        bw |= (bh.to_screen(bh.stars(sky, fell, f).astype(float)) > 0.2) & (img < 0.04)
        bw &= ~fell_px
        frames.append(Image.fromarray((bw * 255).astype(np.uint8)).convert("1"))
    return frames


def bus():
    import bus_anim as m
    parts = m.snoopy()
    return [m.frame(f, *parts) for f in range(N)]


def roof():
    import doghouse_anim as m
    parts = m.load()
    return [m.frame(f, *parts) for f in range(N)]


def bed_filled():
    import bed_anim as m
    bw = m.base_filled()
    return [m.frame(f, bw) for f in range(N)]


def bed():
    import bed_anim as m
    bw = m.base()
    return [m.frame(f, bw) for f in range(N)]


def snow():
    import snow_anim as m
    bw, horizon, nose = m.base()
    return [m.frame(f, bw, horizon, nose) for f in range(N)]


def leaves(style):
    import leaves_anim as m
    parts = m.snoopy()
    return [m.frame(f, parts, style) for f in range(N)]


def couch(look, close=False):
    import couch_anim
    return couch_anim.make(look, couch_anim.CLOSE if close else None, clean=not close)


def office(style):
    import office_anim
    return office_anim.make(style)


def scene_(name, look):
    import scene
    return scene.make(name, look)


REMOVED = {"K1", "K2", "K4", "K5", "K9", "K10", "K11", "K12", "K13", "K14", "K15", "K16",
           "K18", "K19", "K20", "K22", "K26", "K27", "K28"}


def stand(theme):
    import stand_anim
    return stand_anim.make(theme)


def cartoon_(name):
    import cartoon
    return cartoon.make(name)


def face_():
    import face_anim
    return face_anim.make()


def roof_theme(theme):
    import roof_themes
    return roof_themes.make(theme)


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        os.remove(os.path.join(OUT, old))
    plan = [
        ("Black hole", "tilted, fine grain", lambda: blackhole(15, 0.0049, "grain")),
        ("Black hole", "tilted, text characters", lambda: blackhole(15, 0.0049, "ascii")),
        ("Black hole", "edge-on, like the film", lambda: blackhole(6, 0.0049, "grain")),
        ("Black hole", "seen from above", lambda: blackhole(28, 0.0052, "grain")),
        ("Black hole", "close-up", lambda: blackhole(13, 0.0039, "grain")),
        ("Snoopy night bus", "clean bus, lights passing", bus),
        ("Snoopy starry roof", "stars, shooting star, z's", roof),
        ("Snoopy in bed", "white Snoopy, black outlines, dark room", bed_filled),
        ("Snoopy in bed", "line art, light", lambda: invert(bed())),
        ("Snoopy in the snow", "night", snow),
        ("Snoopy in the snow", "snowy day (light)", lambda: invert(snow())),
        ("Snoopy in the leaves", "line art", lambda: leaves("lines")),
        ("Snoopy in the leaves", "solid white", lambda: leaves("solid")),
        ("Snoopy in the leaves", "line art, light", lambda: invert(leaves("lines"))),
        ("Snoopy cosy armchair", "snow in the window, lamp, steam", lambda: scene_("cozy", "dark")),
        ("Snoopy cosy armchair", "light", lambda: scene_("cozy", "light")),
        ("Snoopy on the couch", "the whole room, TV flicker, dust in the light", lambda: couch("dark")),
        ("Snoopy on the couch", "light", lambda: couch("light")),
        ("Snoopy at the window", "warm light flickering", lambda: scene_("window", "dark")),
        ("Snoopy at the window", "light", lambda: scene_("window", "light")),
        ("Snoopy with laptop", "music notes, screen glow", lambda: scene_("laptop", "dark")),
        ("Snoopy with laptop", "light", lambda: scene_("laptop", "light")),
        ("Snoopy night bus", "sharp picture: galaxy, lights streaming", lambda: scene_("bus", "dark")),
        ("Snoopy night bus", "sharp picture, light", lambda: scene_("bus", "light")),
        ("Snoopy on the couch", "close-up: Snoopy bigger", lambda: couch("dark", True)),
        ("Snoopy on the couch", "close-up, light", lambda: couch("light", True)),
        ("Snoopy off to work", "line art, walking past the city", lambda: office("lines")),
        ("Snoopy off to work", "clean, solid white", lambda: office("clean")),
    ]
    plan += [
        ("Snoopy starry roof", "meteor shower", lambda: roof_theme("meteors")),
        ("Snoopy starry roof", "snowy night", lambda: roof_theme("snow")),
        ("Snoopy starry roof", "fireflies", lambda: roof_theme("fireflies")),
        ("Snoopy starry roof", "northern lights", lambda: roof_theme("aurora")),
        ("Snoopy starry roof", "constellations drawing in", lambda: roof_theme("stars")),
        ("Snoopy starry roof", "rain and far lightning", lambda: roof_theme("rain")),
    ]
    plan += [
        ("Snoopy standing", "hello: hearts float up", lambda: stand("hello")),
        ("Snoopy standing", "starry night", lambda: stand("stars")),
        ("Snoopy standing", "snow", lambda: stand("snow")),
        ("Snoopy standing", "music, bobbing to the beat", lambda: stand("music")),
        ("Snoopy standing", "autumn leaves", lambda: stand("autumn")),
        ("Snoopy standing", "fireflies", lambda: stand("fireflies")),
        ("Snoopy flying ace", "wind, scarf flapping, Woodstock", lambda: cartoon_("ace")),
        ("Snoopy on his doghouse", "clouds drifting, birds, grass", lambda: cartoon_("houseday")),
        ("Snoopy heart hug", "heart beating, hearts floating", lambda: cartoon_("heart")),
        ("Snoopy driving", "night drive: moon, stars, wheels turning", lambda: cartoon_("car")),
        ("Snoopy face", "blinks, glances, nose twitch", face_),
        ("Saturn", "rings, moon, storm, shadows", lambda: __import__("saturn_anim").make("classic")),
        ("Saturn", "close-up", lambda: __import__("saturn_anim").make("close")),
        ("Snoopy title card", "letters bounce, head bobs, nose twitch", lambda: __import__("title_anim").make()),
    ]
    index = []
    for i, (group, look, make) in enumerate(plan, 1):
        key = f"K{i}"
        if key in REMOVED:                       # taken off the page (numbers stay the same)
            continue
        print(f"{key}: {group} - {look}")
        index.append({"id": key, "group": group, "look": look, "file": save(make(), key)})
    with open(os.path.join(OUT, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    print(f"{len(index)} options in {OUT}")


if __name__ == "__main__":
    main()
