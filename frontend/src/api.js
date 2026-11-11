// Talks to the bridge running on this computer. Every call carries the
// bridge's per-run token; the bridge only answers this computer.
import { useEffect, useRef, useState } from "react";

let token = null;

async function getToken() {
  if (!token) {
    const r = await fetch("api/token", { cache: "no-store" });
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
  const r = await fetch(path, opt);
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
      try {
        const s = await call("api/state");
        if (alive.current) { setState(s); setDown(false); }
      } catch {
        if (alive.current) setDown(true);
      }
      if (alive.current) timer = setTimeout(tick, 1000);
    };
    tick();
    return () => { alive.current = false; clearTimeout(timer); };
  }, []);
  return { state, down };
}
