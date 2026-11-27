import React from "react";
import { createRoot } from "react-dom/client";
import "@fontsource/space-grotesk/400.css";
import "@fontsource/space-grotesk/500.css";
import "@fontsource/space-grotesk/700.css";
import "@fontsource/jetbrains-mono/400.css";
import "@fontsource/jetbrains-mono/500.css";
import "./base.css";
import "./app.css";
import App from "./App.jsx";
import { isSpotifyPopup } from "./spotify.js";

// installable: opens like an app from the home screen, and works offline
if (import.meta.env.PROD && "serviceWorker" in navigator) {
  window.addEventListener("load", () => navigator.serviceWorker.register("./sw.js").catch(() => {}));
}

// if anything trips while drawing, start the page over by itself rather than staying blank
class Recover extends React.Component {
  state = { n: 0, broken: false };
  static getDerivedStateFromError() { return { broken: true }; }
  componentDidCatch(err) {
    console.error(err);
    if (this.state.n < 5) setTimeout(() => this.setState((s) => ({ n: s.n + 1, broken: false })), 800);
  }
  render() {
    if (!this.state.broken) return <App key={this.state.n} />;
    return this.state.n < 5 ? null : (
      <div className="crash"><p>Something went wrong on this page.</p>
        <button className="btn primary" onClick={() => location.reload()}>Reload</button></div>
    );
  }
}

createRoot(document.getElementById("root")).render(isSpotifyPopup
  ? <div className="crash"><p>Spotify connected. This window closes by itself.</p></div>
  : <React.StrictMode><Recover /></React.StrictMode>);
