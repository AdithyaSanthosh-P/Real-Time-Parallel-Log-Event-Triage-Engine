#!/usr/bin/env python3
"""Validate one bp_bench JSON object (stdin or file argument): required keys, types, and
sane values. Exit 0 if valid, 1 otherwise (message on stderr)."""
import json
import sys

VARIANTS = {"fork", "seq-recompile", "seq-precompiled", "threads-pattern", "threads-data", "dfa"}


def check(obj):
    errs = []

    def need(key, typ):
        if key not in obj:
            errs.append(f"missing key {key!r}")
        elif not isinstance(obj[key], typ) or isinstance(obj[key], bool):
            errs.append(f"{key!r} has type {type(obj[key]).__name__}")

    need("variant", str)
    for k in ("inputs", "pool", "threads", "warmup", "setup_ns"):
        need(k, int)
    for k in ("total_seconds", "inputs_per_s"):
        need(k, (int, float))
    need("cpus", list)
    need("mask_hash", str)
    if obj.get("variant") not in VARIANTS:
        errs.append(f"unknown variant {obj.get('variant')!r}")
    if isinstance(obj.get("inputs"), int) and obj["inputs"] <= 0:
        errs.append("inputs must be positive")
    if isinstance(obj.get("total_seconds"), (int, float)) and obj["total_seconds"] <= 0:
        errs.append("total_seconds must be positive")
    if isinstance(obj.get("cpus"), list) and not obj["cpus"]:
        errs.append("cpus is empty")
    lat = obj.get("latency_ns")
    if lat is not None:
        ks = ("p50", "p90", "p99", "min", "max")
        if not all(isinstance(lat.get(k), int) for k in ks):
            errs.append("latency_ns needs integer p50/p90/p99/min/max")
        elif not (lat["min"] <= lat["p50"] <= lat["p90"] <= lat["p99"] <= lat["max"]):
            errs.append("latency percentiles are not ordered")
    if obj.get("variant") == "fork":
        fk = obj.get("fork")
        if not isinstance(fk, dict) or not all(
                k in fk for k in ("mean_fork_to_collect_ns", "mean_child_regcomp_ns",
                                  "mean_child_regexec_ns", "incomplete_inputs")):
            errs.append("fork variant needs a 'fork' object with its breakdown")
    return errs


def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    try:
        obj = json.loads(text)
    except ValueError as e:
        print(f"invalid JSON: {e}", file=sys.stderr)
        return 1
    errs = check(obj)
    for e in errs:
        print(e, file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
