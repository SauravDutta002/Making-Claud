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
const OTA = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";  // firmware updates go here (firmware 2.0+)
const PIC = "6e400005-b5a3-f393-e0a9-e50e24dcca9e";  // My picture, live (firmware 2.1+)
const native = Capacitor.isNativePlatform();

export const canBluetooth = native || (typeof navigator !== "undefined" && !!navigator.bluetooth);

/* ---- the two transports: pick(), open(dev, onData, onDrop) -> { write(bytes), ota(bytes)|null, close() } ---- */
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
    const ota = await svc.getCharacteristic(OTA).catch(() => null);      // older firmware: no Bluetooth updates
    const pic = await svc.getCharacteristic(PIC).catch(() => null);      // older firmware: no pictures
    return { piece: 180, write: (b) => rx.writeValueWithoutResponse(b), sure: (b) => rx.writeValueWithResponse(b),
             ota: ota && ((b) => ota.writeValueWithResponse(b)), pic: pic && ((b) => pic.writeValueWithResponse(b)), close: () => d.gatt.disconnect() };
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
    // how big a piece the link takes (Android drops anything bigger): 20 bytes unless the phone got more
    const mtu = await BleClient.getMtu(dev.id).catch(() => 23);
    const svcs = await BleClient.getServices(dev.id).catch(() => []);
    const hasPic = svcs.some((sv) => sv.characteristics?.some((c) => c.uuid.toLowerCase() === PIC));
    return {
      piece: Math.max(20, Math.min(500, mtu - 3)),
      fast: (on) => BleClient.requestConnectionPriority(dev.id, on ? 1 : 0).catch(() => {}),   // HIGH while updating
      write: (b) => BleClient.writeWithoutResponse(dev.id, NUS, RX, new DataView(b.buffer, b.byteOffset, b.byteLength)),
      sure: (b) => BleClient.write(dev.id, NUS, RX, new DataView(b.buffer, b.byteOffset, b.byteLength)),
      ota: (b) => BleClient.write(dev.id, NUS, OTA, new DataView(b.buffer, b.byteOffset, b.byteLength)),
      pic: hasPic ? (b) => BleClient.write(dev.id, NUS, PIC, new DataView(b.buffer, b.byteOffset, b.byteLength)) : null,
      close: () => BleClient.disconnect(dev.id),
    };
  },
};
const T = native ? nat : web;

export function useKeychain(onEvent) {
  const [state, setState] = useState({ link: "off", name: "", screen: 0, count: 0, bri: 5, vol: 3, snd: true, names: [], fw: null });
  const waiters = useRef([]);                    // someone waiting for a line from the keychain (updates)
  const link = useRef(null), dev = useRef(null), buf = useRef(""), queue = useRef(Promise.resolve());
  const wanted = useRef(false);                  // stay connected: reconnect by itself if the link drops
  const evt = useRef(onEvent); evt.current = onEvent;

  function onLine(line) {
    for (const w of waiters.current.slice()) if (w.test(line)) { waiters.current.splice(waiters.current.indexOf(w), 1); w.done(line); }
    if (line.startsWith("F:")) {                 // F:<version>,ota
      const [version, ...rest] = line.slice(2).split(",");
      setState((s) => ({ ...s, fw: { version, ota: rest.includes("ota") } }));
    } else if (line.startsWith("K:")) {
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

  /** send one line (queued, split into BLE-sized pieces). The keychain confirms each piece and it's tried
   *  again if Android drops it (it does, when busy) - except `lossy` ones (the beat): a lost one doesn't matter. */
  const beatBusy = useRef(false);
  function send(line, lossy = false) {
    if (!link.current) return Promise.resolve(false);
    if (lossy) {                                  // the beat never queues up behind itself: skip one instead
      if (beatBusy.current) return Promise.resolve(false);
      beatBusy.current = true;
    }
    const data = new TextEncoder().encode(line + "\n");
    queue.current = queue.current.then(async () => {
      const l = link.current, n = l?.piece || 20;
      for (let i = 0; i < data.length; i += n) {
        const part = data.slice(i, i + n);
        if (lossy || !l.sure) { await l.write(part); continue; }
        try { await l.sure(part); } catch { await new Promise((r) => setTimeout(r, 60)); await link.current.sure(part); }
      }
      return true;
    }).catch(() => false).finally(() => { if (lossy) beatBusy.current = false; });
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
    setState((s) => ({ ...s, link: "connecting", name: d.name, fw: null }));
    link.current = await T.open(d, onData, onDrop);
    setState((s) => ({ ...s, link: "on", pic: !!link.current?.pic }));
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

  /** wait for a line from the keychain that starts with one of the prefixes */
  function waitLine(prefixes, ms) {
    return new Promise((res, rej) => {
      const w = { test: (l) => prefixes.some((p) => l.startsWith(p)), done: (l) => { clearTimeout(t); res(l); } };
      const t = setTimeout(() => { waiters.current.splice(waiters.current.indexOf(w), 1); rej(new Error("The keychain didn't answer")); }, ms);
      waiters.current.push(w);
    });
  }

  /** update the firmware over Bluetooth: bin = Uint8Array, md5 = hex; onProgress(0..1) */
  async function updateFirmware(bin, md5, onProgress) {
    const l = link.current;
    if (!l) throw new Error("Connect to the keychain first");
    if (!l.ota) throw new Error("This keychain's firmware can't update over Bluetooth yet: update it once with the USB cable");
    const ready = waitLine(["U:ready", "U:err"], 20000);
    await send(`U:${bin.length},${md5}`);
    const r = await ready;
    if (r.startsWith("U:err")) throw new Error("The keychain said: " + r.slice(6));
    const piece = Math.max(20, Math.min(l.piece || 20, +r.split(",")[1] || 20));   // what both ends can take
    l.fast?.(true);
    try {
      for (let i = 0; i < bin.length; i += piece) {
        if (!link.current) throw new Error("The connection dropped. Nothing changed: try again");
        await l.ota(bin.slice(i, i + piece));
        onProgress?.(Math.min(1, (i + piece) / bin.length));
      }
    } catch (e) { send("U:abort"); l.fast?.(false); throw e; }
    const ok = waitLine(["U:ok", "U:err"], 30000);
    await send("U:end");
    const fin = await ok;
    if (fin.startsWith("U:err")) throw new Error("The keychain said: " + fin.slice(6));
    // it restarts now on the new firmware, and the app reconnects by itself
  }

  /** My picture: send the changed spans [{ off, bytes }] (in link-sized pieces), in order with the lines */
  function sendPicture(spans) {
    const l = link.current;
    if (!l?.pic) return Promise.resolve(false);
    const n = Math.max(1, (l.piece || 20) - 2);
    queue.current = queue.current.then(async () => {
      for (const { off, bytes } of spans)
        for (let i = 0; i < bytes.length; i += n) {
          const part = bytes.subarray(i, i + n), o = off + i, pkt = new Uint8Array(part.length + 2);
          pkt[0] = o >> 8; pkt[1] = o & 255; pkt.set(part, 2);
          try { await link.current.pic(pkt); }
          catch { await new Promise((r) => setTimeout(r, 60)); await link.current.pic(pkt); }   // once more, then give up
        }
      return true;
    }).catch(() => false);
    return queue.current;
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
  return { ...state, lastName, connect, reconnect, disconnect, send, act, updateFirmware, sendPicture };
}
