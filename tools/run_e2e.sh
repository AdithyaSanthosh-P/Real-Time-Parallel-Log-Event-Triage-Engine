#!/usr/bin/env bash
# End-to-end check: run the triage binary on tests/corpus/synthetic_small.log and
# compare (seq, primary, labels) with tests/expected/synthetic_small.expected.
#
# usage: tools/run_e2e.sh [path/to/triage]        (default: bin/triage)
# env:   E2E_ENGINES="posix dfa"                  engines to run (default: both)
#
# Per engine it runs three ways - file input, stdin, and file input with a tiny
# batch size (-b 7) - and also checks that the output files are valid JSONL and
# that alerts.jsonl holds exactly the HIGH/CRITICAL records of store.jsonl.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bin="${1:-bin/triage}"
engines="${E2E_ENGINES:-posix dfa}"
corpus=tests/corpus/synthetic_small.log
expected=tests/expected/synthetic_small.expected

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

grep -v '^#' "$expected" > "$tmp/expected.tsv"

# The expected file must be what the independent reference classifier derives (spec 5).
python3 -I tools/ref_classify.py --explain "$corpus" > "$tmp/ref.tsv"
if ! diff -u "$tmp/expected.tsv" <(cut -f1-3 "$tmp/ref.tsv") > "$tmp/diff.txt"; then
    head -40 "$tmp/diff.txt"; echo "e2e FAIL: $expected is stale (tools/ref_classify.py disagrees)"; exit 1
fi
cut -f1,4 "$tmp/ref.tsv" > "$tmp/ref_sev.tsv"
python3 -I tools/ref_classify.py --pii-values "$corpus" > "$tmp/pii_values.txt"
if [ "$(wc -l < "$tmp/pii_values.txt")" -lt 10 ]; then
    echo "e2e FAIL: reference classifier found fewer than 10 PII values in the corpus"; exit 1
fi

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
    if ! diff -u "$tmp/ref_sev.tsv" <(cut -f1,4 "$tmp/store.tsv") > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: severity differs from tools/ref_classify.py"; fail=1; return
    fi
    awk -F'\t' '$4=="HIGH" || $4=="CRITICAL" {print $1}' "$tmp/store.tsv" > "$tmp/want_alerts.txt"
    if ! diff -u "$tmp/want_alerts.txt" "$tmp/alert_seqs.txt" > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: alerts.jsonl is not the HIGH/CRITICAL subset"; fail=1; return
    fi
    # spec 7 / T3.5: no PII value found by the reference classifier may appear in ANY output file
    if grep -r -F -f "$tmp/pii_values.txt" "$out" > "$tmp/leaks.txt"; then
        head -5 "$tmp/leaks.txt"; echo "e2e FAIL [$label]: unmasked PII value in the output"; fail=1; return
    fi
    for f in store.jsonl alerts.jsonl pii_quarantine.jsonl; do
        [ -f "$out/$f" ] || { echo "e2e FAIL [$label]: $f missing"; fail=1; return; }
    done
    python3 -I tools/jsonl_summary.py "$out/pii_quarantine.jsonl" | cut -f1 > "$tmp/q_seqs.txt"
    awk -F'\t' '$3 ~ /PII/ {print $1}' "$tmp/store.tsv" > "$tmp/want_q.txt"
    if ! diff -u "$tmp/want_q.txt" "$tmp/q_seqs.txt" > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: pii_quarantine.jsonl is not the PII-labelled lines"; fail=1; return
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
