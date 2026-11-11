import React, { useEffect, useMemo, useState } from "react";
import { useBridge, sendLine, saveSettings } from "./api.js";

/* ------------------------------------------------------------ pieces */

// Claude Buddy's own pixel mascot: a little antenna creature (8x8, scaled up)
const MASCOT = [
  "...rr...",
  "...r....",
  ".rrrrrr.",
  "rrwrrwrr",
  "rrwrrwrr",
  "rrrrrrrr",
  ".rr..rr.",
  ".r....r.",
];
function Mascot({ className = "" }) {
  return (
    <div className={"mascot " + className} aria-hidden="true">
      {MASCOT.join("").split("").map((c, i) => <i key={i} className={c === "r" ? "r" : c === "w" ? "w" : ""} />)}
    </div>
  );
}

function PixelPlus({ style }) {
  return <span className="plus" style={style} aria-hidden="true" />;
}

// The device: a wide desk screen on a little stand, its OLED glowing
function Device({ mode }) {
  return (
    <div className={"device " + mode} aria-hidden="true">
      <div className="bubble">{mode === "music" ? "♪ ♪" : mode === "off" ? "zz" : "HEY!"}</div>
      <div className="body">
        <div className="screen">
          <span className="eye" /><span className="eye" />
          {mode === "music" && <span className="note">♪</span>}
        </div>
        <span className="pad" />
      </div>
      <div className="stand" />
      <div className="shadow" />
      <span className="zap z1" /><span className="zap z2" /><span className="zap z3" />
    </div>
  );
}

function Toggle({ checked, disabled, onChange, label }) {
  return (
    <label className={"switch" + (disabled ? " off" : "")}>
      <input type="checkbox" checked={checked} disabled={disabled} aria-label={label}
             onChange={(e) => onChange(e.target.checked)} />
      <span />
    </label>
  );
}

function Meter({ value }) {
  return <div className="meter"><i style={{ width: Math.max(0, Math.min(100, value)) + "%" }} /></div>;
}

function Section({ title, hint, children, className = "" }) {
  return (
    <section className={"card " + className}>
      <h2><span className="sq" aria-hidden="true" />{title}</h2>
      {hint && <p className="hint">{hint}</p>}
      {children}
    </section>
  );
}

const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

const FEATURES = [
  ["music", "Music", "Record player, equalizer and DJ while a song plays"],
  ["lyrics", "Lyrics", "Synced lyrics with small Claude hopping along"],
  ["notify", "Notifications", "GitHub and chat messages as cards on the screen"],
  ["calendar", "Meeting alerts", "10, 5 and 1 minute before a meeting"],
  ["away", "Welcome back", "\"Oh, you're back!\" after 10 minutes away"],
  ["typing", "Typing face", "Reacts while you type to Claude"],
  ["usage", "Claude usage", "Your 5-hour and weekly limits on the board"],
];

const TRIES = [
  ["Say hello", "F:hello"],
  ["Play a game", "G:1"],
  ["Notification", "O:2,Claude Buddy: this is a test notification"],
  ["Meeting alert", "N:5,Test meeting"],
  ["Badge", "V:0,Test badge"],
];

/* ------------------------------------------------------------ the page */

export default function App() {
  const { state: s, down } = useBridge();
  const linked = !!s && (s.link === "usb" || s.link === "ble");
  const playing = !!(s && s.music && s.music.state === 1);
  const mode = down || !s ? "off" : playing ? "music" : linked ? "on" : "off";

  const head = down ? ["START THE", "BRIDGE"]
    : !s ? ["SAY HI TO", "YOUR BUDDY"]
    : linked ? ["YOUR DESK", "BUDDY IS LIVE"]
    : ["LOOKING FOR", "YOUR BUDDY"];
  const tag = down ? "OFFLINE" : !s ? "…" : s.link === "usb" ? `USB · ${s.port}` : s.link === "ble" ? "BLUETOOTH" : "SEARCHING";

  return (
    <div className="page">
      <PixelPlus style={{ top: 60, left: "46%" }} />
      <PixelPlus style={{ top: 330, left: "50%" }} />
      <PixelPlus style={{ top: 650, right: "6%" }} />

      <header className="nav">
        <div className="logo">
          <span className="logo-px">CLAUDE</span>
          <span className="logo-bold">BUDDY</span>
        </div>
        <span className={"chip " + (linked ? "ok" : down ? "bad" : "warn")}>
          <i />{down ? "Bridge not running" : !s ? "Connecting…" : linked
            ? (s.link === "usb" ? `Connected over USB (${s.port})` : "Connected over Bluetooth" + (s.rssi != null ? ` · ${s.rssi} dBm` : ""))
            : "Looking for the board…"}
        </span>
        {s && <span className="ver">v{s.version}</span>}
      </header>

      <section className="hero">
        <div className="hero-text">
          <p className="eyebrow">THE LITTLE SCREEN THAT REACTS TO CLAUDE CODE</p>
          <h1><span className="red">{head[0]}</span><span>{head[1]}</span></h1>
          <p className="lede">
            {down ? <>The settings page talks to the bridge on this computer. Start it with <code>claude-buddy</code> and this page comes alive.</>
              : linked ? "Thinking, cheering, singing along. Tune what it shows and how it sounds, right here."
              : "Plug the board in over USB, or switch it on: the bridge finds it over Bluetooth by itself."}
          </p>
          <div className="tag-row">
            <span className="tag">{tag}</span>
            <span className="tag-note">{linked ? "Changes apply instantly" : "Settings save on this computer"}</span>
          </div>
          <div className="cta">
            <button className="btn primary" disabled={!linked} onClick={() => sendLine("F:hello")}>Say hello <span aria-hidden="true">→</span></button>
            <button className="btn ghost" disabled={!linked} onClick={() => sendLine("G:1")}><Mascot className="tiny" /> Play a game</button>
          </div>
        </div>
        <div className="hero-art">
          <Mascot className="big" />
          <Device mode={mode} />
        </div>
      </section>

      {s && <Dashboard s={s} linked={linked} />}

      <footer className="foot">
        {s ? <>Your stats and settings stay on this computer, in <code>{s.data}</code>. Only this computer can open this page.</>
          : "Only this computer can open this page."}
      </footer>
    </div>
  );
}

function Dashboard({ s, linked }) {
  return (
    <>
      <div className="stats">
        <div className="stat wide">
          <span className="k">NOW PLAYING</span>
          {s.music && s.music.state ? (
            <>
              <span className="v small">{s.music.state === 1 ? "▶ " : "❚❚ "}{s.music.title}</span>
              {s.music.lyric && <span className="lyric">“{s.music.lyric}”</span>}
              {s.music.dur > 0 && <><Meter value={(100 * s.music.pos) / s.music.dur} /><span className="sub">{mmss(s.music.pos)} / {mmss(s.music.dur)}</span></>}
            </>
          ) : <span className="v small muted">Nothing playing</span>}
        </div>
        <div className="stat">
          <span className="k">TODAY</span>
          <span className="v">{s.today.commits}<small>commits</small></span>
          <span className="sub">{s.today.tasks} tasks · {s.today.tests} tests passed</span>
        </div>
        <div className="stat">
          <span className="k">STREAK</span>
          <span className="v">{s.streak}<small>day{s.streak === 1 ? "" : "s"}</small></span>
          <span className="sub">{s.badges} of {s.badgesTotal} badges</span>
        </div>
        <div className="stat">
          <span className="k">CLAUDE USAGE</span>
          {s.usage && s.usage.five >= 0 ? (
            <>
              <span className="sub">5-hour · {s.usage.five}%</span><Meter value={s.usage.five} />
              {s.usage.week >= 0 && <><span className="sub">Weekly · {s.usage.week}%</span><Meter value={s.usage.week} /></>}
            </>
          ) : <span className="v small muted">Not known yet</span>}
        </div>
        {s.todos.total > 0 && (
          <div className="stat">
            <span className="k">CLAUDE'S TO-DOS</span>
            <span className="v">{s.todos.done}<small>/ {s.todos.total}</small></span>
            <span className="sub">{s.todos.current || "All done"}</span>
          </div>
        )}
      </div>

      <div className="grid">
        <ScreenCard s={s} linked={linked} />
        <Section title="WHAT IT REACTS TO" hint="Switch anything off or on. It applies right away and is remembered.">
          <div className="toggles">
            {FEATURES.map(([key, name, desc]) => {
              const locked = s.locked.includes(key);
              return (
                <div className="toggle" key={key}>
                  <div><strong>{name}</strong><small>{locked ? "Turned off by a command-line option" : desc}</small></div>
                  <Toggle label={name} checked={!!s.settings[key] && !locked} disabled={locked}
                          onChange={(v) => saveSettings({ [key]: v })} />
                </div>
              );
            })}
          </div>
        </Section>
        <LyricsCard s={s} />
        <NotifyCard s={s} />
        <CalendarCard s={s} />
        <Section title="ACTIVITY" className="log-card">
          <div className="log">
            {s.events.slice().reverse().map(([t, m], i) => <div key={i}><span>{t}</span>{m}</div>)}
          </div>
        </Section>
      </div>
    </>
  );
}

function ScreenCard({ s, linked }) {
  const b = linked ? s.board : null;
  const [face, setFace] = useState(s.faces[0] ? s.faces[0][0] : 0);
  return (
    <Section title="THE SCREEN" hint={b ? "Saved on the board itself." : "Connect the board to change these."}>
      <label className="field">Brightness
        <input type="range" min="0" max={b ? b.briCount - 1 : 7} step="1" disabled={!b}
               key={"bri" + (b ? b.bri : "x")} defaultValue={b ? b.bri : 0}
               onChange={(e) => sendLine("F:bri," + e.target.value)} />
      </label>
      <label className="field">Volume
        <input type="range" min="0" max={b ? b.volCount - 1 : 6} step="1" disabled={!b}
               key={"vol" + (b ? b.vol : "x")} defaultValue={b ? b.vol : 0}
               onChange={(e) => sendLine("F:vol," + e.target.value)} />
      </label>
      <div className="field row">Sound
        <div className="seg" role="group" aria-label="Sound">
          {[["Off", 0], ["Voice", 1], ["All", 2]].map(([n, v]) => (
            <button key={v} disabled={!b} aria-pressed={!!b && b.snd === v} onClick={() => sendLine("F:snd," + v)}>{n}</button>
          ))}
        </div>
      </div>
      <div className="field row">
        <select value={face} disabled={!linked} onChange={(e) => setFace(+e.target.value)} aria-label="Face">
          {s.faces.map(([id, name]) => <option key={id} value={id}>#{id} {name}</option>)}
        </select>
        <button className="btn small" disabled={!linked} onClick={() => sendLine("F:face," + face)}>Show it</button>
      </div>
      <div className="tries">
        {TRIES.map(([n, line]) => <button key={n} className="btn small ghost" disabled={!linked} onClick={() => sendLine(line)}>{n}</button>)}
      </div>
    </Section>
  );
}

function LyricsCard({ s }) {
  const [v, setV] = useState(s.settings.lyrics_offset);
  useEffect(() => setV(s.settings.lyrics_offset), [s.settings.lyrics_offset]);
  const label = v === 0 ? "On time" : v > 0 ? `${v.toFixed(1)} s earlier` : `${(-v).toFixed(1)} s later`;
  return (
    <Section title="LYRICS TIMING" hint="Lines arriving late? Slide towards earlier.">
      <input type="range" min="-2" max="2" step="0.1" value={v} aria-label="Lyrics timing"
             onChange={(e) => setV(+e.target.value)} onMouseUp={() => saveSettings({ lyrics_offset: v })}
             onTouchEnd={() => saveSettings({ lyrics_offset: v })} onKeyUp={() => saveSettings({ lyrics_offset: v })} />
      <div className="scale"><span>later</span><strong>{label}</strong><span>earlier</span></div>
    </Section>
  );
}

function NotifyCard({ s }) {
  const [apps, setApps] = useState(s.settings.notify_apps);
  const [saved, setSaved] = useState(false);
  return (
    <Section title="CHAT APPS" hint="Windows notifications from these apps go to the board. Separate them with commas.">
      <div className="field row">
        <input type="text" value={apps} onChange={(e) => setApps(e.target.value)} aria-label="Apps" />
        <button className="btn small" onClick={async () => { await saveSettings({ notify_apps: apps }); setSaved(true); setTimeout(() => setSaved(false), 1800); }}>Save</button>
      </div>
      {saved && <span className="saved">Saved</span>}
    </Section>
  );
}

function CalendarCard({ s }) {
  const [url, setUrl] = useState("");
  const ics = s.settings.ics;
  const add = () => {
    const u = url.trim();
    if (!/^(https?|webcal):\/\//i.test(u)) return;
    saveSettings({ ics: [...ics, u] }); setUrl("");
  };
  return (
    <Section title="CALENDAR" hint={'For meeting alerts. Google Calendar: Settings, your calendar, "Secret address in iCal format".'}>
      <ul className="links">
        {ics.length ? ics.map((u, i) => (
          <li key={u}><code>{u}</code><button className="btn small ghost" onClick={() => saveSettings({ ics: ics.filter((_, k) => k !== i) })}>Remove</button></li>
        )) : <li className="hint">No calendar links yet.</li>}
      </ul>
      <div className="field row">
        <input type="url" value={url} placeholder="https://calendar.google.com/calendar/ical/…" onChange={(e) => setUrl(e.target.value)}
               onKeyDown={(e) => e.key === "Enter" && add()} aria-label="New calendar link" />
        <button className="btn small" onClick={add}>Add</button>
      </div>
    </Section>
  );
}
