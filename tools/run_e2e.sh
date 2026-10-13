#!/usr/bin/env bash
# End-to-end check: run the triage binary on tests/corpus/synthetic_small.log and
# compare (seq, primary, labels) with tests/expected/synthetic_small.expected.
#
# usage: tools/run_e2e.sh [path/to/triage]        (default: bin/triage)
# env:   E2E_ENGINES="posix dfa"                  engines to run (default: both)
#
# Per engine it runs four ways - file input, stdin, file input with a tiny
# batch size (-b 7), and 4 workers with -b 5 - and also checks that the output
# files are valid JSONL and that alerts.jsonl holds exactly the HIGH/CRITICAL
# records of store.jsonl. Then the Phase 4 runtime: --drop accounting, clean
# shutdown on a write error, and the 1 Hz stats line.
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
cut -f1,7 "$tmp/ref.tsv" > "$tmp/ref_flags.tsv"
if [ "$(awk -F'\t' '$7 != "-"' "$tmp/ref.tsv" | wc -l)" -lt 3 ]; then
    echo "e2e FAIL: the reference finds fewer than 3 correlator escalations in $corpus"; exit 1
fi
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
    if ! diff -u "$tmp/ref_flags.tsv" <(cut -f1,6 "$tmp/store.tsv") > "$tmp/diff.txt"; then
        head -40 "$tmp/diff.txt"; echo "e2e FAIL [$label]: escalation flags differ from tools/ref_classify.py"; fail=1; return
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
    out="$tmp/$engine-w4";    "$bin" --quiet --engine "$engine" -w 4 -b 5 -o "$out" "$corpus"
    check "$engine -w 4 -b 5" "$out"
done

# ---- T5.1: correlator escalations on a real log ----
# Every field of every record (primary, labels, severity, confidence, flags) must
# match the reference classifier, the sample must produce escalations, and every
# escalated record must be in alerts.jsonl.
ssh=tests/corpus/OpenSSH_2k.log
python3 -I tools/ref_classify.py --explain "$ssh" \
    | awk -F'\t' '{printf "%s\t%s\t%s\t%s\t%.2f\t%s\n", $1, $2, $3, $4, $5, $7}' > "$tmp/ssh_ref.tsv"
out="$tmp/ssh"
"$bin" --quiet --engine dfa -w 4 -o "$out" "$ssh"
python3 -I tools/jsonl_summary.py "$out/store.jsonl" > "$tmp/ssh.tsv"
python3 -I tools/jsonl_summary.py "$out/alerts.jsonl" | cut -f1 > "$tmp/ssh_alerts.txt"
awk -F'\t' '$6 != "-" {print $1}' "$tmp/ssh.tsv" > "$tmp/ssh_esc.txt"
nesc="$(wc -l < "$tmp/ssh_esc.txt")"
if ! diff -u "$tmp/ssh_ref.tsv" "$tmp/ssh.tsv" > "$tmp/diff.txt"; then
    head -20 "$tmp/diff.txt"; echo "e2e FAIL [OpenSSH_2k]: differs from tools/ref_classify.py"; fail=1
elif [ "$nesc" -lt 1 ]; then
    echo "e2e FAIL [OpenSSH_2k]: no correlator escalations"; fail=1
elif [ -n "$(comm -23 <(sort "$tmp/ssh_esc.txt") <(sort "$tmp/ssh_alerts.txt"))" ]; then
    echo "e2e FAIL [OpenSSH_2k]: escalated records missing from alerts.jsonl"; fail=1
else
    echo "e2e ok   [OpenSSH_2k]: $nesc escalations ($(awk -F'\t' '$6 != "-" {print $6}' "$tmp/ssh.tsv" | sort | uniq -c | awk '{printf "%s%s %s", sep, $1, $2; sep=", "}')), all in alerts.jsonl, every field equal to the reference"
fi

# ---- Phase 4 runtime ----
big="$tmp/big.log"
for _ in 1 2 3 4 5 6 7 8 9 10; do cat tests/corpus/*.log; done > "$big"
big_lines="$(wc -l < "$big")"

# --drop: every line is either written or counted as dropped
out="$tmp/drop"
"$bin" --engine posix -w 1 -b 1 --drop -o "$out" "$big" 2> "$tmp/drop.err"
dropped="$(sed -n 's/.* \([0-9]*\) dropped -> .*/\1/p' "$tmp/drop.err")"
written="$(wc -l < "$out/store.jsonl")"
uniq_seqs="$(python3 -I tools/jsonl_summary.py "$out/store.jsonl" | cut -f1 | sort -u | wc -l)"
if [ -z "$dropped" ] || [ $((written + dropped)) -ne "$big_lines" ] || [ "$uniq_seqs" -ne "$written" ]; then
    cat "$tmp/drop.err"; echo "e2e FAIL [--drop]: $written written + ${dropped:-?} dropped != $big_lines lines"; fail=1
else
    echo "e2e ok   [--drop]: $written written + $dropped dropped = $big_lines lines"
fi

# a write error stops every thread: exit status 1, an error message, no hang
out="$tmp/full"; mkdir -p "$out"; ln -s /dev/full "$out/store.jsonl"
set +e
timeout 60 "$bin" --quiet --engine dfa -w 4 -o "$out" "$big" 2> "$tmp/full.err"
rc=$?
set -e
if [ "$rc" -ne 1 ] || ! grep -q "write error" "$tmp/full.err"; then
    cat "$tmp/full.err"; echo "e2e FAIL [write error]: exit status $rc (124 = hung)"; fail=1
else
    echo "e2e ok   [write error]: exit 1 with \"$(head -1 "$tmp/full.err")\""
fi

# the stats line appears about once a second while input is flowing
out="$tmp/stats"
{ head -1000 "$corpus"; sleep 3.5; } | "$bin" --engine posix -w 2 -o "$out" - 2> "$tmp/stats.err"
nstats="$(grep -c ' lines/s | queues .* | AUTH .* | dropped ' "$tmp/stats.err" || true)"
if [ "$nstats" -lt 2 ]; then
    cat "$tmp/stats.err"; echo "e2e FAIL [stats]: expected >= 2 stats lines, got $nstats"; fail=1
else
    echo "e2e ok   [stats]: $nstats stats lines, e.g. \"$(grep -m1 'lines/s' "$tmp/stats.err")\""
fi

exit $fail
