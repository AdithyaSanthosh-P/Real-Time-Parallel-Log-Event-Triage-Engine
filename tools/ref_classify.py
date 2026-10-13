#!/usr/bin/env python3
"""Reference classifier: an independent Python implementation of spec §3.4 and §5.1-5.3,
used to derive and check tests/expected/*.expected.

It shares nothing with the C code except the rule file and the spec: lines are normalized
as in §4.2 notes (%xx decoding, '+' -> space, control characters dropped), rules are
matched with Python's `re` on bytes, validators (§4.3) run on every candidate span,
context keywords are a case-insensitive substring test, then per-rule scores, noisy-OR
per category, tau, and the primary/severity rules of §5.3.

usage: ref_classify.py [-r RULES] [--tau T] [--explain] LOGFILE
Prints "seq<TAB>primary<TAB>labels" per stream line (labels sorted, '-' if none);
--explain adds severity, confidence of the primary, and surviving rule ids.
Correlator escalations (§6) are not modelled here."""
import argparse
import re
import sys

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
        rid, cat, sev, base, flags, val, _ext, ctx, rx = cols
        cat = cat.decode()
        rules.append({
            "id": rid.decode(), "cat": cat.rstrip("*"), "no_label": cat.endswith("*"),
            "sev": SEVS.index(sev.decode()), "base": float(base), "validator": val.decode(),
            "ctx": [] if ctx == b"-" else [c.lower() for c in ctx.split(b",")],
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


def classify(rules, line, tau):
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
    labels = [c for c in CATS if conf[c] is not None and conf[c] + EPS >= tau]
    primary = None
    for c in labels:
        if primary is None or (sev[c], conf[c], -TIE.index(c)) > (sev[primary], conf[primary], -TIE.index(primary)):
            primary = c
    return labels, primary, (SEVS[sev[primary]] if primary else "INFO"), (conf[primary] if primary else 0.0), survivors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-r", default="rules/default.rules")
    ap.add_argument("--tau", type=float, default=0.5)
    ap.add_argument("--explain", action="store_true")
    ap.add_argument("log")
    a = ap.parse_args()
    rules = load_rules(a.r)
    data = open(a.log, "rb").read()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    for seq, raw in enumerate(lines):
        labels, primary, sev, conf, surv = classify(rules, normalize(raw.rstrip(b"\r")), a.tau)
        row = f"{seq}\t{LABEL[primary] if primary else '-'}\t{','.join(sorted(LABEL[c] for c in labels)) or '-'}"
        if a.explain:
            row += f"\t{sev}\t{conf:.4f}\t{','.join(surv) or '-'}"
        print(row)


if __name__ == "__main__":
    main()
