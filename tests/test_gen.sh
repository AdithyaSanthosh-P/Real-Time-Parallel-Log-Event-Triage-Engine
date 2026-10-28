#!/usr/bin/env bash
# T6.3: the synthetic generator is reproducible, hits its configured rates exactly, and its
# ground truth agrees with the engine's labels.
#
# usage: tests/test_gen.sh BIN_DIR
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
bin_dir="$1"
gen="$bin_dir/gen"
triage="$bin_dir/triage"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# 1. a million lines with a fixed seed: identical bytes twice, different bytes for another seed
"$gen" -n 1000000 --seed 42 --truth "$tmp/t1.tsv" > "$tmp/a.log" 2> "$tmp/gen.err"
"$gen" -n 1000000 --seed 42 --truth "$tmp/t2.tsv" 2>/dev/null | cmp - "$tmp/a.log"
cmp "$tmp/t1.tsv" "$tmp/t2.tsv"
"$gen" -n 1000 --seed 43 2>/dev/null | cmp -s - <(head -1000 "$tmp/a.log") && { echo "test_gen FAIL: seeds 42 and 43 agree"; exit 1; }
[ "$(wc -l < "$tmp/a.log")" -eq 1000000 ]
echo "test_gen ok   [reproducible]: sha256 $(sha256sum < "$tmp/a.log" | cut -c1-16)... (1,000,000 lines, seed 42)"

# 2. ground-truth counts equal the configured rates (default rates; allowed error 1 %)
python3 -I - "$tmp/t1.tsv" <<'PY'
import collections, sys
want = {"AUTH_FAILURE": 0.05, "SECURITY_THREAT": 0.03, "ERROR": 0.05, "PERF_WARNING": 0.03, "PII": 0.03}
n = collections.Counter(); kinds = collections.Counter()
for l in open(sys.argv[1]):
    _, lab, kind = l.rstrip("\n").split("\t")
    n[lab] += 1
    kinds[kind.split("_")[0]] += 1
total = sum(n.values())
assert total == 1_000_000, total
for lab, p in want.items():
    exp = round(total * p)
    assert abs(n[lab] - exp) <= 0.01 * exp, (lab, n[lab], exp)
assert abs(kinds["lookalike"] - 20000) <= 200, kinds["lookalike"]
print("test_gen ok   [rates]: " + ", ".join(f"{k} {n[k]}" for k in want) + f", lookalike {kinds['lookalike']}")
PY

# 2b. other rates and a payload file
printf '1 UNION SELECT 1,2\n# comment\n<script>x</script>\n' > "$tmp/p.txt"
"$gen" -n 20000 --seed 5 --rates auth=0.5,sec=0.5 --payloads "$tmp/p.txt" --truth "$tmp/t3.tsv" > "$tmp/c.log" 2>/dev/null
[ "$(cut -f3 "$tmp/t3.tsv" | grep -c '^auth_')" -eq 10000 ] && [ "$(cut -f3 "$tmp/t3.tsv" | grep -c '^sec_')" -eq 10000 ]
grep -q -i -e 'UNION' -e 'script' "$tmp/c.log"
! "$gen" --rates auth=0.7,sec=0.7 > /dev/null 2>&1          # rates over 1 are refused
! "$gen" --rates bogus=0.1 > /dev/null 2>&1
echo "test_gen ok   [options]: custom rates, payload file, invalid rates refused"

# 3. the engine's labels equal the ground truth, line by line (both engines), at -w 4
"$gen" -n 100000 --seed 9 --truth "$tmp/t4.tsv" > "$tmp/d.log" 2>/dev/null
for eng in dfa posix; do
    "$triage" --quiet --engine "$eng" -w 4 -o "$tmp/out_$eng" "$tmp/d.log"
    python3 -I tools/check_gen.py "$tmp/t4.tsv" "$tmp/out_$eng/store.jsonl" | tail -1 | sed "s/^/test_gen ok   [$eng labels]: /"
done

# 4. pacing: 300 lines at 200 lines/s take about 1.5 s
t0=$(date +%s%N)
"$gen" -n 300 --seed 1 --rate 200 > /dev/null 2>&1
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
[ "$ms" -ge 1400 ] && [ "$ms" -le 2500 ] || { echo "test_gen FAIL: pacing took $ms ms"; exit 1; }
echo "test_gen ok   [pacing]: 300 lines at 200/s in $ms ms"
