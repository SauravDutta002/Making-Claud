// The keychain's screens, exactly as the hardware plays them (screens.bin,
// made by tools/export_web.py from frames.h), plus the built-in record player.
export const W = 128, H = 64, FRAME = 1024;
export const PIXEL = [127, 233, 255];          // the OLED's colour, as in the case render

let loading = null;
/** Promise of [{ name, delays: number[], frames: Uint8Array[] }] */
export function loadScreens() {
  if (!loading) {
    loading = (async () => {
      const r = await fetch(new URL("screens.bin", document.baseURI));
      if (!r.ok || !("DecompressionStream" in window)) throw new Error("screens");
      const raw = new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
      let o = 3;
      const n = raw[o++], list = [];
      const dec = new TextDecoder();
      for (let i = 0; i < n; i++) {
        const len = raw[o++];
        const name = dec.decode(raw.slice(o, o + len)); o += len;
        const count = raw[o] | (raw[o + 1] << 8); o += 2;
        const delays = [];
        for (let k = 0; k < count; k++) { delays.push(raw[o] | (raw[o + 1] << 8)); o += 2; }
        list.push({ name, delays, count, frames: [] });
      }
      for (const s of list) {
        let prev = new Uint8Array(FRAME);
        for (let k = 0; k < s.count; k++) {
          const f = new Uint8Array(FRAME);
          for (let i = 0; i < FRAME; i++) f[i] = raw[o + i] ^ prev[i];
          o += FRAME; s.frames.push(f); prev = f;
        }
      }
      return list;
    })().catch((e) => { loading = null; throw e; });
  }
  return loading;
}

/** a 1-bit drawing surface, like the keychain's display */
export class Bits {
  constructor() { this.b = new Uint8Array(W * H); }
  clear() { this.b.fill(0); }
  px(x, y, c = 1) { x |= 0; y |= 0; if (x >= 0 && x < W && y >= 0 && y < H) this.b[y * W + x] = c; }
  fill(x, y, w, h, c = 1) { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) this.px(x + i, y + j, c); }
  frame(f) { for (let p = 0; p < W * H; p++) this.b[p] = f[p >> 3] & (0x80 >> (p & 7)) ? 1 : 0; }
  circle(cx, cy, r, fill, c = 1) {
    for (let y = -r; y <= r; y++) for (let x = -r; x <= r; x++) {
      const q = x * x + y * y;
      if (fill ? q <= r * r + r : q <= r * r + r && q >= (r - 1) * (r - 1) + (r - 1)) this.px(cx + x, cy + y, c);
    }
  }
  line(x0, y0, x1, y1, c = 1) {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1);
    for (let i = 0; i <= n; i++) this.px(Math.round(x0 + (x1 - x0) * i / n), Math.round(y0 + (y1 - y0) * i / n), c);
  }
  tri(ax, ay, bx, by, cx, cy) {
    const s = (px, py, x1, y1, x2, y2) => (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2);
    for (let y = Math.min(ay, by, cy); y <= Math.max(ay, by, cy); y++)
      for (let x = Math.min(ax, bx, cx); x <= Math.max(ax, bx, cx); x++) {
        const d1 = s(x, y, ax, ay, bx, by), d2 = s(x, y, bx, by, cx, cy), d3 = s(x, y, cx, cy, ax, ay);
        if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))) this.px(x, y);
      }
  }
  text(s, x, y) {
    for (const ch of s) {
      let c = ch.charCodeAt(0); if (c < 32 || c > 126) c = 63;
      for (let i = 0; i < 5; i++) {
        const col = parseInt(FONT.substr(((c - 32) * 5 + i) * 2, 2), 16);
        for (let j = 0; j < 8; j++) if (col & (1 << j)) this.px(x + i, y + j);
      }
      x += 6;
    }
  }
  paint(ctx, img) {
    for (let p = 0; p < W * H; p++) {
      const o = p * 4, on = this.b[p];
      img.data[o] = on ? PIXEL[0] : 0; img.data[o + 1] = on ? PIXEL[1] : 0; img.data[o + 2] = on ? PIXEL[2] : 0; img.data[o + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
  }
}

/** the record player, drawn like keychain.ino's drawPlayer() */
export function drawPlayer(d, f, song = "Lo-fi Nights - Snoopy", playing = true) {
  const RX = 64, ang = playing ? f * 0.07 : 0;
  let [title, artist = ""] = song.split(" - ");
  const w = title.length * 6;
  if (w <= 128 - RX) d.text(title, RX, 10);
  else { const span = w + 30, off = Math.floor(f / 3) % span; d.text(title, RX - off, 10); d.text(title, RX - off + span, 10); }
  if (artist.length > 10) artist = artist.slice(0, 8) + "..";
  d.text(artist, RX, 21); d.fill(0, 8, RX - 1, 12, 0);
  const pos = Math.floor(f * 0.025) % 180, len = 126 - RX, fw = Math.floor(len * pos / 180);
  for (let x = RX + fw; x < RX + len; x += 2) d.px(x, 35);
  d.fill(RX, 35, fw, 1); d.fill(RX + fw - 1, 34, 3, 3);
  const B = [72, 95, 117];
  d.fill(B[0] - 5, 47, 1, 7); d.tri(B[0], 47, B[0], 53, B[0] - 4, 50); d.tri(B[0] + 4, 47, B[0] + 4, 53, B[0], 50);
  if (playing) { d.fill(B[1] - 3, 47, 2, 7); d.fill(B[1] + 1, 47, 2, 7); } else d.tri(B[1] - 2, 46, B[1] - 2, 54, B[1] + 3, 50);
  d.tri(B[2] - 4, 47, B[2] - 4, 53, B[2], 50); d.tri(B[2], 47, B[2], 53, B[2] + 4, 50); d.fill(B[2] + 5, 47, 1, 7);
  const cx = 30, cy = 32;
  d.circle(cx, cy, 28, false); d.circle(cx, cy, 27, false);
  for (const r of [24, 20, 16]) {
    const n = Math.floor(2 * Math.PI * r), sh = Math.floor(ang * r);
    for (let k = 0; k < n; k++) { if (Math.floor((k + sh) / 3) % 3 === 0) continue; const a = k * 2 * Math.PI / n; d.px(cx + Math.round(Math.cos(a) * r), cy + Math.round(Math.sin(a) * r)); }
  }
  for (let r = 13; r <= 25; r++) { d.px(cx + Math.floor(r * 0.7), cy - Math.floor(r * 0.7)); d.px(cx - Math.floor(r * 0.7), cy + Math.floor(r * 0.7)); }
  d.circle(cx, cy, 10, true);
  for (let k = 0; k < 3; k++) {
    const a = ang + k * Math.PI / 3, dx = Math.round(Math.cos(a) * 7), dy = Math.round(Math.sin(a) * 7);
    d.line(cx - dx, cy - dy, cx + dx, cy + dy, 0); d.line(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, 0);
  }
  d.circle(cx, cy, 1, true); d.circle(60, 6, 3, true); d.circle(60, 6, 1, true, 0);
  const ex = playing ? 55 : 62, ey = playing ? 24 : 27, tx = playing ? 47 : 60, ty = playing ? 22 : 34;
  d.line(60, 8, ex, ey); d.line(61, 8, ex + 1, ey); d.line(ex, ey, tx, ty); d.fill(tx - 2, ty - 1, 4, 3);
}

// the display's own 5x7 font (Adafruit GFX "classic"), printable ASCII
const FONT = "000000000000005f00000007000700147f147f14242a7f2a12231308646236495620500008070300001c2241000041221c002a1c7f1c2a08083e080800807030000808080808000060600020100804023e5149453e00427f400072494949462141494d331814127f1027454545393c4a49493141211109073649494936464949291e0000140000004034000000081422411414141414004122140802015909063e415d594e7c1211127c7f494949363e414141227f4141413e7f494949417f090909013e414151737f0808087f00417f41002040413f017f081422417f404040407f021c027f7f0408107f3e4141413e7f090909063e4151215e7f09192946264949493203017f01033f4040403f1f2040201f3f4038403f631408146303047804036159494d43007f4141410204081020004141417f04020102044040404040000307080020545478407f284444383844444428384444287f385454541800087e090218a4a49c787f0804047800447d40002040403d007f1028440000417f40007c047804787c080404783844444438fc1824241818242418fc7c08040408485454542404043f44243c4040207c1c2040201c3c4030403c44281028444c9090907c4464544c440008364100000077000000413608000201020402";
