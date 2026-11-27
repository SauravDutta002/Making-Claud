// Keychain firmware updates. The newest firmware lives on the website
// (public/firmware/, made by tools/release_fw.py). Two ways onto the keychain:
//   - Bluetooth: the app sends it to the running keychain (firmware 2.0+)
//   - USB cable (Chrome / Edge on a computer): flashed like the Arduino IDE
//     does, so it also works on a keychain that has never been updated.
import { useEffect, useState } from "react";
import { Capacitor } from "@capacitor/core";

// in the Android app the files come from the website; on the web, from right here
const BASE = Capacitor.isNativePlatform() ? "https://keychain-flame.vercel.app/firmware/" : "./firmware/";
export const canUsb = typeof navigator !== "undefined" && "serial" in navigator;

/** "2.0.1" > "2.0.0" ? */
export function newer(a, b) {
  const pa = String(a || "0").split(".").map(Number), pb = String(b || "0").split(".").map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  return false;
}

/** the newest firmware on the website: { version, size, md5, built, files } | null */
export function useLatestFirmware() {
  const [m, setM] = useState(null);
  useEffect(() => {
    fetch(BASE + "manifest.json", { cache: "no-store" }).then((r) => (r.ok ? r.json() : null)).then(setM).catch(() => {});
  }, []);
  return m;
}

async function getFile(name) {
  const r = await fetch(BASE + name, { cache: "no-store" });
  if (!r.ok) throw new Error("Couldn't download the firmware");
  return new Uint8Array(await r.arrayBuffer());
}

/** the firmware itself, for a Bluetooth update */
export async function downloadFirmware(man) {
  const bin = await getFile("keychain.bin");
  if (bin.length !== man.size) throw new Error("The download was cut off: try again");
  return bin;
}

/** update with the USB cable: pick the port, flash every part, restart it. onProgress(0..1), onStep(text) */
export async function flashUsb(man, onProgress, onStep) {
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });   // Espressif (the ESP32-C3's own USB)
  onStep?.("Downloading the firmware…");
  const parts = await Promise.all(man.files.map(async (f) => ({ data: await getFile(f.file), address: f.address })));
  const total = parts.reduce((n, p) => n + p.data.length, 0);
  const { ESPLoader, Transport } = await import("esptool-js");
  const transport = new Transport(port, false);
  try {
    onStep?.("Connecting to the keychain…");
    const loader = new ESPLoader({ transport, baudrate: 460800, terminal: { clean() {}, writeLine() {}, write() {} } });
    await loader.main();
    onStep?.("Updating…");
    const before = parts.map((p, i) => parts.slice(0, i).reduce((n, q) => n + q.data.length, 0));
    await loader.writeFlash({
      fileArray: parts, flashMode: "keep", flashFreq: "keep", flashSize: "keep", eraseAll: false, compress: true,
      reportProgress: (i, written, size) => onProgress?.((before[i] + (written / size) * parts[i].data.length) / total),
    });
    onStep?.("Restarting…");
    await loader.after("hard_reset");
  } finally {
    await transport.disconnect().catch(() => {});
  }
}
