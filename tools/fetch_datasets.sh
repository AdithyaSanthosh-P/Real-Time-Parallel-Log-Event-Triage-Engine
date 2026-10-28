#!/usr/bin/env bash
# Download the full Loghub datasets into data/ (gitignored) (plan T7.1, spec §10.1).
#
#   tools/fetch_datasets.sh [--force] [openssh apache linux bgl ...]   (default: those four)
#   tools/fetch_datasets.sh --ait [SCENARIO]                           AIT-LDS v2.0: labelled web/auth logs
#
# Sources (Zenodo record 8196385, linked from the Loghub README):
#   openssh  SSH.tar.gz      655,146 lines    apache  Apache.tar.gz    56,481 lines
#   linux    Linux.tar.gz     25,567 lines    bgl     BGL.zip       4,747,963 lines
# AIT-LDS v2.0 (Zenodo record 5789064) has eight scenarios of 7-27 GB each, mostly packet captures;
# --ait reads only the labelled Apache and auth logs of one scenario (default russellmitchell) with
# HTTP range requests (a few MB) and converts the AIT line labels to <log>.truth.tsv (seq, ATTACK|-, labels).
# Nothing here runs in the tests. Prints size, line count and sha256 of every file.
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

fetch_ait() {   # only the labelled web/auth logs, by HTTP range requests (see tools/fetch_ait.py)
    python3 -I "$root/tools/fetch_ait.py" "${1:-russellmitchell}" --out "$data/ait"
}

if [ "$ait" -eq 1 ]; then fetch_ait "${want[0]:-}"; exit 0; fi
[ "${#want[@]}" -gt 0 ] || want=(openssh apache linux bgl)
for n in "${want[@]}"; do
    [ -n "${URL[$n]:-}" ] || { echo "fetch_datasets: unknown dataset '$n' (openssh apache linux bgl)" >&2; exit 2; }
    fetch_loghub "$n"
done
