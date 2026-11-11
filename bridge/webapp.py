"""
webapp.py - the Claude Buddy settings page, served by the bridge.

While the bridge runs, open http://localhost:47801 (or run `claude-buddy-settings`).
The page itself is a React app (frontend/), built into bridge/web/.

Only this computer can reach it: the server listens on 127.0.0.1, refuses
other host names (so other websites can't reach it through DNS tricks), and
every API call needs a per-run token. A page from another site can't read
that token, because browsers don't let one site read another site's replies.
"""
import json
import mimetypes
import os
import queue
import re
import secrets
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")
PORT = 47801
URL = f"http://localhost:{PORT}"
LINE_RE = re.compile(r"^[EFGHNOV]:[\x20-\x7e]{0,70}$")   # what the page may send to the board
TYPES = {".webmanifest": "application/manifest+json", ".bin": "application/octet-stream", ".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml",
         ".woff2": "font/woff2", ".woff": "font/woff", ".png": "image/png", ".ico": "image/x-icon"}


class WebApp:
    def __init__(self, port=PORT):
        self.port, self.url = port, f"http://localhost:{port}"
        self.token = secrets.token_hex(16)
        self.state = {}                       # filled by the bridge twice a second
        self.outbox = queue.Queue()           # lines for the board
        self.changes = queue.Queue()          # settings changes {key: value}
        self.actions = queue.Queue()          # "flash": put the newest firmware on the board
        self.ok, self.err = False, ""
        app = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _local(self):
                host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
                return host in ("localhost", "127.0.0.1")

            def _send(self, code, body, ctype="application/json"):
                data = body.encode("utf-8") if isinstance(body, str) else body
                self.send_response(code)
                self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith(("text", "application/json")) else ""))
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("X-Frame-Options", "DENY")
                self.end_headers()
                self.wfile.write(data)

            def _authorized(self):
                return self._local() and secrets.compare_digest(self.headers.get("X-Buddy-Token") or "", app.token)

            def _file(self, path):
                """A file of the built page, never anything outside bridge/web/."""
                rel = path.split("?", 1)[0].lstrip("/") or "index.html"
                full = os.path.realpath(os.path.join(WEB, rel))
                if not full.startswith(os.path.realpath(WEB) + os.sep) or not os.path.isfile(full):
                    return False
                ext = os.path.splitext(full)[1].lower()
                ctype = TYPES.get(ext) or mimetypes.guess_type(full)[0] or "application/octet-stream"
                with open(full, "rb") as f:
                    self._send(200, f.read(), ctype)
                return True

            def do_GET(self):
                if not self._local():
                    return self._send(403, '{"error":"open it as localhost"}')
                if self.path.startswith("/api/token"):
                    return self._send(200, json.dumps({"token": app.token}))
                if self.path.startswith("/api/state"):
                    if not self._authorized():
                        return self._send(403, '{"error":"reload the page"}')
                    return self._send(200, json.dumps(app.state))
                if not self._file(self.path):
                    if not self._file("/index.html"):
                        return self._send(404, "The settings page isn't built. Run: cd frontend && npm run build", "text/plain")

            def do_POST(self):
                if not self._authorized():
                    return self._send(403, '{"error":"reload the page"}')
                n = min(int(self.headers.get("Content-Length") or 0), 65536)
                try:
                    body = json.loads(self.rfile.read(n) or b"{}")
                except ValueError:
                    return self._send(400, '{"error":"bad request"}')
                if not isinstance(body, dict):
                    return self._send(400, '{"error":"bad request"}')
                if self.path == "/api/settings":
                    app.changes.put(body)
                    return self._send(200, '{"ok":true}')
                if self.path == "/api/firmware":
                    if body.get("action") != "flash":
                        return self._send(400, '{"error":"bad request"}')
                    app.actions.put("flash")
                    return self._send(200, '{"ok":true}')
                if self.path == "/api/board":
                    line = str(body.get("line", ""))
                    if not LINE_RE.match(line):
                        return self._send(400, '{"error":"not allowed"}')
                    app.outbox.put(line)
                    return self._send(200, '{"ok":true}')
                return self._send(404, '{"error":"not found"}')

        try:
            self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
            self.server.daemon_threads = True
            threading.Thread(target=self.server.serve_forever, daemon=True).start()
            self.ok = True
        except OSError as e:                  # port taken: the bridge still runs without the page
            self.err = str(e)


def open_page():
    """The claude-buddy-settings command."""
    webbrowser.open(URL)
    print(f"Opening {URL}  (the bridge must be running: claude-buddy)")
