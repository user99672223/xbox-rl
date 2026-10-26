#!/usr/bin/env python3
"""Minimal mock of the Xbox Device Portal REST surface that tools/*.ps1 use.

Purpose: exercise deploy/run/logs/push-game/push-file/kill end-to-end on the laptop
(tools/test/test-tools.ps1) so scripting bugs are found before the console is involved.
It encodes OUR ASSUMPTIONS about Device Portal - it does not prove the real console
behaves this way. Plain HTTP on 127.0.0.1 (use XBOX_URL=http://127.0.0.1:11444).

Mock-only endpoints:  POST /mock/run   -> writes a fresh log.txt (simulating a tile press)
                      GET  /mock/state -> JSON dump of what was received
"""
import base64, json, re, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

USER, PASS = "willy", "mock"
PKGS = {}        # fullname -> {Name, Version:{..}, ...}
FILES = {}       # (dir, name) -> bytes length (content kept for small files)
CONTENT = {}
LOG = []         # request log
state = {"install_polls": 0}
lock = threading.Lock()

PROBE_LOG = """[    0.001 t0001] I log: log opened
[    0.002 t0001] I probe: ================ PROBE BEGIN ================
[    0.100 t0002] I probe: ---- rwx: OK (0.9 ms)
[    0.200 t0002] I probe: PROBE_RESULT sysinfo=OK rwx=OK alloc4g=OK d3d12=OK d3d11on12=OK | required=PASS
[    0.300 t0002] I probe: ================ PROBE END (300 ms) ================
[    5.000 t0001] I host: exiting application
"""

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def _auth(self):
        h = self.headers.get("Authorization", "")
        ok = h.startswith("Basic ") and base64.b64decode(h[6:]).decode() == f"{USER}:{PASS}"
        if not ok:
            self._send(401, b"")
        return ok

    def _send(self, code, body=b"", ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n) if n else b""

    def _route(self, method):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        with lock:
            LOG.append(f"{method} {u.path} {sorted(q)}")
        if u.path.startswith("/mock/"):
            if u.path == "/mock/run":
                with lock: FILES[("\\LocalState", "log.txt")] = len(PROBE_LOG); CONTENT[("\\LocalState", "log.txt")] = PROBE_LOG.encode()
                return self._send(200, {})
            if u.path == "/mock/state":
                with lock:
                    return self._send(200, {"packages": list(PKGS), "files": {f"{d}|{n}": s for (d, n), s in FILES.items()}, "log": LOG})
        if not self._auth(): return
        body = self._body()
        p = u.path
        if p == "/api/os/info":
            return self._send(200, {"ComputerName": "MOCK-XBOX"})
        if p == "/api/app/packagemanager/packages":
            with lock: return self._send(200, {"InstalledPackages": list(PKGS.values())})
        if p == "/api/app/packagemanager/certificate" and method == "POST":
            return self._send(200, {})
        if p == "/api/app/packagemanager/package" and method == "POST":
            names = re.findall(rb'filename="?([^";\r\n]+)"?', body)
            msix = [n.decode() for n in names if n.endswith(b".msix")]
            if not msix: return self._send(400, {"Reason": "no msix"})
            m = re.match(r"XboxRL_(\d+)\.(\d+)\.(\d+)\.(\d+)_x64\.msix", msix[0])
            ma, mi, bu, re_ = (int(x) for x in m.groups())
            full = f"xbox-rl.host_{ma}.{mi}.{bu}.{re_}_x64__mockpub"
            with lock:
                for k in list(PKGS): del PKGS[k]   # in-place update
                PKGS[full] = {"Name": "xbox-rl.host", "PackageFamilyName": "xbox-rl.host_mockpub", "PackageFullName": full,
                              "PackageRelativeId": "App", "Version": {"Major": ma, "Minor": mi, "Build": bu, "Revision": re_}}
                state["install_polls"] = 0
            LOG.append("  uploaded: " + ", ".join(n.decode() for n in names))
            return self._send(202, {})
        if p == "/api/app/packagemanager/state":
            with lock:
                state["install_polls"] += 1
                done = state["install_polls"] >= 2
            return self._send(200, {"Code": 0 if done else 1, "CodeText": "ok" if done else "installing", "Reason": "", "Success": done})
        if p == "/api/filesystem/apps/file":
            d, name = q.get("path", ""), q.get("filename", "")
            if method == "GET":
                with lock: data = CONTENT.get((d, name))
                if data is None: return self._send(404, {"Reason": "not found"})
                return self._send(200, data, "application/octet-stream")
            if method == "POST":
                m = re.search(rb'filename="?([^";\r\n]+)"?', body)
                fn = m.group(1).decode() if m else "unknown"
                with lock:
                    FILES[(d, fn)] = len(body);
                    if len(body) < 4096: CONTENT[(d, fn)] = body
                return self._send(200, {})
        if p == "/api/filesystem/apps/files":
            d = q.get("path", "")
            with lock:
                items = [{"Name": n, "Type": 32, "SizeInBytes": s} for (dd, n), s in FILES.items() if dd == d]
            return self._send(200, {"Items": items})
        if p == "/api/taskmanager/app":
            return self._send(403, {"Reason": "forbidden on Xbox"})
        self._send(404, {"Reason": "mock: unknown endpoint " + p})

    def do_GET(self): self._route("GET")
    def do_POST(self): self._route("POST")
    def do_DELETE(self): self._route("DELETE")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 11444
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
