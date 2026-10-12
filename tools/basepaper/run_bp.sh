#!/bin/bash
# Phase R benchmark runner (plan R4, spec §10.4). Runs the experiments RB1-RB5 and writes raw
# JSON/text under OUT/<exp>/, machine info to OUT/machine.txt, then calls bp_eval.py
# (summaries) and plot.py (one figure per experiment).
#
#   run_bp.sh [--smoke | --full] [--out DIR] [--exp RB1,RB2,...] [--runs N]
#
#   --smoke   small sizes, 2 runs per point; default output out/basepaper-smoke (not a result)
#   --full    full sizes, 5 runs per point; default output results/basepaper
#
# Every point is run RUNS times, interleaved (run 1 of everything, then run 2, ...) so
# drift hits all variants alike. Nothing is tuned: sizes are fixed below and numbers only
# ever come from the files this script writes.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${BIN:-$ROOT/bin}
BENCH=$BIN/bp_bench
FORK=$BIN/bp_fork
TESTS=$ROOT/tests/basepaper
TOOLS=$ROOT/tools/basepaper

MODE=smoke
OUT=""
EXPS="RB1,RB2,RB3,RB4,RB5"
RUNS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --smoke) MODE=smoke ;;
        --full) MODE=full ;;
        --out) OUT=$2; shift ;;
        --exp) EXPS=$2; shift ;;
        --runs) RUNS=$2; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "run_bp.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$MODE" = full ]; then
    : "${RUNS:=5}"; RB1_REP=1000; RB2_SIZES="1 10 100 1000 10000"; RB3_KS="1 2 4 6 8"
    RB3_SCALE=1; RB5_N=10000; : "${OUT:=$ROOT/results/basepaper}"
else
    : "${RUNS:=2}"; RB1_REP=30; RB2_SIZES="1 10 100"; RB3_KS="1 2"
    RB3_SCALE=200; RB5_N=200; : "${OUT:=$ROOT/out/basepaper-smoke}"
fi
# RB3 input counts per variant (full size); each run must take long enough to time. Smoke divides.
rb3_n() {
    local n
    case "$1" in
        fork) n=2000 ;;
        seq-recompile) n=20000 ;;
        threads-pattern) n=100000 ;;
        seq-precompiled|threads-data) n=1000000 ;;
        dfa) n=10000000 ;;
    esac
    n=$((n / RB3_SCALE)); [ "$n" -ge 100 ] || n=100
    echo "$n"
}

for b in "$BENCH" "$FORK"; do
    [ -x "$b" ] || { echo "run_bp.sh: $b missing; run 'make bp' first" >&2; exit 1; }
done
want() { case ",$EXPS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

mkdir -p "$OUT"
# the labeled inputs, one per line (column 1), kept with the results for provenance
grep -a -v '^#' "$TESTS/labeled.tsv" | cut -f1 > "$OUT/labeled_inputs.txt"

# ---- CPU selection for RB3: first logical CPU of each physical core, in core order -----------
mapfile -t CORE_CPUS < <(lscpu -p=CPU,CORE | grep -v '^#' | sort -t, -k2,2n -k1,1n | awk -F, '!seen[$2]++ {print $1}')
NPHYS=${#CORE_CPUS[@]}

# ---- machine.txt ----------------------------------------------------------------------------
{
    echo "run_bp.sh mode: $MODE   runs per point: $RUNS   date: $(date -u +%FT%TZ)"
    echo "git: $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo ' (working tree dirty)')"
    echo; echo "## uname -a"; uname -a
    echo; echo "## gcc --version"; gcc --version | head -1
    echo; echo "## build flags (make print-flags)"; make -s -C "$ROOT" print-flags
    echo; echo "## lscpu"; lscpu
    echo; echo "## per-CPU: CPU,CORE,SOCKET,MAXMHZ (hybrid CPU: higher max MHz = performance cores)"
    lscpu -p=CPU,CORE,SOCKET,MAXMHZ | grep -v '^# '
    echo; echo "## physical cores: $NPHYS"
    echo "RB3 CPU order (first logical CPU of each core, core order): ${CORE_CPUS[*]}"
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo n/a)"
    echo "governor cpu0: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "affinity of this script: $(taskset -p $$ 2>/dev/null | sed 's/.*: //')"
    echo "load average at start: $(cut -d' ' -f1-3 /proc/loadavg)"
} > "$OUT/machine.txt"

# Run a bp_bench command, store stdout as JSON, and validate it. Usage: bench FILE ARGS...
bench() {
    local file=$1; shift
    "$@" > "$file"
    python3 "$TOOLS/check_bench_json.py" "$file" || { echo "invalid JSON from: $*" >&2; exit 1; }
}
log() { echo "[$(date +%T)] $*" >&2; }

# Variants with their thread options (RB1/RB2).
RB1_VARIANTS=("fork" "seq-recompile" "seq-precompiled" "threads-pattern" "threads-data -t 1" "dfa -t 1")
RB2_VARIANTS=("fork" "seq-recompile" "seq-precompiled" "threads-pattern" "threads-data -t 5" "dfa -t 5")
vtag() { echo "$1" | sed 's/ -t /_t/'; }

# ---- RB1: single-input latency (paper §4.1) --------------------------------------------------
if want RB1; then
    mkdir -p "$OUT/rb1"
    for r in $(seq 1 "$RUNS"); do
        for k in 1 2 3 4 5; do
            sed -n "${k}p" "$TESTS/table41.txt" > "$OUT/rb1/in$k.txt"
            for v in "${RB1_VARIANTS[@]}"; do
                log "RB1 run $r input $k $v"
                # shellcheck disable=SC2086
                bench "$OUT/rb1/in${k}__$(vtag "$v")__run$r.json" \
                    "$BENCH" --variant $v -f "$OUT/rb1/in$k.txt" -n "$RB1_REP" --warmup "$RB1_REP"
            done
        done
    done
fi

# ---- RB2: batch size (paper §5.3) -------------------------------------------------------------
if want RB2; then
    mkdir -p "$OUT/rb2"
    for r in $(seq 1 "$RUNS"); do
        for n in $RB2_SIZES; do
            for v in "${RB2_VARIANTS[@]}"; do
                log "RB2 run $r batch $n $v"
                # shellcheck disable=SC2086
                bench "$OUT/rb2/n${n}__$(vtag "$v")__run$r.json" \
                    "$BENCH" --variant $v -f "$OUT/labeled_inputs.txt" -n "$n" --warmup $((n < 100 ? n : 100)) --no-latency
            done
        done
    done
fi

# ---- RB3: core scaling (paper §4.3) -----------------------------------------------------------
if want RB3; then
    mkdir -p "$OUT/rb3"
    for r in $(seq 1 "$RUNS"); do
        for k in $RB3_KS; do
            if [ "$k" -gt "$NPHYS" ]; then echo "RB3: skipping k=$k (only $NPHYS physical cores)" >&2; continue; fi
            cpus=$(printf '%s,' "${CORE_CPUS[@]:0:$k}"); cpus=${cpus%,}
            specs=("fork" "threads-pattern" "threads-data -t $k" "dfa -t $k")
            [ "$k" -eq 1 ] && specs+=("seq-precompiled")
            for v in "${specs[@]}"; do
                name=${v%% *}
                n=$(rb3_n "$name")
                log "RB3 run $r k=$k cpus=$cpus $v (n=$n)"
                # shellcheck disable=SC2086
                bench "$OUT/rb3/k${k}__${name}__run$r.json" \
                    taskset -c "$cpus" "$BENCH" --variant $v -f "$OUT/labeled_inputs.txt" -n "$n" \
                    --warmup $((n / 10 + 1)) --no-latency
            done
        done
    done
fi

# ---- RB4: accuracy (paper §4.2) ---------------------------------------------------------------
if want RB4; then
    mkdir -p "$OUT/rb4"
    for mode in none paper; do
        log "RB4 normalize=$mode"
        "$FORK" -f "$OUT/labeled_inputs.txt" --quiet --normalize=$mode > "$OUT/rb4/masks_$mode.txt"
        python3 "$TOOLS/bp_eval.py" accuracy --labels "$TESTS/labeled.tsv" \
            --masks "$OUT/rb4/masks_$mode.txt" --mode "$mode" --out "$OUT/rb4/accuracy_$mode.json"
    done
fi

# ---- RB5: kernel vs user time (paper §5.3) ----------------------------------------------------
if want RB5; then
    mkdir -p "$OUT/rb5"
    WRAP=()
    if perf stat -e task-clock true > /dev/null 2> "$OUT/rb5/perf_check.txt"; then
        WRAP=(perf stat -x, -e task-clock,context-switches,cpu-migrations,page-faults)
    else
        { echo "perf stat unavailable on this machine; using /usr/bin/time -v and getrusage (in the JSON) instead."
          cat "$OUT/rb5/perf_check.txt"; } > "$OUT/rb5/perf_unavailable.txt"
    fi
    rm -f "$OUT/rb5/perf_check.txt"
    for r in $(seq 1 "$RUNS"); do
        for v in fork seq-recompile; do
            log "RB5 run $r $v (n=$RB5_N)"
            f="$OUT/rb5/${v}__run$r"
            if [ ${#WRAP[@]} -gt 0 ]; then
                /usr/bin/time -v -o "$f.time.txt" "${WRAP[@]}" -o "$f.perf.csv" "$BENCH" --variant "$v" \
                    -f "$OUT/labeled_inputs.txt" -n "$RB5_N" --warmup 100 --no-latency > "$f.json"
            else
                /usr/bin/time -v -o "$f.time.txt" "$BENCH" --variant "$v" \
                    -f "$OUT/labeled_inputs.txt" -n "$RB5_N" --warmup 100 --no-latency > "$f.json"
            fi
            python3 "$TOOLS/check_bench_json.py" "$f.json"
        done
    done
fi

python3 "$TOOLS/bp_eval.py" summarize --dir "$OUT"
python3 "$ROOT/tools/plot.py" --results "$OUT" --out "$OUT/figures"
log "done: $OUT"
