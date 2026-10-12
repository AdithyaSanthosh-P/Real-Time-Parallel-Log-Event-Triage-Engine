#!/usr/bin/env python3
"""Evaluation helpers for Phase R (plan R4).

  bp_eval.py accuracy  --labels labeled.tsv --masks MASKS --mode none|paper --out accuracy.json
      Per-category precision / recall / F1 and overall accuracy of bp_fork's masks against the
      ground truth in labeled.tsv (see gen_bp.py: truth describes what a line IS).
      MASKS has one line per labeled line: 5 characters (EMAIL PHONE QUESTION GREETING NAME,
      '1' = matched) or ERR (rejected by --normalize=paper; counted as "predicted nothing").

  bp_eval.py summarize --dir results/basepaper
      Reads the raw files run_bp.sh wrote under rb1..rb5 and writes <dir>/rbN/summary.json
      with median, min and max over the runs of each point.
Standard library only."""
import argparse
import glob
import json
import os
import re
import statistics
import sys

CATS = ["EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"]


def div(a, b):
    return a / b if b else None


def f1_from_counts(tp, fp, fn):
    """F1 = 2TP / (2TP + FP + FN); 0 when there are errors but no true positive (so a pattern that
    never fires correctly scores 0, not 'undefined'); None only when the category never occurs
    and is never predicted."""
    d = 2 * tp + fp + fn
    return 2 * tp / d if d else None


# ---- accuracy (RB4) -------------------------------------------------------------------------

def load_labels(path):
    rows = []
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            if line.startswith("#"):
                continue
            cols = line.rstrip("\n").split("\t")
            rows.append((cols[0], set(c for c in cols[1].split(",") if c != "NONE"), cols[2]))
    return rows


def load_masks(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            out.append(None if line == "ERR" else {CATS[i] for i, ch in enumerate(line) if ch == "1"})
    return out


def prf(tp, fp, fn):
    p, r = div(tp, tp + fp), div(tp, tp + fn)
    return p, r, f1_from_counts(tp, fp, fn)


def accuracy(labels, masks, mode):
    if len(labels) != len(masks):
        raise SystemExit(f"{len(labels)} labeled lines but {len(masks)} mask lines")
    counts = {c: dict(tp=0, fp=0, fn=0, tn=0) for c in CATS}
    exact = 0
    rejected = 0
    tmpl = {}
    for (text, truth, t), pred in zip(labels, masks):
        if pred is None:
            rejected += 1
            pred = set()
        # confusion counts: truth in/out x prediction in/out
        for c in CATS:
            if c in truth and c in pred:
                counts[c]["tp"] += 1
            elif c not in truth and c in pred:
                counts[c]["fp"] += 1
            elif c in truth and c not in pred:
                counts[c]["fn"] += 1
            else:
                counts[c]["tn"] += 1
        ok = truth == pred
        exact += ok
        d = tmpl.setdefault(t, dict(lines=0, exact_match=0))
        d["lines"] += 1
        d["exact_match"] += ok

    n = len(labels)
    per = {}
    for c in CATS:
        k = counts[c]
        p, r, f = prf(k["tp"], k["fp"], k["fn"])
        per[c] = dict(k, precision=p, recall=r, f1=f, accuracy=div(k["tp"] + k["tn"], n))
    tp = sum(counts[c]["tp"] for c in CATS)
    fp = sum(counts[c]["fp"] for c in CATS)
    fn = sum(counts[c]["fn"] for c in CATS)
    tn = sum(counts[c]["tn"] for c in CATS)
    mp, mr, mf = prf(tp, fp, fn)
    f1s = [per[c]["f1"] for c in CATS if per[c]["f1"] is not None]

    def group(cs):
        return div(sum(per[c]["tp"] + per[c]["tn"] for c in cs), n * len(cs))

    for d in tmpl.values():
        d["exact_match_accuracy"] = div(d["exact_match"], d["lines"])
    return {
        "mode": mode,
        "lines": n,
        "rejected_by_input_module": rejected,
        "note": "deterministic (no timing involved), so a single run; ground truth = what each "
                "line is (gen_bp.py), not what the regexes match",
        "per_category": per,
        "overall": {
            "exact_match_accuracy": div(exact, n),
            "label_accuracy": div(tp + tn, tp + tn + fp + fn),
            "micro_precision": mp, "micro_recall": mr, "micro_f1": mf,
            "macro_f1": sum(f1s) / len(f1s) if f1s else None,
        },
        "label_accuracy_by_group": {
            "structured (EMAIL, PHONE)": group(["EMAIL", "PHONE"]),
            "natural language (QUESTION, GREETING)": group(["QUESTION", "GREETING"]),
            "NAME": group(["NAME"]),
        },
        "by_template": dict(sorted(tmpl.items())),
    }


# ---- summaries (RB1-RB5) -----------------------------------------------------------------------

def spread(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values),
            "runs": len(values)}


def load_json(path):
    with open(path) as f:
        return json.load(f)


def group_runs(directory, pattern):
    """{(key parts...): [json, ...]} for files like <a>__<b>__runN.json."""
    groups = {}
    for path in sorted(glob.glob(os.path.join(directory, pattern))):
        m = re.match(r"(.+)__run(\d+)\.json$", os.path.basename(path))
        if not m:
            continue
        groups.setdefault(m.group(1), []).append(load_json(path))
    return groups


def summarize_rb1(d):
    inputs = []
    for k in range(1, 6):
        p = os.path.join(d, f"in{k}.txt")
        inputs.append(open(p, encoding="utf-8", errors="replace").read().rstrip("\n") if os.path.exists(p) else "")
    out = []
    for key, runs in group_runs(d, "in*__*__run*.json").items():
        inp, variant = key.split("__", 1)
        k = int(inp[2:])
        row = {"input_index": k, "input": inputs[k - 1], "variant": variant,
               "p50_ns": spread([r["latency_ns"]["p50"] for r in runs]),
               "p99_ns": spread([r["latency_ns"]["p99"] for r in runs])}
        if variant == "fork":
            for f in ("mean_fork_to_collect_ns", "mean_child_regcomp_ns", "mean_child_regexec_ns"):
                row[f] = spread([r["fork"][f] for r in runs])
        out.append(row)
    return sorted(out, key=lambda r: (r["input_index"], r["variant"]))


def summarize_rb2(d):
    out = []
    for key, runs in group_runs(d, "n*__*__run*.json").items():
        n, variant = key.split("__", 1)
        out.append({"batch": int(n[1:]), "variant": variant,
                    "inputs_per_s": spread([r["inputs_per_s"] for r in runs])})
    return sorted(out, key=lambda r: (r["variant"], r["batch"]))


def summarize_rb3(d):
    rows = {}
    for key, runs in group_runs(d, "k*__*__run*.json").items():
        k, variant = key.split("__", 1)
        rows[(variant, int(k[1:]))] = {
            "variant": variant, "k": int(k[1:]), "inputs": runs[0]["inputs"],
            "cpus": runs[0]["cpus"], "inputs_per_s": spread([r["inputs_per_s"] for r in runs])}
    seq = rows.get(("seq-precompiled", 1))
    for (variant, k), row in sorted(rows.items()):
        base = rows.get((variant, 1))
        med = row["inputs_per_s"]["median"]
        row["speedup_vs_same_variant_k1"] = med / base["inputs_per_s"]["median"] if base else None
        row["speedup_vs_seq_precompiled"] = med / seq["inputs_per_s"]["median"] if seq else None
        # the paper reports 5.62x at 8 cores for a 5-pattern task; record whether we exceed 5
        for field in ("speedup_vs_same_variant_k1", "speedup_vs_seq_precompiled"):
            row["exceeds_5_" + field] = row[field] is not None and row[field] > 5
    return sorted(rows.values(), key=lambda r: (r["variant"], r["k"]))


def parse_time_v(path):
    """Selected fields of `/usr/bin/time -v` output."""
    want = {
        "User time (seconds)": ("user_s", float), "System time (seconds)": ("sys_s", float),
        "Maximum resident set size (kbytes)": ("max_rss_kb", int),
        "Minor (reclaiming a frame) page faults": ("minor_faults", int),
        "Voluntary context switches": ("voluntary_ctxt_switches", int),
        "Involuntary context switches": ("involuntary_ctxt_switches", int),
    }
    res = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            for label, (name, typ) in want.items():
                if line.startswith(label + ":"):
                    res[name] = typ(line.split(":", 1)[1].strip())
    return res


def parse_perf_csv(path):
    res = {}
    if not os.path.exists(path):
        return res
    with open(path) as f:
        for line in f:
            parts = line.strip().split(",")
            if len(parts) >= 3 and parts[0] not in ("", "#") and not parts[0].startswith("#"):
                try:
                    res[parts[2]] = float(parts[0])
                except ValueError:
                    res[parts[2]] = parts[0]       # e.g. <not supported>
    return res


def summarize_rb5(d):
    out = []
    for key, runs in group_runs(d, "*__run*.json").items():
        variant = key
        ru = [r["rusage"] for r in runs]
        row = {"variant": variant, "inputs": runs[0]["inputs"],
               "total_seconds": spread([r["total_seconds"] for r in runs])}
        for f in ("user_s", "sys_s", "children_user_s", "children_sys_s", "voluntary_ctxt_switches",
                  "involuntary_ctxt_switches", "minor_faults"):
            row["rusage_" + f] = spread([x[f] for x in ru])
        times = [parse_time_v(p) for p in sorted(glob.glob(os.path.join(d, f"{variant}__run*.time.txt")))]
        for f in ("user_s", "sys_s", "max_rss_kb", "minor_faults", "voluntary_ctxt_switches",
                  "involuntary_ctxt_switches"):
            row["time_v_" + f] = spread([t.get(f) for t in times])
        perfs = [parse_perf_csv(p) for p in sorted(glob.glob(os.path.join(d, f"{variant}__run*.perf.csv")))]
        if perfs:
            for ev in perfs[0]:
                row["perf_" + ev] = spread([p.get(ev) for p in perfs if isinstance(p.get(ev), float)])
        out.append(row)
    if not glob.glob(os.path.join(d, "*.perf.csv")):
        out.append({"note": "perf stat unavailable (see perf_unavailable.txt); user/sys time and "
                            "context switches come from /usr/bin/time -v and getrusage"})
    return out


def summarize(directory):
    jobs = [("rb1", summarize_rb1), ("rb2", summarize_rb2), ("rb3", summarize_rb3), ("rb5", summarize_rb5)]
    for name, fn in jobs:
        d = os.path.join(directory, name)
        if not os.path.isdir(d):
            continue
        rows = fn(d)
        with open(os.path.join(d, "summary.json"), "w") as f:
            json.dump(rows, f, indent=1)
        print(f"bp_eval: {name}/summary.json ({len(rows)} rows)", file=sys.stderr)
    d = os.path.join(directory, "rb4")
    if os.path.isdir(d):
        res = {m: load_json(os.path.join(d, f"accuracy_{m}.json"))
               for m in ("none", "paper") if os.path.exists(os.path.join(d, f"accuracy_{m}.json"))}
        with open(os.path.join(d, "summary.json"), "w") as f:
            json.dump(res, f, indent=1)
        print(f"bp_eval: rb4/summary.json ({len(res)} modes)", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("accuracy")
    a.add_argument("--labels", required=True)
    a.add_argument("--masks", required=True)
    a.add_argument("--mode", required=True)
    a.add_argument("--out", required=True)
    s = sub.add_parser("summarize")
    s.add_argument("--dir", required=True)
    args = ap.parse_args()
    if args.cmd == "accuracy":
        res = accuracy(load_labels(args.labels), load_masks(args.masks), args.mode)
        with open(args.out, "w") as f:
            json.dump(res, f, indent=1)
        o = res["overall"]
        print(f"bp_eval: normalize={args.mode}: exact-match accuracy {o['exact_match_accuracy']:.3f}, "
              f"label accuracy {o['label_accuracy']:.3f}, macro F1 {o['macro_f1']:.3f}, "
              f"rejected {res['rejected_by_input_module']}", file=sys.stderr)
    else:
        summarize(args.dir)


if __name__ == "__main__":
    main()
