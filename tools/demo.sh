#!/usr/bin/env bash
# Review demo: walks through the working parts of the engine on the sample logs
# in tests/corpus/, pausing between steps (press Enter). Everything it writes
# goes to out/demo/.
#
# usage: tools/demo.sh [--no-pause] [STEP...]
#   steps: build compiler classify pii correlate parallel basepaper (default: all)
# Talking points for each step: docs/demo_guide.md
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
pause=1
if [ "${1:-}" = "--no-pause" ]; then pause=0; shift; fi
steps="${*:-build compiler classify pii correlate parallel basepaper}"
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
    line = r["line"] if len(r["line"]) <= 90 else r["line"][:87] + "..."
    seq, sev, conf = r["seq"], r["severity"], r["confidence"]
    print(f"  #{seq:<5} {sev:<8} conf {conf:.2f}  {labels:<30} {flags}")
    print(f"         {line}")
'
}

step_build() {
    title "1. Build and self-check"
    run make -s
    say "Unit tests, end-to-end checks and the sanitizer runs are in 'make test', 'make e2e', 'make compare'."
    wait_key
}

step_compiler() {
    title "2. Our regex compiler: rules -> Thompson NFA -> one combined DFA"
    say "26 rules, compiled at startup (sizes and build time on the first line):"
    run bin/triage -o "$demo/empty" /dev/null
    say "Anything outside the regular subset is rejected with its position, never approximated:"
    printf 'BAD\tERR\tLOW\t0.5\t-\t-\t-\t-\tfailed \\d+ times\n' > "$demo/bad.rules"
    run cat "$demo/bad.rules"
    run bin/triage -r "$demo/bad.rules" -o "$demo/empty" /dev/null || true
    say "The automata of a 2-rule file as a Graphviz diagram:"
    run bin/triage --quiet -r tests/fixtures/tiny.rules --dump-dot "$demo/tiny.dot"
    if command -v dot > /dev/null; then
        run dot -Tpng "$demo/tiny.dot" -o "$demo/tiny.png"
        say "-> $demo/tiny.png"
    else
        say "-> $demo/tiny.dot written (install graphviz to render it: sudo apt install graphviz)"
    fi
    wait_key
}

step_classify() {
    title "3. Classification: multi-label, confidence, severity"
    run bin/triage --engine dfa -w 4 -o "$demo/synthetic" tests/corpus/synthetic_small.log
    say "High/critical alerts (out/demo/synthetic/alerts.jsonl), first 6:"
    head -6 "$demo/synthetic/alerts.jsonl" | show
    say "Look-alikes are not flagged: checksums decide (Luhn for cards, Verhoeff for Aadhaar)."
    grep -F -e 'card=4111 1111 1111 1112' -e 'order id 523456789012' "$demo/synthetic/store.jsonl" | show
    wait_key
}

step_pii() {
    title "4. PII masking: values never reach any output file"
    say "Records in pii_quarantine.jsonl, first 6 (the line text is masked):"
    head -6 "$demo/synthetic/pii_quarantine.jsonl" | show
    wait_key
}

step_correlate() {
    title "5. Correlator: attacks that no single line shows (real OpenSSH log)"
    run bin/triage --engine dfa -w 4 -o "$demo/ssh" tests/corpus/OpenSSH_2k.log
    say "Escalations by kind:"
    grep -o '"flags":\["[A-Z_]*"' "$demo/ssh/alerts.jsonl" | sed 's/"flags":\["//; s/"$//' | sort | uniq -c
    say "First brute-force and first password-spray escalation:"
    { grep -m1 ESCALATED_BRUTE_FORCE "$demo/ssh/alerts.jsonl"; grep -m1 ESCALATED_PASSWORD_SPRAY "$demo/ssh/alerts.jsonl"; } | show
    say "Brute force followed by a successful login (synthetic sequence) is CRITICAL:"
    grep -F ESCALATED_FAIL_THEN_SUCCESS "$demo/synthetic/alerts.jsonl" | show
    wait_key
}

step_parallel() {
    title "6. Parallel pipeline: same answers with 1 or 8 workers"
    run bin/triage --quiet -w 1 -o "$demo/w1" tests/corpus/OpenSSH_2k.log
    run bin/triage --quiet -w 8 -o "$demo/w8" tests/corpus/OpenSSH_2k.log
    if cmp -s "$demo/w1/store.jsonl" "$demo/w8/store.jsonl" && cmp -s "$demo/w1/alerts.jsonl" "$demo/w8/alerts.jsonl"; then
        printf '%sidentical output (%s records)%s\n' "$G" "$(wc -l < "$demo/w8/store.jsonl")" "$N"
    else
        say "outputs differ only in the ingest-time stamp of lines without a timestamp; records compared by tools/compare_runs.sh"
    fi
    say "Live stream from a pipe, with the 1-per-second status line (lines/s, queue per worker, counts):"
    { cat tests/corpus/OpenSSH_2k.log; sleep 2.5; cat tests/corpus/synthetic_small.log; sleep 1.5; } \
        | bin/triage --engine posix -w 4 -o "$demo/stream" - || true
    wait_key
}

step_basepaper() {
    title "7. Base paper reproduced and measured (results/basepaper/)"
    say "Verdict per claim (full tables and figures in results/basepaper/SUMMARY.md and figures/):"
    python3 -I -c '
import re
for line in open("results/basepaper/SUMMARY.md"):
    if line.startswith("## Detail tables"):
        break
    if re.match(r"\| [0-9] \|", line):
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        verdict = re.sub(r"\*\*(.*?)\*\*.*", r"\1", cells[4])
        print(f"  {cells[0]}. {cells[1][:66]:<66}  {verdict}")
'
    ls results/basepaper/figures/
}

for s in $steps; do
    case "$s" in
        build|compiler|classify|pii|correlate|parallel|basepaper) "step_$s" ;;
        *) echo "unknown step '$s'" >&2; exit 2 ;;
    esac
done
title "Done - outputs in $demo/"
