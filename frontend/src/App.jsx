import React, { useEffect, useRef, useState } from "react";
import { useBridge, sendLine, saveSettings, flashFirmware, updateApp, HOSTED } from "./api.js";
import { useDevice, canUsb, canBle } from "./device.js";
import Oled, { Thumb } from "./Oled.jsx";
import { LOGO_BODY, LOGO_PHONES } from "./logo.js";
import Welcome, { useWelcome } from "./Welcome.jsx";

/* ------------------------------------------------------------ small pieces */

// the logo: small Claude in pixels, headphones on (as on the board's chilling face)
function Mascot({ size = 30 }) {
  return (
    <svg className="mascot" width={size} height={size * 12 / 16} viewBox="0 0 16 12" shapeRendering="crispEdges" aria-hidden="true">
      <path d={LOGO_BODY} fill="var(--accent)" />
      <path d={LOGO_PHONES} fill="currentColor" />
    </svg>
  );
}

// the buddy as it's printed, drawn like a product sheet: box, screen window,
// little arms, four legs, the touch pad. The screen plays the firmware's own
// night-sky faces (headphones on while music plays) and follows the brightness.
function Device({ face, dim, level = 1 }) {
  return (
    <figure className={"device" + (dim ? " off" : "")} aria-hidden="true">
      <div className="dev">
        <i className="arm l" /><i className="arm r" />
        <div className="shell">
          <i className="screw a" /><i className="screw b" /><i className="screw c" /><i className="screw d" />
          <div className="window" style={{ "--lum": dim ? 0.35 : 0.45 + 0.55 * level }}>
            <Oled face={face} dim={dim} />
          </div>
          <span className="print">CLAUDE BUDDY</span>
          <i className="pad" />
        </div>
        <div className="legs"><i /><i /><i /><i /></div>
      </div>
    </figure>
  );
}

function Toggle({ checked, disabled, onChange, label }) {
  return (
    <label className={"switch" + (disabled ? " off" : "")}>
      <input type="checkbox" role="switch" checked={checked} disabled={disabled} aria-label={label}
             onChange={(e) => onChange(e.target.checked)} />
      <span />
    </label>
  );
}

// a stepped level, like the bars on a hardware meter: click a bar to set it
function Steps({ count, value, disabled, onChange, label }) {
  const [v, setV] = useState(value);
  useEffect(() => setV(value), [value]);
  return (
    <div className="steps" role="radiogroup" aria-label={label}>
      {Array.from({ length: count }, (_, i) => (
        <button key={i} role="radio" aria-checked={v === i} aria-label={`${label} ${i}`} disabled={disabled}
                className={i <= v && !disabled ? "on" : ""} style={{ "--h": 30 + (70 * i) / Math.max(1, count - 1) + "%" }}
                onClick={() => { setV(i); onChange(i); }} />
      ))}
      <output>{disabled ? "–" : `${v}/${count - 1}`}</output>
    </div>
  );
}

// every face as a picture: click one to preview it on the buddy, then show it
function FacePicker({ faces, value, onPick }) {
  const [open, setOpen] = useState(false);
  const box = useRef(null);
  useEffect(() => {
    if (!open) return;
    const off = (e) => { if (box.current && !box.current.contains(e.target)) setOpen(false); };
    const esc = (e) => { if (e.key === "Escape") setOpen(false); };
    document.addEventListener("pointerdown", off);
    document.addEventListener("keydown", esc);
    return () => { document.removeEventListener("pointerdown", off); document.removeEventListener("keydown", esc); };
  }, [open]);
  useEffect(() => {
    if (open) box.current?.querySelector(".pick-grid")?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, [open]);
  const cur = faces.find(([id]) => id === value);
  const tag = (id) => "#" + String(id).padStart(2, "0");
  return (
    <div className="picker" ref={box}>
      <button className="pick-btn" aria-haspopup="listbox" aria-expanded={open} onClick={() => setOpen(!open)}>
        {cur ? <Thumb face={cur[0]} /> : <span className="thumb empty" />}
        <span className="pick-name">{cur ? <><small>{tag(cur[0])}</small>{cur[1]}</> : "Choose a face"}</span>
        <span className="chev" aria-hidden="true">▾</span>
      </button>
      {open && (
        <div className="pick-grid" role="listbox" aria-label="Faces">
          {faces.map(([id, name]) => (
            <button key={id} role="option" aria-selected={id === value} onClick={() => { onPick(id); setOpen(false); }}>
              <Thumb face={id} />
              <span><small>{tag(id)}</small>{name}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

const Meter = ({ value }) => <div className="meter"><i style={{ width: Math.max(0, Math.min(100, value)) + "%" }} /></div>;
const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

function Panel({ n, title, hint, children }) {
  return (
    <section className="panel">
      <header>
        {n && <span className="num">{n}</span>}
        <div><h2>{title}</h2>{hint && <p>{hint}</p>}</div>
      </header>
      <div className="body">{children}</div>
    </section>
  );
}

function Row({ label, hint, children }) {
  return (
    <div className="row">
      <div className="row-text"><strong>{label}</strong>{hint && <small>{hint}</small>}</div>
      <div className="row-ctl">{children}</div>
    </div>
  );
}

/* ------------------------------------------------------------ data */

const TABS = [
  ["overview", "Overview", false],
  ["screen", "Screen & sound", false],
  ["reactions", "Reactions", true],      // true: needs Claude Buddy running on this computer
  ["music", "Music", true],
  ["alerts", "Alerts", true],
  ["setup", "Install & uninstall", false],
];

const REACTIONS = [
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

// faces you can pick when talking to the board directly (Claude Buddy sends its own list)
const FACES = [
  [0, "Idle"], [3, "Bright idea"], [12, "Coffee"], [13, "Music"], [14, "Headbang"], [15, "Dance"],
  [16, "Equalizer"], [18, "Love"], [19, "Petted"], [21, "Stargazing"], [22, "Dreaming"], [24, "Feeling great"],
  [25, "Not amused"], [26, "Oh no"], [27, "Hmmm"], [28, "Got it"], [29, "Whoa"], [35, "Trophy"],
  [36, "Goal"], [42, "Paper plane"], [45, "Goal celebration"], [46, "Bicycle kick"], [47, "Build crane"],
  [53, "Profile"], [59, "Plane throw"], [60, "Juggling three"], [64, "Record player"], [65, "Clock"],
  [67, "Fixing the wiring"], [68, "DJ set"], [69, "Night sky"], [70, "Chilling"],
];

const INSTALL = [
  ["Windows", "PowerShell", "irm https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.ps1 | iex"],
  ["macOS", "Terminal", "curl -LsSf https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/install.sh | sh"],
];

/* ------------------------------------------------------------ sections */

function Copy({ text }) {
  const [done, setDone] = useState(false);
  return (
    <button className="btn ghost" onClick={async () => {
      try { await navigator.clipboard.writeText(text); setDone(true); setTimeout(() => setDone(false), 1600); } catch {}
    }}>{done ? "Copied ✓" : "Copy"}</button>
  );
}

function Install({ n = "02", title = "Make it react to Claude Code" }) {
  return (
    <Panel n={n} title={title} hint="One line installs Claude Buddy on this computer. It passes Claude Code's activity to the board, starts by itself at every login, and this page finds it by itself.">
      {INSTALL.map(([os, app, cmd]) => (
        <div className="cmd" key={os}>
          <span className="label">{os} <em>{app}</em></span>
          <div className="cmdline"><code><span aria-hidden="true">›</span>{cmd}</code><Copy text={cmd} /></div>
        </div>
      ))}
    </Panel>
  );
}

// which face the board most likely shows for what Claude is doing (for the drawing)
const EVENT_FACE = {
  think: 2, read: 4, type: 5, write: 47, search: 31, web: 8, bash: 30, ps: 30, build: 47,
  git: 57, plane: 42, merge: 58, test: 9, need: 10, done: 55, fail: 39, bug: 24, compact: 11,
  pkg: 34, wait: 41, key: 44, limit: 38,
};
const isActive = (c) => !!c && c.ago < 45 && !["idle", "key", "done", "limit"].includes(c.ev);

function EqIcon({ on }) {
  return <span className={"eq" + (on ? " on" : "")} aria-hidden="true"><i /><i /><i /><i /></span>;
}

const splitTitle = (t) => {
  const i = (t || "").lastIndexOf(" - ");
  return i > 0 ? [t.slice(0, i), t.slice(i + 3)] : [t || "", ""];
};
const hm = (min) => (min >= 60 ? `${Math.floor(min / 60)}h ${String(min % 60).padStart(2, "0")}m` : `${min}m`);

/* the overview: music, usage, streak, today, to-dos and quick controls */
function Dashboard({ s, ctl }) {
  const m = s.music;
  const playing = m && m.state ? m.state : 0;
  const [song, artist] = splitTitle(m && m.title);
  const u = s.usage;
  const td = s.todos || { total: 0 };
  const b = ctl.linked ? ctl.board : null;
  return (
    <div className="dash">
      <div className={"card c6 music" + (playing ? "" : " idle")}>
        <div className="card-head"><span className="label">Now playing</span><EqIcon on={playing === 1} /></div>
        {playing ? (
          <>
            <div className="song"><strong>{song}</strong>{artist && <span>{artist}</span>}{playing === 2 && <em>Paused</em>}</div>
            {m.lyric && <p className="lyric">“{m.lyric}”</p>}
            {m.dur > 0 && (
              <div className="prog">
                <Meter value={(100 * m.pos) / m.dur} />
                <div><span>{mmss(m.pos)}</span><span>{mmss(m.dur)}</span></div>
              </div>
            )}
          </>
        ) : <p className="quiet">Nothing playing. Put on a song and your buddy dances along, with lyrics when it finds them.</p>}
      </div>

      <div className="card c3">
        <div className="card-head"><span className="label">Claude usage</span></div>
        {u && u.five >= 0 ? (
          <div className="bars">
            <div><span>5-hour</span><b>{u.five}%</b></div><Meter value={u.five} />
            {u.week >= 0 && <><div><span>Weekly</span><b>{u.week}%</b></div><Meter value={u.week} /></>}
            {u.reset > 0 && <small className="sub">5-hour limit resets in {hm(u.reset)}</small>}
          </div>
        ) : <p className="quiet">Shows up once Claude Code has run.</p>}
      </div>

      <div className="card c3">
        <div className="card-head"><span className="label">Streak</span></div>
        <span className="big-num">{s.streak}<small>day{s.streak === 1 ? "" : "s"}</small></span>
        <Meter value={(100 * s.badges) / Math.max(1, s.badgesTotal)} />
        <small className="sub">{s.badges} of {s.badgesTotal} badges</small>
      </div>

      <div className="card c4">
        <div className="card-head"><span className="label">Today</span></div>
        <div className="stats">
          <div><b>{s.today.commits}</b><span>commits</span></div>
          <div><b>{s.today.tasks}</b><span>tasks</span></div>
          <div><b>{s.today.tests}</b><span>tests passed</span></div>
        </div>
      </div>

      {td.total > 0 && (
        <div className="card c4">
          <div className="card-head"><span className="label">Claude's to-dos</span><span className="mono">{td.done}/{td.total}</span></div>
          <Meter value={(100 * td.done) / td.total} />
          <p className="todo">{td.current || "All done"}</p>
        </div>
      )}

      <div className={"card quick " + (td.total > 0 ? "c4" : "c8")}>
        <div className="card-head"><span className="label">Quick controls</span>{!b && <small className="sub">Connect the board</small>}</div>
        <div className="qrow"><span>Brightness</span>
          <Steps label="Brightness" count={b ? b.briCount : 8} value={b ? b.bri : 0} disabled={!b} onChange={(i) => ctl.send("F:bri," + i)} /></div>
        <div className="qrow"><span>Volume</span>
          <Steps label="Volume" count={b ? b.volCount : 7} value={b ? b.vol : 0} disabled={!b} onChange={(i) => ctl.send("F:vol," + i)} /></div>
        <div className="qrow"><span>Sound</span>
          <div className="seg" role="group" aria-label="Sound">
            {[["Off", 0], ["Voice", 1], ["All", 2]].map(([n, v]) => (
              <button key={v} disabled={!b} aria-pressed={!!b && b.snd === v} onClick={() => ctl.send("F:snd," + v)}>{n}</button>
            ))}
          </div></div>
      </div>
    </div>
  );
}

function Screen({ ctl, faces, pick, setPick }) {
  const b = ctl.linked ? ctl.board : null;
  return (
    <>
      <Panel n="01" title="Display and sound" hint={b ? "Saved on the board itself." : "Connect the board to change these."}>
        <Row label="Brightness">
          <Steps label="Brightness" count={b ? b.briCount : 8} value={b ? b.bri : 0} disabled={!b} onChange={(i) => ctl.send("F:bri," + i)} />
        </Row>
        <Row label="Volume">
          <Steps label="Volume" count={b ? b.volCount : 7} value={b ? b.vol : 0} disabled={!b} onChange={(i) => ctl.send("F:vol," + i)} />
        </Row>
        <Row label="Sound" hint="Voice: only Claude's reactions. All: every scene's sounds too.">
          <div className="seg" role="group" aria-label="Sound">
            {[["Off", 0], ["Voice", 1], ["All", 2]].map(([n, v]) => (
              <button key={v} disabled={!b} aria-pressed={!!b && b.snd === v} onClick={() => ctl.send("F:snd," + v)}>{n}</button>
            ))}
          </div>
        </Row>
      </Panel>
      <Panel n="02" title="Show a face" hint="Pick one to preview it on the buddy above. Show it puts it on the board for 20 seconds.">
        <div className="inline">
          <FacePicker faces={faces} value={pick} onPick={setPick} />
          <button className="btn primary" disabled={!ctl.linked || pick == null} onClick={() => ctl.send("F:face," + pick)}>Show it</button>
        </div>
      </Panel>
      <Panel n="03" title="Try it out" hint="Sends a sample to the board.">
        <div className="chips">
          {TRIES.map(([n, line]) => <button key={n} className="btn ghost" disabled={!ctl.linked} onClick={() => ctl.send(line)}>{n}</button>)}
        </div>
      </Panel>
    </>
  );
}

function Reactions({ s }) {
  return (
    <Panel n="01" title="What the board reacts to" hint="Changes apply right away and are remembered.">
      {REACTIONS.map(([key, name, desc]) => {
        const locked = s.locked.includes(key);
        return (
          <Row key={key} label={name} hint={locked ? "Turned off by a command-line option" : desc}>
            <Toggle label={name} checked={!!s.settings[key] && !locked} disabled={locked} onChange={(v) => saveSettings({ [key]: v })} />
          </Row>
        );
      })}
    </Panel>
  );
}

function Music({ s }) {
  const [v, setV] = useState(s.settings.lyrics_offset);
  useEffect(() => setV(s.settings.lyrics_offset), [s.settings.lyrics_offset]);
  const label = v === 0 ? "On time" : v > 0 ? `${v.toFixed(1)} s earlier` : `${(-v).toFixed(1)} s later`;
  const commit = () => saveSettings({ lyrics_offset: v });
  return (
    <>
      <Panel n="01" title="Music" hint="What plays on this computer shows on the board.">
        {[["music", "Music faces", "Record player, equalizer, DJ and chilling under the stars while a song plays"],
          ["lyrics", "Lyrics", "Synced lyrics, with small Claude hopping along"]].map(([key, name, desc]) => {
          const locked = s.locked.includes(key);
          return (
            <Row key={key} label={name} hint={locked ? "Turned off by a command-line option" : desc}>
              <Toggle label={name} checked={!!s.settings[key] && !locked} disabled={locked} onChange={(x) => saveSettings({ [key]: x })} />
            </Row>
          );
        })}
      </Panel>
      <Panel n="02" title="Lyrics timing" hint="Lines arriving late? Slide towards earlier.">
        <input type="range" min="-2" max="2" step="0.1" value={v} aria-label="Lyrics timing"
               onChange={(e) => setV(+e.target.value)} onMouseUp={commit} onTouchEnd={commit} onKeyUp={commit} />
        <div className="scale"><span>later</span><strong>{label}</strong><span>earlier</span></div>
      </Panel>
    </>
  );
}

function Alerts({ s }) {
  const [apps, setApps] = useState(s.settings.notify_apps);
  const [saved, setSaved] = useState(false);
  const [url, setUrl] = useState("");
  const ics = s.settings.ics;
  const add = () => {
    const u = url.trim();
    if (!/^(https?|webcal):\/\//i.test(u)) return;
    saveSettings({ ics: [...ics, u] }); setUrl("");
  };
  return (
    <>
      <Panel n="01" title="Chat apps" hint="Windows notifications from these apps go to the board. Separate them with commas.">
        <div className="inline">
          <input type="text" value={apps} onChange={(e) => setApps(e.target.value)} aria-label="Apps" />
          <button className="btn primary" onClick={async () => { await saveSettings({ notify_apps: apps }); setSaved(true); setTimeout(() => setSaved(false), 1800); }}>{saved ? "Saved ✓" : "Save"}</button>
        </div>
      </Panel>
      <Panel n="02" title="Calendar links" hint={'For meeting alerts. Google Calendar: Settings → your calendar → "Secret address in iCal format".'}>
        {ics.length > 0 && (
          <ul className="links">
            {ics.map((u, i) => (
              <li key={u}><code>{u}</code><button className="btn ghost" onClick={() => saveSettings({ ics: ics.filter((_, k) => k !== i) })}>Remove</button></li>
            ))}
          </ul>
        )}
        <div className="inline">
          <input type="url" value={url} placeholder="https://calendar.google.com/calendar/ical/…" onChange={(e) => setUrl(e.target.value)}
                 onKeyDown={(e) => e.key === "Enter" && add()} aria-label="New calendar link" />
          <button className="btn primary" onClick={add}>Add</button>
        </div>
      </Panel>
    </>
  );
}

/* everything about getting Claude Buddy on (and off) a computer */
const RAW = "https://raw.githubusercontent.com/SauravDutta002/Making-Claud/main/";
const SETUP = {
  win: { name: "Windows", app: "PowerShell", open: "Start menu → type PowerShell → Enter",
    install: `irm ${RAW}install.ps1 | iex`, uninstall: `irm ${RAW}uninstall.ps1 | iex` },
  mac: { name: "macOS", app: "Terminal", open: "Spotlight (⌘ Space) → type Terminal → Enter",
    install: `curl -LsSf ${RAW}install.sh | sh`, uninstall: `curl -LsSf ${RAW}uninstall.sh | sh` },
};
const STEPS = [
  ["Installs uv", "It brings its own Python, so you don't need Python already."],
  ["Installs Claude Buddy", "The claude-buddy commands listed below."],
  ["Connects Claude Code", "Adds hooks and a status line to ~/.claude/settings.json, after backing up the old file."],
  ["Offers a firmware update", "Only if the board is plugged in over USB. Say yes the first time."],
  ["Starts Claude Buddy", "In the background, right away and at every login. No window to keep open, and it restarts itself if it ever stops."],
];
const COMMANDS = [
  ["claude-buddy-service", "Starts Claude Buddy in the background (it already starts at login)"],
  ["claude-buddy", "The live dashboard in a terminal, instead of the background one"],
  ["claude-buddy-settings", "Opens this page"],
  ["claude-buddy-flash", "Puts the latest firmware on a board plugged in over USB (the Update firmware button on this page does the same)"],
  ["claude-buddy-setup", "Connects Claude Code again, e.g. after you reset its settings"],
  ["claude-buddy-setup --remove", "Disconnects Claude Code, keeps everything else"],
];

function Cmd({ text }) {
  return <div className="cmdline"><code><span aria-hidden="true">›</span>{text}</code><Copy text={text} /></div>;
}

function Setup() {
  const [os, setOs] = useState(() => /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent) ? "mac" : "win");
  const o = SETUP[os];
  return (
    <>
      <Panel n="01" title="Install" hint="You need Claude Code, and Windows 10/11 or macOS. One command does everything.">
        <div className="seg" role="group" aria-label="Your computer">
          {Object.entries(SETUP).map(([k, v]) => <button key={k} aria-pressed={os === k} onClick={() => setOs(k)}>{v.name}</button>)}
        </div>
        <div className="cmd">
          <span className="label">Open {o.app} <em>{o.open}</em></span>
          <Cmd text={o.install} />
        </div>
        <ol className="steps-list">
          {STEPS.map(([t, d]) => <li key={t}><strong>{t}</strong><small>{d}</small></li>)}
        </ol>
        <p className="callout">When it finishes, <strong>restart Claude Code</strong> (close and reopen any open sessions). Then plug the board in, or switch it on for Bluetooth.</p>
      </Panel>
      <Panel n="02" title="Connect the board">
        <Row label="USB" hint="Plug it into the computer. Claude Buddy finds it by itself." />
        <Row label="Bluetooth" hint={`Power it from anywhere. Claude Buddy finds it and reconnects if the link drops. Don't pair it in your system's Bluetooth settings.`} />
      </Panel>
      <Panel n="03" title="Update" hint="Claude Buddy updates itself: when a new version is out it installs it while you're not working with Claude Code, and keeps your stats, streaks and settings. New board firmware shows up on this page with an Update button. Running the install command again also updates it.">
        <Cmd text={o.install} />
      </Panel>
      <Panel n="04" title="Commands" hint="Type these in a terminal once Claude Buddy is installed.">
        <dl className="commands">
          {COMMANDS.map(([c, d]) => <div key={c}><dt><code>{c}</code></dt><dd>{d}</dd></div>)}
        </dl>
      </Panel>
      <Panel n="05" title="Uninstall" hint="Disconnects Claude Code and removes the commands, the autostart and the shortcuts.">
        <div className="cmd">
          <span className="label">{o.name} <em>{o.app}</em></span>
          <Cmd text={o.uninstall} />
        </div>
        <p className="callout">The board keeps its firmware, and your stats folder stays until you delete it{os === "win" ? <> (<code>%LOCALAPPDATA%\ClaudeBuddy</code>)</> : <> (<code>~/Library/Application Support/ClaudeBuddy</code>)</>}.</p>
      </Panel>
    </>
  );
}

/* how Claude Buddy reaches the board, and which firmware the board runs */
function Connection({ s }) {
  const locked = s.locked.includes("link");
  const mode = s.linkMode || "auto";
  const fw = s.firmware || {};
  return (
    <Panel n="01" title="Connection" hint={locked ? "Set by a command-line option." : "Claude Buddy connects by itself, at every login, and reconnects if the link drops."}>
      <Row label="Connect over" hint={{ auto: "USB when the cable is plugged in, Bluetooth otherwise. Plug the cable in and it switches over.",
        usb: "Only over the USB cable.", ble: "Only over Bluetooth, even with the cable plugged in." }[mode]}>
        <div className="seg" role="group" aria-label="Connect over">
          {[["auto", "Automatic"], ["usb", "USB"], ["ble", "Bluetooth"]].map(([k, n]) => (
            <button key={k} disabled={locked} aria-pressed={mode === k} onClick={() => saveSettings({ link: k })}>{n}</button>
          ))}
        </div>
      </Row>
      <Row label="Update automatically" hint={s.app && s.app.update
        ? `Claude Buddy ${s.app.latest} is out. It updates when Claude Code has been quiet for 10 minutes.`
        : `Claude Buddy ${s.version}${s.app && s.app.latest ? " (the newest)" : ""}. New versions install by themselves, while you're not working with Claude Code.`}>
        <Toggle label="Update automatically" checked={!!s.settings.auto_update} onChange={(v) => saveSettings({ auto_update: v })} />
      </Row>
      <Row label="Firmware" hint={fw.board == null ? "Connect the board to see its version."
        : fw.update ? `Version ${fw.latest} is ready to install.` : "Up to date."}>
        <span className="mono">{fw.board == null ? "–" : fw.board || "older"}{fw.latest && fw.board !== fw.latest ? ` → ${fw.latest}` : ""}</span>
      </Row>
    </Panel>
  );
}

/* a new Claude Buddy: it installs itself (or now, with the button) */
function AppBar({ s }) {
  const a = s.app || {};
  if (a.state === "updating")
    return <div className="fwbar busy" role="status"><div><strong>Updating Claude Buddy to {a.latest}…</strong>
      <small>It restarts by itself in about a minute; this page comes back on its own.</small></div></div>;
  if (a.state === "error" && a.msg)
    return <div className="fwbar err" role="alert"><div><strong>Claude Buddy didn't update</strong><small>{a.msg}</small></div>
      <button className="btn primary" onClick={updateApp}>Try again</button></div>;
  if (!a.update || !a.can) return null;
  return (
    <div className="fwbar">
      <div>
        <strong>Claude Buddy {a.latest} is out</strong>
        <small>{s.settings.auto_update ? "It installs itself once Claude Code has been quiet for 10 minutes. Or now:" : "Automatic updates are off."}</small>
      </div>
      <button className="btn primary" onClick={updateApp}>Update now</button>
    </div>
  );
}

/* new firmware: a banner on every tab, the button, then the progress */
function FirmwareBar({ s }) {
  const fw = s.firmware || {};
  const usb = s.link === "usb";
  if (fw.state === "flashing")
    return (
      <div className="fwbar busy" role="status">
        <div><strong>Updating the firmware…</strong><small>Keep the cable plugged in. The board restarts when it's done.</small></div>
        <div className="fwprog"><div className="meter"><i style={{ width: (fw.pct || 0) + "%" }} /></div><span>{fw.pct || 0}%</span></div>
      </div>
    );
  if (fw.state === "error" && fw.msg)
    return <div className="fwbar err" role="alert"><div><strong>Firmware not updated</strong><small>{fw.msg}</small></div>
      {fw.update && <button className="btn primary" disabled={!usb} onClick={flashFirmware}>Try again</button>}</div>;
  if (!fw.update) return null;
  return (
    <div className="fwbar">
      <div>
        <strong>New firmware for your buddy: {fw.latest}</strong>
        <small>{usb ? `It's on ${fw.board || "an older version"}. Takes about 20 seconds.` : "Connect the board with a USB cable to update it (Bluetooth can't flash)."}</small>
      </div>
      <button className="btn primary" disabled={!usb} onClick={flashFirmware}>Update firmware</button>
    </div>
  );
}

/* little notes in the corner: connected, disconnected, updated */
function useToasts() {
  const [list, setList] = useState([]);
  const push = (text, kind = "ok") => {
    const id = Date.now() + Math.random();
    setList((l) => [...l.slice(-2), { id, text, kind }]);
    setTimeout(() => setList((l) => l.filter((t) => t.id !== id)), 3800);
  };
  return [list, push];
}

/* the browser's "install this app" offer */
function useInstall() {
  const [ev, setEv] = useState(null);
  useEffect(() => {
    const on = (e) => { e.preventDefault(); setEv(e); };
    const done = () => setEv(null);
    window.addEventListener("beforeinstallprompt", on);
    window.addEventListener("appinstalled", done);
    return () => { window.removeEventListener("beforeinstallprompt", on); window.removeEventListener("appinstalled", done); };
  }, []);
  return ev && (async () => { ev.prompt(); await ev.userChoice.catch(() => {}); setEv(null); });
}

/* light (default) or dark, remembered on this device */
function useTheme() {
  const [t, setT] = useState(() => document.documentElement.dataset.theme || "light");
  useEffect(() => {
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem("buddy-theme", t); } catch {}
  }, [t]);
  return [t, () => setT(t === "dark" ? "light" : "dark")];
}

function useTab() {
  const read = () => {
    const h = location.hash.replace("#", "");
    return TABS.some(([id]) => id === h) ? h : "overview";
  };
  const [tab, setTab] = useState(read);
  useEffect(() => {
    const on = () => setTab(read());
    window.addEventListener("hashchange", on);
    return () => window.removeEventListener("hashchange", on);
  }, []);
  return tab;
}

/* ------------------------------------------------------------ the app */

export default function App() {
  const { state: s, down } = useBridge();
  const bridge = !!s && !down;
  const dev = useDevice(HOSTED && !bridge);
  const tab = useTab();
  const install = useInstall();
  const [welcome, endWelcome, replayWelcome] = useWelcome();
  const [theme, flipTheme] = useTheme();
  const [toasts, toast] = useToasts();

  // one way to reach the board, whichever is there: Claude Buddy on this computer, or a direct link
  const ctl = bridge
    ? { via: "bridge", linked: s.link === "usb" || s.link === "ble", board: s.board, send: sendLine }   // tabs show even while it's away
    : dev.link ? { via: dev.link.kind, linked: true, board: dev.cfg, send: dev.send } : null;
  const linked = !!ctl && ctl.linked;
  const [pick, setPick] = useState(null);          // a face picked on the Screen tab, previewed on the stage
  const b = linked ? ctl.board : null;
  const level = b && b.briCount > 1 ? b.bri / (b.briCount - 1) : 1;
  const tabs = TABS.filter(([, , needsBridge]) => !needsBridge || bridge);
  const shown = tabs.some(([id]) => id === tab) ? tab : "overview";
  const claude = bridge ? s.claude : null;
  const busy = isActive(claude);
  // the drawing: a face you picked, else what Claude is doing, else music / the night sky
  const face = shown === "screen" && pick != null ? pick
    : claude && claude.ev === "limit" && claude.ago < 3600 ? 38
    : busy && claude.ago < 20 && EVENT_FACE[claude.ev] != null ? EVENT_FACE[claude.ev]
    : bridge && s.music && s.music.state === 1 ? 70 : 69;
  const flashing = bridge && s.link === "flash";

  // "connected" / "disconnected" as it happens (and when the page first finds it)
  const linkKey = linked ? (ctl.via === "bridge" ? s.link + (s.port || "") : ctl.via) : flashing ? "flash" : "";
  const prevLink = useRef(null);
  useEffect(() => {
    const was = prevLink.current;
    prevLink.current = linkKey;
    if (linkKey === was || linkKey === "flash") return;
    if (linkKey) {
      const how = ctl.via === "bridge" ? (s.link === "usb" ? `over USB (${s.port})` : "over Bluetooth") : `over ${ctl.via === "usb" ? "USB" : "Bluetooth"}`;
      toast(`Connected ${how}`);
    } else if (was && was !== "flash") toast("Disconnected - looking for it…", "warn");
  }, [linkKey]);
  const updating = useRef(false);                  // the last we heard: Claude Buddy was updating itself
  if (bridge) updating.current = !!(s.app && s.app.state === "updating");
  const fwState = bridge && s.firmware ? s.firmware.state : "";
  const prevFw = useRef(fwState);
  useEffect(() => {
    if (prevFw.current === "flashing" && fwState === "done") toast(s.firmware.msg || "Firmware updated");
    prevFw.current = fwState;
  }, [fwState]);

  // what the stage says
  let status, title, sub;
  if (flashing) {
    status = "wait"; title = "Updating…";
    sub = `Putting new firmware on the board: ${s.firmware.pct || 0}%. Keep the cable plugged in.`;
  } else if (ctl && ctl.via === "bridge") {
    status = linked ? "ok" : "wait";
    title = linked ? "Connected" : "Looking for it…";
    sub = s.link === "usb" ? `Over USB on ${s.port}.`
      : s.link === "ble" ? "Over Bluetooth" + (s.rssi != null ? ` · signal ${s.rssi} dBm` : "") + "."
      : s.linkMode === "usb" ? "Plug it in with the USB cable: Claude Buddy finds it by itself."
      : "Plug it in with USB, or switch it on: Claude Buddy finds it over Bluetooth.";
  } else if (ctl) {
    status = "ok"; title = "Connected";
    sub = `Straight over ${ctl.via === "usb" ? "USB" : "Bluetooth"}, from this browser.`;
  } else if (HOSTED) {
    status = "wait"; title = "Say hi to your buddy";
    sub = !canUsb && !canBle
      ? "This browser can't reach the board. Open this page in Chrome or Edge on a computer, or install Claude Buddy below."
      : "Plug it in with USB or switch it on, then connect. If Claude Buddy is running on this computer, the page finds it by itself.";
  } else {
    status = down ? "bad" : "wait";
    title = down ? (updating.current ? "Updating Claude Buddy…" : "Claude Buddy isn't running") : "Connecting…";
    sub = !down ? "" : updating.current ? "Installing the new version. It starts again by itself in about a minute; this page comes back on its own."
      : "It starts by itself when you log in, and again within a minute if it ever stops. You can also start it now; this page comes back by itself.";
  }

  return (
    <div className="app">
      <header className="top">
        <a className="logo" href="#overview" aria-label="Claude Buddy">
          <Mascot />
          <span>Claude <b>Buddy</b></span>
        </a>
        <span className={"led " + status}><i />{flashing ? "Updating…" : status === "ok" ? (ctl.via === "bridge" ? (s.link === "usb" ? `USB · ${s.port}` : "Bluetooth") : `${ctl.via.toUpperCase()} · direct`) : status === "bad" ? "Offline" : "Not connected"}</span>
        <span className="end">
          {install && <button className="btn ghost sm" onClick={install}>Install app</button>}
          {bridge && <span className="ver">v{s.version}</span>}
          <button className="theme" onClick={flipTheme} aria-label={theme === "dark" ? "Light theme" : "Dark theme"} title={theme === "dark" ? "Light theme" : "Dark theme"}>
            {theme === "dark" ? "☀" : "☾"}
          </button>
        </span>
      </header>

      <section className="stage">
        <Device face={face} dim={!linked} level={level} />
        <div className="stage-text">
          <span className={"eyebrow " + status}>{status === "ok" ? "● Live" : "○ Standby"}</span>
          <h1>{title}</h1>
          {sub && <p>{sub}</p>}
          {bridge && !flashing && (
            <div className={"now" + (busy ? " busy" : "")}>
              <i aria-hidden="true" />
              {busy ? <span>Claude is <b>{claude.label}</b>{claude.doing && claude.doingAgo < 60 && <em>{claude.doing}</em>}</span>
                : claude && claude.ev === "limit" && claude.ago < 3600 ? <span>Claude hit the <b>usage limit</b></span>
                : <span>Claude Code is quiet</span>}
            </div>
          )}
          <div className="actions">
            {ctl ? (
              <>
                <button className="btn primary" disabled={!linked} onClick={() => ctl.send("F:hello")}>Say hello</button>
                <button className="btn ghost" disabled={!linked} onClick={() => ctl.send("G:1")}>Play a game</button>
                {ctl.via !== "bridge" && <button className="btn link" onClick={dev.disconnect}>Disconnect</button>}
              </>
            ) : !HOSTED && down ? (
              <a className="btn primary" href="claude-buddy://start">Start Claude Buddy</a>
            ) : HOSTED && (
              <>
                <button className="btn primary" disabled={!canUsb || dev.busy} onClick={dev.connectUsb}>Connect over USB</button>
                <button className="btn ghost" disabled={!canBle || dev.busy} onClick={dev.connectBle}>Connect over Bluetooth</button>
              </>
            )}
          </div>
          {dev.busy && <p className="note">Connecting…</p>}
          {dev.error && <p className="note err">{dev.error}</p>}
        </div>
      </section>

      {ctl ? (
        <>
          <nav className="tabs" aria-label="Settings">
            {tabs.map(([id, name], i) => (
              <a key={id} href={"#" + id} aria-current={shown === id ? "page" : undefined}>
                <span>{String(i + 1).padStart(2, "0")}</span>{name}
              </a>
            ))}
          </nav>
          <main className="content">
            {bridge && <AppBar s={s} />}
            {bridge && <FirmwareBar s={s} />}
            {shown === "overview" && (ctl.via === "bridge" ? <><Dashboard s={s} ctl={ctl} /><Connection s={s} /></> : <Install n="01" />)}
            {shown === "screen" && <Screen ctl={ctl} faces={(bridge && s.faces) || FACES} pick={pick} setPick={setPick} />}
            {shown === "reactions" && <Reactions s={s} />}
            {shown === "music" && <Music s={s} />}
            {shown === "alerts" && <Alerts s={s} />}
            {shown === "setup" && <Setup />}
          </main>
        </>
      ) : HOSTED && <main className="content"><Setup /></main>}

      {welcome && !HOSTED && (
        <Welcome s={bridge ? s : null} linked={linked} install={install}
                 onHello={() => ctl && ctl.send("F:hello")} onDone={endWelcome} />
      )}

      <div className="toasts" aria-live="polite">
        {toasts.map((t) => <div key={t.id} className={"toast " + t.kind}>{t.text}</div>)}
      </div>

      <footer className="foot">
        <span>Claude Buddy · a desk companion for Claude Code{!HOSTED && <> · <button className="linkish" onClick={replayWelcome}>Replay the welcome</button></>}</span>
        {bridge && <span>Settings stay on your computer, in <code>{s.data}</code></span>}
      </footer>
    </div>
  );
}
