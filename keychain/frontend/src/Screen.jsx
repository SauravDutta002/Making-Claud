// A live keychain screen on a canvas: one of the screens from screens.bin,
// or the record player. `player` = { song, playing } for the record player.
import React, { useEffect, useRef } from "react";
import { W, H, Bits, loadScreens, drawPlayer } from "./screens.js";

export default function Screen({ index, player, picture, className = "oled", still = false }) {
  const ref = useRef(null);
  const want = useRef({ index, player, picture });
  want.current = { index, player, picture };

  useEffect(() => {
    let raf = 0, alive = true, last = 0, k = 0, f = 0, shown = -2, acc = 0, seen = true;
    const canvas = ref.current, ctx = canvas.getContext("2d"), img = ctx.createImageData(W, H), d = new Bits();
    const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches || still;
    const io = new IntersectionObserver(([e]) => { seen = e.isIntersecting; if (seen && !raf) raf = requestAnimationFrame(draw); });
    io.observe(canvas);
    let list = null;
    loadScreens().then((l) => { list = l; }).catch(() => {});
    function draw(t) {
      if (!alive || !seen) { raf = 0; return; }
      const dt = Math.min(200, t - (last || t)); last = t;
      const { index: i, player: p, picture: pic } = want.current;
      if (i !== shown) { shown = i; k = 0; f = 0; acc = 0; }
      d.clear();
      if (pic) d.frame(pic);
      else if (p) { if (!reduce) f += dt / 25; drawPlayer(d, Math.floor(f), p.song, p.playing); }
      else if (list && list[i]) {
        const s = list[i];
        if (!reduce) { acc += dt; let ms = s.delays[k] || 70; while (acc >= ms) { acc -= ms; k = (k + 1) % s.count; ms = s.delays[k] || 70; } }
        else k = Math.min(40, s.count - 1);
        d.frame(s.frames[k]);
      }
      d.paint(ctx, img);
      raf = requestAnimationFrame(draw);
    }
    raf = requestAnimationFrame(draw);
    return () => { alive = false; io.disconnect(); cancelAnimationFrame(raf); };
  }, [still]);

  return <canvas ref={ref} width={W} height={H} className={className} aria-hidden="true" />;
}
