// The keychain over Bluetooth: the same "wireless serial cable" (Nordic UART)
// the Claude face uses. One text line per message (see keychain.ino BRIDGE).
import { useEffect, useRef, useState } from "react";

const NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // we write here
const TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // the keychain notifies here

export const canBluetooth = typeof navigator !== "undefined" && !!navigator.bluetooth;

export function useKeychain(onEvent) {
  const [state, setState] = useState({ link: "off", name: "", screen: 0, count: 0, bri: 5, vol: 3, snd: true, names: [] });
  const rx = useRef(null), dev = useRef(null), buf = useRef(""), queue = useRef(Promise.resolve());
  const evt = useRef(onEvent); evt.current = onEvent;

  function onLine(line) {
    if (line.startsWith("K:")) {
      const [screen, count, bri, vol, snd] = line.slice(2).split(",").map(Number);
      setState((s) => ({ ...s, screen, count, bri, vol, snd: !!snd, names: Array.from({ length: count }, (_, i) => s.names[i] || "") }));
    } else if (line.startsWith("N:")) {
      const i = line.indexOf(",", 2), n = +line.slice(2, i), name = line.slice(i + 1);
      setState((s) => { const names = s.names.slice(); names[n] = name; return { ...s, names }; });
    }
  }

  /** send one line (queued, split into BLE-sized pieces) */
  function send(line) {
    if (!rx.current) return Promise.resolve(false);
    const data = new TextEncoder().encode(line + "\n");
    queue.current = queue.current.then(async () => {
      for (let i = 0; i < data.length; i += 180) await rx.current?.writeValueWithoutResponse(data.slice(i, i + 180));
      return true;
    }).catch(() => false);
    return queue.current;
  }

  async function attach(device) {
    setState((s) => ({ ...s, link: "connecting", name: device.name || "Keychain" }));
    const server = await device.gatt.connect();
    const svc = await server.getPrimaryService(NUS);
    rx.current = await svc.getCharacteristic(RX);
    const tx = await svc.getCharacteristic(TX);
    await tx.startNotifications();
    tx.addEventListener("characteristicvaluechanged", (e) => {
      buf.current += new TextDecoder().decode(e.target.value);
      let k;
      while ((k = buf.current.indexOf("\n")) >= 0) {
        const line = buf.current.slice(0, k).trim(); buf.current = buf.current.slice(k + 1);
        if (line) onLine(line);
      }
    });
    setState((s) => ({ ...s, link: "on" }));
    evt.current?.("connected", device.name);
    await send("I:");
  }

  async function connect() {
    try {
      const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }, { namePrefix: "Snoopy" }], optionalServices: [NUS] });
      dev.current = device;
      device.addEventListener("gattserverdisconnected", () => {
        rx.current = null;
        setState((s) => ({ ...s, link: "off" }));
        evt.current?.("disconnected");
      });
      await attach(device);
    } catch (e) {
      setState((s) => ({ ...s, link: "off" }));
      if (e.name !== "NotFoundError") evt.current?.("error", e.message);
    }
  }

  async function reconnect() {
    if (dev.current && !rx.current) { try { await attach(dev.current); } catch { setState((s) => ({ ...s, link: "off" })); } }
  }

  function disconnect() { dev.current?.gatt?.disconnect(); }

  // optimistic local updates, so the controls feel instant
  const act = {
    show: (i) => { setState((s) => ({ ...s, screen: i })); return send("S:" + i); },
    next: () => { setState((s) => ({ ...s, screen: s.count ? (s.screen + 1) % s.count : 0 })); return send("S:+"); },
    bri: (v) => { setState((s) => ({ ...s, bri: v })); return send("B:" + v); },
    vol: (v) => { setState((s) => ({ ...s, vol: v })); return send("V:" + v); },
    snd: (on) => { setState((s) => ({ ...s, snd: on })); return send("O:" + (on ? 1 : 0)); },
  };

  useEffect(() => () => disconnect(), []);
  return { ...state, connect, reconnect, disconnect, send, act };
}
