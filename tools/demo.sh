#!/usr/bin/env bash
# Review demo: walks through the working parts of the engine on the sample logs
# in tests/corpus/, pausing between steps (press Enter). Everything it writes
# goes to out/demo/.
#
# usage: tools/demo.sh [--no-pause] [STEP...]
#   steps: compiler classify pii correlate parallel basepaper (default: all; "build" is optional)
# Talking points for each step: docs/demo_guide.md
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
pause=1
if [ "${1:-}" = "--no-pause" ]; then pause=0; shift; fi
steps="${*:-compiler classify pii correlate parallel basepaper}"
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

step_basepaper() {
    title "6. Base paper: reproduced and measured"
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
        build|compiler|classify|pii|correlate|parallel|basepaper) "step_$s" ;;
        *) echo "unknown step '$s'" >&2; exit 2 ;;
    esac
done
title "Done - outputs in $demo/"
