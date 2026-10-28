#!/usr/bin/env python3
"""Fetch the labelled web and auth logs of one AIT-LDS v2.0 scenario (plan T7.1, spec §10.1).

  fetch_ait.py [SCENARIO] [--out data/ait]          default scenario: russellmitchell

The scenario zips are 7-27 GB, almost all of it packet captures. The zip's central directory
and the members we need are read with HTTP range requests, so only a few megabytes are
downloaded: for every file under labels/ that is an Apache access/error log or an auth log,
the log itself (gather/<host>/logs/...) and its labels. Output, in OUT/<scenario>/:

  <host>__<logname>.log          the log, unchanged
  <host>__<logname>.truth.tsv    one row per line: seq (0-based), ATTACK or "-", AIT labels
                                 (comma list); a line is ATTACK if the AIT label file lists it

AIT labels are per line number (1-based) in a JSON-lines file; unlisted lines are normal.
Source: Landauer et al., AIT Log Data Set V2.0, Zenodo record 5789064 (CC BY 4.0).
Standard library only. Prints sizes, line counts and sha256."""
import hashlib
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

RECORD = "https://zenodo.org/records/5789064/files/{}.zip?download=1"
WANT = re.compile(r"(access\.log|error\.log|(^|/)auth\.log)[^/]*$")


class RangeFile(io.RawIOBase):
    """A read-only seekable file over an HTTP URL, fetched with Range requests."""

    def __init__(self, url):
        self.url, self.pos = url, 0
        with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=60) as f:
            self.size = int(f.headers["Content-Length"])

    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def read(self, n=-1):
        n = self.size - self.pos if n < 0 else min(n, self.size - self.pos)
        if n <= 0:
            return b""
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{self.pos + n - 1}"})
        with urllib.request.urlopen(req, timeout=120) as f:
            data = f.read()
        self.pos += len(data)
        return data

    def readinto(self, b):
        d = self.read(len(b))
        b[:len(d)] = d
        return len(d)


def main():
    args = [a for a in sys.argv[1:]]
    out = "data/ait"
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    scenario = args[0] if args else "russellmitchell"
    dest = os.path.join(out, scenario)
    os.makedirs(dest, exist_ok=True)
    z = zipfile.ZipFile(io.BufferedReader(RangeFile(RECORD.format(scenario)), 1 << 16))
    names = set(z.namelist())
    todo = sorted(n for n in names if n.startswith("labels/") and WANT.search(n) and not n.endswith("/"))
    if not todo:
        sys.exit(f"fetch_ait: no labelled access/error/auth logs in {scenario}")
    for lab in todo:
        rel = lab[len("labels/"):]                      # <host>/logs/.../<file>
        src = "gather/" + rel
        if src not in names:
            print(f"skip {rel}: no log at {src}", file=sys.stderr)
            continue
        host, name = rel.split("/")[0], os.path.basename(rel)
        base = os.path.join(dest, f"{host}__{name}")
        log = z.read(src)
        with open(base + ".log", "wb") as f:
            f.write(log)
        nlines = log.count(b"\n") + (0 if log.endswith(b"\n") or not log else 1)
        attack = {}
        for l in z.read(lab).decode("utf-8", "replace").splitlines():
            if l.strip():
                r = json.loads(l)
                attack[r["line"]] = ",".join(sorted(r["labels"])) or "attack"
        with open(base + ".truth.tsv", "w") as f:
            for seq in range(nlines):
                lab_s = attack.get(seq + 1)
                f.write(f"{seq}\t{'ATTACK' if lab_s else '-'}\t{lab_s or 'normal'}\n")
        bad = [n for n in attack if n < 1 or n > nlines]
        print(f"  {host}__{name:55s} {len(log):10d} bytes {nlines:8d} lines {len(attack):7d} attack lines"
              f"{' (' + str(len(bad)) + ' label line numbers out of range!)' if bad else ''}"
              f"  sha256 {hashlib.sha256(log).hexdigest()[:16]}...")
        if bad:
            sys.exit(1)


if __name__ == "__main__":
    main()
