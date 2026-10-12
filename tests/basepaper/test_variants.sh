#!/bin/sh
# R2 check: every comparison variant gives the same match masks as the paper's fork
# program on every input file in tests/basepaper/, and each variant's JSON validates.
# usage: test_variants.sh BIN_DIR
set -eu
bin=${1:?usage: test_variants.sh BIN_DIR}
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
checked=0
# plain files hold one input per line; the .txt/.tsv fixtures with tab-separated columns
# carry the input in column 1 ('#' lines are comments).
for f in "$here"/*.txt "$here"/*.tsv; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    grep -a -v '^#' "$f" | cut -f1 > "$tmp/in"
    lines=$(wc -l < "$tmp/in")
    [ "$lines" -gt 0 ] || continue
    "$bin/bp_fork" -f "$tmp/in" --quiet > "$tmp/ref"
    for spec in "seq-recompile" "seq-precompiled" "threads-pattern" "threads-data -t 3" "dfa -t 1" "dfa -t 4" "fork"; do
        # shellcheck disable=SC2086
        "$bin/bp_bench" --variant $spec -f "$tmp/in" --dump-masks "$tmp/out" > "$tmp/json"
        python3 "$here/../../tools/basepaper/check_bench_json.py" "$tmp/json" || { echo "bad JSON: $name / $spec"; fail=1; }
        if ! cmp -s "$tmp/ref" "$tmp/out"; then
            echo "MASK MISMATCH: $name / $spec"; diff "$tmp/ref" "$tmp/out" | head -5; fail=1
        fi
        checked=$((checked + 1))
    done
    # cycling through the pool (-n larger than the file) must not change per-input masks
    big=$((lines * 3 + 1))
    "$bin/bp_bench" --variant seq-precompiled -f "$tmp/in" -n "$big" --dump-masks "$tmp/a" > /dev/null
    "$bin/bp_bench" --variant dfa -t 4 -f "$tmp/in" -n "$big" --warmup 5 --dump-masks "$tmp/b" > /dev/null
    cmp -s "$tmp/a" "$tmp/b" || { echo "MASK MISMATCH (-n $big): $name / dfa"; fail=1; }
    echo "  $name: $lines inputs, all variants agree"
done

[ "$checked" -gt 0 ] || { echo "no input files found"; exit 1; }
[ "$fail" -eq 0 ] && echo "test_variants: $checked variant runs agree with bp_fork"
exit "$fail"
