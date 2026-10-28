#!/usr/bin/env bash
# Accuracy experiments E6 and E7 (plan T7.4, spec §10.3). Deterministic: every input is a fixed
# generator seed or a downloaded data set, so one run per point (no timing involved).
#
#   run_eval.sh [--out DIR]          default results/
#
#   E6  accuracy of the labels: precision / recall / F1 per category, confusion matrix and calibration
#         a) generator ground truth (tools/gen, seed 11, 200,000 lines)
#         b) AIT-LDS v2.0 labelled logs (data/ait, via tools/fetch_datasets.sh --ait): ATTACK lines
#            against SECURITY_THREAT or AUTH_FAILURE
#         c) full Loghub logs (data/*.log): no labels, so only the label distribution is recorded
#         (hand-labelled sample: tools/sample_for_labeling.py + label_eval.py --annot, done by people)
#   E7  validators on / off: the same runs with rules/novalidators.rules (validator column "-" and
#         base score 0.6 for the three validator rules, so a pattern-only match is a label, as in a
#         plain regex classifier), PII precision / recall on the generator truth and PII labels on
#         the full logs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/results"
[ "${1:-}" = "--out" ] && OUT=$2
TRIAGE="$ROOT/bin/triage"; GEN="$ROOT/bin/gen"
mkdir -p "$OUT/e6" "$OUT/e7" "$ROOT/data/bench"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# rule set without validators (E7)
awk -F'\t' 'BEGIN{OFS="\t"} /^#/ {print; next} $6 != "-" {$6="-"; $4="0.6"} {print}' \
    "$ROOT/rules/default.rules" > "$ROOT/rules/novalidators.rules"

cmds="$OUT/e6/commands.txt"; : > "$cmds"
note() { echo "$*" >> "$cmds"; }

# ---- a) generator truth ----
GN=200000
note "bin/gen -n $GN --seed 11 --truth data/bench/gen11.truth.tsv > data/bench/gen11.log"
"$GEN" -n $GN --seed 11 --truth "$ROOT/data/bench/gen11.truth.tsv" > "$ROOT/data/bench/gen11.log" 2>/dev/null
for variant in default novalidators; do
    note "bin/triage --quiet -w 4 -r rules/$variant.rules -o <tmp> data/bench/gen11.log"
    "$TRIAGE" --quiet -w 4 -r "$ROOT/rules/$variant.rules" -o "$tmp/gen_$variant" "$ROOT/data/bench/gen11.log"
    d="$OUT/e6/synthetic"; [ $variant = novalidators ] && d="$OUT/e7/synthetic_novalidators"
    note "tools/label_eval.py --truth data/bench/gen11.truth.tsv --pred <tmp>/store.jsonl --out ${d#$ROOT/}"
    python3 "$ROOT/tools/label_eval.py" --truth "$ROOT/data/bench/gen11.truth.tsv" --pred "$tmp/gen_$variant/store.jsonl" \
        --out "$d" > "$d.txt"
done

# ---- b) AIT ----
if ls "$ROOT"/data/ait/*/*.truth.tsv > /dev/null 2>&1; then
    mkdir -p "$OUT/e6/ait"
    for t in "$ROOT"/data/ait/*/*.truth.tsv; do
        log="${t%.truth.tsv}.log"; name=$(basename "$log" .log)
        note "bin/triage --quiet -w 4 -o <tmp> data/ait/.../$name.log"
        "$TRIAGE" --quiet -w 4 -o "$tmp/ait" "$log"
        note "tools/label_eval.py --truth data/ait/.../$name.truth.tsv --pred <tmp>/store.jsonl --attack-map ATTACK=SECURITY_THREAT+AUTH_FAILURE --out results/e6/ait/$name"
        python3 "$ROOT/tools/label_eval.py" --truth "$t" --pred "$tmp/ait/store.jsonl" \
            --attack-map "ATTACK=SECURITY_THREAT+AUTH_FAILURE" --out "$OUT/e6/ait/$name" > "$OUT/e6/ait/$name.txt"
    done
fi

# ---- c) label distribution on the full logs, with and without validators ----
python3 - "$ROOT" "$OUT" "$tmp" <<'PY'
import collections, json, os, subprocess, sys
root, out, tmp = sys.argv[1:4]
files = [("OpenSSH", "data/OpenSSH.log"), ("Apache", "data/Apache.log"), ("Linux", "data/Linux.log"),
         ("BGL", "data/BGL.log"), ("HDFS_2k", "tests/corpus/HDFS_2k.log")]
res = {}
for name, rel in files:
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        continue
    res[name] = {"lines": sum(1 for _ in open(p, "rb"))}
    for variant in ("default", "novalidators"):
        d = os.path.join(tmp, f"{name}_{variant}")
        subprocess.run([os.path.join(root, "bin/triage"), "--quiet", "-w", "4", "-r",
                        os.path.join(root, f"rules/{variant}.rules"), "-o", d, p], check=True)
        prim, pii_lines, esc = collections.Counter(), 0, collections.Counter()
        for l in open(os.path.join(d, "store.jsonl"), encoding="utf-8"):
            r = json.loads(l)
            prim[r["primary"] or "none"] += 1
            pii_lines += "PII" in r["labels"]
            esc.update(r["flags"])
        res[name][variant] = {"primary": dict(prim), "lines_labelled_pii": pii_lines, "flags": dict(esc)}
json.dump(res, open(os.path.join(out, "e6", "loghub_full.json"), "w"), indent=1)
json.dump({k: {"lines": v["lines"], "lines_labelled_pii_validators": v["default"]["lines_labelled_pii"],
               "lines_labelled_pii_no_validators": v["novalidators"]["lines_labelled_pii"]} for k, v in res.items()},
          open(os.path.join(out, "e7", "pii_on_logs.json"), "w"), indent=1)
for k, v in res.items():
    print(f"{k:9s} {v['lines']:9,d} lines: PII-labelled lines {v['novalidators']['lines_labelled_pii']:7d} without validators, "
          f"{v['default']['lines_labelled_pii']:7d} with")
PY
echo "run_eval: results in $OUT/e6 and $OUT/e7"
