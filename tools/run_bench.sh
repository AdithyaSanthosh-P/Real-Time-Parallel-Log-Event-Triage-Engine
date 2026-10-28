#!/usr/bin/env bash
# Benchmark runner (plan T6.5, spec §10.2/§10.3). Runs named experiment matrices of
# `bin/triage --bench`, every point several times, and saves the raw JSON.
#
#   run_bench.sh [--smoke | --full] [--exp E1,E2,E3,E4] [--out DIR] [--runs N]
#
#   --smoke   small inputs, 2 runs per point; default output out/bench-smoke (not a result)
#   --full    full sizes, 5 runs per point; default output results/
#
#   E1  architecture: fork -w1 | posix -w1 | posix -wN | dfa -wN       (N = physical cores)
#   E2  scaling: posix and dfa, -w 1 .. N
#   E3  rule count: bench_5 .. bench_64 rule sets, posix -w1 vs dfa -w1   (the headline)
#   E4  batch size: dfa -w4 with -b 1 .. 1024
#
# Output (DIR is results/ for --full): DIR/machine.txt; per experiment DIR/<exp>/{points.tsv,
# commands.txt, raw/<point>__run<k>.json, summary.json}. Points are run interleaved (run 1 of
# every point, then run 2, ...) so drift hits all points alike. The input is a fixed-seed
# generator file (tools/gen), preloaded into memory by --bench. Nothing is tuned: sizes are fixed
# below, and numbers only ever come from the files this script writes. Figures: tools/plot.py.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/bin}
TRIAGE=$BIN/triage
GEN=$BIN/gen

MODE=smoke; OUT=""; EXPS="E1,E2,E3,E4"; RUNS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --smoke) MODE=smoke ;;
        --full) MODE=full ;;
        --out) OUT=$2; shift ;;
        --exp) EXPS=$2; shift ;;
        --runs) RUNS=$2; shift ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "run_bench.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -x "$TRIAGE" ] && [ -x "$GEN" ] || { echo "run_bench.sh: run 'make' first" >&2; exit 2; }

PHYS=$(lscpu -p=CORE,SOCKET | grep -v '^#' | sort -u | wc -l)
if [ "$MODE" = full ]; then
    : "${RUNS:=5}"; : "${OUT:=$ROOT/results}"
    N_MAIN=1000000; N_FORK=2000; N_E3=300000; WLIST="1 2 4 6 8 10 12 14"; BLIST="1 4 16 64 256 1024"
else
    : "${RUNS:=2}"; : "${OUT:=$ROOT/out/bench-smoke}"
    N_MAIN=20000; N_FORK=100; N_E3=20000; WLIST="1 2"; BLIST="16 256"
fi
WLIST=$(for w in $WLIST; do [ "$w" -le "$PHYS" ] && echo "$w"; done | tr '\n' ' ')
mkdir -p "$OUT" "$ROOT/data/bench"

# ---- input: fixed-seed synthetic logs, regenerated only when missing ----
input() {   # input N -> path
    local f="$ROOT/data/bench/gen_$1.log"
    [ -s "$f" ] || "$GEN" -n "$1" --seed 1 > "$f" 2>/dev/null
    echo "$f"
}

# ---- machine info ----
{
    echo "date: $(date -u +%FT%TZ)"
    echo "commit: $(git -C "$ROOT" rev-parse HEAD) $(git -C "$ROOT" diff --quiet HEAD -- src rules tools || echo '(src/rules/tools modified)')"
    echo "mode: $MODE, runs per point: $RUNS, physical cores: $PHYS"
    echo "uname: $(uname -a)"
    echo "gcc: $(gcc --version | head -1)"
    echo "flags: $(make -s -C "$ROOT" print-flags)"
    echo; lscpu
    echo; free -h
    echo; echo "cpu governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
} > "$OUT/machine.txt"

# ---- a point = engine, workers, batch, rules file, input lines ----
declare -a P_ID P_ENG P_W P_B P_RULES P_N P_FILE
add_point() {   # add_point ID ENGINE WORKERS BATCH RULES_FILE LINES
    P_ID+=("$1"); P_ENG+=("$2"); P_W+=("$3"); P_B+=("$4"); P_RULES+=("$5"); P_N+=("$6"); P_FILE+=("$(input "$6")")
}

run_experiment() {   # run_experiment EXP
    local exp=$1 d="$OUT/$(echo "$1" | tr 'A-Z' 'a-z')"
    P_ID=(); P_ENG=(); P_W=(); P_B=(); P_RULES=(); P_N=(); P_FILE=()
    local def=rules/default.rules
    case "$exp" in
        E1) add_point c1-fork fork 1 256 $def "$N_FORK"
            add_point c2-posix-w1 posix 1 256 $def "$N_MAIN"
            add_point c3-posix-w$PHYS posix "$PHYS" 256 $def "$N_MAIN"
            add_point c4-dfa-w$PHYS dfa "$PHYS" 256 $def "$N_MAIN" ;;
        E2) for e in posix dfa; do for w in $WLIST; do add_point "$e-w$w" "$e" "$w" 256 $def "$N_MAIN"; done; done ;;
        E3) for n in 5 10 20 32 64; do for e in posix dfa; do
                add_point "$e-r$n" "$e" 1 256 "rules/bench_$n.rules" "$N_E3"; done; done ;;
        E4) for b in $BLIST; do add_point "dfa-b$b" dfa 4 "$b" $def "$N_MAIN"; done ;;
        *) echo "run_bench.sh: unknown experiment $exp" >&2; exit 2 ;;
    esac
    rm -rf "$d"; mkdir -p "$d/raw"
    : > "$d/points.tsv"; : > "$d/commands.txt"
    local i
    for i in "${!P_ID[@]}"; do
        # E3 shows the rule count in the point; the others use the default rule set
        local rl; rl=$(basename "${P_RULES[$i]}" .rules)
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "${P_ID[$i]}" "${P_ENG[$i]}" "${P_W[$i]}" "${P_B[$i]}" "$rl" "${P_N[$i]}" >> "$d/points.tsv"
        echo "bin/triage --bench --engine ${P_ENG[$i]} -w ${P_W[$i]} -b ${P_B[$i]} -r ${P_RULES[$i]} data/bench/$(basename "${P_FILE[$i]}")   # ${P_ID[$i]}, $RUNS runs" >> "$d/commands.txt"
    done
    local run
    for run in $(seq "$RUNS"); do
        for i in "${!P_ID[@]}"; do
            echo "[$exp] run $run/$RUNS ${P_ID[$i]}" >&2
            "$TRIAGE" --bench --engine "${P_ENG[$i]}" -w "${P_W[$i]}" -b "${P_B[$i]}" -r "$ROOT/${P_RULES[$i]}" \
                "${P_FILE[$i]}" > "$d/raw/${P_ID[$i]}__run$run.json"
        done
    done
    python3 "$ROOT/tools/bench_eval.py" summarize "$d"
}

for e in $(echo "$EXPS" | tr ',' ' '); do run_experiment "$e"; done
python3 "$ROOT/tools/plot.py" --results "$OUT" --out "$OUT/figures"
echo "run_bench: results in $OUT"
