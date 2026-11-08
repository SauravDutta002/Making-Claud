"""
flash.py - put the latest Claude Buddy firmware on the board (over USB).

    claude-buddy-flash            # finds the board and flashes it
    claude-buddy-flash --check    # just print the board's port (empty if none)

Writes the bootloader, partition table and app separately, so the board's
saved settings (brightness, volume, XP and level) survive the update.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, "firmware")
PARTS = [("0x0", "bootloader.bin"), ("0x8000", "partitions.bin"),
         ("0xe000", "boot_app0.bin"), ("0x10000", "claude-buddy.bin")]

try:
    from .claude_bridge import find_port
except ImportError:
    from claude_bridge import find_port


def main():
    ap = argparse.ArgumentParser(description="Install the latest firmware on the Claude Buddy board")
    ap.add_argument("--port", help="serial port (default: find the board)")
    ap.add_argument("--check", action="store_true", help="only print the board's port")
    a = ap.parse_args()

    port = a.port or find_port()
    if a.check:
        print(port or "")
        return 0
    if not port:
        print("No board found on USB - plug it in with a data cable and try again.")
        return 1
    missing = [f for _, f in PARTS if not os.path.exists(os.path.join(FIRMWARE, f))]
    if missing:
        print("Firmware files missing: " + ", ".join(missing))
        return 1

    import esptool
    args = ["--chip", "esp32c3", "--port", port, "--baud", "921600", "write_flash"]
    for addr, name in PARTS:
        args += [addr, os.path.join(FIRMWARE, name)]
    print(f"Flashing the board on {port}...")
    try:
        esptool.main(args)
    except SystemExit as e:                       # esptool exits on its own errors
        if e.code:
            print("Flashing failed - unplug the board, plug it back in, close anything "
                  "using its port (the bridge, Arduino Serial Monitor) and try again.")
            return 1
    except Exception as e:
        print(f"Flashing failed: {e}")
        return 1
    print("Done - the board restarts with the new firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
