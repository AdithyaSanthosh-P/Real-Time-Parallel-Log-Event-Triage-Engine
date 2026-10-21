#!/usr/bin/env bash
# Review demo: walks through the working parts of the engine on the sample logs
# in tests/corpus/, pausing between steps (press Enter). Everything it writes
# goes to out/demo/.
#
# usage: tools/demo.sh [--no-pause] [STEP...]
#   steps: status compiler classify pii correlate parallel speed basepaper (default: all; "build" is optional)
# Talking points for each step: docs/demo_guide.md
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
pause=1
if [ "${1:-}" = "--no-pause" ]; then pause=0; shift; fi
steps="${*:-status compiler classify pii correlate parallel speed basepaper}"
demo=out/demo
mkdir -p "$demo"

if [ -t 1 ]; then B=$'\e[1m'; C=$'\e[36m'; G=$'\e[32m'; Y=$'\e[33m'; N=$'\e[0m'; else B= C= G= Y= N=; fi
title() { printf '\n%s== %s ==%s\n' "$B" "$1" "$N"; }
say()   { printf '%s%s%s\n' "$Y" "$1" "$N"; }
run()   { printf '%s$ %s%s\n' "$C" "$*" "$N"; "$@"; }
wait_key() { if [ "$pause" -eq 1 ]; then read -r -p "${B}[Enter for the next step]${N} " _; fi; }

# One compact line per JSONL record: seq, severity, primary, labels, flags, then the line.
show() {
    python3 -I -c '
import json, sys
for t in sys.stdin:
    r = json.loads(t)
    labels = ",".join(r["labels"]) or "-"
    flags = ",".join(r["flags"])
    line = r["line"] if len(r["line"]) <= 84 else r["line"][:81] + "..."
    seq, sev, conf = r["seq"], r["severity"], r["confidence"]
    print(f"  #{seq:<5} {sev:<8} conf {conf:.2f}  {labels:<30} {flags}")
    print(f"         {line}")
'
}

step_build() {
    title "Build"
    run make -s
    wait_key
}

step_status() {
    title "0. Status: tasks of docs/implementation_plan.md"
    python3 -I -c '
import re
done = total = 0
todo = []
for line in open("docs/implementation_plan.md"):
    m = re.match(r"- \[( |x)\] \*{0,2}(T[0-7]\.\d+|R\d)\b", line)
    if m and m.group(2) != "R5":              # R5 is optional; Phase 8 (T8.x) is stretch
        total += 1
        done += m.group(1) == "x"
        if m.group(1) == " ":
            todo.append(m.group(2))
print(f"  required tasks done: {done} of {total}  ({100 * done / total:.0f} %)")
print("  open: " + " ".join(todo))
'
    say "Done: compiler, classifier, parallel pipeline, correlator, base-paper study. Open: benchmark tooling (Phase 6), full datasets and labelled evaluation (Phase 7)."
    wait_key
}

step_compiler() {
    title "1. Compiler: rules -> NFA -> one combined DFA"
    printf '%s$ bin/triage <26 rules>   (startup line only)%s\n' "$C" "$N"
    bin/triage -o "$demo/empty" /dev/null 2>&1 | sed -n 1p
    say "Unsupported syntax is rejected with its position (rule: failed \\d+ times):"
    printf 'BAD\tERR\tLOW\t0.5\t-\t-\t-\t-\tfailed \\d+ times\n' > "$demo/bad.rules"
    run bin/triage -r "$demo/bad.rules" -o "$demo/empty" /dev/null || true
    run bin/triage --quiet -r tests/fixtures/tiny.rules --dump-dot "$demo/tiny.dot"
    if command -v dot > /dev/null; then
        dot -Tpng "$demo/tiny.dot" -o "$demo/tiny.png"
        say "NFA + DFA picture of a 2-rule file: $demo/tiny.png  (open it now)"
    else
        say "NFA + DFA of a 2-rule file written to $demo/tiny.dot (graphviz not installed)"
    fi
    wait_key
}

step_classify() {
    title "2. Classification: labels, confidence, checksum validation"
    run bin/triage --quiet -w 4 -o "$demo/synthetic" tests/corpus/synthetic_small.log
    say "A URL-encoded SQL injection, and a line with two labels:"
    grep -E '^\{"seq":(42|50),' "$demo/synthetic/alerts.jsonl" | show
    say "Look-alikes get no label: bad Luhn checksum, bad Verhoeff checksum:"
    grep -F -e 'card=4111 1111 1111 1112' -e 'order id 523456789012' "$demo/synthetic/store.jsonl" | show
    wait_key
}

step_pii() {
    title "3. PII masking (e-mail, card, Aadhaar)"
    grep -E '^\{"seq":(22|107|113),' "$demo/synthetic/pii_quarantine.jsonl" | show
    wait_key
}

step_correlate() {
    title "4. Correlator: attacks spread over many lines (real OpenSSH log)"
    run bin/triage --quiet -w 4 -o "$demo/ssh" tests/corpus/OpenSSH_2k.log
    grep -o '"flags":\["[A-Z_]*"' "$demo/ssh/alerts.jsonl" | sed 's/"flags":\["//; s/"$//' | sort | uniq -c
    say "First brute force, and failures followed by a successful login (CRITICAL):"
    { grep -m1 ESCALATED_BRUTE_FORCE "$demo/ssh/alerts.jsonl"
      grep -F ESCALATED_FAIL_THEN_SUCCESS "$demo/synthetic/alerts.jsonl"; } | show
    wait_key
}

step_parallel() {
    title "5. Parallel: 1 worker vs 8 workers, then a live stream"
    bin/triage --quiet -w 1 -o "$demo/w1" tests/corpus/OpenSSH_2k.log
    bin/triage --quiet -w 8 -o "$demo/w8" tests/corpus/OpenSSH_2k.log
    if cmp -s "$demo/w1/store.jsonl" "$demo/w8/store.jsonl" && cmp -s "$demo/w1/alerts.jsonl" "$demo/w8/alerts.jsonl"; then
        printf '%s-w 1 and -w 8: identical output (%s records)%s\n' "$G" "$(wc -l < "$demo/w8/store.jsonl")" "$N"
    else
        say "-w 1 and -w 8 differ only in ingest-time stamps (checked by tools/compare_runs.sh)"
    fi
    say "Live stream, status line every second (queues = backlog per worker):"
    { head -500 tests/corpus/OpenSSH_2k.log; sleep 2.3; } \
        | bin/triage --engine posix -w 4 -o "$demo/stream" - 2>&1 | grep 'lines/s' || true
    wait_key
}

step_speed() {
    title "6. Speed: DFA engine vs C library regexec (1 worker, same 26 rules)"
    local big="$demo/big.log" n t0 t1
    for _ in $(seq 20); do cat tests/corpus/*.log; done > "$big"
    n=$(wc -l < "$big")
    say "$n log lines; each run includes rule compilation, so absolute numbers are lower than in the report:"
    for e in dfa posix; do
        t0=$(date +%s.%N)
        bin/triage --quiet --engine "$e" -w 1 -o "$demo/speed_$e" "$big"
        t1=$(date +%s.%N)
        awk -v e="$e" -v n="$n" -v a="$t0" -v b="$t1" 'BEGIN { printf "  %-5s %6.2f s   %9.0f lines/s\n", e, b - a, n / (b - a) }'
    done
    wait_key
}

step_basepaper() {
    title "7. Base paper: reproduced and measured"
    python3 -I -c '
import re
want = {"1": "per-pattern parallelism speeds up classification",
        "3": "near-linear scaling to 6 cores",
        "6": "3.7 ms parallel vs 8.9 ms sequential"}
for line in open("results/basepaper/SUMMARY.md"):
    if line.startswith("## Detail tables"):
        break
    m = re.match(r"\| ([0-9]) \|", line)
    if m and m.group(1) in want:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        verdict = re.sub(r"[*][*](.*?)[*][*].*", r"\1", cells[4])
        print(f"  claim {cells[0]}: {want[cells[0]]:<48} -> {verdict}")
'
    say "All six claims, tables and figures: results/basepaper/SUMMARY.md, results/basepaper/figures/"
}

for s in $steps; do
    case "$s" in
        build|status|compiler|classify|pii|correlate|parallel|speed|basepaper) "step_$s" ;;
        *) echo "unknown step '$s'" >&2; exit 2 ;;
    esac
done
title "Done - outputs in $demo/"
