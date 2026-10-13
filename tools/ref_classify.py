#!/usr/bin/env python3
"""Reference classifier: an independent Python implementation of spec §3.4, §5.1-5.3 and
the correlator of §6, used to derive and check tests/expected/*.expected.

It shares nothing with the C code except the rule file and the spec: lines are normalized
as in §4.2 notes (%xx decoding, '+' -> space, control characters dropped), rules are
matched with Python's `re` on bytes, validators (§4.3) run on every candidate span,
context keywords are a case-insensitive substring test, then per-rule scores, noisy-OR
per category, tau, and the primary/severity rules of §5.3. The correlator (§6) runs over
the lines in order with the windows documented in src/correlate.h (BRUTE_FORCE,
PASSWORD_SPRAY, FAIL_THEN_SUCCESS, ERROR_STORM) on log time (syslog / ISO-8601 at the
start of the line, else the time the script started), with its own timestamp parser and
IP / user extraction.

usage: ref_classify.py [-r RULES] [--tau T] [--explain] [--no-correlate] LOGFILE
Prints "seq<TAB>primary<TAB>labels" per stream line (labels sorted, '-' if none);
--explain adds severity, confidence of the primary, surviving rule ids, and the
escalation flags ('-' if none)."""
import argparse
import calendar
import re
import sys
import time

CATS = ["AUTH", "SEC", "ERR", "PERF", "PII"]
LABEL = {"AUTH": "AUTH_FAILURE", "SEC": "SECURITY_THREAT", "ERR": "ERROR",
         "PERF": "PERF_WARNING", "PII": "PII"}
SEVS = ["INFO", "LOW", "MEDIUM", "HIGH", "CRITICAL"]
TIE = ["SEC", "PII", "AUTH", "ERR", "PERF"]          # §5.3 tie order, first wins
EPS = 1e-6                                           # base values are stored as float in C

V_D = [[0,1,2,3,4,5,6,7,8,9],[1,2,3,4,0,6,7,8,9,5],[2,3,4,0,1,7,8,9,5,6],
       [3,4,0,1,2,8,9,5,6,7],[4,0,1,2,3,9,5,6,7,8],[5,9,8,7,6,0,4,3,2,1],
       [6,5,9,8,7,1,0,4,3,2],[7,6,5,9,8,2,1,0,4,3],[8,7,6,5,9,3,2,1,0,4],
       [9,8,7,6,5,4,3,2,1,0]]
V_P = [[0,1,2,3,4,5,6,7,8,9],[1,5,7,6,2,8,3,0,9,4],[5,8,0,3,7,9,6,1,4,2],
       [8,9,1,6,0,4,3,5,2,7],[9,4,5,3,1,2,6,8,7,0],[4,2,8,6,5,7,3,9,0,1],
       [2,7,9,3,8,0,6,4,1,5],[7,0,4,6,9,1,3,2,5,8]]


def load_rules(path):
    rules = []
    for line in open(path, "rb").read().split(b"\n"):
        line = line.rstrip(b"\r")
        if not line or line.startswith(b"#"):
            continue
        cols = line.split(b"\t", 8)
        rid, cat, sev, base, flags, val, ext, ctx, rx = cols
        cat = cat.decode()
        rules.append({
            "id": rid.decode(), "cat": cat.rstrip("*"), "no_label": cat.endswith("*"),
            "sev": SEVS.index(sev.decode()), "base": float(base), "validator": val.decode(),
            "ctx": [] if ctx == b"-" else [c.lower() for c in ctx.split(b",")],
            "ip": b"ip" in ext.split(b","), "user": b"user" in ext.split(b","),
            "re": re.compile(rx, re.I if b"i" in flags.replace(b"-", b"") else 0),
        })
    return rules


def normalize(b):
    out = bytearray()
    i = 0
    hexd = b"0123456789abcdefABCDEF"
    while i < len(b):
        c = b[i]
        if c == 0x25 and len(b) - i >= 3 and b[i + 1] in hexd and b[i + 2] in hexd:
            c = int(b[i + 1:i + 3], 16)
            i += 3
        else:
            i += 1
            if c == 0x2B:
                c = 0x20
        if (c < 0x20 and c != 0x09) or c == 0x7F:
            continue
        out.append(c)
    return bytes(out)


def luhn(d):
    if not (13 <= len(d) <= 19 and d.isdigit()):
        return False
    total = 0
    for i, ch in enumerate(reversed(d)):
        x = int(ch)
        if i % 2:
            x = x * 2 - 9 if x * 2 > 9 else x * 2
        total += x
    return total % 10 == 0


def verhoeff(d):
    if not d or not d.isdigit():
        return False
    c = 0
    for i, ch in enumerate(reversed(d)):
        c = V_D[c][V_P[i % 8][int(ch)]]
    return c == 0


def pan(s):
    return re.fullmatch(r"[A-Z]{3}[ABCFGHJLPT][A-Z][0-9]{4}[A-Z]", s) is not None


def validate(kind, span):
    s = span.decode("latin-1").strip()
    s = re.sub(r"^[^0-9A-Za-z]+|[^0-9A-Za-z]+$", "", s).replace(" ", "").replace("-", "")
    if kind == "luhn":
        return luhn(s)
    if kind == "verhoeff":
        return len(s) == 12 and s[0] in "23456789" and verhoeff(s)
    if kind == "pan":
        return pan(s)
    return True


def spans(rx, line):
    """Every match start, restarting one byte after the previous start (as extract.c)."""
    pos = 0
    while pos <= len(line):
        m = rx.search(line, pos)
        if not m:
            return
        yield line[m.start():m.end()]
        pos = m.start() + 1


def evidence(rules, line):
    """§5.1/5.2: per-category noisy-OR confidence and max severity over surviving rules."""
    conf = {c: None for c in CATS}
    sev = {c: -1 for c in CATS}
    survivors = []
    for r in rules:
        if not r["re"].search(line):
            continue
        ok = r["validator"] == "-" or any(validate(r["validator"], s) for s in spans(r["re"], line))
        if not ok:
            continue                                  # §5.1: discard
        survivors.append(r["id"])
        if r["no_label"]:
            continue
        s = r["base"] + (0.25 if r["validator"] != "-" else 0.0)
        if any(k in line.lower() for k in r["ctx"]):
            s += 0.15
        s = min(max(s, 0.0), 1.0)
        c = r["cat"]
        conf[c] = s if conf[c] is None else 1 - (1 - conf[c]) * (1 - s)     # noisy-OR
        sev[c] = max(sev[c], r["sev"])
    return conf, sev, survivors


def decide(conf, sev, tau):
    """§5.3: labels, primary, line severity and the primary's confidence."""
    labels = [c for c in CATS if conf[c] is not None and conf[c] + EPS >= tau]
    primary = None
    for c in labels:
        if primary is None or (sev[c], conf[c], -TIE.index(c)) > (sev[primary], conf[primary], -TIE.index(primary)):
            primary = c
    return labels, primary, (SEVS[sev[primary]] if primary else "INFO"), (conf[primary] if primary else 0.0)


# ---- §6 correlator ----

MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
SYSLOG_RE = re.compile(rb"\[?(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) {1,2}([0-9]{1,2}) ([0-9]{2}):([0-9]{2}):([0-9]{2})")
ISO_RE = re.compile(rb"\[?([0-9]{4})-([0-9]{2})-([0-9]{2})[T ]([0-9]{2}):([0-9]{2}):([0-9]{2})"
                    rb"(?:[.,][0-9]+)?(?:(Z)|([+-])([0-9]{2}):?([0-9]{2})?)?")


def epoch(y, mo, d, h, mi, s):
    if not (1 <= mo <= 12) or d < 1 or d > calendar.monthrange(y, mo)[1] or h > 23 or mi > 59 or s > 60:
        return None
    return calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))


def log_time(raw, year):
    """Timestamp at the start of the raw line (spec §6), as UTC epoch seconds, or None."""
    m = ISO_RE.match(raw)
    if m:
        t = epoch(*(int(m.group(i)) for i in range(1, 7)))
        if t is not None and m.group(8):
            hh, mm = int(m.group(9)), int(m.group(10) or 0)
            if hh <= 23 and mm <= 59:
                t -= (1 if m.group(8) == b"+" else -1) * (hh * 3600 + mm * 60)
        return t
    m = SYSLOG_RE.match(raw)
    if m:
        return epoch(year, MONTHS.index(m.group(1).decode()) + 1, int(m.group(2)),
                     int(m.group(3)), int(m.group(4)), int(m.group(5)))
    return None


IPV4_RE = re.compile(rb"([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})")


def source_ip(span):
    """Last space-separated token of the span that is a strict dotted quad."""
    for tok in reversed(span.split(b" ")):
        m = IPV4_RE.fullmatch(tok)
        if m and all(int(g) <= 255 for g in m.groups()):
            return tok.decode()
    return None


def user_name(span, line):
    """The login name: 'invalid user X' / ' for X' in the span, 'sudo: X :' or a 'user=X' token."""
    tok = lambda b, stop=b" ": re.split(b"[" + re.escape(stop) + b"]", b, maxsplit=1)[0][:31]
    i = span.lower().find(b"invalid user ")
    if i >= 0:
        return tok(span[i + 13:])
    i = span.find(b" for ")
    if i >= 0:
        return tok(span[i + 5:])
    m = re.search(rb"sudo: ([^ ]*) : ", line)
    if m:
        return m.group(1)[:31]
    m = re.search(rb"(?:^|[ ;])user=([^ ;]*)", line)
    if m:
        return m.group(1)[:31]
    return b""


def entities(rules, line):
    """(source ip, user) from the first span of the first matched rule asking for each."""
    ip, user = None, b""
    for r in rules:
        m = r["re"].search(line)
        if not m:
            continue
        span = line[m.start():m.end()]
        if r["ip"] and ip is None:
            ip = source_ip(span)
        if r["user"] and not user:
            user = user_name(span, line)
    return ip, user


class Correlator:
    """Sliding windows of §6 with the defaults of src/correlate.h."""
    MAXRETRY, FINDTIME, SPRAY_USERS, SPRAY_WINDOW, STORM_LINES, STORM_WINDOW, RING = 5, 60, 5, 300, 100, 60, 64

    def __init__(self):
        self.fails = {}          # ip -> list of (time, user), last RING
        self.bf_last = {}
        self.spray_last = {}
        self.errors = []         # last STORM_LINES error times
        self.storm = False

    @staticmethod
    def within(t, now, w):
        return now - w <= t <= now

    def observe(self, now, auth_labelled, err_labelled, success, ip, user):
        flags = []
        if ip and auth_labelled:
            ring = self.fails.setdefault(ip, [])
            ring.append((now, user))
            del ring[:-self.RING]
            n = sum(self.within(t, now, self.FINDTIME) for t, _ in ring)
            if n >= self.MAXRETRY and (ip not in self.bf_last or now - self.bf_last[ip] >= self.FINDTIME):
                self.bf_last[ip] = now
                flags.append("BRUTE_FORCE")
            users = {u for t, u in ring if u and self.within(t, now, self.SPRAY_WINDOW)}
            if len(users) >= self.SPRAY_USERS and (ip not in self.spray_last or now - self.spray_last[ip] >= self.SPRAY_WINDOW):
                self.spray_last[ip] = now
                flags.append("PASSWORD_SPRAY")
        elif ip and success:
            n = sum(self.within(t, now, self.FINDTIME) for t, _ in self.fails.get(ip, []))
            if n >= self.MAXRETRY:
                flags.append("FAIL_THEN_SUCCESS")
        if err_labelled:
            self.errors.append(now)
            del self.errors[:-self.STORM_LINES]
            if sum(self.within(t, now, self.STORM_WINDOW) for t in self.errors) >= self.STORM_LINES:
                if not self.storm:
                    self.storm = True
                    flags.append("ERROR_STORM")
            else:
                self.storm = False
        return flags


ESCALATION = {"BRUTE_FORCE": ("SEC", 3), "PASSWORD_SPRAY": ("SEC", 3),
              "FAIL_THEN_SUCCESS": ("SEC", 4), "ERROR_STORM": ("ERR", 3)}     # category, severity
FLAG_NAME = {"BRUTE_FORCE": "ESCALATED_BRUTE_FORCE", "PASSWORD_SPRAY": "ESCALATED_PASSWORD_SPRAY",
             "FAIL_THEN_SUCCESS": "ESCALATED_FAIL_THEN_SUCCESS", "ERROR_STORM": "ERROR_STORM"}   # as in records


def pii_values(rules, line):
    """Values the PII rules found on a line (valid spans only), in every form that must not
    survive in any output sink: the span as written, with separators stripped, an e-mail's
    local part, and a key=value secret's value."""
    vals = set()
    for r in rules:
        if r["cat"] != "PII" or not r["re"].search(line):
            continue
        for sp in spans(r["re"], line):
            if r["validator"] != "-" and not validate(r["validator"], sp):
                continue
            t = sp.decode("latin-1")
            t = re.sub(r"^[^0-9A-Za-z]+|[^0-9A-Za-z]+$", "", t)
            if "=" in t:
                vals.add(t.split("=", 1)[1])
            elif "@" in t:
                vals.add(t.split("@", 1)[0])
            else:
                vals.add(t)
                vals.add(t.replace(" ", "").replace("-", ""))
    return {v for v in vals if len(v) >= 4}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-r", default="rules/default.rules")
    ap.add_argument("--tau", type=float, default=0.5)
    ap.add_argument("--explain", action="store_true")
    ap.add_argument("--no-correlate", action="store_true", help="leave out the correlator (§6)")
    ap.add_argument("--pii-values", action="store_true",
                    help="print the PII values found in the log, one per line, instead of the classification")
    ap.add_argument("log")
    a = ap.parse_args()
    rules = load_rules(a.r)
    data = open(a.log, "rb").read()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    if a.pii_values:
        found = set()
        for raw in lines:
            found |= pii_values(rules, normalize(raw.rstrip(b"\r")))
        for v in sorted(found):
            print(v)
        return
    corr = Correlator()
    start = int(time.time())
    year = time.gmtime(start).tm_year
    success_ids = {r["id"] for r in rules if r["cat"] == "AUTH" and r["no_label"]}
    for seq, raw in enumerate(lines):
        raw = raw.rstrip(b"\r")
        line = normalize(raw)
        conf, sevs, surv = evidence(rules, line)
        labels, primary, sev, pconf = decide(conf, sevs, a.tau)
        flags = []
        if not a.no_correlate and surv:
            ip, user = entities(rules, line)
            now = log_time(raw, year)
            flags = corr.observe(start if now is None else now, "AUTH" in labels, "ERR" in labels,
                                 bool(success_ids & set(surv)), ip, user)
            for f in flags:                           # §5.2: escalation -> conf 0.95
                c, s = ESCALATION[f]
                conf[c] = max(conf[c] or 0.0, 0.95)
                sevs[c] = max(sevs[c], s)
            if flags:
                labels, primary, sev, pconf = decide(conf, sevs, a.tau)
        row = f"{seq}\t{LABEL[primary] if primary else '-'}\t{','.join(sorted(LABEL[c] for c in labels)) or '-'}"
        if a.explain:
            row += f"\t{sev}\t{pconf:.4f}\t{','.join(surv) or '-'}\t{','.join(FLAG_NAME[f] for f in flags) or '-'}"
        print(row)


if __name__ == "__main__":
    main()
