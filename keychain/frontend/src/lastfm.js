// What's playing, for free (no Spotify Premium needed): Spotify sends every song
// you play to Last.fm ("scrobbling"), and Last.fm says what you're listening to
// right now. Needs your Last.fm username and a free API key.
import { useEffect, useRef, useState } from "react";

const API = "https://ws.audioscrobbler.com/2.0/";
const get = (k) => { try { return localStorage.getItem(k) || ""; } catch { return ""; } };
const put = (k, v) => { try { localStorage.setItem(k, v); } catch {} };

export const savedLastfm = () => ({ user: get("lf-user"), key: get("lf-key") });
export function saveLastfm(user, key) { put("lf-user", user.trim()); put("lf-key", key.trim()); }
export function forgetLastfm() { put("lf-user", ""); put("lf-key", ""); }

async function call(params) {
  const r = await fetch(API + "?" + new URLSearchParams({ ...params, format: "json" }));
  const d = await r.json().catch(() => ({}));
  if (d.error) throw new Error(d.error === 10 ? "Last.fm doesn't know that API key." : d.error === 6 ? "Last.fm doesn't know that username." : "Last.fm: " + d.message);
  if (!r.ok) throw new Error(`Last.fm isn't answering (${r.status}).`);
  return d;
}

/** { linked, track: { title, artist, playing, pos, dur, art } | null, error }, refreshed every 8 s */
export function useLastfm(version) {
  const [st, setSt] = useState({ linked: false, track: null, error: "" });
  const since = useRef({ key: "", at: 0, dur: 0 });
  useEffect(() => {
    const { user, key } = savedLastfm();
    if (!user || !key) { setSt({ linked: false, track: null, error: "" }); return; }
    let alive = true;
    async function poll() {
      try {
        const d = await call({ method: "user.getrecenttracks", user, api_key: key, limit: 1 });
        if (!alive) return;
        const t = [].concat(d.recenttracks?.track || [])[0];
        const now = t && t["@attr"]?.nowplaying === "true";
        if (!now) { setSt({ linked: true, track: null, error: "" }); since.current.key = ""; return; }
        const title = t.name, artist = t.artist?.["#text"] || t.artist?.name || "";
        const id = title + "|" + artist;
        if (id !== since.current.key) {                       // a new song: when did it start, how long is it
          since.current = { key: id, at: Date.now(), dur: 0 };
          call({ method: "track.getInfo", api_key: key, artist, track: title })
            .then((i) => { since.current.dur = (+i.track?.duration || 0) / 1000; }).catch(() => {});
        }
        const art = (t.image || []).find((i) => i.size === "large")?.["#text"] || "";
        setSt({ linked: true, error: "",
                track: { title, artist, playing: true, art, pos: (Date.now() - since.current.at) / 1000, dur: since.current.dur } });
      } catch (e) { if (alive) setSt((s) => ({ ...s, linked: true, error: e.message })); }
    }
    poll();
    const iv = setInterval(poll, 8000);
    return () => { alive = false; clearInterval(iv); };
  }, [version]);
  return st;
}
