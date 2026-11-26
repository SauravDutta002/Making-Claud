"""
export_web.py - packs the keychain's screens (from frames.h) for the remote app,
so its previews play exactly what the hardware shows.

    python tools/export_web.py      -> web/public/screens.bin

Format (gzipped): b"KC1", count (1 byte); per screen: name length (1) + name,
frame count (2, little-endian), per frame its delay in ms (2); then every
frame as 1024 bytes (rows of 16 bytes, leftmost pixel = 0x80), XOR'd against
the previous frame of the same screen (small after gzip).
"""
import gzip
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))


def main():
    h = open(os.path.join(ROOT, "frames.h"), encoding="utf-8").read()
    count = int(re.search(r"#define KC_ANIM_COUNT (\d+)", h).group(1))
    anims = re.findall(r'\{\s*"([^"]*)",\s*(\d+),\s*(\d+)\s*\}', h)[:count]
    delays = [int(x) for x in re.findall(r"\d+", re.search(r"KC_DELAY\[\][^{]*\{([^}]*)\}", h).group(1))]
    body = re.search(r"KC_FRAMES\[\]\[1024\][^{]*\{(.*)\};", h, re.S).group(1)
    frames = [bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", blk)) for blk in re.findall(r"\{([^{}]*)\}", body)]

    head = bytearray(b"KC1") + bytes([len(anims)])
    data = bytearray()
    for name, first, n in anims:
        first, n = int(first), int(n)
        nb = name.encode("utf-8")[:60]
        head += bytes([len(nb)]) + nb + struct.pack("<H", n)
        for k in range(n):
            head += struct.pack("<H", delays[first + k])
        prev = bytes(1024)
        for k in range(n):
            cur = frames[first + k]
            data += bytes(a ^ b for a, b in zip(cur, prev))
            prev = cur
    out = os.path.join(ROOT, "web", "public", "screens.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(gzip.compress(bytes(head + data), 9))
    print(f"wrote {out}: {len(anims)} screens, {sum(int(a[2]) for a in anims)} frames, {os.path.getsize(out) // 1024} KB")


if __name__ == "__main__":
    main()
