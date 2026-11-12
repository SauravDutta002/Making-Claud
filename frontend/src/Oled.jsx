// The board's real screen: every face recorded from the firmware's own drawing
// code. faces.bin (gzipped): "BF1", face count, then per face (id, frames);
// then the frames, each a 1-bit "lit" plane and a "this pixel is Claude" plane,
// XOR'd against the face's previous frame. Played at 20 fps.
import React, { useEffect, useRef } from "react";

const W = 128, H = 64, PLANE = 1024, FRAME = 2 * PLANE;
const LIT = [234, 244, 255], CLAUDE = [217, 119, 87];

let facesP = null;
/** Promise of Map<faceId, Uint8Array[]> */
export function loadFaces() {
  if (!facesP) {
    facesP = (async () => {
      const r = await fetch(new URL("faces.bin", document.baseURI));
      if (!r.ok || !("DecompressionStream" in window)) throw new Error("faces");
      const raw = new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
      const n = raw[3], map = new Map();
      let off = 4 + n * 3;
      for (let j = 0; j < n; j++) {
        const id = raw[4 + j * 3], count = raw[5 + j * 3] | (raw[6 + j * 3] << 8);
        const list = [];
        let prev = new Uint8Array(FRAME);
        for (let k = 0; k < count; k++) {
          const f = new Uint8Array(FRAME);
          for (let i = 0; i < FRAME; i++) f[i] = raw[off + i] ^ prev[i];
          off += FRAME; list.push(f); prev = f;
        }
        map.set(id, list);
      }
      return map;
    })().catch((e) => { facesP = null; throw e; });
  }
  return facesP;
}

/** paint one recorded frame into a 128x64 ImageData */
export function paint(img, f) {
  for (let p = 0; p < W * H; p++) {
    const bit = 0x80 >> (p & 7), o = p * 4;
    const c = f[p >> 3] & bit ? (f[PLANE + (p >> 3)] & bit ? CLAUDE : LIT) : null;
    img.data[o] = c ? c[0] : 0; img.data[o + 1] = c ? c[1] : 0; img.data[o + 2] = c ? c[2] : 0; img.data[o + 3] = 255;
  }
}

/** face: a face id (69 night sky, 70 chilling...); dim: the board isn't connected */
export default function Oled({ face = 69, dim = false }) {
  const ref = useRef(null);
  const faceRef = useRef(face);
  faceRef.current = face;

  useEffect(() => {
    let raf, alive = true, k = 0, last = 0, shown = -1;
    const ctx = ref.current.getContext("2d");
    const img = ctx.createImageData(W, H);
    const still = matchMedia("(prefers-reduced-motion: reduce)").matches;
    loadFaces().then((faces) => {
      const draw = (t) => {
        if (!alive) return;
        if (t - last >= 50) {
          last = t;
          if (faceRef.current !== shown) { shown = faceRef.current; k = still ? 40 : 0; }
          const list = faces.get(shown) || faces.get(69);
          if (list) { paint(img, list[k % list.length]); ctx.putImageData(img, 0, 0); }
          if (!still) k++;
        }
        raf = requestAnimationFrame(draw);
      };
      raf = requestAnimationFrame(draw);
    }).catch(() => {});
    return () => { alive = false; cancelAnimationFrame(raf); };
  }, []);

  return <canvas ref={ref} width={W} height={H} className={"oled" + (dim ? " dim" : "")} aria-hidden="true" />;
}

/** a still of one face, for the picker */
export function Thumb({ face }) {
  const ref = useRef(null);
  // plays the face's recording (10 fps) while it's on screen; a still frame otherwise
  useEffect(() => {
    let alive = true, raf = 0, last = 0, k = 0, seen = false, list = null;
    const canvas = ref.current;
    const ctx = canvas.getContext("2d");
    const img = ctx.createImageData(W, H);
    const still = matchMedia("(prefers-reduced-motion: reduce)").matches;
    const draw = (t) => {
      if (!alive || !seen || !list) { raf = 0; return; }
      if (t - last >= 100) { last = t; paint(img, list[k % list.length]); ctx.putImageData(img, 0, 0); k += 2; }
      raf = requestAnimationFrame(draw);
    };
    const io = new IntersectionObserver(([e]) => {
      seen = e.isIntersecting;
      if (seen && !raf && !still) raf = requestAnimationFrame(draw);
    });
    loadFaces().then((faces) => {
      list = faces.get(face);
      if (!alive || !list) return;
      k = Math.min(40, list.length - 1);
      paint(img, list[k]); ctx.putImageData(img, 0, 0);
      io.observe(canvas);
    }).catch(() => {});
    return () => { alive = false; io.disconnect(); cancelAnimationFrame(raf); };
  }, [face]);
  return <canvas ref={ref} width={W} height={H} className="thumb" aria-hidden="true" />;
}
