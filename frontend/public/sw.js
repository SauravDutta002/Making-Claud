// Claude Buddy's service worker: the page opens even offline (it talks to the
// board over USB or Bluetooth, or to the helper on this computer, never the web).
// Pages: network first, so an update shows up at once. Files: cache first
// (their names change with every build). The API is never cached.
const CACHE = "claude-buddy-v2";
const SHELL = ["./", "faces.bin", "manifest.webmanifest", "icon-192.png"];

self.addEventListener("install", (e) => {
  self.skipWaiting();
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).catch(() => {}));
});

self.addEventListener("activate", (e) => {
  e.waitUntil((async () => {
    for (const k of await caches.keys()) if (k !== CACHE) await caches.delete(k);
    await self.clients.claim();
  })());
});

self.addEventListener("fetch", (e) => {
  const req = e.request;
  const url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== location.origin || url.pathname.includes("/api/")) return;
  if (req.mode === "navigate") {
    e.respondWith(fetch(req).then((r) => {
      const copy = r.clone();
      caches.open(CACHE).then((c) => c.put("./", copy));
      return r;
    }).catch(() => caches.match("./")));
    return;
  }
  if (!url.pathname.includes("/assets/")) {      // same name every build (faces, icons): network first
    e.respondWith(fetch(req).then((r) => {
      if (r.ok) { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(req, copy)); }
      return r;
    }).catch(() => caches.match(req)));
    return;
  }
  e.respondWith(caches.match(req).then((hit) => hit || fetch(req).then((r) => {
    if (r.ok) { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(req, copy)); }
    return r;
  })));
});
