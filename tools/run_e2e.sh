#!/usr/bin/env bash
# End-to-end check: run the triage binary on tests/corpus/synthetic_small.log and
# compare (seq, primary, labels) with tests/expected/synthetic_small.expected.
#
# usage: tools/run_e2e.sh [path/to/triage]        (default: bin/triage)
# env:   E2E_ENGINES="posix dfa"                  engines to run (default: posix)
#
# Per engine it runs three ways - file input, stdin, and file input with a tiny
# batch size (-b 7) - and also checks that the output files are valid JSONL and
# that alerts.jsonl holds exactly the HIGH/CRITICAL records of store.jsonl.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bin="${1:-bin/triage}"
engines="${E2E_ENGINES:-posix}"
corpus=tests/corpus/synthetic_small.log
expected=tests/expected/synthetic_small.expected

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

grep -v '^#' "$expected" > "$tmp/expected.tsv"

check() {   # check LABEL OUTDIR
    local label="$1" out="$2"
    if ! python3 -I tools/check_jsonl.py "$out/store.jsonl" "$out/alerts.jsonl" > "$tmp/check.log"; then
        cat "$tmp/check.log"; echo "e2e FAIL [$label]: invalid JSONL"; fail=1; return
    fi
    python3 -I tools/jsonl_summary.py "$out/store.jsonl" > "$tmp/store.tsv"
    python3 -I tools/jsonl_summary.py "$out/alerts.jsonl" | cut -f1 > "$tmp/alert_seqs.txt"
    if ! diff -u "$tmp/expected.tsv" <(cut -f1-3 "$tmp/store.tsv") > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: classification differs from $expected"; fail=1; return
    fi
    awk -F'\t' '$4=="HIGH" || $4=="CRITICAL" {print $1}' "$tmp/store.tsv" > "$tmp/want_alerts.txt"
    if ! diff -u "$tmp/want_alerts.txt" "$tmp/alert_seqs.txt" > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: alerts.jsonl is not the HIGH/CRITICAL subset"; fail=1; return
    fi
    echo "e2e ok   [$label]: $(wc -l < "$tmp/store.tsv") records, $(wc -l < "$tmp/alert_seqs.txt") alerts"
}

for engine in $engines; do
    out="$tmp/$engine-file";  "$bin" --quiet --engine "$engine" -w 1 -o "$out" "$corpus"
    check "$engine file" "$out"
    out="$tmp/$engine-stdin"; "$bin" --quiet --engine "$engine" -w 1 -o "$out" - < "$corpus"
    check "$engine stdin" "$out"
    out="$tmp/$engine-b7";    "$bin" --quiet --engine "$engine" -w 1 -b 7 -o "$out" "$corpus"
    check "$engine -b 7" "$out"
done

exit $fail
