#!/usr/bin/env python3
"""Summarize the raw `triage --bench` runs of one experiment (plan T6.5).

  bench_eval.py summarize EXP_DIR

EXP_DIR holds points.tsv (id, engine, workers, batch, rules, lines per point; written by
run_bench.sh) and raw/<id>__run<k>.json (one JSON object per run, as printed by --bench).
Writes EXP_DIR/summary.json: one row per point with median/min/max over the runs of
lines_per_s, mb_per_s, the latency percentiles, and the DFA size. Standard library only."""
import glob
import json
import os
import statistics
import sys


def agg(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def summarize(exp_dir):
    rows = []
    for line in open(os.path.join(exp_dir, "points.tsv")):
        pid, engine, workers, batch, rules, lines = line.rstrip("\n").split("\t")
        runs = []
        for p in sorted(glob.glob(os.path.join(exp_dir, "raw", pid + "__run*.json"))):
            with open(p) as f:
                runs.append(json.load(f))
        if not runs:
            continue
        for r in runs:
            if r["engine"] != engine or r["workers"] != int(workers) and engine != "fork" or r["lines"] != int(lines):
                raise SystemExit(f"bench_eval: {pid}: run does not match its point: {r['engine']} w{r['workers']} "
                                 f"{r['lines']} lines")
        row = {"id": pid, "engine": engine, "workers": int(workers), "batch": int(batch), "rules": rules,
               "lines": int(lines), "runs": len(runs),
               "lines_per_s": agg([r["lines_per_s"] for r in runs]),
               "mb_per_s": agg([r["mb_per_s"] for r in runs]),
               "seconds": agg([r["seconds"] for r in runs]),
               "dfa_states": runs[0]["dfa_states"], "dfa_groups": runs[0]["dfa_groups"],
               "build_ms": runs[0]["build_ms"]}
        x = row["lines_per_s"]
        row["spread_pct"] = (x["max"] - x["min"]) / x["median"] * 100       # run-to-run spread, % of the median
        for q in ("p50", "p90", "p99", "p99.9"):
            row["latency_" + q.replace(".", "_") + "_us"] = agg([r["latency_us"][q] for r in runs])
        rows.append(row)
    with open(os.path.join(exp_dir, "summary.json"), "w") as f:
        json.dump(rows, f, indent=1)
    return rows


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "summarize":
        raise SystemExit(__doc__)
    rows = summarize(sys.argv[2])
    for r in rows:
        x = r["lines_per_s"]
        warn = "   <-- spread over 15 %: the machine was not steady, rerun" if r["spread_pct"] > 15 else ""
        print(f"{r['id']:28s} {x['median']:12,.0f} lines/s  (min {x['min']:,.0f}, max {x['max']:,.0f}, {r['runs']} runs){warn}")


if __name__ == "__main__":
    main()
