#!/usr/bin/env python3
"""Validate triage JSONL output (spec section 7).

Every line of every file given must be a JSON object with the record fields
and types written by src/route.c. Prints file:line for each problem and
exits 1 if there was any; exits 0 (and prints a one-line summary per file)
otherwise.

usage: check_jsonl.py FILE [FILE ...]
"""
import json
import sys

LABELS = {"AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"}
SEVERITIES = {"INFO", "LOW", "MEDIUM", "HIGH", "CRITICAL"}
FIELDS = ["seq", "ts", "primary", "severity", "confidence", "labels", "rules", "flags", "line"]


def check_record(rec):
    """Return a list of problems with one decoded record."""
    if not isinstance(rec, dict):
        return ["not a JSON object"]
    problems = []
    if list(rec.keys()) != FIELDS:
        problems.append(f"fields are {list(rec.keys())}, expected {FIELDS}")
        return problems
    if not isinstance(rec["seq"], int) or isinstance(rec["seq"], bool) or rec["seq"] < 0:
        problems.append("seq is not a non-negative integer")
    if not (isinstance(rec["ts"], str) and len(rec["ts"]) == 19 and rec["ts"][10] == "T"):
        problems.append("ts is not YYYY-MM-DDTHH:MM:SS")
    if rec["primary"] is not None and rec["primary"] not in LABELS:
        problems.append(f"unknown primary {rec['primary']!r}")
    if rec["severity"] not in SEVERITIES:
        problems.append(f"unknown severity {rec['severity']!r}")
    if not isinstance(rec["confidence"], (int, float)) or not 0 <= rec["confidence"] <= 1:
        problems.append("confidence is not a number in [0, 1]")
    if not (isinstance(rec["labels"], list) and set(rec["labels"]) <= LABELS
            and len(set(rec["labels"])) == len(rec["labels"])):
        problems.append("labels is not a list of distinct known labels")
    elif (rec["primary"] is None) != (len(rec["labels"]) == 0):
        problems.append("primary and labels disagree about whether the line has a label")
    elif rec["primary"] is not None and rec["labels"][0] != rec["primary"]:
        problems.append("labels[0] is not the primary")
    if not (isinstance(rec["rules"], list) and all(isinstance(r, str) for r in rec["rules"])):
        problems.append("rules is not a list of strings")
    if not (isinstance(rec["flags"], list) and all(isinstance(f, str) for f in rec["flags"])):
        problems.append("flags is not a list of strings")
    if not isinstance(rec["line"], str):
        problems.append("line is not a string")
    return problems


def check_file(path):
    count = 0
    bad = 0
    with open(path, "r", encoding="utf-8", newline="\n") as f:
        for lineno, text in enumerate(f, 1):
            if not text.endswith("\n"):
                print(f"{path}:{lineno}: missing newline at end of line")
                bad += 1
            body = text.rstrip("\n")
            try:
                rec = json.loads(body)
            except ValueError as e:
                print(f"{path}:{lineno}: invalid JSON: {e}")
                bad += 1
                continue
            for p in check_record(rec):
                print(f"{path}:{lineno}: {p}")
                bad += 1
            count += 1
    return count, bad


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    total_bad = 0
    for path in argv[1:]:
        try:
            count, bad = check_file(path)
        except (OSError, UnicodeDecodeError) as e:
            print(f"{path}: {e}")
            total_bad += 1
            continue
        total_bad += bad
        print(f"{path}: {count} records, {bad} problems")
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
