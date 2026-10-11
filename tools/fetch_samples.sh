#!/usr/bin/env bash
# Download Loghub's 2k-line sample logs into tests/corpus/ (T0.2).
# Source: https://github.com/logpai/loghub  (<System>/<System>_2k.log)
# Idempotent: existing files are kept unless --force is given.
set -euo pipefail

BASE="https://raw.githubusercontent.com/logpai/loghub/master"
SYSTEMS=(OpenSSH Apache Linux BGL HDFS)

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/tests/corpus"
force=0
[[ "${1:-}" == "--force" ]] && force=1

mkdir -p "$dest"
for sys in "${SYSTEMS[@]}"; do
    out="$dest/${sys}_2k.log"
    if [[ -s "$out" && $force -eq 0 ]]; then
        echo "keep   $out"
        continue
    fi
    tmp="$out.tmp"
    curl -fsSL --retry 3 --max-time 120 -o "$tmp" "$BASE/$sys/${sys}_2k.log"
    mv "$tmp" "$out"
    echo "fetch  $out  ($(wc -l < "$out") lines)"
done
