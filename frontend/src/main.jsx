import React from "react";
import { createRoot } from "react-dom/client";
import "@fontsource/silkscreen/400.css";
import "@fontsource/silkscreen/700.css";
import "@fontsource/space-grotesk/400.css";
import "@fontsource/space-grotesk/500.css";
import "@fontsource/space-grotesk/700.css";
import "@fontsource/jetbrains-mono/400.css";
import "@fontsource/jetbrains-mono/500.css";
import "./styles.css";
import App from "./App.jsx";

// installable app: works offline, opens like an app from the home screen / dock
if (import.meta.env.PROD && "serviceWorker" in navigator) {
  window.addEventListener("load", () => navigator.serviceWorker.register("./sw.js").catch(() => {}));
}

// if anything ever trips while drawing, start the page over by itself (a few
// times) rather than leaving a blank screen until someone refreshes
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

createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <Recover />
  </React.StrictMode>
);
