"""
pack_frames.py - shrinks frames.h into frames_z.h for the firmware.

frames.h keeps every frame whole (1 KB each, easy to read and edit, and what
export_web.py reads). The keychain only needs what CHANGES from one frame to
the next: each frame is XORed with the one before it (the first frame of each
animation with a blank screen), then run-length packed. ~1.4 MB -> ~170 KB,
which leaves room for two copies of the firmware - needed to update it over
Bluetooth (the new copy is written next to the running one).

    python tools/pack_frames.py          (make_frames.py runs it for you)

Packing, per frame:  c < 128: c+1 bytes follow as they are
                     c >= 128: the next byte, repeated c-126 times (2..129)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pack(d):
    out, lit, i, n = bytearray(), bytearray(), 0, len(d)

    def flush():
        nonlocal lit
        while lit:
            c = lit[:128]
            out.append(len(c) - 1)
            out.extend(c)
            lit = lit[128:]

    while i < n:
        j = i
        while j < n and d[j] == d[i] and j - i < 129:
            j += 1
        if j - i >= 2:
            flush()
            out.append(126 + (j - i))
            out.append(d[i])
            i = j
        else:
            lit.append(d[i])
            i += 1
    flush()
    return bytes(out)


def unpack(z, prev):
    out, i = bytearray(), 0
    while len(out) < 1024:
        c = z[i]; i += 1
        if c < 128:
            out.extend(z[i:i + c + 1]); i += c + 1
        else:
            out.extend([z[i]] * (c - 126)); i += 1
    return bytes(a ^ b for a, b in zip(out, prev)), i


def main():
    src = os.path.join(ROOT, "frames.h")
    s = open(src, encoding="utf-8").read()
    anims = [(m.group(1), int(m.group(2)), int(m.group(3)))
             for m in re.finditer(r'\{ "(.*?)", (\d+), (\d+) \}', s.split("KC_DELAY")[0])]
    delays = [int(x) for x in re.findall(r"\d+", s.split("KC_DELAY[] PROGMEM = {", 1)[1].split("};", 1)[0])]
    body = s.split("KC_FRAMES[][1024] PROGMEM = {", 1)[1]
    frames = [bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", b)) for b in re.findall(r"\{([^{}]*)\}", body)]
    assert all(len(f) == 1024 for f in frames) and len(frames) == len(delays) == sum(a[2] for a in anims), "frames.h looks odd"

    data, offs = bytearray(), []
    for _, first, cnt in anims:
        prev = bytes(1024)
        for k in range(cnt):
            f = frames[first + k]
            z = pack(bytes(a ^ b for a, b in zip(f, prev)))
            back, used = unpack(z, prev)                 # check it round-trips
            assert back == f and used == len(z)
            offs.append(len(data)); data.extend(z); prev = f
    offs.append(len(data))

    L = ["// made by tools/pack_frames.py from frames.h - don't edit, run it again", "#pragma once", "#include <Arduino.h>", "",
         f"#define KC_ANIM_COUNT {len(anims)}", f"#define KC_FRAME_COUNT {len(frames)}", "",
         "struct KcAnim { const char* name; uint16_t first, count; };", "const KcAnim KC_ANIMS[] = {"]
    L += [f'  {{ "{n}", {a}, {c} }},' for n, a, c in anims]
    L += ["};", "", "const uint16_t KC_DELAY[] PROGMEM = {"]
    L += ["  " + ", ".join(map(str, delays[i:i + 16])) + "," for i in range(0, len(delays), 16)]
    L += ["};", "", "// where each frame's packed changes start in KC_Z (one extra at the end)", "const uint32_t KC_ZOFF[] PROGMEM = {"]
    L += ["  " + ", ".join(map(str, offs[i:i + 12])) + "," for i in range(0, len(offs), 12)]
    L += ["};", "", "const uint8_t KC_Z[] PROGMEM = {"]
    L += ["  " + ",".join("0x%02X" % b for b in data[i:i + 32]) + "," for i in range(0, len(data), 32)]
    L += ["};", ""]
    open(os.path.join(ROOT, "frames_z.h"), "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"frames_z.h: {len(anims)} screens, {len(frames)} frames, {len(frames)} KB -> {len(data) // 1024} KB")


if __name__ == "__main__":
    sys.exit(main())
