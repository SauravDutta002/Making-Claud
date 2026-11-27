import React, { useEffect, useRef, useState } from "react";
import Screen from "./Screen.jsx";
import { loadScreens } from "./screens.js";
import { useKeychain, canBluetooth } from "./ble.js";
import { useSpotify, spotifyLogin, spotifyLogout, spotifyRedirect, savedClientId } from "./spotify.js";
import { useLastfm, savedLastfm, saveLastfm, forgetLastfm } from "./lastfm.js";

const TABS = [["screens", "Screens"], ["music", "Now playing"], ["settings", "Settings"], ["help", "Help"]];
const PLAYER = "Record player";

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
function Keychain({ index, player, dim }) {
  return (
    <figure className={"kc-fig" + (dim ? " off" : "")} aria-hidden="true">
      <div className="kc">
        <div className="kc-loop"><i className="kc-hole" /></div>
        <div className="kc-case">
          <div className="kc-window">
            <Screen index={index} player={player} />
            <i className="kc-glass" />
          </div>
        </div>
      </div>
    </figure>
  );
}

const ICONS = {
  screens: <path d="M4 6h7v6H4zM13 6h7v6h-7zM4 14h7v4H4zM13 14h7v4h-7z" />,
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

function Screens({ names, current, onPick, linked }) {
  return (
    <section className="panel">
      <div className="panel-head"><h2>Screens</h2><span className="mono">{names.length}</span></div>
      <p className="lead">{linked ? "Tap one to show it on the keychain." : "Everything the keychain can show. Connect to pick one."}</p>
      <div className="kc-grid">
        {names.map((name, i) => (
          <button key={i} className={"kc-tile" + (i === current ? " cur" : "")} onClick={() => onPick(i)}>
            <span className="kc-thumb"><Screen index={i} player={name === PLAYER ? { song: "Lo-fi Nights - Snoopy", playing: true } : null} className="thumb" /></span>
            <span className="kc-tile-name"><span className="num">{String(i + 1).padStart(2, "0")}</span>{name}</span>
          </button>
        ))}
      </div>
    </section>
  );
}

function Music({ kc, sp, lf, relinkLastfm, manual, setManual, sendManual, showPlayer }) {
  const [cid, setCid] = useState(savedClientId());
  const [lfUser, setLfUser] = useState(savedLastfm().user);
  const [lfKey, setLfKey] = useState(savedLastfm().key);
  const t = lf.track || sp.track;
  const source = lf.track ? "Last.fm" : sp.track ? "Spotify" : lf.linked ? "Last.fm" : sp.linked ? "Spotify" : "";
  return (
    <>
      <section className="panel">
        <div className="panel-head"><h2>Now playing</h2>{source && <span className="pill ok">{source}</span>}</div>
        <div className="np-card">
          {t?.art ? <img src={t.art} alt="" className="np-art" /> : <div className={"np-disc" + (t?.playing ? " spin" : "")} />}
          <div className="np-text">
            <b>{t ? t.title : source ? "Nothing playing" : "Link your music"}</b>
            <small>{t ? t.artist : source ? "Play a song in Spotify: it shows here and on the keychain." : "Set up Last.fm below (free) and your Spotify songs show up by themselves."}</small>
            {t && t.dur > 0 && (
              <div className="np-prog"><span>{mmss(Math.min(t.pos, t.dur))}</span><div className="meter"><i style={{ width: `${Math.min(100, (100 * t.pos) / t.dur)}%` }} /></div><span>{mmss(t.dur)}</span></div>
            )}
          </div>
        </div>
        <div className="actions"><button className="btn ghost" disabled={kc.link !== "on"} onClick={showPlayer}>Show record player</button></div>
        {(lf.error || sp.error) && <p className="note err">{lf.error || sp.error}</p>}
      </section>

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
  const track = lf.track || sp.track;
  const tab = useTab();
  const [theme, flipTheme] = useTheme();
  const install = useInstall();
  const [localNames, setLocalNames] = useState([]);
  const [preview, setPreview] = useState(0);
  const [manual, setManual] = useState("");
  const [song, setSong] = useState(null);                 // what the record player is showing

  useEffect(() => { loadScreens().then((l) => setLocalNames([...l.map((s) => s.name), PLAYER])).catch(() => {}); }, []);

  const linked = kc.link === "on";
  const names = linked && kc.names.length ? kc.names.map((n, i) => n || localNames[i] || "…") : localNames;
  const current = linked ? kc.screen : preview;
  const playerIndex = names.indexOf(PLAYER);

  // Spotify -> keychain: new song, play/pause, or every 15 s to keep the progress in step
  const sent = useRef({ key: "", at: 0 });
  useEffect(() => {
    const t = track;
    if (!t) return;
    setSong({ song: `${t.title} - ${t.artist}`, playing: t.playing });
    if (!linked) return;
    const key = t.title + t.artist + t.playing;
    if (key !== sent.current.key || Date.now() - sent.current.at > 15000) {
      sent.current = { key, at: Date.now() };
      kc.send(`M:${t.playing ? 1 : 2},${t.title} - ${t.artist}`.slice(0, 90));
      kc.send(`Q:${Math.round(t.pos)},${Math.round(t.dur)}`);
    }
  }, [track?.title, track?.artist, track?.playing, Math.floor((track?.pos || 0) / 15), linked]);

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
          {install && <button className="btn ghost sm" onClick={install}>Install app</button>}
          <button className="theme" onClick={flipTheme} aria-label="Theme">{theme === "dark" ? "☀" : "☾"}</button>
        </span>
      </header>

      <section className="stage">
        <Keychain index={current} player={playerArgs} dim={!linked} />
        <div className="stage-text">
          <span className={"eyebrow " + (linked ? "ok" : "")}>{linked ? "● Live" : "○ Standby"}</span>
          <h1>{title}</h1>
          <p>{sub}</p>
          {names[current] && <div className={"now" + (linked ? " busy" : "")}><i /><span>{linked ? "Showing" : "Preview"} · <b>{names[current]}</b></span></div>}
          {song && isPlayer && <div className="now"><i /><span>♪ <b>{song.song}</b></span></div>}
          <div className="actions">
            {linked ? (
              <>
                <button className="btn primary" onClick={kc.act.next}>Next screen</button>
                <button className="btn link" onClick={kc.disconnect}>Disconnect</button>
              </>
            ) : (
              kc.lastName
                ? <>
                    <button className="btn primary" disabled={!canBluetooth || kc.link === "connecting"} onClick={kc.reconnect}>
                      {kc.link === "connecting" ? "Reconnecting…" : `Reconnect to ${kc.lastName}`}</button>
                    <button className="btn link" onClick={kc.connect}>Another keychain</button>
                  </>
                : <button className="btn primary" disabled={!canBluetooth || kc.link === "connecting"} onClick={kc.connect}>Connect</button>
            )}
          </div>
        </div>
      </section>

      <nav className="tabs" aria-label="Sections">
        {TABS.map(([id, name], i) => (
          <a key={id} href={"#" + id} aria-current={tab === id ? "page" : undefined}><span>{String(i + 1).padStart(2, "0")}</span>{name}</a>
        ))}
      </nav>

      <main className="content">
        {tab === "screens" && <Screens names={names} current={current} onPick={pick} linked={linked} />}
        {tab === "music" && <Music kc={kc} sp={sp} lf={lf} relinkLastfm={() => setLfVersion((v) => v + 1)} manual={manual} setManual={setManual} sendManual={sendManual} showPlayer={showPlayer} />}
        {tab === "settings" && <Settings kc={kc} />}
        {tab === "help" && <Help />}
      </main>

      <nav className="dock" aria-label="Sections">
        {TABS.map(([id, name]) => (
          <a key={id} href={"#" + id} aria-current={tab === id ? "page" : undefined}>
            <svg viewBox="0 0 24 24" aria-hidden="true">{ICONS[id]}</svg>
            <span>{name}</span>
          </a>
        ))}
      </nav>

      <div className="toasts" aria-live="polite">{toasts.map((t) => <div key={t.id} className={"toast " + t.kind}>{t.text}</div>)}</div>
      <footer className="foot"><span>Keychain Remote · talks to your keychain over Bluetooth, nothing leaves your phone</span></footer>
    </div>
  );
}
