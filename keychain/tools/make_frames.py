"""
make_frames.py - turn your pictures into animations for the keychain screen.

    python make_frames.py art.gif                      # one animation
    python make_frames.py bus.gif rain.gif logo.png    # several: tap the keychain to switch
    python make_frames.py art.gif --fit contain --invert --preview
    python make_frames.py "bus.gif=Night bus" "house.gif=Starry roof"   # with names

Accepts GIF, animated WebP/PNG, still images (PNG/JPG/...), or a folder of
numbered frames. Each frame becomes 128x64 black & white (1 KB), written to
../frames.h; then open keychain.ino in the Arduino IDE and upload.

Options
  --fit cover|contain   fill the screen (crop the edges) or fit inside it (bars)
  --dither bayer|floyd|none
                        how grey becomes dots. bayer (default) stays steady
                        in animations; floyd looks finer on stills, but its
                        dots shimmer when they move; none = a hard threshold
  --threshold N         0-255, for --dither none (default 128)
  --contrast X          boost contrast before converting (default 1.4)
  --invert              swap black and white (for art on a light background)
  --fps N               speed when the file has no timing of its own (default 12)
  --max-frames N        cap the frames per animation (default 300)
  --style dots|poster   dots (default): grey becomes dots. poster: the bright
                        subject in solid white with a black gap around it, the
                        rest as thin outlines (clean for characters on a busy
                        background)
  --key bright|pale     what counts as "light". pale picks out pale/white things
                        on a colourful background (a white dog on pink seats)
                        better than plain brightness
  --edges N             poster outlines: lower = more lines (default 20, 0 = none)
  --preview             also write preview.png: every frame as the screen shows it
Needs Pillow:  pip install pillow
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageEnhance, ImageFilter, ImageOps, ImageSequence

W, H = 128, 64
HERE = os.path.dirname(os.path.abspath(__file__))
BAYER = [[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
         [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
         [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
         [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]


def frames_of(path, fps):
    """[(RGBA image, ms)] from a file or a folder of frames."""
    if os.path.isdir(path):
        names = sorted(n for n in os.listdir(path)
                       if n.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp")))
        return [(Image.open(os.path.join(path, n)).convert("RGBA"), int(1000 / fps)) for n in names]
    im = Image.open(path)
    out = []
    for f in ImageSequence.Iterator(im):
        ms = f.info.get("duration") or int(1000 / fps)
        out.append((f.convert("RGBA"), max(20, int(ms))))
    return out


def fit(img, how, size=(W, H)):
    """The picture on a black background, cropped or fitted to the screen (RGB)."""
    bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
    img = Image.alpha_composite(bg, img).convert("RGB")
    if how == "contain":
        img.thumbnail(size, Image.LANCZOS)
        canvas = Image.new("RGB", size, (0, 0, 0))
        canvas.paste(img, ((size[0] - img.width) // 2, (size[1] - img.height) // 2))
        return canvas
    return ImageOps.fit(img, size, Image.LANCZOS)


def gray(rgb, key):
    """bright: ordinary brightness. pale: the darkest of R, G, B, so strong colours
    go dark and only pale / white things stay light."""
    if key == "pale":
        r, g, b = rgb.split()
        return ImageChops.darker(ImageChops.darker(r, g), b)
    return rgb.convert("L")


def poster(img, g, a, lut):
    """Solid subject + a black gap around it + outlines of everything else."""
    g = ImageEnhance.Contrast(g.point(lut)).enhance(a.contrast)
    if a.invert:
        g = ImageOps.invert(g)
    subj = g.point(lambda v: 255 if v > a.threshold else 0)
    if not a.edges:
        return subj.convert("1")
    big = fit(img, a.fit, (W * 2, H * 2)).convert("L").filter(ImageFilter.GaussianBlur(1.4))
    e = ImageOps.autocontrast(big.filter(ImageFilter.FIND_EDGES)).resize((W, H), Image.BOX)
    lines = e.point(lambda v: 255 if v > a.edges else 0)
    lines = ImageChops.subtract(lines, subj.filter(ImageFilter.MaxFilter(3)))
    return ImageChops.lighter(subj, lines).convert("1")


def levels(grays):
    """One black/white point for the whole animation (per frame, the background would flicker)."""
    hist = [0] * 256
    for g in grays:
        for i, n in enumerate(g.histogram()):
            hist[i] += n
    total, cut = sum(hist), sum(hist) * 0.005
    lo = next(i for i in range(256) if sum(hist[:i + 1]) > cut)
    hi = next(i for i in range(255, -1, -1) if sum(hist[i:]) > cut)
    if hi <= lo:
        lo, hi = 0, 255
    return lambda v: max(0, min(255, (v - lo) * 255 // (hi - lo)))


def to_bits(img, a, lut):
    img = ImageEnhance.Contrast(img.point(lut)).enhance(a.contrast)
    if a.invert:
        img = ImageOps.invert(img)
    if a.dither == "floyd":
        return img.convert("1")                                  # Floyd-Steinberg
    px = img.load()
    out = Image.new("1", (W, H), 0)
    o = out.load()
    for y in range(H):
        for x in range(W):
            t = a.threshold if a.dither == "none" else (BAYER[y % 8][x % 8] + 0.5) * 4
            o[x, y] = 255 if px[x, y] > t else 0
    return out


def pack(bw):
    """Adafruit drawBitmap layout: rows top to bottom, 16 bytes each, leftmost pixel = 0x80."""
    px = bw.load()
    data = bytearray()
    for y in range(H):
        for xb in range(0, W, 8):
            b = 0
            for i in range(8):
                if px[xb + i, y]:
                    b |= 0x80 >> i
            data.append(b)
    return bytes(data)


def main():
    ap = argparse.ArgumentParser(description="Pictures -> keychain animations (frames.h)")
    ap.add_argument("inputs", nargs="+", help="GIFs, images or folders of frames; one animation each")
    ap.add_argument("--fit", choices=["cover", "contain"], default="cover")
    ap.add_argument("--dither", choices=["bayer", "floyd", "none"], default="bayer")
    ap.add_argument("--threshold", type=int, default=128)
    ap.add_argument("--contrast", type=float, default=1.4)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--fps", type=float, default=12)
    ap.add_argument("--max-frames", type=int, default=300)
    ap.add_argument("--style", choices=["dots", "poster"], default="dots")
    ap.add_argument("--key", choices=["bright", "pale"], default="bright")
    ap.add_argument("--edges", type=int, default=20)
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "frames.h"))
    a = ap.parse_args()

    anims, all_frames, delays, shots = [], [], [], []
    for spec in a.inputs:
        path, label = spec, None
        if "=" in spec and os.path.exists(spec.rsplit("=", 1)[0]):     # art.gif=Its name
            path, label = spec.rsplit("=", 1)
        src = frames_of(path, a.fps)
        if not src:
            sys.exit(f"no frames in {path}")
        if len(src) > a.max_frames:
            print(f"  {path}: {len(src)} frames, keeping the first {a.max_frames}")
            src = src[:a.max_frames]
        name = (label or os.path.splitext(os.path.basename(path.rstrip("/\\")))[0])[:24].replace('"', "'")
        anims.append((name, len(all_frames), len(src)))
        grays = [gray(fit(img, a.fit), a.key) for img, _ in src]
        lut = levels(grays)
        for g, (img, ms) in zip(grays, src):
            bw = poster(img, g, a, lut) if a.style == "poster" else to_bits(g, a, lut)
            all_frames.append(pack(bw))
            delays.append(min(ms, 65535))
            shots.append(bw)
        print(f"  {name}: {len(src)} frame(s)")

    kb = len(all_frames)
    if kb > 850:
        print(f"note: {kb} KB of frames. That's more than the default 1.3 MB program space holds; in the "
              "Arduino IDE pick Tools > Partition Scheme > \"Huge APP (3MB No OTA)\" (room for about 2,700 frames)")

    L = ["// made by tools/make_frames.py - run it again to change the pictures", "#pragma once",
         "#include <Arduino.h>", "",
         f"#define KC_ANIM_COUNT {len(anims)}", f"#define KC_FRAME_COUNT {len(all_frames)}", "",
         "struct KcAnim { const char* name; uint16_t first, count; };",
         "const KcAnim KC_ANIMS[] = {"]
    L += [f'  {{ "{n}", {f}, {c} }},' for n, f, c in anims]
    L += ["};", "", "const uint16_t KC_DELAY[] PROGMEM = {"]
    L += ["  " + ", ".join(str(d) for d in delays[i:i + 16]) + "," for i in range(0, len(delays), 16)]
    L += ["};", "", "const uint8_t KC_FRAMES[][1024] PROGMEM = {"]
    for fr in all_frames:
        L.append("  {")
        L += ["    " + ",".join(f"0x{b:02x}" for b in fr[i:i + 32]) + "," for i in range(0, 1024, 32)]
        L.append("  },")
    L.append("};")
    with open(a.out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print(f"wrote {os.path.normpath(a.out)}: {len(anims)} animation(s), {len(all_frames)} frame(s)")

    if a.preview:
        cols = min(8, len(shots))
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (W + 4) + 4, rows * (H + 4) + 4), (40, 40, 40))
        for i, bw in enumerate(shots):
            tile = bw.convert("RGB").point(lambda v: 235 if v else 0)
            sheet.paste(tile, (4 + (i % cols) * (W + 4), 4 + (i // cols) * (H + 4)))
        sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
        p = os.path.join(os.path.dirname(os.path.abspath(a.out)), "preview.png")
        sheet.save(p)
        print(f"preview: {p}")


if __name__ == "__main__":
    main()
