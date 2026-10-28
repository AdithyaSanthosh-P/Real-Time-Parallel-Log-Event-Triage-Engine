#!/usr/bin/env python3
"""Draw a stratified sample of log lines for hand labelling (plan T7.2, spec §10.1).

  sample_for_labeling.py --store NAME=PATH [--store NAME=PATH ...] [--n 1500] [--seed 1] [--out DIR]

PATH is a store.jsonl written by `triage`; NAME says which data set it came from. Lines are
grouped into strata by the engine's primary label (AUTH_FAILURE, SECURITY_THREAT, ERROR,
PERF_WARNING, PII) plus "none" (no rule labelled the line), and the sample takes an equal quota
from every stratum (a stratum with fewer lines gives all of them; the unused quota goes to the
others). Without this, "none" would be 99 % of the sample and the rare categories would have a
handful of lines. Because the strata are not proportional, evaluation must weight them back
(label_eval.py does, using the stratum sizes in key.csv).

Output in DIR (default out/labeling), all with the same line ids in a shuffled order:
  annotator_A.csv, annotator_B.csv   id, source, line, then one empty column per category.
                                     Fill 1 (the line belongs to the category) or 0. Label
                                     INDEPENDENTLY: do not look at the other's file or the key.
  key.csv                            id, source, seq, predicted primary and labels, confidence,
                                     stratum size (for weighting). Do not open before labelling.
The annotator files do not show the engine's prediction, so it cannot bias the labels. Lines
with masked personal data are shown masked (that is what the store holds).
Standard library only."""
import argparse
import csv
import json
import os
import random
import sys

CATS = ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--store", action="append", required=True, metavar="NAME=PATH")
    ap.add_argument("--n", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="out/labeling")
    args = ap.parse_args()

    strata = {c: [] for c in CATS + ["none"]}
    for spec in args.store:
        name, _, path = spec.partition("=")
        if not path:
            sys.exit(f"sample_for_labeling: --store wants NAME=PATH, got '{spec}'")
        with open(path, encoding="utf-8") as f:
            for l in f:
                r = json.loads(l)
                strata[r["primary"] or "none"].append((name, r))

    rng = random.Random(args.seed)
    # equal quota; give what a small stratum cannot use to the others
    quota = {k: 0 for k in strata}
    left = args.n
    open_ = [k for k in strata if strata[k]]
    while left > 0 and open_:
        share = max(left // len(open_), 1)
        for k in list(open_):
            take = min(share, len(strata[k]) - quota[k], left)
            quota[k] += take
            left -= take
            if quota[k] == len(strata[k]):
                open_.remove(k)
            if left == 0:
                break
    sample = []
    for k, items in strata.items():
        for name, r in rng.sample(items, quota[k]):
            sample.append((k, name, r))
    rng.shuffle(sample)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "key.csv"), "w", newline="") as kf:
        kw = csv.writer(kf)
        kw.writerow(["id", "source", "seq", "stratum", "stratum_size", "stratum_sampled", "primary", "labels", "confidence"])
        for i, (k, name, r) in enumerate(sample, 1):
            kw.writerow([i, name, r["seq"], k, len(strata[k]), quota[k], r["primary"] or "", ";".join(r["labels"]),
                         r["confidence"]])
    for who in ("A", "B"):
        with open(os.path.join(args.out, f"annotator_{who}.csv"), "w", newline="") as af:
            aw = csv.writer(af)
            aw.writerow(["id", "source", "line"] + CATS)
            for i, (k, name, r) in enumerate(sample, 1):
                aw.writerow([i, name, r["line"]] + [""] * len(CATS))
    print(f"sample_for_labeling: {len(sample)} lines -> {args.out}/annotator_A.csv, annotator_B.csv, key.csv", file=sys.stderr)
    for k in strata:
        print(f"  {k:16s} {len(strata[k]):9d} lines in the data, {quota[k]:5d} sampled", file=sys.stderr)


if __name__ == "__main__":
    main()
