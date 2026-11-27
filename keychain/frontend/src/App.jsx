import React, { useEffect, useRef, useState } from "react";
import Screen from "./Screen.jsx";
import Draw from "./Draw.jsx";
import { loadScreens } from "./screens.js";
import { useKeychain, canBluetooth } from "./ble.js";
import { useSpotify, spotifyLogin, spotifyLogout, spotifyRedirect, savedClientId } from "./spotify.js";
import { useLastfm, savedLastfm, saveLastfm, forgetLastfm } from "./lastfm.js";
import { useNowPlaying, useBeat, openNotificationAccess, isNativeApp } from "./nowplaying.js";
import { useLatestFirmware, downloadFirmware, flashUsb, newer, canUsb } from "./firmware.js";

const TABS = [["screens", "Screens"], ["draw", "Draw"], ["music", "Now playing"], ["settings", "Settings"], ["help", "Help"]];
const PLAYER = "Record player";
const PICTURE = "My picture";
const DOCK_NAMES = { music: "Music" };       // short names for the bottom bar on phones

// the picture on the board, kept on this device too
function loadPicture() {
  try { const t = localStorage.getItem("kc-picture"); return t ? Uint8Array.from(atob(t), (c) => c.charCodeAt(0)) : null; } catch { return null; }
}
// the Android app: built by GitHub on every push (.github/workflows/keychain-android.yml)
const ANDROID_APP = "https://github.com/SauravDutta002/Making-Claud/releases/download/keychain-android/KeychainRemote.apk";
const ANDROID_PAGE = "https://github.com/SauravDutta002/Making-Claud/releases/tag/keychain-android";

function GetAndroid() {                      // only on the web: the app itself doesn't need it
  if (isNativeApp) return null;
  return (
    <section className="panel android-cta">
      <div className="panel-head"><h2>Get the Android app</h2></div>
      <p className="lead">It shows what's playing in <b>YouTube, YouTube Music, Spotify</b> or any app, and stays connected to your keychain by itself.</p>
      <div className="actions">
        <a className="btn primary" href={ANDROID_APP} download>Download for Android</a>
        <a className="btn link" href={ANDROID_PAGE} target="_blank" rel="noopener">Release page</a>
      </div>
      <ol className="steps-list">
        <li><span>Open this page in <b>Chrome</b> (not the GitHub app) and tap <b>Download for Android</b>.</span></li>
        <li><span>When it's downloaded, open <b>Files &gt; Downloads &gt; KeychainRemote.apk</b> and tap <b>Install</b>.</span></li>
        <li><span>If Android asks, allow installing from <b>Files</b>. If Play Protect warns, tap <b>Install anyway</b>.</span></li>
      </ol>
    </section>
  );
}

// in the Android app: is a newer build on GitHub? (release notes say "build N")
// the Android app: which build this is, the newest on GitHub, and 0 / the newer one
function useAppUpdate(tick = 0) {
  const [newest, setNewest] = useState(0);
  useEffect(() => {
    if (!isNativeApp || !__BUILD__) return;
    fetch("https://api.github.com/repos/SauravDutta002/Making-Claud/releases/tags/keychain-android", { cache: "no-store" })
      .then((r) => r.json()).then((rel) => setNewest(+((rel.body || "").match(/build (\d+)/) || [])[1] || 0))
      .catch(() => {});
  }, [tick]);
  return { build: __BUILD__, newest, next: newest > __BUILD__ ? newest : 0 };
}

function Versions({ kc, fw, app, onCheck }) {
  return (
    <section className="panel">
      <div className="panel-head"><h2>Versions</h2>{!app.next && !fw.update && (app.newest || fw.known) && <span className="pill ok">Up to date</span>}</div>
      <Row title="This app" hint={isNativeApp ? (app.newest ? `Newest: build ${app.newest}` : "Checking…") : "The website: always the newest"}>
        <span className="mono">{isNativeApp ? `build ${app.build || "dev"}` : "web"}</span></Row>
      <Row title="Keychain firmware" hint={fw.latest ? `Newest: ${fw.latest.version}` : "Checking…"}>
        <span className="mono">{fw.version || (kc.link === "on" ? "…" : "connect to see")}</span></Row>
      <div className="actions"><button className="btn ghost sm" onClick={onCheck}>Check for updates</button></div>
    </section>
  );
}

// the pop-up: what's new (the app, the keychain's firmware), with the buttons right there
function UpdateDialog({ appNext, fw, onClose }) {
  const fwBusy = fw.job && !fw.job.err;
  return (
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="upd-title" onClick={(e) => e.target === e.currentTarget && !fwBusy && onClose()}>
      <div className="modal-card">
        <span className="eyebrow ok">● New</span>
        <h2 id="upd-title">Update available</h2>
        {!!appNext && (
          <div className="upd-row">
            <div><b>The app</b><small>A newer version is out (build {appNext}). Install it over this one: your settings stay.</small></div>
            <a className="btn primary sm" href={ANDROID_APP} target="_blank" rel="noopener">Download</a>
          </div>
        )}
        {fw.update && (
          <div className="upd-row">
            <div><b>Keychain firmware {fw.latest.version}</b>
              <small>{fwBusy ? fw.job.step : fw.canBle ? `It's on ${fw.version}. Over Bluetooth, about a minute.` : `It's on ${fw.version}: update it once with the USB cable from a computer.`}</small>
              {fwBusy && <FwProgress job={fw.job} />}
              {fw.job?.err && <small className="err">{fw.job.err}</small>}</div>
            {!fwBusy && (fw.canBle
              ? <button className="btn primary sm" onClick={fw.viaBle}>Update</button>
              : canUsb && <button className="btn primary sm" onClick={fw.viaUsb}>Update with cable</button>)}
          </div>
        )}
        <div className="actions"><button className="btn ghost" disabled={fwBusy} onClick={onClose}>{fwBusy ? "Updating…" : "Later"}</button></div>
      </div>
    </div>
  );
}

/* ------------------------------------------------------------ small pieces */

function Logo() {
  return (
    <svg className="kc-logo" width="26" height="30" viewBox="0 0 26 30" aria-hidden="true">
      <circle cx="13" cy="5" r="3.2" fill="none" stroke="currentColor" strokeWidth="2" />
      <rect x="2" y="9" width="22" height="20" rx="3" fill="currentColor" />
      <rect x="5.5" y="14" width="15" height="9" rx="1.5" fill="var(--bg)" />
      <path d="M8 17.5h2M16 17.5h2M11 20.5q2 1.2 4 0" stroke="var(--accent)" strokeWidth="1.4" fill="none" strokeLinecap="round" />
    </svg>
  );
}

// the keychain, drawn from its CAD model: case, hanging loop with its hole, the
// screen window playing what the keychain shows, and the real measurements
function Keychain({ index, player, picture, dim }) {
  return (
    <figure className={"device kc-fig" + (dim ? " off" : "")} aria-hidden="true">
      <div className="kc">
        <div className="kc-loop"><i className="kc-hole" /></div>
        <div className="kc-case">
          <i className="screw a" /><i className="screw b" /><i className="screw c" /><i className="screw d" />
          <div className="kc-window">
            <Screen index={index} player={player} picture={picture} />
          </div>
          <span className="print">SNOOPY · KEYCHAIN</span>
          <i className="pad" />
        </div>
      </div>
    </figure>
  );
}

const ICONS = {
  screens: <path d="M4 6h7v6H4zM13 6h7v6h-7zM4 14h7v4H4zM13 14h7v4h-7z" />,
  draw: <path d="M5 19l1-4L16 5l3 3L9 18l-4 1zM14 7l3 3" />,
  music: <><circle cx="12" cy="12" r="8" /><circle cx="12" cy="12" r="2.4" /></>,
  settings: <><path d="M5 7h14M5 12h14M5 17h14" /><circle cx="9" cy="7" r="1.8" /><circle cx="15" cy="12" r="1.8" /><circle cx="8" cy="17" r="1.8" /></>,
  help: <><circle cx="12" cy="12" r="8" /><path d="M9.6 9.6a2.5 2.5 0 1 1 3.3 2.4c-.6.3-.9.8-.9 1.5M12 16.5v.2" /></>,
};

function Toggle({ checked, disabled, onChange, label }) {
  return (
    <label className={"switch" + (disabled ? " off" : "")}>
      <input type="checkbox" role="switch" checked={checked} disabled={disabled} aria-label={label} onChange={(e) => onChange(e.target.checked)} />
      <span />
    </label>
  );
}

// a stepped level, like the bars on the keychain's own settings page
function Steps({ count, value, disabled, onChange, label }) {
  return (
    <div className="steps" role="radiogroup" aria-label={label}>
      {Array.from({ length: count }, (_, i) => (
        <button key={i} role="radio" aria-checked={value === i} aria-label={`${label} ${i + 1}`} disabled={disabled}
                className={i <= value && !disabled ? "on" : ""} style={{ "--h": 30 + (70 * i) / Math.max(1, count - 1) + "%" }}
                onClick={() => onChange(i)} />
      ))}
    </div>
  );
}

function Row({ title, hint, children }) {
  return (
    <div className="row">
      <div className="row-text"><b>{title}</b>{hint && <small>{hint}</small>}</div>
      <div className="row-ctl">{children}</div>
    </div>
  );
}

function useToasts() {
  const [list, setList] = useState([]);
  const push = (text, kind = "") => {
    const id = Math.random();
    setList((l) => [...l, { id, text, kind }]);
    setTimeout(() => setList((l) => l.filter((t) => t.id !== id)), 3200);
  };
  return [list, push];
}

function useTab() {
  const read = () => { const h = location.hash.slice(1); return TABS.some(([id]) => id === h) ? h : "screens"; };
  const [tab, setTab] = useState(read);
  useEffect(() => { const on = () => setTab(read()); addEventListener("hashchange", on); return () => removeEventListener("hashchange", on); }, []);
  return tab;
}

function useTheme() {
  const [t, setT] = useState(() => document.documentElement.dataset.theme || "light");
  useEffect(() => { document.documentElement.dataset.theme = t; try { localStorage.setItem("kc-theme", t); } catch {} }, [t]);
  return [t, () => setT(t === "dark" ? "light" : "dark")];
}

function useInstall() {
  const [ev, setEv] = useState(null);
  useEffect(() => {
    const on = (e) => { e.preventDefault(); setEv(e); };
    addEventListener("beforeinstallprompt", on);
    addEventListener("appinstalled", () => setEv(null));
    return () => removeEventListener("beforeinstallprompt", on);
  }, []);
  return ev && (async () => { ev.prompt(); await ev.userChoice.catch(() => {}); setEv(null); });
}

const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

/* ------------------------------------------------------------ the tabs */

function Screens({ names, current, onPick, linked, picture }) {
  return (
    <section className="panel">
      <div className="panel-head"><h2>Screens</h2><span className="mono">{names.length}</span></div>
      <p className="lead">{linked ? "Tap one to show it on the keychain." : "Everything the keychain can show. Connect to pick one."}</p>
      <div className="kc-grid">
        {names.map((name, i) => (
          <button key={i} className={"kc-tile" + (i === current ? " cur" : "")} onClick={() => onPick(i)}>
            <span className="kc-thumb"><Screen index={i} player={name === PLAYER ? { song: "Lo-fi Nights - Snoopy", playing: true } : null}
                    picture={name === PICTURE ? picture || new Uint8Array(1024) : null} className="thumb" /></span>
            <span className="kc-tile-name"><span className="num">{String(i + 1).padStart(2, "0")}</span>{name}</span>
          </button>
        ))}
      </div>
    </section>
  );
}

function PhoneMusic({ np, beat }) {
  return (
    <section className="panel">
      <div className="panel-head"><h2>Music on this phone</h2>{np.granted && <span className="pill ok">On</span>}</div>
      {np.granted ? (
        <p className="lead">Whatever plays on this phone - YouTube, YouTube Music, Spotify, any app - shows on the keychain by itself.
          {np.track?.app && <> Now: <b>{np.track.app}</b>.</>}</p>
      ) : (
        <>
          <p className="lead">Let the app see what's playing on this phone, from any app (YouTube, YouTube Music, Spotify...). Android calls this
            <b> Notification access</b>; the app only reads the song, never your notifications.</p>
          <div className="actions"><button className="btn primary" onClick={openNotificationAccess}>Allow access</button></div>
          <p className="hint">In the list, switch on <b>Keychain Remote</b>, then come back.</p>
        </>
      )}
      <Row title="Move to the beat" hint="The record player and equalizer follow the song's loudness. Android asks for the microphone for this; it only reads what this phone plays.">
        <Toggle checked={beat.running} label="Move to the beat" onChange={beat.set} />
      </Row>
      {beat.granted && !beat.running && localStorage.getItem("kc-beat") === "1" &&
        <p className="hint">This phone doesn't share its sound: the keychain keeps a gentle beat of its own.</p>}
    </section>
  );
}

function Music({ kc, sp, lf, np, beat, relinkLastfm, manual, setManual, sendManual, showPlayer }) {
  const [cid, setCid] = useState(savedClientId());
  const [lfUser, setLfUser] = useState(savedLastfm().user);
  const [lfKey, setLfKey] = useState(savedLastfm().key);
  const t = np.track || lf.track || sp.track;
  const source = np.track ? np.track.app || "Phone" : lf.track ? "Last.fm" : sp.track ? "Spotify"
    : np.granted ? "Phone" : lf.linked ? "Last.fm" : sp.linked ? "Spotify" : "";
  return (
    <>
      <section className="panel">
        <div className="panel-head"><h2>Now playing</h2>{source && <span className="pill ok">{source}</span>}</div>
        <div className="np-card">
          {t?.art ? <img src={t.art} alt="" className="np-art" /> : <div className={"np-disc" + (t?.playing ? " spin" : "")} />}
          <div className="np-text">
            <b>{t ? t.title : source ? "Nothing playing" : "Link your music"}</b>
            <small>{t ? t.artist : source ? "Play something: it shows here and on the keychain." : isNativeApp ? "Allow access below and anything you play shows up by itself." : "Set up Last.fm below (free) and your Spotify songs show up by themselves."}</small>
            {t && t.dur > 0 && (
              <div className="np-prog"><span>{mmss(Math.min(t.pos, t.dur))}</span><div className="meter"><i style={{ width: `${Math.min(100, (100 * t.pos) / t.dur)}%` }} /></div><span>{mmss(t.dur)}</span></div>
            )}
          </div>
        </div>
        <div className="actions"><button className="btn ghost" disabled={kc.link !== "on"} onClick={showPlayer}>Show record player</button></div>
        {(lf.error || sp.error) && <p className="note err">{lf.error || sp.error}</p>}
      </section>

      {isNativeApp && <PhoneMusic np={np} beat={beat} />}
      <GetAndroid />
      {!isNativeApp && <>
      <section className="panel">
        <div className="panel-head"><h2>Follow Spotify (free)</h2>{lf.linked && <span className="pill ok">Linked</span>}</div>
        <p className="lead">Works with the normal Spotify app, no Premium: Spotify tells Last.fm what you play, and this reads it.</p>
        {lf.linked ? (
          <div className="actions">
            <span className="mono">{savedLastfm().user}</span>
            <button className="btn link" onClick={() => { forgetLastfm(); setLfUser(""); setLfKey(""); relinkLastfm(); }}>Unlink</button>
          </div>
        ) : (
          <>
            <ol className="steps-list">
              <li><span>Make a free account at <b>last.fm</b>.</span></li>
              <li><span>In the <b>Spotify app</b>: Settings &gt; Apps &gt; <b>Last.fm</b> &gt; Connect (or on last.fm: Settings &gt; Applications &gt; Spotify Scrobbling).</span></li>
              <li><span>Get a free API key at <b>last.fm/api/account/create</b> (any app name).</span></li>
              <li><span>Fill in both below and tap <b>Link</b>.</span></li>
            </ol>
            <div className="inline">
              <input className="field" value={lfUser} onChange={(e) => setLfUser(e.target.value)} placeholder="Last.fm username" />
              <input className="field" value={lfKey} onChange={(e) => setLfKey(e.target.value)} placeholder="API key" />
              <button className="btn primary" disabled={!lfUser.trim() || !lfKey.trim()} onClick={() => { saveLastfm(lfUser, lfKey); relinkLastfm(); }}>Link</button>
            </div>
          </>
        )}
      </section>

      <section className="panel">
        <details className="setup">
          <summary>Spotify Premium? Link Spotify directly instead</summary>
          <div className="actions" style={{ marginTop: 10 }}>
            {sp.linked
              ? <button className="btn ghost" onClick={() => { spotifyLogout(); location.reload(); }}>Disconnect Spotify</button>
              : <button className="btn primary" disabled={!cid} onClick={() => spotifyLogin(cid)}>Connect Spotify</button>}
          </div>
          {!sp.linked && (
            <>
              <ol className="steps-list">
                <li><span>Open <b>developer.spotify.com/dashboard</b> and create an app (needs Premium).</span></li>
                <li><span>Under <b>Redirect URIs</b> add exactly:
                  <span className="inline"><code>{spotifyRedirect()}</code>
                    <button className="btn ghost sm" onClick={() => navigator.clipboard?.writeText(spotifyRedirect())}>Copy</button></span>
                  and tick <b>Web API</b>.</span></li>
                <li><span>Paste its <b>Client ID</b>, then tap <b>Connect Spotify</b>.</span></li>
              </ol>
              <input className="field" value={cid} onChange={(e) => setCid(e.target.value.trim())} placeholder="Spotify Client ID" />
            </>
          )}
        </details>
      </section>
      </>}
      <section className="panel">
        <div className="panel-head"><h2>Or type a song</h2></div>
        <div className="inline">
          <input className="field" value={manual} onChange={(e) => setManual(e.target.value)} placeholder="Song - Artist"
                 onKeyDown={(e) => e.key === "Enter" && sendManual(true)} />
          <button className="btn primary" disabled={kc.link !== "on" || !manual.trim()} onClick={() => sendManual(true)}>Play</button>
          <button className="btn ghost" disabled={kc.link !== "on"} onClick={() => sendManual(false)}>Pause</button>
        </div>
        <p className="hint">Playing something else (YouTube, a radio app)? Type it here.</p>
      </section>
    </>
  );
}

/* ------------------------------------------------------------ firmware updates */

// one update at a time, shared by the banner and the Settings section
function useFirmware(kc, toast, tick) {
  const latest = useLatestFirmware(tick);
  const [job, setJob] = useState(null);           // { how, pct, step } while it runs; { err } if it failed
  const linked = kc.link === "on";
  const known = linked && kc.count > 0;           // the keychain has told us about itself (F: comes before K:)
  const version = kc.fw?.version || (known ? "1.x" : "");
  const canBle = known && !!kc.fw?.ota;
  const update = !!latest && known && (!kc.fw || newer(latest.version, kc.fw.version));

  async function run(how, work) {
    let lock = null;
    try { lock = await navigator.wakeLock?.request("screen"); } catch {}   // keep the phone awake meanwhile
    setJob({ how, pct: 0, step: "Downloading the firmware…" });
    try {
      await work((pct) => setJob((j) => ({ ...j, pct, step: how === "ble" ? "Sending it over Bluetooth…" : j.step })),
                 (step) => setJob((j) => ({ ...j, step })));
      setJob(null);
      toast(`Keychain updated to ${latest.version}`);
    } catch (e) {
      if (e?.name === "NotFoundError") setJob(null);   // closed the port picker
      else setJob({ how, err: e?.message || "Something went wrong" });
    } finally { lock?.release?.().catch(() => {}); }
  }
  const viaBle = () => run("ble", async (pct) => kc.updateFirmware(await downloadFirmware(latest), latest.md5, pct));
  const viaUsb = () => run("usb", (pct, step) => flashUsb(latest, pct, step));
  return { latest, job, version, update, canBle, known, viaBle, viaUsb, clear: () => setJob(null) };
}

function FwProgress({ job }) {
  const pct = Math.round((job.pct || 0) * 100);
  return <div className="fwprog"><div className="meter"><i style={{ width: pct + "%" }} /></div><span>{pct}%</span></div>;
}

// the Claude Buddy-style banner on top: "New firmware for your keychain"
function FirmwareBar({ fw }) {
  const { job, latest } = fw;
  if (job && !job.err)
    return (
      <div className="fwbar busy" role="status">
        <div><strong>Updating the keychain…</strong>
          <small>{job.step} {job.how === "ble" ? "Keep the phone close and this app open." : "Keep the cable plugged in."} It restarts when it's done.</small></div>
        <FwProgress job={job} />
      </div>
    );
  if (job?.err)
    return <div className="fwbar err" role="alert"><div><strong>Keychain not updated</strong><small>{job.err} Your keychain still works as before.</small></div>
      <button className="btn ghost" onClick={fw.clear}>OK</button></div>;
  if (!fw.update) return null;
  return (
    <div className="fwbar">
      <div>
        <strong>New firmware for your keychain: {latest.version}</strong>
        <small>{fw.canBle ? `It's on ${fw.version}. Over Bluetooth, about a minute.`
          : `It's on ${fw.version}, which can't update over Bluetooth yet: update it once with the USB cable${canUsb ? "" : " from a computer (Chrome or Edge)"}.`}</small>
      </div>
      <div className="actions">
        {fw.canBle && <button className="btn primary" onClick={fw.viaBle}>Update over Bluetooth</button>}
        {canUsb && <button className={"btn " + (fw.canBle ? "ghost" : "primary")} onClick={fw.viaUsb}>Update with USB cable</button>}
      </div>
    </div>
  );
}

function FirmwarePanel({ kc, fw }) {
  const busy = fw.job && !fw.job.err;
  const linked = kc.link === "on";
  return (
    <section className="panel">
      <div className="panel-head"><h2>Firmware</h2>{fw.known && !fw.update && <span className="pill ok">Up to date</span>}</div>
      <Row title="On the keychain" hint={linked ? (fw.known ? "" : "Asking it…") : "Connect to see it"}>
        <span className="mono">{fw.version || "-"}</span></Row>
      <Row title="Newest" hint={fw.latest ? `Released ${fw.latest.built}` : "Looking…"}><span className="mono">{fw.latest?.version || "-"}</span></Row>
      <Row title="Over Bluetooth" hint={fw.canBle ? "The app sends it to the keychain. Keep the phone close." : linked ? "Needs firmware 2.0 or newer: update once with the cable." : "Connect to the keychain first."}>
        <button className="btn primary sm" disabled={!fw.canBle || busy || !fw.latest} onClick={fw.viaBle}>Update</button></Row>
      <Row title="With the USB cable" hint={canUsb ? "Plug the keychain into this computer. Works on any keychain, even one that's never been updated." : "On a computer, in Chrome or Edge."}>
        <button className="btn ghost sm" disabled={!canUsb || busy || !fw.latest} onClick={fw.viaUsb}>Update</button></Row>
      {busy && <FwProgress job={fw.job} />}
    </section>
  );
}

function Settings({ kc }) {
  const off = kc.link !== "on";
  return (
    <section className="panel">
      <div className="panel-head"><h2>Settings</h2></div>
      <Row title="Brightness" hint="The same 8 steps as on the keychain"><Steps count={8} value={kc.bri} disabled={off} onChange={kc.act.bri} label="Brightness" /></Row>
      <Row title="Volume" hint="How loud its little chirps are"><Steps count={7} value={kc.vol} disabled={off} onChange={kc.act.vol} label="Volume" /></Row>
      <Row title="Sound" hint="Chirps when you tap"><Toggle checked={kc.snd} disabled={off} onChange={kc.act.snd} label="Sound" /></Row>
      {off && <p className="hint">Connect to the keychain to change these.</p>}
    </section>
  );
}

function Help() {
  return (
    <>
      <GetAndroid />
      <section className="panel">
        <div className="panel-head"><h2>Connect</h2></div>
        <ol className="steps-list">
          <li><span>Tap the keychain to wake it.</span></li>
          <li><span>Press <b>Connect</b> and pick <b>Snoopy Keychain</b>.</span></li>
          <li><span>Use <b>Chrome</b> on Android or a computer. On iPhone, use the <b>Bluefy</b> browser.</span></li>
        </ol>
      </section>
      <section className="panel">
        <div className="panel-head"><h2>On the keychain</h2></div>
        <dl className="commands">
          <div><dt>Tap</dt><dd>Next screen</dd></div>
          <div><dt>Double tap</dt><dd>Settings: brightness, volume, sound</dd></div>
          <div><dt>Hold 3 s</dt><dd>Sleep (tap to wake)</dd></div>
        </dl>
      </section>
    </>
  );
}

/* ------------------------------------------------------------ the app */

export default function App() {
  const [toasts, toast] = useToasts();
  const kc = useKeychain((what, arg) => {
    if (what === "connected") toast(`Connected to ${arg || "the keychain"}`);
    if (what === "disconnected") toast("Keychain disconnected", "warn");
    if (what === "error") toast(arg || "Couldn't connect", "warn");
  });
  const sp = useSpotify();
  const [lfVersion, setLfVersion] = useState(0);
  const lf = useLastfm(lfVersion);
  const np = useNowPlaying();
  const [checkTick, setCheckTick] = useState(0);
  const fw = useFirmware(kc, toast, checkTick);
  const app = useAppUpdate(checkTick);
  const appNext = app.next;
  const checking = useRef(false);
  function checkUpdates() { checking.current = true; setCheckTick((t) => t + 1); }
  // after "Check for updates": the pop-up if there's something, else say so
  useEffect(() => {
    if (!checking.current) return;
    const t = setTimeout(() => {
      checking.current = false;
      if (app.next || fw.update) setShowUpd(true); else toast("Everything is up to date");
    }, 1500);
    return () => clearTimeout(t);
  }, [checkTick, app.newest, fw.latest]);
  const updates = (appNext ? 1 : 0) + (fw.update ? 1 : 0);
  const [showUpd, setShowUpd] = useState(false);
  // pops up by itself once for each new version (then the button on top stays)
  const updKey = `${appNext}|${fw.update ? fw.latest.version : ""}`;
  useEffect(() => {
    if (!updates) return;
    let seen = "";
    try { seen = localStorage.getItem("kc-upd-seen") || ""; } catch {}
    if (seen !== updKey) { setShowUpd(true); try { localStorage.setItem("kc-upd-seen", updKey); } catch {} }
  }, [updKey]);
  useEffect(() => { if (fw.job && !fw.job.err) setShowUpd(true); }, [!!fw.job]);
  // the beat -> keychain, ~8x a second while a song plays (A:<0-99>, as Claude Buddy's bridge sends)
  const beatAt = useRef(0), linkedRef = useRef(false), playingRef = useRef(false);
  const beat = useBeat((level) => {
    const now = Date.now();
    if (!linkedRef.current || !playingRef.current || now - beatAt.current < 120) return;
    beatAt.current = now;
    kc.send("A:" + level, true);
  });
  const track = np.track || lf.track || sp.track;
  const tab = useTab();
  const [theme, flipTheme] = useTheme();
  const install = useInstall();
  const [localNames, setLocalNames] = useState([]);
  const [preview, setPreview] = useState(0);
  const [manual, setManual] = useState("");
  const [song, setSong] = useState(null);                 // what the record player is showing
  const [picture, setPictureState] = useState(loadPicture);
  const picSave = useRef(0);
  function setPicture(b) {
    setPictureState(b);
    clearTimeout(picSave.current);
    picSave.current = setTimeout(() => { try { localStorage.setItem("kc-picture", btoa(String.fromCharCode(...b))); } catch {} }, 600);
  }

  useEffect(() => { loadScreens().then((l) => setLocalNames([...l.map((s) => s.name), PLAYER, PICTURE])).catch(() => {}); }, []);

  const linked = kc.link === "on";
  linkedRef.current = linked; playingRef.current = !!track?.playing;
  const names = linked && kc.names.length ? kc.names.map((n, i) => n || localNames[i] || "…") : localNames;
  const current = linked ? kc.screen : preview;
  const playerIndex = names.indexOf(PLAYER);

  // Spotify -> keychain: new song, play/pause, or every 15 s to keep the progress in step
  const sent = useRef({ key: "", at: 0 });
  useEffect(() => { sent.current = { key: "", at: 0 }; }, [linked]);   // (re)connected: tell it again
  useEffect(() => {
    const t = track;
    if (!t) {                                    // the music stopped: the keychain goes back to its screen
      if (linked && sent.current.key) { kc.send("M:0,"); sent.current = { key: "", at: 0 }; }
      return;
    }
    setSong({ song: `${t.title} - ${t.artist}`, playing: t.playing });
    if (!linked) return;
    const key = t.title + t.artist + t.playing, now = Date.now(), c = sent.current;
    const fresh = key !== c.key;
    if (fresh || now - c.at > 15000) {                     // the song, and playing / paused
      Object.assign(c, { key, at: now });
      kc.send(`M:${t.playing ? 1 : 2},${t.title} - ${t.artist}`.slice(0, 90));
    }
    // the progress bar: again whenever the phone's position drifts from what the keychain counts (a seek, a pause)
    const expect = (c.pos || 0) + (c.playing ? (now - (c.posAt || now)) / 1000 : 0);
    if (fresh || Math.abs(t.pos - expect) > 2.5 || now - (c.posAt || 0) > 15000) {
      Object.assign(c, { pos: t.pos, posAt: now, playing: t.playing });
      kc.send(`Q:${Math.round(t.pos)},${Math.round(t.dur)}`);
    }
  }, [track?.title, track?.artist, track?.playing, Math.round(track?.pos || 0), linked]);

  function sendManual(play) {
    const text = (manual.trim() || song?.song || "").slice(0, 80);
    if (!text) return;
    setSong({ song: text, playing: play });
    kc.send(`M:${play ? 1 : 2},${text}`);
    if (play) kc.send("Q:0,0");
    toast(play ? "Playing on the keychain" : "Paused");
  }
  const pick = (i) => (linked ? kc.act.show(i) : setPreview(i));
  const showPlayer = () => playerIndex >= 0 && kc.act.show(playerIndex);
  const isPlayer = names[current] === PLAYER;
  const playerArgs = isPlayer ? song || { song: "Lo-fi Nights - Snoopy", playing: true } : null;
  const pictureArgs = names[current] === PICTURE ? picture || new Uint8Array(1024) : null;

  function linkButtons(small) {
    const sm = small ? " sm" : "";
    if (linked) return (
      <>
        <button className={"btn primary" + sm} onClick={kc.act.next}>{small ? "Next" : "Next screen"}</button>
        {!small && <button className="btn link" onClick={kc.disconnect}>Disconnect</button>}
      </>
    );
    const busy = !canBluetooth || kc.link === "connecting";
    if (kc.lastName) return (
      <>
        <button className={"btn primary" + sm} disabled={busy} onClick={kc.reconnect}>
          {kc.link === "connecting" ? "Reconnecting…" : small ? "Reconnect" : `Reconnect to ${kc.lastName}`}</button>
        {!small && <button className="btn link" onClick={kc.connect}>Another keychain</button>}
      </>
    );
    return <button className={"btn primary" + sm} disabled={busy} onClick={kc.connect}>Connect</button>;
  }
  const title = linked ? "Connected" : kc.link === "connecting" ? "Connecting…" : "Keychain";
  const sub = linked ? `Showing ${names[current] || "…"}.`
    : canBluetooth ? "Tap the keychain to wake it, then connect over Bluetooth."
    : "This browser can't use Bluetooth. Open this page in Chrome, or Bluefy on iPhone.";

  return (
    <div className="app">
      <header className="top">
        <a className="logo" href="#screens"><Logo /><span>Keychain <b>Remote</b></span></a>
        <span className={"led " + (linked ? "ok" : kc.link === "connecting" ? "wait" : "")}><i />{linked ? "Bluetooth" : kc.link === "connecting" ? "Connecting" : "Not connected"}</span>
        <span className="end">
          {updates > 0 && <button className="btn primary sm upd-btn" onClick={() => setShowUpd(true)}><i />Update{updates > 1 ? "s" : ""}</button>}
          {install && <button className="btn ghost sm" onClick={install}>Install app</button>}
          <button className="theme" onClick={flipTheme} aria-label="Theme">{theme === "dark" ? "☀" : "☾"}</button>
        </span>
      </header>

      {tab === "screens" ? (
      <section className="stage">
        <Keychain index={current} player={playerArgs} picture={pictureArgs} dim={!linked} />
        <div className="stage-text">
          <span className={"eyebrow " + (linked ? "ok" : "")}>{linked ? "● Live" : "○ Standby"}</span>
          <h1>{title}</h1>
          <p>{sub}</p>
          {names[current] && <div className={"now" + (linked ? " busy" : "")}><i /><span>{linked ? "Showing" : "Preview"} · <b>{names[current]}</b></span></div>}
          {song && isPlayer && <div className="now"><i /><span>♪ <b>{song.song}</b></span></div>}
          <div className="actions">{linkButtons(false)}</div>
        </div>
      </section>
      ) : (
      // the other tabs: a slim bar with the live screen, so the page is about the tab
      <section className={"mini" + (linked ? "" : " off")}>
        <a className="mini-screen" href="#screens" aria-label="Screens"><Screen index={current} player={playerArgs} picture={pictureArgs} /></a>
        <div className="mini-text">
          <span className={"eyebrow " + (linked ? "ok" : "")}>{linked ? "● Live" : kc.link === "connecting" ? "○ Connecting…" : "○ Standby"}</span>
          <b>{song && isPlayer ? song.song : names[current] || "Keychain"}</b>
          <small>{song && isPlayer ? (song.playing ? "Playing on the keychain" : "Paused") : linked ? `On ${kc.name || "your keychain"}` : "Not connected"}</small>
        </div>
        <div className="actions">{linkButtons(true)}</div>
      </section>
      )}

      <nav className="tabs" aria-label="Sections">
        {TABS.map(([id, name], i) => (
          <a key={id} href={"#" + id} aria-current={tab === id ? "page" : undefined}><span>{String(i + 1).padStart(2, "0")}</span>{name}</a>
        ))}
      </nav>

      <main className="content">
        {fw.job && <FirmwareBar fw={fw} />}
        {tab === "screens" && <Screens names={names} current={current} onPick={pick} linked={linked} picture={picture} />}
        {tab === "draw" && <Draw kc={kc} picture={picture} setPicture={setPicture} />}
        {tab === "music" && <Music kc={kc} sp={sp} lf={lf} np={np} beat={beat} relinkLastfm={() => setLfVersion((v) => v + 1)} manual={manual} setManual={setManual} sendManual={sendManual} showPlayer={showPlayer} />}
        {tab === "settings" && <><Settings kc={kc} /><FirmwarePanel kc={kc} fw={fw} /><Versions kc={kc} fw={fw} app={app} onCheck={checkUpdates} /></>}
        {tab === "help" && <Help />}
      </main>

      <nav className="dock" aria-label="Sections">
        {TABS.map(([id, name]) => (
          <a key={id} href={"#" + id} aria-current={tab === id ? "page" : undefined}>
            <svg viewBox="0 0 24 24" aria-hidden="true">{ICONS[id]}</svg>
            <span>{DOCK_NAMES[id] || name}</span>
          </a>
        ))}
      </nav>

      {showUpd && updates > 0 && <UpdateDialog appNext={appNext} fw={fw} onClose={() => setShowUpd(false)} />}

      <div className="toasts" aria-live="polite">{toasts.map((t) => <div key={t.id} className={"toast " + t.kind}>{t.text}</div>)}</div>
      <footer className="foot"><span>Keychain Remote · talks to your keychain over Bluetooth, nothing leaves your phone</span></footer>
    </div>
  );
}
