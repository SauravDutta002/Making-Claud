// The first thing you see after installing: pixel Claude builds itself, the
// headphones drop on, it says hi, checks everything is wired up, offers to
// install the app, then flies up into the logo and the dashboard opens.
// Shown when the page is opened as /?welcome (the installer does that).
import React, { useEffect, useRef, useState } from "react";
import { LOGO_BODY, LOGO_PHONES } from "./logo.js";

const still = () => matchMedia("(prefers-reduced-motion: reduce)").matches;

/** "M3 4h10v1h-10z..." -> every lit pixel as [x, y] */
function pixels(path) {
  const out = [];
  for (const m of path.matchAll(/M(\d+) (\d+)h(\d+)/g)) {
    const x = +m[1], y = +m[2], w = +m[3];
    for (let i = 0; i < w; i++) out.push([x + i, y]);
  }
  return out;
}
const BODY = pixels(LOGO_BODY), PHONES = pixels(LOGO_PHONES);

/** show the welcome? (?welcome in the address); done() also tidies the address */
export function useWelcome() {
  const [on, setOn] = useState(() => new URLSearchParams(location.search).has("welcome"));
  const done = () => {
    setOn(false);
    const u = new URL(location.href);
    u.searchParams.delete("welcome");
    history.replaceState(null, "", u.pathname + u.search + u.hash);
  };
  return [on, done, () => setOn(true)];
}

function Check({ state, children, sub }) {
  return (
    <li className={"wl-check " + state}>
      <i aria-hidden="true">{state === "ok" ? "✓" : state === "warn" ? "!" : ""}</i>
      <span>{children}{sub && <small>{sub}</small>}</span>
    </li>
  );
}

const HELLO = "Hi, I'm Claude Buddy.";

export default function Welcome({ s, linked, install, onHello, onDone }) {
  const [t, setT] = useState(still() ? 9 : 0);     // the beat we're on
  const [typed, setTyped] = useState(still() ? HELLO.length : 0);
  const [leaving, setLeaving] = useState(false);
  const art = useRef(null);
  const asked = useRef(false);

  // the beats: 1 headphones, 2 hello, 3-5 checks, 6 the app offer
  useEffect(() => {
    if (t >= 9) return;
    const at = [1300, 2000, 3300, 3800, 4300, 5000];
    const ids = at.map((ms, i) => setTimeout(() => setT((v) => Math.max(v, i + 1)), ms));
    return () => ids.forEach(clearTimeout);
  }, []);
  useEffect(() => {
    if (t < 2 || typed >= HELLO.length) return;
    const id = setTimeout(() => setTyped(typed + 1), 45);
    return () => clearTimeout(id);
  }, [t, typed]);

  // the board says hi too, the moment it's here
  useEffect(() => {
    if (t >= 5 && linked && !asked.current) { asked.current = true; onHello && onHello(); }
  }, [t, linked]);

  useEffect(() => {
    const k = (e) => e.key === "Escape" && leave();
    addEventListener("keydown", k);
    return () => removeEventListener("keydown", k);
  });

  // fly into the header logo, fade the curtain, then hand over to the page
  function leave() {
    if (leaving) return;
    const to = document.querySelector(".top .logo .mascot");
    const from = art.current;
    if (to && from && !still()) {
      const a = from.getBoundingClientRect(), b = to.getBoundingClientRect();
      from.style.setProperty("--fx", `${b.left + b.width / 2 - (a.left + a.width / 2)}px`);
      from.style.setProperty("--fy", `${b.top + b.height / 2 - (a.top + a.height / 2)}px`);
      from.style.setProperty("--fs", `${b.width / a.width}`);
    }
    setLeaving(true);
    document.documentElement.classList.add("entering");
    setTimeout(onDone, still() ? 0 : 900);
    setTimeout(() => document.documentElement.classList.remove("entering"), 2200);
  }

  const standalone = matchMedia("(display-mode: standalone)").matches;
  const hooks = !s ? "wait" : s.hooks === false ? "warn" : "ok";
  const where = !s ? "" : s.link === "usb" ? `over USB on ${s.port}` : "over Bluetooth";

  return (
    <div className={"welcome" + (leaving ? " out" : "")} role="dialog" aria-modal="true" aria-label="Welcome to Claude Buddy">
      <div className="wl-glow" aria-hidden="true" />
      <div className="wl-body">
        <div className={"wl-art" + (t >= 1 ? " phones" : "") + (t >= 2 ? " alive" : "")} ref={art} aria-hidden="true">
          <svg viewBox="-1 -3 18 16" shapeRendering="crispEdges">
            <g className="wl-claude">
              {BODY.map(([x, y], i) => (
                <rect key={i} x={x} y={y} width="1.02" height="1.02" fill="var(--accent)"
                      style={{ "--d": `${(y - 4) * 70 + ((x * 37) % 11) * 28}ms`, "--r": `${((x * 53 + y * 17) % 7) - 3}` }} />
              ))}
            </g>
            <g className="wl-phones" fill="currentColor">
              {PHONES.map(([x, y], i) => <rect key={i} x={x} y={y} width="1.02" height="1.02" />)}
            </g>
            <g className="wl-notes" fill="var(--accent)">
              <path d="M-1 1h1v3h-1zM0 1h1v1h-1zM-2 3h1v1h-1z" />
              <path d="M16 0h1v3h-1zM17 0h1v1h-1zM15 2h1v1h-1z" />
            </g>
          </svg>
          <i className="wl-shadow" />
        </div>

        <h1 className="wl-hello" aria-label={HELLO}>
          {HELLO.slice(0, typed)}<span className={"wl-caret" + (typed >= HELLO.length ? " idle" : "")} aria-hidden="true" />
        </h1>
        <p className={"wl-sub" + (t >= 2 ? " on" : "")}>Your little desk companion for Claude Code. Let's make sure everything's ready.</p>

        <ul className="wl-checks">
          {t >= 3 && <Check state={s ? "ok" : "wait"}>Installed, and running in the background</Check>}
          {t >= 4 && (
            <Check state={hooks} sub={hooks === "warn" ? "Run claude-buddy-setup, then restart Claude Code" : "Restart any open Claude Code windows to wake it up"}>
              {hooks === "warn" ? "Not hooked into Claude Code yet" : "Hooked into Claude Code"}
            </Check>
          )}
          {t >= 5 && (
            <Check state={linked ? "ok" : "wait"} sub={linked ? "It just said hi on its screen" : "Plug it in with USB, or switch it on for Bluetooth"}>
              {linked ? `Found your buddy ${where}` : "Looking for your buddy…"}
            </Check>
          )}
        </ul>

        <div className={"wl-cta" + (t >= 6 ? " on" : "")}>
          {install && !standalone ? (
            <>
              <div className="wl-app">
                <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="14" rx="2" /><path d="M8 21h8M12 18v3" /></svg>
                <span><b>Get the app</b><small>Its own window and a Start menu icon, one click away.</small></span>
              </div>
              <div className="wl-buttons">
                <button className="btn primary" onClick={async () => { await install(); leave(); }}>Install the app</button>
                <button className="btn ghost" onClick={leave}>Maybe later</button>
              </div>
            </>
          ) : (
            <div className="wl-buttons"><button className="btn primary" onClick={leave}>Let's go</button></div>
          )}
        </div>
      </div>
      <button className="wl-skip" onClick={leave}>Skip</button>
    </div>
  );
}
