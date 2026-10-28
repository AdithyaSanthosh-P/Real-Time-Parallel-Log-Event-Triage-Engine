#!/usr/bin/env bash
# Download the full Loghub datasets into data/ (gitignored) (plan T7.1, spec §10.1).
#
#   tools/fetch_datasets.sh [--force] [openssh apache linux bgl ...]   (default: those four)
#   tools/fetch_datasets.sh --ait [SCENARIO]                           AIT-LDS v2.0 scenario (large!)
#
# Sources (Zenodo record 8196385, linked from the Loghub README):
#   openssh  SSH.tar.gz      655,146 lines    apache  Apache.tar.gz    56,481 lines
#   linux    Linux.tar.gz     25,567 lines    bgl     BGL.zip       4,747,963 lines
# AIT-LDS v2.0 (Zenodo record 5789064) has eight scenarios of 7-27 GB each; --ait downloads one
# (default russellmitchell, the smallest, 7.1 GB), checks its md5 and extracts only the labelled
# web/auth logs. Nothing here runs in the tests. Prints size, line count and sha256 of every file.
# Idempotent: existing files are kept unless --force is given.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
data="$root/data"
mkdir -p "$data"
force=0
ait=0
want=()
for a in "$@"; do
    case "$a" in
        --force) force=1 ;;
        --ait) ait=1 ;;
        *) want+=("$a") ;;
    esac
done

Z=https://zenodo.org/records/8196385/files
declare -A URL=( [openssh]="$Z/SSH.tar.gz?download=1" [apache]="$Z/Apache.tar.gz?download=1"
                 [linux]="$Z/Linux.tar.gz?download=1" [bgl]="$Z/BGL.zip?download=1" )
declare -A OUT=( [openssh]=OpenSSH.log [apache]=Apache.log [linux]=Linux.log [bgl]=BGL.log )

report() {   # report FILE
    printf '  %-28s %12s bytes %10s lines  sha256 %s\n' "$(basename "$1")" "$(stat -c %s "$1")" \
        "$(wc -l < "$1")" "$(sha256sum "$1" | cut -d' ' -f1)"
}

fetch_loghub() {   # fetch_loghub NAME
    local name="$1" out="$data/${OUT[$1]}" tmp
    if [ -s "$out" ] && [ "$force" -eq 0 ]; then echo "keep   $out"; report "$out"; return; fi
    tmp="$(mktemp -d "$data/.dl.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    echo "fetch  $name ..."
    curl -fL --retry 3 --retry-delay 5 --max-time 3600 -C - -o "$tmp/archive" "${URL[$name]}"
    case "${URL[$name]}" in
        *.zip*) unzip -q -o "$tmp/archive" -d "$tmp/x" ;;
        *)      mkdir "$tmp/x"; tar -xzf "$tmp/archive" -C "$tmp/x" ;;
    esac
    local f
    f="$(find "$tmp/x" -type f -name '*.log' | head -1)"      # the full log (e.g. SSH.log)
    [ -n "$f" ] || { echo "fetch_datasets: no .log in the $name archive" >&2; return 1; }
    mv "$f" "$out"
    report "$out"
}

fetch_ait() {
    local sc="${1:-russellmitchell}" dir="$data/ait"
    mkdir -p "$dir"
    local zip="$dir/$sc.zip"
    local md5
    md5="$(curl -fsSL "https://zenodo.org/api/records/5789064" | python3 -I -c "
import json, sys
for f in json.load(sys.stdin)['files']:
    if f['key'] == '$sc.zip': print(f['checksum'].split(':')[1])")"
    [ -n "$md5" ] || { echo "fetch_datasets: unknown AIT scenario '$sc'" >&2; return 1; }
    echo "fetch  AIT-LDS v2.0 $sc (md5 $md5) ..."
    curl -fL --retry 3 --retry-delay 5 -C - -o "$zip" "https://zenodo.org/records/5789064/files/$sc.zip?download=1"
    [ "$(md5sum "$zip" | cut -d' ' -f1)" = "$md5" ] || { echo "fetch_datasets: md5 mismatch for $zip" >&2; return 1; }
    unzip -q -o "$zip" -d "$dir/$sc"
    echo "  extracted to $dir/$sc (labels: <scenario>_labels* / gather/*/logs; see the AIT README)"
}

if [ "$ait" -eq 1 ]; then fetch_ait "${want[0]:-}"; exit 0; fi
[ "${#want[@]}" -gt 0 ] || want=(openssh apache linux bgl)
for n in "${want[@]}"; do
    [ -n "${URL[$n]:-}" ] || { echo "fetch_datasets: unknown dataset '$n' (openssh apache linux bgl)" >&2; exit 2; }
    fetch_loghub "$n"
done
