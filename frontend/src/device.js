// Talks to the board straight from the browser, without the bridge:
// USB (Web Serial) or Bluetooth (Web Bluetooth, the board's Nordic UART service).
// Chrome and Edge on a computer; Bluetooth also on Android.
import { useEffect, useRef, useState } from "react";

const NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // page -> board
const NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // board -> page
const ESPRESSIF = 0x303a;

export const canUsb = typeof navigator !== "undefined" && "serial" in navigator;
export const canBle = typeof navigator !== "undefined" && "bluetooth" in navigator;

/** board lines like "F:3,8,2,7,1" -> { bri, briCount, vol, volCount, snd } */
function parseCfg(line) {
  const m = /^F:(\d+),(\d+),(\d+),(\d+),(\d+)/.exec(line);
  return m ? { bri: +m[1], briCount: +m[2], vol: +m[3], volCount: +m[4], snd: +m[5] } : null;
}

class UsbLink {
  constructor(port) { this.port = port; this.kind = "usb"; }
  async open(onLine, onClose) {
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this.closed = false;
    (async () => {
      const dec = new TextDecoder();
      let buf = "";
      try {
        this.reader = this.port.readable.getReader();
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          buf += dec.decode(value, { stream: true });
          let i;
          while ((i = buf.indexOf("\n")) >= 0) { onLine(buf.slice(0, i).trim()); buf = buf.slice(i + 1); }
          if (buf.length > 512) buf = "";
        }
      } catch { /* unplugged */ }
      if (!this.closed) onClose();
    })();
  }
  async send(line) { await this.writer.write(new TextEncoder().encode(line + "\n")); }
  async close() {
    this.closed = true;
    try { await this.reader?.cancel(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port.close(); } catch {}
  }
}

class BleLink {
  constructor(dev) { this.dev = dev; this.kind = "ble"; this.q = Promise.resolve(); }
  async open(onLine, onClose) {
    this.dev.addEventListener("gattserverdisconnected", () => { if (!this.closed) onClose(); });
    const srv = await this.dev.gatt.connect();
    const svc = await srv.getPrimaryService(NUS);
    this.rx = await svc.getCharacteristic(NUS_RX);
    const tx = await svc.getCharacteristic(NUS_TX);
    let buf = "";
    const dec = new TextDecoder();
    tx.addEventListener("characteristicvaluechanged", (e) => {
      buf += dec.decode(e.target.value);
      let i;
      while ((i = buf.indexOf("\n")) >= 0) { onLine(buf.slice(0, i).trim()); buf = buf.slice(i + 1); }
    });
    await tx.startNotifications();
  }
  send(line) {                                   // 20-byte pieces, one after another
    const data = new TextEncoder().encode(line + "\n");
    this.q = this.q.then(async () => {
      for (let i = 0; i < data.length; i += 20) await this.rx.writeValueWithResponse(data.slice(i, i + 20));
    }).catch(() => {});
    return this.q;
  }
  async close() { this.closed = true; try { this.dev.gatt.disconnect(); } catch {} }
}

/** The direct link: { link, cfg, error, busy, connectUsb, connectBle, send, disconnect } */
export function useDevice(enabled) {
  const [link, setLink] = useState(null);
  const [cfg, setCfg] = useState(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const cur = useRef(null);

  const attach = async (l) => {
    setBusy(true); setError("");
    try {
      await l.open((line) => { const c = parseCfg(line); if (c) setCfg(c); }, () => {
        if (cur.current === l) { cur.current = null; setLink(null); setCfg(null); }
      });
      cur.current = l; setLink(l);
      await l.send("F:?");                       // what are the brightness, volume and sound now?
    } catch (e) {
      setError(l.kind === "usb"
        ? "Couldn't open the board. If the Claude Buddy bridge is running, it's using the USB port: use the bridge instead, or close it."
        : "Couldn't connect over Bluetooth. Make sure the board is switched on and nothing else is connected to it.");
      try { await l.close(); } catch {}
    }
    setBusy(false);
  };

  const connectUsb = async () => {
    try {
      const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF }] });
      await attach(new UsbLink(port));
    } catch { /* picker closed */ }
  };
  const connectBle = async () => {
    try {
      const dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }, { name: "Claude Buddy" }], optionalServices: [NUS] });
      await attach(new BleLink(dev));
    } catch { /* picker closed */ }
  };
  const disconnect = async () => {
    const l = cur.current; cur.current = null; setLink(null); setCfg(null);
    if (l) await l.close();
  };
  const send = (line) => cur.current ? cur.current.send(line).catch(() => {}) : Promise.resolve();

  // a USB board this site was allowed before: connect by itself
  useEffect(() => {
    if (!enabled || !canUsb || cur.current) return;
    let stop = false;
    navigator.serial.getPorts().then((ports) => {
      const p = ports.find((x) => x.getInfo().usbVendorId === ESPRESSIF);
      if (p && !stop && !cur.current) attach(new UsbLink(p));
    }).catch(() => {});
    return () => { stop = true; };
  }, [enabled]);

  // the board drops into "solo" after 15 s of silence: keep saying hi while linked
  useEffect(() => {
    if (!link) return;
    const t = setInterval(() => cur.current && cur.current.send("I:1").catch(() => {}), 5000);
    return () => clearInterval(t);
  }, [link]);

  // the bridge took over: let go of the board
  useEffect(() => { if (!enabled && cur.current) disconnect(); }, [enabled]);

  return { link, cfg, error, busy, connectUsb, connectBle, send, disconnect };
}
