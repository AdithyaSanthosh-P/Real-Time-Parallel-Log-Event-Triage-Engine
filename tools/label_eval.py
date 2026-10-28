#!/usr/bin/env python3
"""Evaluate triage's labels against ground truth (plan T7.3, spec §5.4, §10.1).

Two kinds of ground truth:

  label_eval.py --truth TRUTH.tsv --pred STORE.jsonl [--attack-map ATTACK=SECURITY_THREAT,AUTH_FAILURE]
      TRUTH.tsv has one row per line, "seq<TAB>labels<TAB>kind": the generator's ground truth
      (tools/gen) or an AIT-LDS conversion (tools/fetch_ait.py). labels is a comma list of
      categories or "-" for none. --attack-map treats a truth label such as ATTACK as "any of
      these categories" (an AIT line is an attack; the engine may call it SEC or AUTH).

  label_eval.py --annot A.csv [B.csv] --key key.csv --pred-key
      Hand labels from tools/sample_for_labeling.py (annotator files with 1/0 per category and the key).
      With two annotators: Cohen's kappa per category, and the evaluation is run against A, against B and
      against the lines where both agree. Strata are weighted back by stratum_size / stratum_sampled,
      so numbers describe the whole data set, not the sample.

Reports per category precision / recall / F1 (multi-label: each category is a binary decision per
line), a confusion matrix of primary labels, and the calibration of the primary label's confidence:
reliability diagram (10 bins) and Expected Calibration Error ECE = sum_b |B_b|/N * |acc(B_b) - conf(B_b)|,
where acc is the fraction of lines in the bin whose primary label is among the true labels.
--out DIR writes result.json and reliability.svg. Standard library only."""
import argparse
import csv
import json
import os
import sys

CATS = ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"]
PRIORITY = ["SECURITY_THREAT", "PII", "AUTH_FAILURE", "ERROR", "PERF_WARNING"]      # spec §5.3 tie order


def prf(tp, fp, fn):
    p = tp / (tp + fp) if tp + fp else None
    r = tp / (tp + fn) if tp + fn else None
    f = 2 * p * r / (p + r) if p and r else (0.0 if p is not None and r is not None else None)
    return p, r, f


def evaluate(items, cats):
    """items: [(truth_set, pred_set, pred_primary, pred_conf, weight)] -> result dict."""
    res = {"categories": {}, "n": sum(1 for _ in items), "weighted_n": sum(w for *_, w in items)}
    for c in cats:
        tp = sum(w for t, p, _, _, w in items if c in t and c in p)
        fp = sum(w for t, p, _, _, w in items if c not in t and c in p)
        fn = sum(w for t, p, _, _, w in items if c in t and c not in p)
        p, r, f = prf(tp, fp, fn)
        res["categories"][c] = {"tp": tp, "fp": fp, "fn": fn, "precision": p, "recall": r, "f1": f,
                                "support": tp + fn}
    res["exact_match"] = sum(w for t, p, *_, w in items if t == p) / res["weighted_n"] if items else None
    # confusion of primary labels: truth primary (by §5.3 priority) vs predicted primary
    def tprim(t):
        for c in PRIORITY:
            if c in t:
                return c
        return "none"
    names = [c for c in PRIORITY if c in cats] + ["none"]
    conf = {a: {b: 0.0 for b in names} for a in names}
    for t, p, pp, _, w in items:
        a, b = tprim(t & set(cats)), pp or "none"
        if b in conf[a]:
            conf[a][b] += w
    res["confusion"] = {"rows": "truth primary", "cols": "predicted primary", "labels": names, "matrix": conf}
    # calibration of the primary label's confidence
    bins = [[0.0, 0.0, 0.0] for _ in range(10)]          # weight, weight*conf, weight*correct
    for t, p, pp, cf, w in items:
        if pp is None or pp not in cats:
            continue
        b = min(int(cf * 10), 9)
        bins[b][0] += w
        bins[b][1] += w * cf
        bins[b][2] += w * (pp in t)
    total = sum(b[0] for b in bins)
    ece = sum(abs(b[2] / b[0] - b[1] / b[0]) * b[0] / total for b in bins if b[0]) if total else None
    res["calibration"] = {"ece": ece, "lines_with_label": total,
                          "bins": [{"lo": i / 10, "hi": (i + 1) / 10, "weight": b[0],
                                    "mean_confidence": b[1] / b[0] if b[0] else None,
                                    "observed_precision": b[2] / b[0] if b[0] else None} for i, b in enumerate(bins)]}
    return res


def kappa(a, b):
    """Cohen's kappa for two 0/1 lists."""
    n = len(a)
    if n == 0:
        return None
    po = sum(x == y for x, y in zip(a, b)) / n
    pa, pb = sum(a) / n, sum(b) / n
    pe = pa * pb + (1 - pa) * (1 - pb)
    return (po - pe) / (1 - pe) if pe < 1 else None


def reliability_svg(cal, title):
    W, H, M = 420, 360, 50
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif" font-size="11">',
           '<rect width="100%" height="100%" fill="white"/>',
           f'<text x="{M}" y="22" font-size="13" font-weight="bold">{title}</text>']
    sx = lambda v: M + v * (W - 2 * M)
    sy = lambda v: H - M - v * (H - 2 * M)
    for t in range(0, 11, 2):
        v = t / 10
        out.append(f'<line x1="{sx(v):.1f}" x2="{sx(v):.1f}" y1="{sy(0)}" y2="{sy(1)}" stroke="#ddd"/>')
        out.append(f'<line x1="{sx(0)}" x2="{sx(1)}" y1="{sy(v):.1f}" y2="{sy(v):.1f}" stroke="#ddd"/>')
        out.append(f'<text x="{sx(v):.1f}" y="{H - M + 15}" text-anchor="middle">{v:.1f}</text>')
        out.append(f'<text x="{M - 6}" y="{sy(v) + 4:.1f}" text-anchor="end">{v:.1f}</text>')
    out.append(f'<line x1="{sx(0)}" y1="{sy(0)}" x2="{sx(1)}" y2="{sy(1)}" stroke="#888" stroke-dasharray="4 3"/>')
    for b in cal["bins"]:
        if b["weight"]:
            x, y = sx(b["mean_confidence"]), sy(b["observed_precision"])
            out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="#2f6db5"/>')
    out.append(f'<text x="{(W) / 2}" y="{H - 10}" text-anchor="middle">mean confidence of the primary label (per bin)</text>')
    out.append(f'<text x="14" y="{H / 2}" text-anchor="middle" transform="rotate(-90 14 {H / 2})">observed precision</text>')
    e = cal["ece"]
    out.append(f'<text x="{sx(0.02)}" y="{sy(0.95)}">ECE = {"n/a" if e is None else f"{e:.3f}"}   (dashed = perfect)</text>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def load_pred(path):
    preds = {}
    with open(path, encoding="utf-8") as f:
        for l in f:
            r = json.loads(l)
            preds[r["seq"]] = r
    return preds


def run_truth(args):
    amap = {}
    for part in (args.attack_map or "").split(","):
        if part:
            k, _, v = part.partition("=")
            amap[k] = v.split("+") if "+" in v else [v]
    preds = load_pred(args.pred)
    cats, items = list(CATS), []
    attack_mode = bool(amap)
    if attack_mode:
        cats = list(amap)
    for l in open(args.truth):
        seq, labels, _kind = l.rstrip("\n").split("\t")
        seq = int(seq)
        if seq not in preds:
            sys.exit(f"label_eval: seq {seq} is in the truth but not in the predictions")
        r = preds[seq]
        truth = set() if labels == "-" else set(labels.split(","))
        pred = set(r["labels"])
        if attack_mode:
            pred = {k for k, targets in amap.items() if pred & set(targets)}
        items.append((truth, pred, r["primary"], r["confidence"], 1.0))
    if len(items) != len(preds):
        sys.exit(f"label_eval: truth has {len(items)} lines, predictions {len(preds)}")
    return {"source": f"{args.truth} vs {args.pred}", "all": evaluate(items, cats)}


def read_annot(path):
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            vals = [r[c].strip() for c in CATS]
            if any(v not in ("0", "1") for v in vals):
                sys.exit(f"label_eval: {path}: line id {r['id']}: every category must be 0 or 1")
            rows[int(r["id"])] = {c for c, v in zip(CATS, vals) if v == "1"}
    return rows


def run_annot(args):
    key = {}
    with open(args.key, newline="") as f:
        for r in csv.DictReader(f):
            key[int(r["id"])] = r
    annots = [read_annot(p) for p in args.annot]
    for a in annots:
        if set(a) != set(key):
            sys.exit("label_eval: annotator file and key have different line ids")

    def items_for(truth_of, ids):
        out = []
        for i in ids:
            k = key[i]
            w = int(k["stratum_size"]) / int(k["stratum_sampled"])
            out.append((truth_of(i), set(filter(None, k["labels"].split(";"))), k["primary"] or None,
                        float(k["confidence"]), w))
        return out
    res = {"source": ",".join(args.annot) + " vs " + args.key}
    ids = sorted(key)
    for name, a in zip("AB", annots):
        res[f"vs_{name}"] = evaluate(items_for(lambda i, a=a: a[i], ids), CATS)
    if len(annots) == 2:
        A, B = annots
        res["kappa"] = {c: kappa([int(c in A[i]) for i in ids], [int(c in B[i]) for i in ids]) for c in CATS}
        res["kappa"]["all"] = kappa([int(c in A[i]) for i in ids for c in CATS], [int(c in B[i]) for i in ids for c in CATS])
        agree = [i for i in ids if A[i] == B[i]]
        res["both_agree"] = evaluate(items_for(lambda i: A[i], agree), CATS)
        res["agreement"] = {"lines": len(ids), "identical_label_sets": len(agree)}
    return res


def show(name, r):
    print(f"\n== {name}   ({r['n']} lines, weighted {r['weighted_n']:.0f})")
    print(f"  {'category':16s} {'precision':>9s} {'recall':>8s} {'F1':>7s} {'support':>10s}")
    f = lambda v: "   n/a" if v is None else f"{v:6.3f}"
    for c, m in r["categories"].items():
        print(f"  {c:16s} {f(m['precision']):>9s} {f(m['recall']):>8s} {f(m['f1']):>7s} {m['support']:10.0f}")
    print(f"  exact label-set match: {r['exact_match']:.4f}   ECE of the primary label: "
          f"{'n/a' if r['calibration']['ece'] is None else format(r['calibration']['ece'], '.4f')}")
    cm = r["confusion"]
    print("  confusion (rows truth primary, columns predicted primary):")
    print("    " + " " * 16 + "".join(f"{n[:9]:>10s}" for n in cm["labels"]))
    for a in cm["labels"]:
        print(f"    {a:16s}" + "".join(f"{cm['matrix'][a][b]:10.0f}" for b in cm["labels"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--truth")
    ap.add_argument("--pred")
    ap.add_argument("--attack-map")
    ap.add_argument("--annot", nargs="+")
    ap.add_argument("--key")
    ap.add_argument("--out")
    args = ap.parse_args()
    if args.truth:
        if not args.pred:
            sys.exit("label_eval: --truth needs --pred")
        res = run_truth(args)
    elif args.annot:
        if not args.key:
            sys.exit("label_eval: --annot needs --key")
        res = run_annot(args)
    else:
        ap.error("give --truth/--pred or --annot/--key")
    for k, v in res.items():
        if isinstance(v, dict) and "categories" in v:
            show(k, v)
    if "kappa" in res:
        print("\nCohen's kappa between the annotators: " + ", ".join(
            f"{c} {'n/a' if v is None else format(v, '.3f')}" for c, v in res["kappa"].items()))
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "result.json"), "w") as f:
            json.dump(res, f, indent=1)
        main_key = "all" if "all" in res else "vs_A"
        with open(os.path.join(args.out, "reliability.svg"), "w") as f:
            f.write(reliability_svg(res[main_key]["calibration"], "Reliability of the primary label"))
        print(f"\nlabel_eval: wrote {args.out}/result.json and reliability.svg")


if __name__ == "__main__":
    main()
