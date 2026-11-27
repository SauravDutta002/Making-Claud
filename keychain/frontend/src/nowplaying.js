// What the phone is playing, from ANY app (YouTube, YouTube Music, Spotify...):
// only in the Android app (NowPlayingPlugin.java reads Android's media sessions,
// like the lock-screen music controls). Needs Notification access once.
import { useEffect, useState } from "react";
import { Capacitor, registerPlugin } from "@capacitor/core";

export const isNativeApp = Capacitor.isNativePlatform();
const NowPlaying = isNativeApp ? registerPlugin("NowPlaying") : null;

export function openNotificationAccess() { return NowPlaying?.openSettings(); }

/** { native, granted, track: { title, artist, playing, pos, dur, app } | null } */
export function useNowPlaying() {
  const [st, setSt] = useState({ native: isNativeApp, granted: false, track: null });
  useEffect(() => {
    if (!NowPlaying) return;
    let alive = true;
    const take = (d) => {
      if (!alive || !d) return;
      setSt({
        native: true, granted: !!d.granted,
        track: d.active ? { title: d.title, artist: d.artist, playing: !!d.playing, pos: d.position || 0, dur: d.duration || 0, app: d.app, art: "" } : null,
      });
    };
    const sub = NowPlaying.addListener("change", take);
    NowPlaying.current().then(take).catch(() => {});
    const iv = setInterval(() => NowPlaying.current().then(take).catch(() => {}), 4000);   // keeps the position fresh
    return () => { alive = false; clearInterval(iv); Promise.resolve(sub).then((s) => s?.remove?.()); };
  }, []);
  return st;
}
