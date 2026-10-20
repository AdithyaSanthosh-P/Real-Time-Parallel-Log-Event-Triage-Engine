#!/usr/bin/env python3
"""Per corpus file: escalations, alerts, PII quarantine records and PII-labelled lines of the current
bin/triage (dfa engine, 1 worker). Writes results/milestone2/correlator.json. Run `make` first."""
import collections
import json
import os
import shutil
import subprocess
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
res = []
for name in ("OpenSSH_2k", "BGL_2k", "HDFS_2k", "Linux_2k", "Apache_2k", "synthetic_small"):
    out = tempfile.mkdtemp()
    subprocess.run([os.path.join(ROOT, "bin/triage"), "--quiet", "-w", "1", "-o", out,
                    os.path.join(ROOT, "tests/corpus", name + ".log")], check=True)
    flags, pii, lines = collections.Counter(), 0, 0
    for l in open(os.path.join(out, "store.jsonl")):
        r = json.loads(l)
        lines += 1
        pii += "PII" in r["labels"]
        flags.update(r["flags"])
    n = lambda f: sum(1 for _ in open(os.path.join(out, f)))
    res.append({"file": name + ".log", "lines": lines, "flags": dict(flags), "alerts": n("alerts.jsonl"),
                "pii_quarantine": n("pii_quarantine.jsonl"), "lines_labelled_pii": pii})
    shutil.rmtree(out)
os.makedirs(os.path.join(ROOT, "results/milestone2"), exist_ok=True)
json.dump(res, open(os.path.join(ROOT, "results/milestone2/correlator.json"), "w"), indent=1)
print(json.dumps(res, indent=1))
