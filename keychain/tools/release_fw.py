"""
release_fw.py - publish a new keychain firmware to the app.

    1. change FW_VERSION in keychain.ino (e.g. "2.0.1")
    2. python tools/release_fw.py
    3. commit + push: the website (Vercel) serves the new firmware, and every
       keychain's app offers "Update" - over Bluetooth, or with the USB cable.

It compiles the firmware and puts it in frontend/public/firmware/:
    keychain.bin       the firmware (what a Bluetooth update sends)
    bootloader.bin, partitions.bin, boot_app0.bin
                       the rest, for an update with the USB cable (which also
                       works on a keychain that has never been updated before)
    manifest.json      version, size, md5 and where each file goes
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "frontend", "public", "firmware")
BUILD = os.path.join(ROOT, "build")
FQBN = "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs"
CLI_GUESS = [
    r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    "arduino-cli",
]


def cli():
    for c in CLI_GUESS:
        if os.path.isfile(c) or shutil.which(c):
            return c
    sys.exit("arduino-cli not found")


def core_file(name):
    base = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~/.arduino15")), "Arduino15", "packages", "esp32", "hardware", "esp32")
    if not os.path.isdir(base):
        base = os.path.expanduser("~/.arduino15/packages/esp32/hardware/esp32")
    for ver in sorted(os.listdir(base), reverse=True):
        p = os.path.join(base, ver, "tools", "partitions", name)
        if os.path.isfile(p):
            return p
    sys.exit(f"{name} not found in the esp32 core")


def main():
    ino = open(os.path.join(ROOT, "keychain.ino"), encoding="utf-8").read()
    version = re.search(r'#define FW_VERSION\s+"([^"]+)"', ino).group(1)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pack_frames.py")], check=True)
    print(f"compiling {version} ...")
    r = subprocess.run([cli(), "compile", "--fqbn", FQBN, "--output-dir", BUILD, ROOT], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    os.makedirs(OUT, exist_ok=True)
    parts = [("bootloader.bin", os.path.join(BUILD, "keychain.ino.bootloader.bin"), 0x0),
             ("partitions.bin", os.path.join(BUILD, "keychain.ino.partitions.bin"), 0x8000),
             ("boot_app0.bin", core_file("boot_app0.bin"), 0xE000),
             ("keychain.bin", os.path.join(BUILD, "keychain.ino.bin"), 0x10000)]
    files = []
    for name, src, addr in parts:
        shutil.copyfile(src, os.path.join(OUT, name))
        files.append({"file": name, "address": addr})
    app = open(os.path.join(OUT, "keychain.bin"), "rb").read()
    man = {"version": version, "size": len(app), "md5": hashlib.md5(app).hexdigest(),
           "built": time.strftime("%Y-%m-%d"), "files": files}
    json.dump(man, open(os.path.join(OUT, "manifest.json"), "w"), indent=2)
    print(f"firmware {version}: {len(app) // 1024} KB -> {OUT}")


if __name__ == "__main__":
    main()
