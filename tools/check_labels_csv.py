#!/usr/bin/env python3
"""Check an annotator's answer (CSV with id and five 0/1 columns, e.g. from an LLM) against the label sheet and
write a sheet-shaped file for label_eval.py (plan T7.2/T7.3).

  check_labels_csv.py SHEET.csv ANSWER.csv --out LABELS.csv

Refuses: missing or extra ids, duplicate ids, a header that lacks a category, any cell other than 0 or 1.
The output keeps id, source and line from the sheet and takes the five category columns from the answer.
Standard library only."""
import argparse
import csv
import sys

CATS = ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sheet")
    ap.add_argument("answer")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sheet = list(csv.DictReader(open(a.sheet, newline="", encoding="utf-8")))
    text = open(a.answer, encoding="utf-8").read().strip()
    text = "\n".join(l for l in text.splitlines() if not l.strip().startswith("```"))     # tolerate a code fence
    rows = list(csv.DictReader(text.splitlines()))
    if not rows or any(c not in rows[0] for c in ["id"] + CATS):
        sys.exit("check_labels_csv: the answer needs the header id," + ",".join(CATS))
    ans = {}
    for r in rows:
        i = r["id"].strip()
        if i in ans:
            sys.exit(f"check_labels_csv: duplicate id {i}")
        vals = [r[c].strip() for c in CATS]
        if any(v not in ("0", "1") for v in vals):
            sys.exit(f"check_labels_csv: id {i}: every category must be 0 or 1, got {vals}")
        ans[i] = vals
    want = {r["id"] for r in sheet}
    if set(ans) != want:
        miss, extra = sorted(want - set(ans), key=int), sorted(set(ans) - want)
        sys.exit(f"check_labels_csv: missing ids {miss[:10]}, unknown ids {extra[:10]}")
    with open(a.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["id", "source", "line"] + CATS)
        for r in sheet:
            w.writerow([r["id"], r["source"], r["line"]] + ans[r["id"]])
    print(f"check_labels_csv: {len(sheet)} rows ok -> {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
