#!/usr/bin/env python3
"""Reduce triage JSONL (store.jsonl or alerts.jsonl) to comparable rows.

Prints one tab-separated row per record, sorted by seq:
    seq  primary  labels  severity
where primary is '-' for a line without labels and labels are the label names
sorted alphabetically and joined by ',' ('-' if none). Used by the end-to-end
check (tools/run_e2e.sh) and by the run-equivalence check (T4.6).

usage: jsonl_summary.py FILE
"""
import json
import sys


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    rows = []
    with open(argv[1], "r", encoding="utf-8") as f:
        for lineno, text in enumerate(f, 1):
            try:
                rec = json.loads(text)
                rows.append((rec["seq"], rec["primary"] or "-",
                             ",".join(sorted(rec["labels"])) or "-", rec["severity"]))
            except (ValueError, KeyError, TypeError) as e:
                print(f"{argv[1]}:{lineno}: {e}", file=sys.stderr)
                return 1
    for seq, primary, labels, severity in sorted(rows):
        print(f"{seq}\t{primary}\t{labels}\t{severity}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
