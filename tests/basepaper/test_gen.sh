#!/bin/sh
# R3 check: gen_bp.py is deterministic and the committed labeled.tsv is its output;
# the file has 1,250 labeled lines covering all five categories and NONE.
set -eu
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$here/../../tools/basepaper/gen_bp.py" "$tmp/a.tsv" 2> "$tmp/log"
python3 "$here/../../tools/basepaper/gen_bp.py" "$tmp/b.tsv" 2> /dev/null
cmp "$tmp/a.tsv" "$tmp/b.tsv" || { echo "gen_bp.py is not deterministic"; exit 1; }
cmp "$tmp/a.tsv" "$here/labeled.tsv" || { echo "labeled.tsv is stale: rerun tools/basepaper/gen_bp.py"; exit 1; }

n=$(grep -a -vc '^#' "$here/labeled.tsv")
[ "$n" -eq 1250 ] || { echo "expected 1250 labeled lines, found $n"; exit 1; }
for c in EMAIL PHONE QUESTION GREETING NAME NONE; do
    k=$(grep -a -v '^#' "$here/labeled.tsv" | cut -f2 | tr ',' '\n' | grep -c "^$c\$" || true)
    [ "$k" -gt 100 ] || { echo "category $c has only $k lines"; exit 1; }
done
# every line: three tab-separated columns, no tab inside the text, <= 256 bytes
bad=$(grep -a -v '^#' "$here/labeled.tsv" | awk -F'\t' 'NF != 3 || length($1) > 256 {c++} END {print c+0}')
[ "$bad" -eq 0 ] || { echo "$bad malformed lines in labeled.tsv"; exit 1; }
echo "test_gen: deterministic; labeled.tsv has $n lines"
sed 's/^/  /' "$tmp/log"
