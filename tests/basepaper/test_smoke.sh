#!/bin/sh
# R4 check: a smoke run of run_bp.sh (small sizes) produces valid JSON for every experiment
# and one figure per experiment. The numbers are NOT results; they go to a temp directory.
# usage: test_smoke.sh BIN_DIR
set -eu
bin=${1:?usage: test_smoke.sh BIN_DIR}
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

BIN=$(cd "$bin" && pwd) "$root/tools/basepaper/run_bp.sh" --smoke --runs 2 --out "$tmp/out" > "$tmp/log" 2>&1 \
    || { cat "$tmp/log"; echo "run_bp.sh --smoke failed"; exit 1; }

python3 - "$tmp/out" <<'PY'
import glob, json, os, sys
out = sys.argv[1]
errs = []
def need(path):
    if not os.path.exists(path):
        errs.append("missing " + path)
    return path
need(f"{out}/machine.txt")
for exp in ("rb1", "rb2", "rb3", "rb4", "rb5"):
    need(f"{out}/{exp}/summary.json")
    need(f"{out}/figures/{exp}.svg")
    if exp != "rb4":
        raw = glob.glob(f"{out}/{exp}/*__run*.json")
        if not raw:
            errs.append(f"{exp}: no raw run files")
        for p in raw:
            json.load(open(p))
for p in glob.glob(f"{out}/rb*/summary.json"):
    json.load(open(p))
a = json.load(open(f"{out}/rb4/accuracy_none.json"))
if a["lines"] != 1250 or a["rejected_by_input_module"] != 0:
    errs.append("rb4 accuracy_none: unexpected line or rejected count")
b = json.load(open(f"{out}/rb4/accuracy_paper.json"))
if not b["rejected_by_input_module"] > 0:
    errs.append("rb4 accuracy_paper: the paper's input module should reject some labeled lines (apostrophes, '/')")
if b["per_category"]["NAME"]["tp"] != 0:
    errs.append("rb4 accuracy_paper: NAME must never match after lowercasing")
# RB3: every variant at k=2 has a speedup vs the same variant at k=1 and vs seq-precompiled
rb3 = json.load(open(f"{out}/rb3/summary.json"))
for r in rb3:
    if r["variant"] != "seq-precompiled" and (r["speedup_vs_same_variant_k1"] is None or r["speedup_vs_seq_precompiled"] is None):
        errs.append(f"rb3: missing speedup for {r['variant']} k={r['k']}")
for e in errs:
    print(e, file=sys.stderr)
sys.exit(1 if errs else 0)
PY
echo "test_smoke: run_bp.sh --smoke produced valid JSON and a figure for RB1-RB5"
