// What's playing, from your Spotify account: whatever the Spotify app (phone,
// PC, speaker) is playing. Sign in once (PKCE, no server). The sign-in opens in
// a small pop-up so this page - and its Bluetooth link - never goes away.
import { useEffect, useRef, useState } from "react";

const AUTH = "https://accounts.spotify.com/authorize";
const TOKEN = "https://accounts.spotify.com/api/token";
const redirect = () => location.origin + location.pathname;
const b64url = (buf) => btoa(String.fromCharCode(...new Uint8Array(buf))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const get = (k) => { try { return localStorage.getItem(k) || ""; } catch { return ""; } };
const put = (k, v) => { try { localStorage.setItem(k, v); } catch {} };

export function spotifyRedirect() { return redirect(); }
export const savedClientId = () => get("sp-client");

// --- the pop-up side: Spotify sends the browser back here with ?code= (or ?error=)
const params = new URLSearchParams(location.search);
const inPopup = !!window.opener && (params.has("code") || params.has("error"));
if (inPopup) {
  try { window.opener.postMessage({ type: "spotify", code: params.get("code"), error: params.get("error") }, location.origin); } catch {}
  setTimeout(() => window.close(), 50);
}
export const isSpotifyPopup = inPopup;

export async function spotifyLogin(clientId) {
  put("sp-client", clientId);
  const verifier = b64url(crypto.getRandomValues(new Uint8Array(48)));
  put("sp-verifier", verifier);
  const challenge = b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)));
  const url = AUTH + "?" + new URLSearchParams({
    client_id: clientId, response_type: "code", redirect_uri: redirect(), code_challenge_method: "S256",
    code_challenge: challenge, scope: "user-read-currently-playing user-read-playback-state", show_dialog: "false",
  });
  const w = window.open(url, "spotify-login", "width=480,height=720");
  if (!w) location.href = url;                    // pop-ups blocked: go the long way round (Bluetooth reconnects after)
}

export function spotifyLogout() { ["sp-token", "sp-refresh", "sp-expires"].forEach((k) => put(k, "")); }

async function tokenRequest(body) {
  const r = await fetch(TOKEN, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(body) });
  const t = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(explain(t.error, t.error_description));
  put("sp-token", t.access_token);
  if (t.refresh_token) put("sp-refresh", t.refresh_token);
  put("sp-expires", String(Date.now() + (t.expires_in - 60) * 1000));
}

// Spotify's error codes, in plain words with what to do
function explain(code, desc = "") {
  const d = (desc || "").toLowerCase();
  if (code === "invalid_client" || d.includes("redirect")) return `Spotify says the Redirect URI doesn't match. In your Spotify app's settings add exactly: ${redirect()}`;
  if (code === "access_denied") return "Spotify sign-in was cancelled.";
  if (code === "invalid_grant") return "That sign-in expired. Tap Connect Spotify again.";
  if (d.includes("client")) return "Spotify doesn't know that Client ID. Copy it again from your Spotify app's settings.";
  return `Spotify: ${desc || code || "sign-in failed"}`;
}

async function token() {
  if (get("sp-token") && Date.now() < +get("sp-expires")) return get("sp-token");
  if (!get("sp-refresh")) return null;
  await tokenRequest({ grant_type: "refresh_token", refresh_token: get("sp-refresh"), client_id: get("sp-client") });
  return get("sp-token");
}

/** { linked, track, error } - refreshed every 5 s */
export function useSpotify() {
  const [st, setSt] = useState({ linked: !!get("sp-refresh"), track: null, error: "" });
  const busy = useRef(false);
  useEffect(() => {
    if (inPopup) return;
    let alive = true;

    async function finish(code, error) {
      if (error) { setSt((s) => ({ ...s, error: explain(error) })); return; }
      if (!code) return;
      try {
        await tokenRequest({ grant_type: "authorization_code", code, redirect_uri: redirect(), client_id: get("sp-client"), code_verifier: get("sp-verifier") });
        setSt((s) => ({ ...s, linked: true, error: "" }));
        poll();
      } catch (e) { setSt((s) => ({ ...s, error: e.message })); }
    }
    // the pop-up hands the result back here
    const onMsg = (e) => { if (e.origin === location.origin && e.data?.type === "spotify") finish(e.data.code, e.data.error); };
    addEventListener("message", onMsg);
    // (or Spotify came back to this page itself, if the pop-up was blocked)
    if (params.has("code") || params.has("error")) { finish(params.get("code"), params.get("error")); history.replaceState(null, "", redirect()); }

    async function poll() {
      if (busy.current) return;
      busy.current = true;
      try {
        const tok = await token();
        if (!alive) return;
        if (!tok) { setSt((s) => ({ ...s, linked: false })); return; }
        const r = await fetch("https://api.spotify.com/v1/me/player/currently-playing", { headers: { Authorization: "Bearer " + tok } });
        if (r.status === 204) { setSt({ linked: true, track: null, error: "" }); return; }
        if (r.status === 401) { put("sp-expires", "0"); return; }
        if (r.status === 403) {
          const t = await r.text();
          setSt((s) => ({ ...s, linked: true, error: /registered|developer/i.test(t)
            ? "Spotify: your account isn't allowed on this Spotify app yet. In the Spotify dashboard open your app > User Management and add your Spotify email."
            : "Spotify refused (403): " + t.slice(0, 120) }));
          return;
        }
        if (!r.ok) { setSt((s) => ({ ...s, error: `Spotify isn't answering (${r.status}).` })); return; }
        const d = await r.json();
        if (!d.item) { setSt({ linked: true, track: null, error: "" }); return; }
        setSt({
          linked: true, error: "",
          track: { title: d.item.name, artist: (d.item.artists || []).map((a) => a.name).join(", "), playing: d.is_playing,
                   pos: d.progress_ms / 1000, dur: d.item.duration_ms / 1000, art: d.item.album?.images?.[1]?.url || "" },
        });
      } catch (e) { setSt((s) => ({ ...s, error: e.message })); }
      finally { busy.current = false; }
    }
    poll();
    const id = setInterval(poll, 5000);
    return () => { alive = false; clearInterval(id); removeEventListener("message", onMsg); };
  }, []);
  return st;
}
