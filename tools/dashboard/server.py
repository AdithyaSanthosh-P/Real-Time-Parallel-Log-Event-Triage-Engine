#!/usr/bin/env python3
"""Triage Desk: a live dashboard for bin/triage (standard library only).

    python3 tools/dashboard/server.py [--out out/live] [--port 8765] [--results results]

Open http://127.0.0.1:8765/ . The page has three views:

  Live         follows the three JSONL files (store, alerts, pii_quarantine) in --out while a run writes
               them, and updates every second: a strip chart per category, the feed of classified lines,
               alerts and escalations, rates, severity and label mix. Any run works: start `bin/triage
               -o out/live -` yourself, or press "Start" and the server runs a paced source for you
               (the generator, or a sample log replayed at a chosen rate).
  Experiments  the saved benchmark runs (results/e1..e4, results/basepaper): throughput, scaling,
               rule count, batch size, and the base paper's designs.
  Accuracy     the saved accuracy runs (generated truth, validators, AIT-LDS, AI-annotated real lines).

The server only reads the output files and saved results; the engine is not changed by it. Files written by
bin/triage are flushed after every batch when its input is a stream (stdin), so a paced source shows up
live. A run from a file is written in large blocks and appears in bursts.

Endpoints: GET /api/state, GET /events (server-sent events, one state per second), GET /api/results,
GET /api/sources, POST /api/run {source, rate, n}, POST /api/stop."""
import argparse
import collections
import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STATIC = os.path.join(os.path.dirname(__file__), "static")
CATS = ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"]
SEVS = ["INFO", "LOW", "MEDIUM", "HIGH", "CRITICAL"]
WINDOW = 180                      # seconds kept for the strip charts

SOURCES = {
    "gen": {"label": "Generated mix", "note": "four log types with injected attacks, errors, PII and look-alikes"},
    "OpenSSH_2k": {"label": "OpenSSH sample", "note": "real SSH log: brute-force and spray episodes", "file": "tests/corpus/OpenSSH_2k.log"},
    "BGL_2k": {"label": "BGL sample", "note": "supercomputer log: an error storm", "file": "tests/corpus/BGL_2k.log"},
    "synthetic_small": {"label": "Annotated sample", "note": "hand-written lines incl. PII and a brute-force run", "file": "tests/corpus/synthetic_small.log"},
}


class Tail:
    """Reads the new complete lines of one JSONL file; restarts when the file is replaced or truncated."""

    def __init__(self, path):
        self.path, self.pos, self.ino, self.partial = path, 0, None, b""

    def read(self):
        """-> (reset, [records]). reset is True when the file was replaced (a new run started)."""
        try:
            st = os.stat(self.path)
        except OSError:
            r = self.ino is not None
            self.pos, self.ino, self.partial = 0, None, b""
            return r, []
        reset = False
        if self.ino != st.st_ino or st.st_size < self.pos:
            reset = self.ino is not None
            self.pos, self.ino, self.partial = 0, st.st_ino, b""
        out = []
        if st.st_size > self.pos:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = self.partial + f.read(min(st.st_size - self.pos, 8 << 20))
            self.pos = st.st_size if len(data) >= st.st_size - self.pos else self.pos + len(data)
            lines = data.split(b"\n")
            self.partial = lines.pop()
            for l in lines:
                if l:
                    try:
                        out.append(json.loads(l))
                    except ValueError:
                        pass
        return reset, out


class State:
    def __init__(self, out_dir):
        self.dir = out_dir
        self.lock = threading.Lock()
        self.tails = {k: Tail(os.path.join(out_dir, f)) for k, f in
                      (("store", "store.jsonl"), ("alerts", "alerts.jsonl"), ("pii", "pii_quarantine.jsonl"))}
        self.proc = None
        self.run = {}
        self.reset()

    def reset(self):
        self.total = 0
        self.cat = collections.Counter()
        self.label = collections.Counter()
        self.sev = collections.Counter()
        self.flags = collections.Counter()
        self.rules = collections.Counter()
        self.alerts_n = 0
        self.pii_n = 0
        self.feed = collections.deque(maxlen=60)
        self.alerts = collections.deque(maxlen=40)
        self.sec = collections.OrderedDict()           # second -> {"n", "cat": [..5], "sev": [..5 max]}
        self.t0 = None
        self.last_at = None
        self.conf_bins = [0] * 10

    def poll(self):
        with self.lock:
            now = int(time.time())
            for key, tail in self.tails.items():
                reset, recs = tail.read()
                if reset and key == "store":
                    self.reset()
                if key == "store":
                    for r in recs:
                        self._add(r, now)
                elif key == "alerts":
                    self.alerts_n += len(recs)
                    for r in recs:
                        self.alerts.appendleft(self._slim(r))
                else:
                    self.pii_n += len(recs)
            while self.sec and next(iter(self.sec)) < now - WINDOW:
                self.sec.popitem(last=False)

    @staticmethod
    def _slim(r):
        return {"seq": r["seq"], "sev": r["severity"], "primary": r["primary"], "labels": r["labels"],
                "conf": r["confidence"], "flags": r["flags"], "rules": r["rules"], "line": r["line"][:220]}

    def _add(self, r, now):
        if self.t0 is None:
            self.t0 = time.time()
        self.last_at = time.time()
        self.total += 1
        self.sev[r["severity"]] += 1
        for l in r["labels"]:
            self.label[l] += 1
        for f in r["flags"]:
            self.flags[f] += 1
        for x in r["rules"]:
            self.rules[x] += 1
        if r["primary"]:
            self.cat[r["primary"]] += 1
            self.conf_bins[min(int(r["confidence"] * 10), 9)] += 1
        b = self.sec.get(now)
        if b is None:
            b = self.sec[now] = {"n": 0, "cat": [0] * 5, "sev": [0] * 5}
        b["n"] += 1
        sv = SEVS.index(r["severity"]) if r["severity"] in SEVS else 0
        for l in r["labels"]:
            if l in CATS:
                i = CATS.index(l)
                b["cat"][i] += 1
                b["sev"][i] = max(b["sev"][i], sv)
        if r["labels"] or r["flags"]:
            self.feed.appendleft(self._slim(r))
        elif self.total % 25 == 0:                       # a thin sample of unlabelled lines for context
            self.feed.appendleft(self._slim(r))

    def snapshot(self):
        self.poll()
        with self.lock:
            now = int(time.time())
            secs = []
            for t in range(now - WINDOW + 1, now + 1):
                b = self.sec.get(t)
                secs.append([t, b["n"], b["cat"], b["sev"]] if b else [t, 0, [0] * 5, [0] * 5])
            recent = sum(x[1] for x in secs[-4:-1]) / 3.0
            running = self.proc is not None and self.proc.poll() is None
            return {
                "now": now, "running": running, "run": self.run, "out": os.path.relpath(self.dir, ROOT),
                "idle_s": None if self.last_at is None else round(time.time() - self.last_at, 1),
                "total": self.total, "rate": round(recent, 1), "alerts": self.alerts_n, "pii": self.pii_n,
                "cat": {c: self.cat.get(c, 0) for c in CATS}, "label": {c: self.label.get(c, 0) for c in CATS},
                "sev": {s: self.sev.get(s, 0) for s in SEVS}, "flags": dict(self.flags),
                "rules": self.rules.most_common(8), "conf_bins": self.conf_bins,
                "series": secs, "feed": list(self.feed), "alert_feed": list(self.alerts),
                "elapsed": 0 if self.t0 is None else round(time.time() - self.t0, 1),
            }

    # ---- running a paced source ----
    def start(self, source, rate, n):
        self.stop()
        if source not in SOURCES:
            raise ValueError("unknown source")
        rate = max(1, min(int(rate), 200000))
        triage = os.path.join(ROOT, "bin", "triage")
        if not os.path.exists(triage):
            raise RuntimeError("bin/triage is missing: run make first")
        os.makedirs(self.dir, exist_ok=True)
        for f in ("store.jsonl", "alerts.jsonl", "pii_quarantine.jsonl"):     # a clean start for the page
            try:
                os.remove(os.path.join(self.dir, f))
            except OSError:
                pass
        cmd = [triage, "--quiet", "-w", "2", "--batch-delay", "20", "-o", self.dir, "-"]
        if source == "gen":
            gen = os.path.join(ROOT, "bin", "gen")
            if not os.path.exists(gen):
                raise RuntimeError("bin/gen is missing: run make first")
            src = subprocess.Popen([gen, "-n", str(n), "--seed", str(int(time.time()) % 10000), "--rate", str(rate)],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=ROOT)
            self.proc = subprocess.Popen(cmd, stdin=src.stdout, cwd=ROOT, stderr=subprocess.DEVNULL)
            src.stdout.close()
            self.feeder = src
        else:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, cwd=ROOT, stderr=subprocess.DEVNULL)
            path = os.path.join(ROOT, SOURCES[source]["file"])
            self.feeder = threading.Thread(target=self._pace, args=(self.proc, path, rate, n), daemon=True)
            self.feeder.start()
        self.run = {"source": source, "label": SOURCES[source]["label"], "rate": rate, "n": n, "started": time.time()}

    @staticmethod
    def _pace(proc, path, rate, n):
        """Replay a file at `rate` lines/s (absolute deadlines), looping until n lines have been sent."""
        with open(path, "rb") as f:
            lines = f.read().splitlines(keepends=True)
        t0, sent = time.monotonic(), 0
        try:
            while sent < n and proc.poll() is None:
                for l in lines:
                    if sent >= n or proc.poll() is not None:
                        break
                    proc.stdin.write(l)
                    sent += 1
                    if sent % max(1, rate // 50) == 0:
                        proc.stdin.flush()
                        delay = t0 + sent / rate - time.monotonic()
                        if delay > 0:
                            time.sleep(delay)
            proc.stdin.close()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def stop(self):
        p, self.proc = self.proc, None
        feeder = getattr(self, "feeder", None)
        if isinstance(feeder, subprocess.Popen) and feeder.poll() is None:
            feeder.terminate()
        if p is not None and p.poll() is None:
            try:
                p.stdin and p.stdin.close()
            except OSError:
                pass
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        self.run = {}


def load_results(res):
    def J(*p):
        try:
            with open(os.path.join(res, *p)) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None
    out = {k: J(k, "summary.json") for k in ("e1", "e2", "e3", "e4")}
    out["basepaper"] = {k: J("basepaper", k, "summary.json") for k in ("rb2", "rb3", "rb4")}
    out["e6"] = (J("e6", "synthetic", "result.json") or {}).get("all")
    out["e7"] = {"pii_on_logs": J("e7", "pii_on_logs.json"), "off": (J("e7", "synthetic_novalidators", "result.json") or {}).get("all")}
    out["ai100"] = (J("e6", "ai_100", "result.json") or {}).get("vs_A")
    out["loghub"] = J("e6", "loghub_full.json")
    out["ait"] = {}
    ait = os.path.join(res, "e6", "ait")
    if os.path.isdir(ait):
        for d in sorted(os.listdir(ait)):
            r = J("e6", "ait", d, "result.json") if os.path.isdir(os.path.join(ait, d)) else None
            if r:
                out["ait"][d] = r["all"]["categories"]["ATTACK"] | {"lines": r["all"]["n"]}
    out["machine"] = None
    try:
        for l in open(os.path.join(res, "machine.txt")):
            if l.startswith("Model name:"):
                out["machine"] = l.split(":", 1)[1].strip()
    except OSError:
        pass
    return out


class Handler(BaseHTTPRequestHandler):
    state = None
    results_dir = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/state":
            return self._send(200, self.state.snapshot())
        if path == "/api/results":
            return self._send(200, load_results(self.results_dir))
        if path == "/api/sources":
            return self._send(200, SOURCES)
        if path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(b"data: " + json.dumps(self.state.snapshot()).encode() + b"\n\n")
                    self.wfile.flush()
                    time.sleep(1)
            except (BrokenPipeError, ConnectionResetError, OSError):
                return
        name = {"/": "index.html"}.get(path, path.lstrip("/"))
        full = os.path.normpath(os.path.join(STATIC, name))
        if not full.startswith(STATIC + os.sep) or not os.path.isfile(full):
            return self._send(404, {"error": "not found"})
        ctype = {"html": "text/html; charset=utf-8", "css": "text/css", "js": "text/javascript"}.get(name.rsplit(".", 1)[-1], "text/plain")
        with open(full, "rb") as f:
            body = f.read()
        if name == "index.html":        # first paint without a round trip: the page starts with the current state
            boot = json.dumps({"state": self.state.snapshot(), "results": load_results(self.results_dir)}).replace("</", "<\\/")
            body = body.replace(b"<!--BOOT-->", b'<script id="boot" type="application/json">' + boot.encode() + b"</script>")
        self._send(200, body, ctype)

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self._send(400, {"error": "bad json"})
        try:
            if self.path == "/api/run":
                self.state.start(body.get("source", "gen"), body.get("rate", 400), int(body.get("n", 1000000)))
                return self._send(200, {"ok": True})
            if self.path == "/api/stop":
                self.state.stop()
                return self._send(200, {"ok": True})
        except (ValueError, RuntimeError) as e:
            return self._send(400, {"error": str(e)})
        self._send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "live"))
    ap.add_argument("--results", default=os.path.join(ROOT, "results"))
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    Handler.state = State(os.path.abspath(a.out))
    Handler.results_dir = os.path.abspath(a.results)
    def poller():                       # the files are read 4 times a second, whether or not a page is open
        while True:
            try:
                Handler.state.poll()
            except Exception as e:      # a half-written line or a vanished file must not stop the dashboard
                print("poll:", e, file=sys.stderr)
            time.sleep(0.25)
    threading.Thread(target=poller, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    signal.signal(signal.SIGTERM, lambda *_: (Handler.state.stop(), sys.exit(0)))
    print(f"Triage Desk on http://{a.host}:{a.port}/  (following {os.path.relpath(a.out, ROOT)})", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        Handler.state.stop()


if __name__ == "__main__":
    main()
