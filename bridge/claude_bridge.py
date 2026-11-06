"""
claude_bridge.py - owns the USB serial port to the OLED face and forwards
events from Claude Code (hooks + status line) to it.

    Claude Code hook / statusline --UDP 127.0.0.1:47800--> this bridge --USB serial--> ESP32

Only this process opens the COM port, so hooks stay fast and never reset the
board. Run it once and leave it open:

    pip install pyserial
    python claude_bridge.py                # auto-detects the ESP32
    python claude_bridge.py --port COM5    # or pick the port yourself
    python claude_bridge.py --send E:think # inject a line (testing)
    python claude_bridge.py --list         # show serial ports
"""
import argparse
import socket
import sys
import time

UDP_HOST, UDP_PORT = "127.0.0.1", 47800

# USB vendor ids: Espressif (ESP32-C3 native USB), then common USB-UART chips.
ESPRESSIF = 0x303A
UART_CHIPS = {0x10C4, 0x1A86, 0x0403}


def find_port():
    from serial.tools import list_ports
    cands = [p for p in list_ports.comports() if p.vid is not None]
    for p in cands:
        if p.vid == ESPRESSIF:
            return p.device
    for p in cands:
        if p.vid in UART_CHIPS:
            return p.device
    return None


def list_ports_cmd():
    from serial.tools import list_ports
    for p in list_ports.comports():
        vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----"
        print(f"{p.device:8} {vid}  {p.description}")


class KeyWatcher:
    """Reports whether *any* key is held while a matching window is in front.

    It never records which key it is - only a yes/no "someone is typing".
    Windows only; elsewhere active() is always False.
    """

    def __init__(self, titles):
        self.titles = [t.strip().lower() for t in titles.split(",") if t.strip()]
        self.ok = sys.platform == "win32"
        if self.ok:
            import ctypes
            self.u = ctypes.windll.user32
            self.ct = ctypes

    def _title(self):
        hwnd = self.u.GetForegroundWindow()
        buf = self.ct.create_unicode_buffer(256)
        self.u.GetWindowTextW(hwnd, buf, 256)
        return buf.value.lower()

    def active(self):
        if not self.ok:
            return False
        down = False
        for vk in range(8, 255):               # 1-6 are mouse buttons
            if self.u.GetAsyncKeyState(vk) & 0x8000:
                down = True
                break
        if not down:
            return False
        title = self._title()
        return any(t in title for t in self.titles)


def send_one(line):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(line.encode(), (UDP_HOST, UDP_PORT))


def open_serial(port, baud):
    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate = port, baud
    ser.timeout, ser.write_timeout = 0, 1
    ser.dtr = False   # keep DTR/RTS low so opening the port doesn't reset the board
    ser.rts = False
    ser.open()
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--send", metavar="LINE")
    ap.add_argument("--no-typing", action="store_true",
                    help="don't show the typing face while you type")
    ap.add_argument("--type-windows",
                    default="claude,antigravity,powershell,command prompt,terminal,cmd",
                    help="comma-separated window-title words that count as 'typing to Claude'")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.list:
        return list_ports_cmd()
    if a.send:
        return send_one(a.send)

    import serial

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((UDP_HOST, UDP_PORT))
    except OSError:
        sys.exit(f"UDP port {UDP_PORT} is busy - is the bridge already running?")
    sock.settimeout(0.1)

    sticky = {}          # last U:/X: line per kind, replayed after a reconnect
    last_event = ("", 0.0)
    ser = None
    keys = None if a.no_typing else KeyWatcher(a.type_windows)
    typing, last_key, last_typing_send = False, 0.0, 0.0
    last_hook = "E:idle"     # what to show again when you stop typing
    print(f"bridge listening on udp://{UDP_HOST}:{UDP_PORT}")

    while True:
        if ser is None:
            port = a.port or find_port()
            if not port:
                print("no ESP32 found - plug it in (or pass --port). retrying...")
                time.sleep(2)
                continue
            try:
                ser = open_serial(port, a.baud)
                print(f"connected on {port}")
                time.sleep(0.3)
                for line in sticky.values():
                    ser.write((line + "\n").encode())
            except (serial.SerialException, OSError) as e:
                print(f"cannot open {port}: {e}")
                ser = None
                time.sleep(2)
                continue

        try:
            data = sock.recvfrom(512)[0].decode(errors="ignore")
        except socket.timeout:
            data = ""

        try:
            for line in data.splitlines():
                line = line.strip()
                if len(line) < 3 or line[1] != ":":
                    continue
                if line[0] in "UX":
                    sticky[line[0]] = line
                if line[0] == "E":
                    ev = line[2:]
                    last_hook = "E:idle" if ev in ("done", "need", "idle") else line
                    if typing and ev not in ("done", "need"):
                        continue       # the typing face wins while you type
                    # drop identical events fired back-to-back
                    now = time.time()
                    if line == last_event[0] and now - last_event[1] < 0.4:
                        continue
                    last_event = (line, now)
                if a.verbose:
                    print(">", line)
                ser.write((line + "\n").encode())
            if keys is not None:
                now = time.time()
                if keys.active():
                    last_key = now
                    if not typing or now - last_typing_send > 1.0:
                        if a.verbose and not typing:
                            print("> typing")
                        typing, last_typing_send = True, now
                        ser.write(b"E:key\n")
                elif typing and now - last_key > 2.0:
                    typing = False
                    ser.write((last_hook + "\n").encode())
            ser.read(ser.in_waiting or 1)   # drain the board's debug prints
        except (serial.SerialException, OSError) as e:
            if isinstance(e, serial.SerialTimeoutException):
                print("board is not reading the port (write timed out).\n"
                      "  -> Arduino IDE: Tools > USB CDC On Boot > Enabled, re-upload the sketch,\n"
                      "     and close the Arduino Serial Monitor.")
            else:
                print(f"serial lost ({e}) - reconnecting...")
            try:
                ser.close()
            except Exception:
                pass
            ser = None


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
