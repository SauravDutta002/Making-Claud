// What's playing, from Spotify. A web page can't see other apps' music, so this
// reads your Spotify "currently playing" (sign in once; PKCE, no server needed).
import { useEffect, useRef, useState } from "react";

const AUTH = "https://accounts.spotify.com/authorize";
const TOKEN = "https://accounts.spotify.com/api/token";
const redirect = () => location.origin + location.pathname;
const b64url = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const get = (k) => { try { return localStorage.getItem(k) || ""; } catch { return ""; } };
const put = (k, v) => { try { localStorage.setItem(k, v); } catch {} };

export function spotifyRedirect() { return redirect(); }

export async function spotifyLogin(clientId) {
  put("sp-client", clientId);
  const verifier = b64url(crypto.getRandomValues(new Uint8Array(48)));
  put("sp-verifier", verifier);
  const challenge = b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)));
  location.href = AUTH + "?" + new URLSearchParams({
    client_id: clientId, response_type: "code", redirect_uri: redirect(), code_challenge_method: "S256",
    code_challenge: challenge, scope: "user-read-currently-playing user-read-playback-state",
  });
}

export function spotifyLogout() { ["sp-token", "sp-refresh", "sp-expires"].forEach((k) => put(k, "")); }

async function tokenRequest(body) {
  const r = await fetch(TOKEN, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(body) });
  if (!r.ok) throw new Error("Spotify sign-in failed");
  const t = await r.json();
  put("sp-token", t.access_token);
  if (t.refresh_token) put("sp-refresh", t.refresh_token);
  put("sp-expires", String(Date.now() + (t.expires_in - 60) * 1000));
}

async function token() {
  if (get("sp-token") && Date.now() < +get("sp-expires")) return get("sp-token");
  if (!get("sp-refresh")) return null;
  await tokenRequest({ grant_type: "refresh_token", refresh_token: get("sp-refresh"), client_id: get("sp-client") });
  return get("sp-token");
}

/** { linked, track: { title, artist, playing, pos, dur, art } | null } refreshed every 5 s */
export function useSpotify() {
  const [st, setSt] = useState({ linked: !!get("sp-refresh"), track: null, error: "" });
  const busy = useRef(false);
  useEffect(() => {
    let alive = true;
    (async () => {
      const code = new URLSearchParams(location.search).get("code");
      if (code) {
        try { await tokenRequest({ grant_type: "authorization_code", code, redirect_uri: redirect(), client_id: get("sp-client"), code_verifier: get("sp-verifier") }); }
        catch (e) { setSt((s) => ({ ...s, error: e.message })); }
        history.replaceState(null, "", redirect());
      }
      poll();
    })();
    async function poll() {
      if (busy.current) return;
      busy.current = true;
      try {
        const tok = await token();
        if (!alive) return;
        if (!tok) { setSt((s) => ({ ...s, linked: false })); return; }
        const r = await fetch("https://api.spotify.com/v1/me/player/currently-playing", { headers: { Authorization: "Bearer " + tok } });
        if (r.status === 204) { setSt({ linked: true, track: null, error: "" }); return; }
        if (!r.ok) return;
        const d = await r.json();
        if (!d.item) return;
        setSt({
          linked: true, error: "",
          track: { title: d.item.name, artist: (d.item.artists || []).map((a) => a.name).join(", "), playing: d.is_playing,
                   pos: d.progress_ms / 1000, dur: d.item.duration_ms / 1000, art: d.item.album?.images?.[1]?.url || "" },
        });
      } catch (e) { setSt((s) => ({ ...s, error: e.message })); }
      finally { busy.current = false; }
    }
    const id = setInterval(poll, 5000);
    return () => { alive = false; clearInterval(id); };
  }, []);
  return st;
}

export const savedClientId = () => get("sp-client");
