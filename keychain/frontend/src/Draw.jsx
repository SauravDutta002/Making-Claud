// The blackboard: draw with a finger (or the mouse) and the keychain shows it as
// you draw; or pick a photo from the gallery, turned into the screen's white dots.
// Only the bytes that changed travel over Bluetooth, so it keeps up live.
import React, { useEffect, useRef, useState } from "react";
import { W, H, Bits } from "./screens.js";

/** pixels (1 per byte) <-> the keychain's picture (1 bit per pixel, rows, high bit first) */
export function pack(px) {
  const b = new Uint8Array(1024);
  for (let p = 0; p < W * H; p++) if (px[p]) b[p >> 3] |= 0x80 >> (p & 7);
  return b;
}
const unpackInto = (d, b) => { if (b && b.length === 1024) d.frame(b); else d.clear(); };

/** the parts of `cur` that differ from `prev`, as [{ off, bytes }] (close ones merged) */
function changes(cur, prev) {
  if (!prev) return [{ off: 0, bytes: cur }];
  const out = [];
  let start = -1, lastDiff = -1;
  for (let i = 0; i < cur.length; i++) {
    if (cur[i] === prev[i]) continue;
    if (start >= 0 && i - lastDiff > 6) { out.push({ off: start, bytes: cur.subarray(start, lastDiff + 1) }); start = -1; }
    if (start < 0) start = i;
    lastDiff = i;
  }
  if (start >= 0) out.push({ off: start, bytes: cur.subarray(start, lastDiff + 1) });
  return out;
}

/** a photo -> white dots on black: fill/fit, auto levels, brightness, contrast, Atkinson dots */
function photoToPixels(img, o, d) {
  const c = document.createElement("canvas");
  c.width = W; c.height = H;
  const x = c.getContext("2d", { willReadFrequently: true });
  x.fillStyle = "#000"; x.fillRect(0, 0, W, H);
  const s = o.fit === "fill" ? Math.max(W / img.width, H / img.height) : Math.min(W / img.width, H / img.height);
  const w = img.width * s, h = img.height * s;
  x.imageSmoothingQuality = "high";
  x.drawImage(img, (W - w) / 2, (H - h) / 2, w, h);
  const data = x.getImageData(0, 0, W, H).data;
  const v = new Float32Array(W * H);
  for (let p = 0; p < W * H; p++) v[p] = 0.299 * data[p * 4] + 0.587 * data[p * 4 + 1] + 0.114 * data[p * 4 + 2];
  const sorted = Float32Array.from(v).sort();                 // stretch to the full range (2% .. 98%)
  const lo = sorted[Math.floor(v.length * 0.02)], hi = Math.max(lo + 1, sorted[Math.floor(v.length * 0.98)]);
  const k = 1 + o.contrast / 100;
  for (let p = 0; p < v.length; p++) {
    let y = ((v[p] - lo) / (hi - lo)) * 255;
    y = (y - 128) * k + 128 + o.bright * 1.6;
    if (o.invert) y = 255 - y;
    v[p] = y;
  }
  for (let y = 0; y < H; y++) for (let xx = 0; xx < W; xx++) {
    const p = y * W + xx, on = v[p] >= 128;
    d.b[p] = on ? 1 : 0;
    if (!o.dots) continue;
    const e = (v[p] - (on ? 255 : 0)) / 8;                     // Atkinson: 6/8 of the error to the neighbours
    const add = (dx, dy) => { const X = xx + dx, Y = y + dy; if (X >= 0 && X < W && Y < H) v[Y * W + X] += e; };
    add(1, 0); add(2, 0); add(-1, 1); add(0, 1); add(1, 1); add(0, 2);
  }
}

const SIZES = [1, 2, 4, 7];

export default function Draw({ kc, picture, setPicture }) {
  const canvas = useRef(null);
  const d = useRef(null);
  if (!d.current) { d.current = new Bits(); unpackInto(d.current, picture); }
  const [tool, setTool] = useState("pen");
  const [size, setSize] = useState(2);
  const [photo, setPhoto] = useState(null);                  // the picked image, while adjusting it
  const [opts, setOpts] = useState({ fit: "fill", contrast: 10, bright: 0, invert: false, dots: true });
  const sent = useRef(null), timer = useRef(0), saveTimer = useRef(0), last = useRef(null), down = useRef(false);
  const linked = kc.link === "on";
  const live = linked && kc.pic;

  const paint = () => { const c = canvas.current; if (!c) return; const ctx = c.getContext("2d"); d.current.paint(ctx, ctx.createImageData(W, H)); };
  useEffect(paint, []);
  useEffect(() => { sent.current = null; if (live) push(); }, [live]);   // (re)connected: send the whole picture once

  function push() {                                          // the changes since last time -> keychain (and the app)
    timer.current = 0;
    const b = pack(d.current.b);
    setPicture(b);
    if (!live) return;
    const ch = changes(b, sent.current);
    if (ch.length) kc.sendPicture(ch);
    sent.current = b;
  }
  function changed(keep = true) {
    paint();
    if (!timer.current) timer.current = setTimeout(push, 45);  // ~20 updates a second, whatever the finger does
    if (keep) { clearTimeout(saveTimer.current); saveTimer.current = setTimeout(() => live && kc.send("P:save"), 1500); }
  }

  function at(e) {
    const r = canvas.current.getBoundingClientRect();
    return [Math.floor(((e.clientX - r.left) / r.width) * W), Math.floor(((e.clientY - r.top) / r.height) * H)];
  }
  function stamp(x, y) {
    const c = tool === "pen" ? 1 : 0, r = (size - 1) / 2;
    if (size === 1) { d.current.px(x, y, c); return; }
    for (let j = -Math.ceil(r); j <= Math.ceil(r); j++)
      for (let i = -Math.ceil(r); i <= Math.ceil(r); i++) if (i * i + j * j <= r * r + 0.6) d.current.px(x + i, y + j, c);
  }
  function stroke(e) {
    const [x, y] = at(e), [lx, ly] = last.current || [x, y];
    const n = Math.max(Math.abs(x - lx), Math.abs(y - ly), 1);
    for (let i = 0; i <= n; i++) stamp(Math.round(lx + ((x - lx) * i) / n), Math.round(ly + ((y - ly) * i) / n));
    last.current = [x, y];
    changed();
  }
  const onDown = (e) => { e.preventDefault(); canvas.current.setPointerCapture(e.pointerId); down.current = true; last.current = null; stroke(e); };
  const onMove = (e) => { if (down.current) stroke(e); };
  const onUp = () => { down.current = false; last.current = null; };

  function clearAll() { d.current.clear(); setPhoto(null); changed(); }

  async function pickPhoto(e) {
    const f = e.target.files?.[0];
    e.target.value = "";
    if (!f) return;
    const url = URL.createObjectURL(f);
    const img = new Image();
    img.onload = () => { setPhoto(img); };
    img.src = url;
  }
  useEffect(() => { if (photo) { photoToPixels(photo, opts, d.current); changed(); } }, [photo, opts]);
  const set = (k) => (e) => setOpts((o) => ({ ...o, [k]: e.target.type === "checkbox" ? e.target.checked : e.target.type === "range" ? +e.target.value : e.target.value }));

  return (
    <section className="panel">
      <div className="panel-head"><h2>Draw</h2>{live && <span className="pill ok">Live on the keychain</span>}</div>
      <p className="lead">Draw on the board and it shows on the keychain as you draw. Or add a photo from your gallery.</p>
      <canvas ref={canvas} width={W} height={H} className="board" onPointerDown={onDown} onPointerMove={onMove}
              onPointerUp={onUp} onPointerCancel={onUp} aria-label="Drawing board" />
      <div className="board-tools">
        <div className="seg" role="group" aria-label="Tool">
          <button aria-pressed={tool === "pen"} onClick={() => setTool("pen")}>Pen</button>
          <button aria-pressed={tool === "erase"} onClick={() => setTool("erase")}>Eraser</button>
        </div>
        <div className="seg sizes" role="group" aria-label="Size">
          {SIZES.map((s) => <button key={s} aria-pressed={size === s} onClick={() => setSize(s)} aria-label={`Size ${s}`}><i style={{ width: 2 + s * 1.6, height: 2 + s * 1.6 }} /></button>)}
        </div>
        <span className="grow" />
        <label className="btn ghost sm">Add a photo<input type="file" accept="image/*" hidden onChange={pickPhoto} /></label>
        <button className="btn ghost sm" onClick={clearAll}>Clear</button>
      </div>
      {photo && (
        <div className="photo-opts">
          <div className="seg" role="group" aria-label="Fit">
            <button aria-pressed={opts.fit === "fill"} onClick={() => setOpts((o) => ({ ...o, fit: "fill" }))}>Fill</button>
            <button aria-pressed={opts.fit === "fit"} onClick={() => setOpts((o) => ({ ...o, fit: "fit" }))}>Whole photo</button>
          </div>
          <label>Brightness<input type="range" min="-60" max="60" value={opts.bright} onChange={set("bright")} /></label>
          <label>Contrast<input type="range" min="-50" max="120" value={opts.contrast} onChange={set("contrast")} /></label>
          <label className="check"><input type="checkbox" checked={opts.dots} onChange={set("dots")} />Dots (softer shading)</label>
          <label className="check"><input type="checkbox" checked={opts.invert} onChange={set("invert")} />Invert</label>
          <button className="btn primary sm" onClick={() => setPhoto(null)}>Done</button>
        </div>
      )}
      {!linked && <p className="hint">Connect to the keychain to see it there. The board keeps your drawing meanwhile.</p>}
      {linked && !kc.pic && <p className="hint">This keychain needs new firmware for drawing: update it in <a href="#settings">Settings &gt; Firmware</a>.</p>}
      {live && <p className="hint">It's saved on the keychain as <b>My picture</b>: tap to it any time, even after it sleeps.</p>}
    </section>
  );
}
