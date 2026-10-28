#!/usr/bin/env python3
"""Compare triage's labels with the generator's ground truth (plan T6.3, spec §10.1).

usage: check_gen.py TRUTH.tsv STORE.jsonl
Prints per-kind agreement and exits 1 on any line whose label set differs from the truth
(lines whose truth is "-" must have no label; others must have exactly that one label)."""
import collections
import json
import sys

truth = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])]
bad = collections.Counter()
total = collections.Counter()
shown = 0
for t, l in zip(truth, open(sys.argv[2])):
    r = json.loads(l)
    seq, label, kind = int(t[0]), t[1], t[2]
    assert r["seq"] == seq, (r["seq"], seq)
    want = set() if label == "-" else {label}
    got = set(r["labels"])
    total[kind] += 1
    if got != want:
        bad[kind] += 1
        if shown < 8:
            shown += 1
            print(f"  seq {seq} {kind}: want {sorted(want)} got {sorted(got)} rules {r['rules']}: {r['line'][:110]}")
assert len(truth) == sum(total.values())
for k in sorted(total):
    print(f"  {k:28s} {total[k]:8d} lines, {bad[k]} differ")
print(f"check_gen: {sum(total.values())} lines, {sum(bad.values())} differ")
sys.exit(1 if bad else 0)
