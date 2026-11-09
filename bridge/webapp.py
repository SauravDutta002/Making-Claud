"""
webapp.py - the Claude Buddy settings page, served by the bridge.

While the bridge runs, open http://localhost:47801 (or run `claude-buddy-settings`).
Only this computer can reach it: the server listens on 127.0.0.1, refuses
other host names, and every API call needs a token that only the page knows.
"""
import json
import os
import queue
import re
import secrets
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 47801
URL = f"http://localhost:{PORT}"
LINE_RE = re.compile(r"^[EFGHNOV]:[\x20-\x7e]{0,70}$")   # what the page may send to the board


class WebApp:
    def __init__(self, port=PORT):
        self.port, self.url = port, f"http://localhost:{port}"
        self.token = secrets.token_hex(16)
        self.state = {}                       # filled by the bridge twice a second
        self.outbox = queue.Queue()           # lines for the board
        self.changes = queue.Queue()          # settings changes {key: value}
        self.ok, self.err = False, ""
        with open(os.path.join(HERE, "web", "settings.html"), encoding="utf-8") as f:
            self.page = f.read()
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
                self.send_header("Content-Type", ctype + "; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("X-Frame-Options", "DENY")
                self.end_headers()
                self.wfile.write(data)

            def _authorized(self):
                return self._local() and secrets.compare_digest(self.headers.get("X-Buddy-Token") or "", app.token)

            def do_GET(self):
                if not self._local():
                    return self._send(403, '{"error":"open it as localhost"}')
                if self.path in ("/", "/index.html"):
                    return self._send(200, app.page.replace("__TOKEN__", app.token), "text/html")
                if self.path == "/api/state":
                    if not self._authorized():
                        return self._send(403, '{"error":"reload the page"}')
                    return self._send(200, json.dumps(app.state))
                return self._send(404, '{"error":"not found"}')

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
