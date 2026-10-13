#!/usr/bin/env bash
# Run-equivalence check for the parallel runtime (plan T4.6).
#
# For every corpus file, engines posix and dfa, and -w 1,2,4,8 (plus a few
# dispatch / batch / input variations), the three sinks of the parallel
# pipeline must hold exactly the records of the single-threaded Phase 3
# pipeline: same lines, byte for byte, in any order. Records are compared
# whole (seq, ts, primary, severity, confidence, labels, rules, flags, masked
# line), which covers the plan's (seq, primary, labels, confidence). The one
# exception is "ts": a line without a parseable timestamp gets the ingest time
# (spec §6), which differs between any two runs, so ts is blanked on both sides.
#
# The reference is the last single-threaded commit (REF_COMMIT, default
# 4f2e1ca = T3.5), built once into build/ref-<commit>/; set REF_BIN to use
# another binary instead.
#
# usage: tools/compare_runs.sh [path/to/triage]        (default: bin/triage)
# env:   WORKERS="1 2 4 8"  ENGINES="posix dfa"  REF_COMMIT=...  REF_BIN=...
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bin="${1:-bin/triage}"
workers="${WORKERS:-1 2 4 8}"
engines="${ENGINES:-posix dfa}"
ref_commit="${REF_COMMIT:-4f2e1ca}"

if [ -z "${REF_BIN:-}" ]; then
    refdir="build/ref-$ref_commit"
    if [ ! -x "$refdir/bin/triage" ]; then
        echo "compare_runs: building the reference binary from commit $ref_commit in $refdir"
        rm -rf "$refdir"; mkdir -p "$refdir"
        git archive "$ref_commit" | tar -x -C "$refdir"
        make -s -C "$refdir" bin/triage > /dev/null
    fi
    REF_BIN="$refdir/bin/triage"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0
runs=0

sorted() { sed 's/^{"seq":\([0-9]*\),"ts":"[^"]*",/{"seq":\1,"ts":"",/' "$1" | LC_ALL=C sort; }

compare() {   # compare LABEL REF_OUT OUT
    local label="$1" ref="$2" out="$3" f
    for f in store.jsonl alerts.jsonl pii_quarantine.jsonl; do
        if ! cmp -s <(sorted "$ref/$f") <(sorted "$out/$f"); then
            diff <(sorted "$ref/$f") <(sorted "$out/$f") | head -6
            echo "compare_runs FAIL [$label]: $f differs from the single-threaded run"
            fail=1
            return
        fi
    done
    runs=$((runs + 1))
}

for log in tests/corpus/*.log; do
    name="$(basename "$log" .log)"
    for engine in $engines; do
        ref="$tmp/ref-$name-$engine"
        "$REF_BIN" --quiet --engine "$engine" -o "$ref" "$log"
        lines="$(wc -l < "$ref/store.jsonl")"
        for w in $workers; do
            out="$tmp/$name-$engine-w$w"
            "$bin" --quiet --engine "$engine" -w "$w" -o "$out" "$log"
            compare "$name $engine -w $w" "$ref" "$out"
            rm -rf "$out"
        done
        # variations at the largest worker count: least-loaded dispatch with
        # tiny batches (many jobs in flight), unpinned, and stdin through a
        # pipe with batch-delay flushing
        wmax="${workers##* }"
        out="$tmp/$name-$engine-least"
        "$bin" --quiet --engine "$engine" -w "$wmax" -b 3 --dispatch least -o "$out" "$log"
        compare "$name $engine -w $wmax -b 3 --dispatch least" "$ref" "$out"
        rm -rf "$out"
        out="$tmp/$name-$engine-pipe"
        cat "$log" | "$bin" --quiet --engine "$engine" -w "$wmax" --no-pin --batch-delay 0 -o "$out" -
        compare "$name $engine -w $wmax stdin --no-pin --batch-delay 0" "$ref" "$out"
        rm -rf "$out" "$ref"
        echo "compare_runs ok   [$name $engine]: $lines records"
    done
done

[ "$fail" -eq 0 ] && echo "compare_runs: $runs runs equal to the single-threaded reference"
exit $fail
