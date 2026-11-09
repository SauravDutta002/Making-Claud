"""
build_firmware.py - compile claude.ino and refresh the firmware the installer ships.

    python tools/build_firmware.py               # build -> bridge/firmware/*.bin
    python tools/build_firmware.py --upload COM5 # ...and flash a board on that port

Needs arduino-cli (it comes with the Arduino IDE 2) with the ESP32 core 3.x and the
Adafruit GFX + Adafruit SH110X libraries installed.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FQBN = "esp32:esp32:esp32c3:CDCOnBoot=cdc"
OUT = os.path.join(ROOT, "bridge", "firmware")


def find_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    for c in (r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
              os.path.expandvars(r"%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
              "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"):
        if os.path.exists(c):
            return c
    sys.exit("arduino-cli not found - install the Arduino IDE 2 (it includes it) or arduino-cli itself.")


def run(cmd):
    print("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--upload", metavar="PORT", help="also flash the board on this serial port")
    a = ap.parse_args()
    cli = find_cli()

    work = tempfile.mkdtemp(prefix="claude-buddy-")
    sketch = os.path.join(work, "claude")              # arduino wants folder name == sketch name
    os.makedirs(sketch)
    shutil.copy(os.path.join(ROOT, "claude.ino"), sketch)
    build = os.path.join(work, "build")
    run([cli, "compile", "--fqbn", FQBN, "--output-dir", build, sketch])

    os.makedirs(OUT, exist_ok=True)
    shutil.copy(os.path.join(build, "claude.ino.bootloader.bin"), os.path.join(OUT, "bootloader.bin"))
    shutil.copy(os.path.join(build, "claude.ino.partitions.bin"), os.path.join(OUT, "partitions.bin"))
    shutil.copy(os.path.join(build, "claude.ino.bin"), os.path.join(OUT, "claude-buddy.bin"))
    data = subprocess.run([cli, "config", "get", "directories.data"], capture_output=True, text=True).stdout.strip()
    boot = sorted(glob.glob(os.path.join(data, "packages", "esp32", "hardware", "esp32", "*", "tools",
                                         "partitions", "boot_app0.bin")))
    if boot:
        shutil.copy(boot[-1], os.path.join(OUT, "boot_app0.bin"))
    print(f"firmware ready in {OUT}")

    if a.upload:
        run([cli, "upload", "--fqbn", FQBN, "--input-dir", build, "-p", a.upload, sketch])
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
