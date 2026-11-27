// The keychain over Bluetooth: the same "wireless serial cable" (Nordic UART)
// the Claude face uses. One text line per message (see keychain.ino BRIDGE).
// Two ways in: the browser's Web Bluetooth, or - in the Android app - native
// Bluetooth (more reliable, and it reconnects by itself on every launch).
import { useEffect, useRef, useState } from "react";
import { Capacitor } from "@capacitor/core";
import { BleClient } from "@capacitor-community/bluetooth-le";

const NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";   // we write here
const TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";   // the keychain notifies here
const native = Capacitor.isNativePlatform();

export const canBluetooth = native || (typeof navigator !== "undefined" && !!navigator.bluetooth);

/* ---- the two transports: pick(), open(dev, onData, onDrop) -> { write(bytes), close() } ---- */
const web = {
  async pick() {
    const d = await navigator.bluetooth.requestDevice({ filters: [{ services: [NUS] }, { namePrefix: "Snoopy" }], optionalServices: [NUS] });
    return { id: d.id, name: d.name || "Keychain", raw: d };
  },
  async known(id) {
    if (!navigator.bluetooth?.getDevices) return null;
    const d = (await navigator.bluetooth.getDevices()).find((x) => x.id === id);
    if (!d) return null;
    if (d.watchAdvertisements) {                  // it may need to see the keychain advertising first
      const seen = new Promise((res) => d.addEventListener("advertisementreceived", res, { once: true }));
      d.watchAdvertisements().catch(() => {});
      await Promise.race([seen, new Promise((res) => setTimeout(res, 4000))]);
    }
    return { id: d.id, name: d.name || "Keychain", raw: d };
  },
  async open(dev, onData, onDrop) {
    const d = dev.raw;
    if (!d._kcDrop) { d.addEventListener("gattserverdisconnected", () => d._kcDrop?.()); }
    d._kcDrop = onDrop;
    const server = await d.gatt.connect();
    const svc = await server.getPrimaryService(NUS);
    const rx = await svc.getCharacteristic(RX);
    const tx = await svc.getCharacteristic(TX);
    await tx.startNotifications();
    tx.addEventListener("characteristicvaluechanged", (e) => onData(new TextDecoder().decode(e.target.value)));
    return { write: (b) => rx.writeValueWithoutResponse(b), close: () => d.gatt.disconnect() };
  },
};

let bleReady = null;
const nat = {
  init() { return (bleReady ||= BleClient.initialize({ androidNeverForLocation: true })); },
  async pick() {                                  // no list to pick from: find the keychain and take it
    await nat.init();
    const d = await new Promise((res, rej) => {
      let done = false;
      const end = (f, v) => { if (done) return; done = true; clearTimeout(t); BleClient.stopLEScan().catch(() => {}); f(v); };
      const t = setTimeout(() => end(rej, new Error("Keychain not found - tap it to wake it, then try again.")), 10000);
      BleClient.requestLEScan({ allowDuplicates: false }, (r) => {
        const name = r.localName || r.device?.name || "";
        if (name.startsWith("Snoopy") || (r.uuids || []).some((u) => u.toLowerCase() === NUS)) end(res, { ...r.device, name: name || r.device.name });
      }).catch((e) => end(rej, e));
    });
    return { id: d.deviceId, name: d.name || "Snoopy Keychain" };
  },
  async known(id) { await nat.init(); return { id, name: localStorage.getItem("kc-name") || "Keychain" }; },
  async open(dev, onData, onDrop) {
    await nat.init();
    await BleClient.connect(dev.id, () => onDrop());
    await BleClient.startNotifications(dev.id, NUS, TX, (dv) => onData(new TextDecoder().decode(dv)));
    return {
      write: (b) => BleClient.writeWithoutResponse(dev.id, NUS, RX, new DataView(b.buffer, b.byteOffset, b.byteLength)),
      close: () => BleClient.disconnect(dev.id),
    };
  },
};
const T = native ? nat : web;

export function useKeychain(onEvent) {
  const [state, setState] = useState({ link: "off", name: "", screen: 0, count: 0, bri: 5, vol: 3, snd: true, names: [] });
  const link = useRef(null), dev = useRef(null), buf = useRef(""), queue = useRef(Promise.resolve());
  const wanted = useRef(false);                  // stay connected: reconnect by itself if the link drops
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
  function onData(text) {
    buf.current += text;
    let k;
    while ((k = buf.current.indexOf("\n")) >= 0) {
      const line = buf.current.slice(0, k).trim(); buf.current = buf.current.slice(k + 1);
      if (line) onLine(line);
    }
  }

  /** send one line (queued, split into BLE-sized pieces) */
  function send(line) {
    if (!link.current) return Promise.resolve(false);
    const data = new TextEncoder().encode(line + "\n");
    queue.current = queue.current.then(async () => {
      for (let i = 0; i < data.length; i += 180) await link.current?.write(data.slice(i, i + 180));
      return true;
    }).catch(() => false);
    return queue.current;
  }

  function onDrop() {
    link.current = null;
    setState((s) => ({ ...s, link: wanted.current ? "connecting" : "off" }));
    evt.current?.("disconnected");
    if (wanted.current) setTimeout(() => retry(1), 800);
  }

  async function attach(d) {
    dev.current = d;
    try { localStorage.setItem("kc-device", d.id); localStorage.setItem("kc-name", d.name || ""); } catch {}
    setState((s) => ({ ...s, link: "connecting", name: d.name }));
    link.current = await T.open(d, onData, onDrop);
    setState((s) => ({ ...s, link: "on" }));
    evt.current?.("connected", d.name);
    await send("I:");
  }

  async function retry(n) {                       // the keychain napped or walked out of range: keep trying, gently
    if (!wanted.current || link.current || !dev.current) return;
    try { await attach(dev.current); }
    catch { setState((s) => ({ ...s, link: "connecting" })); setTimeout(() => retry(n + 1), Math.min(15000, 1500 * n)); }
  }

  async function connect() {
    try {
      const d = await T.pick();
      wanted.current = true;
      await attach(d);
    } catch (e) {
      setState((s) => ({ ...s, link: "off" }));
      if (!/cancel|NotFound|chooser|request/i.test(String(e?.message) + e?.name)) evt.current?.("error", e.message);
    }
  }

  /** at launch / after a reload: the keychain we used last, without asking */
  async function resume() {
    let id = "";
    try { id = localStorage.getItem("kc-device") || ""; } catch {}
    if (!id) return false;
    try {
      const d = await T.known(id);
      if (!d) return false;
      wanted.current = true;
      await attach(d);
      return true;
    } catch {
      if (native && wanted.current) { dev.current = { id, name: localStorage.getItem("kc-name") || "Keychain" }; retry(2); return true; }
      setState((s) => ({ ...s, link: "off" })); return false;
    }
  }

  async function reconnect() {
    if (dev.current && !link.current) { wanted.current = true; try { await attach(dev.current); } catch { setState((s) => ({ ...s, link: "off" })); } }
    else if (!(await resume())) await connect();  // the browser forgot: one tap picks it again
  }

  function disconnect() { wanted.current = false; const l = link.current; link.current = null; l?.close(); setState((s) => ({ ...s, link: "off" })); }

  // optimistic local updates, so the controls feel instant
  const act = {
    show: (i) => { setState((s) => ({ ...s, screen: i })); return send("S:" + i); },
    next: () => { setState((s) => ({ ...s, screen: s.count ? (s.screen + 1) % s.count : 0 })); return send("S:+"); },
    bri: (v) => { setState((s) => ({ ...s, bri: v })); return send("B:" + v); },
    vol: (v) => { setState((s) => ({ ...s, vol: v })); return send("V:" + v); },
    snd: (on) => { setState((s) => ({ ...s, snd: on })); return send("O:" + (on ? 1 : 0)); },
  };

  useEffect(() => {
    resume();
    // coming back to the app (screen on, tab shown): make sure we're still linked
    const onShow = () => { if (document.visibilityState === "visible" && wanted.current && !link.current) retry(1); };
    document.addEventListener("visibilitychange", onShow);
    return () => { document.removeEventListener("visibilitychange", onShow); wanted.current = false; link.current?.close(); };
  }, []);
  let lastName = "";
  try { lastName = localStorage.getItem("kc-name") || ""; } catch {}
  return { ...state, lastName, connect, reconnect, disconnect, send, act };
}
