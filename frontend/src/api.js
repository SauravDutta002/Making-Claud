// Talks to the bridge running on this computer. Every call carries the
// bridge's per-run token; the bridge only answers this computer.
// Served by the bridge (localhost:47801) or the dev server, the API is right
// here; on the hosted website, the page looks for the bridge on this computer.
import { useEffect, useRef, useState } from "react";

export const HOSTED = !import.meta.env.DEV && location.port !== "47801";
const BASE = HOSTED ? "http://127.0.0.1:47801/" : "";

let token = null;

async function getToken() {
  if (!token) {
    const r = await fetch(BASE + "api/token", { cache: "no-store" });
    if (!r.ok) throw new Error("bridge");
    token = (await r.json()).token;
  }
  return token;
}

async function call(path, body) {
  const t = await getToken();
  const opt = { cache: "no-store", headers: { "X-Buddy-Token": t } };
  if (body !== undefined) {
    opt.method = "POST";
    opt.headers["Content-Type"] = "application/json";
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(BASE + path, opt);
  if (r.status === 403) token = null;          // the bridge restarted: fetch a new token
  if (!r.ok) throw new Error(String(r.status));
  return r.json();
}

/** Send one line to the board (see the protocol at the top of claude.ino). */
export const sendLine = (line) => call("api/board", { line }).catch(() => {});
/** Change settings: { lyrics: false }, { lyrics_offset: 0.4 } ... */
export const saveSettings = (change) => call("api/settings", change).catch(() => {});

/** The bridge's live state, refreshed every second. null while it isn't answering. */
export function useBridge() {
  const [state, setState] = useState(null);
  const [down, setDown] = useState(false);
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    let timer;
    const tick = async () => {
      let wait = 1000;
      try {
        const s = await call("api/state");
        if (alive.current) { setState(s); setDown(false); }
      } catch {
        if (alive.current) setDown(true);
        wait = 3000;                           // not running: look again every few seconds
      }
      if (alive.current) timer = setTimeout(tick, wait);
    };
    tick();
    return () => { alive.current = false; clearTimeout(timer); };
  }, []);
  return { state, down };
}
