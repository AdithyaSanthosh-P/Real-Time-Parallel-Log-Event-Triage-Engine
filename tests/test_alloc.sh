#!/usr/bin/env bash
# T4.2: the job pool makes the steady state allocation-free.
#
# usage: tests/test_alloc.sh BIN_DIR [SAN]
#   plain build: runs triage with tests/alloc_count.c preloaded on a ~100k-line
#     and a ~500k-line input (the corpus repeated) for both engines and 1 and 4
#     workers, and requires the allocation count outside glibc's regexec to be
#     the same up to the pool size (each pooled job may grow its buffers once;
#     nothing may grow with the input).
#   SAN=address: runs the ASan/LSan binary on the ~100k-line input with 4
#     workers; LeakSanitizer fails the run on any leak.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bin_dir="$1"
san="${2:-}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

make_input() {   # make_input FILE MIN_LINES
    : > "$1"
    while [ "$(wc -l < "$1")" -lt "$2" ]; do cat tests/corpus/*.log >> "$1"; done
}
make_input "$tmp/small.log" 100000

if [ "$san" = address ]; then
    for engine in dfa posix; do
        "$bin_dir/triage" --quiet --engine "$engine" -w 4 -o "$tmp/out" "$tmp/small.log"
    done
    echo "test_alloc: ASan/LSan clean on $(wc -l < "$tmp/small.log") lines (dfa, posix, -w 4)"
    exit 0
fi
[ -z "$san" ] || { echo "test_alloc: nothing to do for SAN=$san"; exit 0; }

make_input "$tmp/large.log" 500000
shim="$bin_dir/tests/alloc_count.so"
fail=0

count() {   # count ENGINE WORKERS FILE -> allocations outside regexec
    LD_PRELOAD="$shim" "$bin_dir/triage" --quiet --engine "$1" -w "$2" -o "$tmp/out" "$3" 2>&1 \
        | awk '/^alloc_count:/ { print $2 - $4 }'
}

for engine in dfa posix; do
    for w in 1 4; do
        small="$(count "$engine" "$w" "$tmp/small.log")"
        large="$(count "$engine" "$w" "$tmp/large.log")"
        pool=$((w * 4 + w + 4))          # pool_size_for(w, QUEUE_DEPTH = 4)
        diff=$((large - small))
        if [ -z "$small" ] || [ -z "$large" ] || [ "${diff#-}" -gt "$pool" ]; then
            echo "test_alloc FAIL [$engine -w $w]: $small allocations at 100k lines, $large at 500k"
            fail=1
        else
            echo "test_alloc ok   [$engine -w $w]: $small allocations at 100k lines, $large at 500k"
        fi
    done
done
exit $fail
