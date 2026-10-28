#!/usr/bin/env bash
# Build rules/bench_{5,10,20,32,64}.rules (plan T6.4) from rules/default.rules (26 rules) and
# rules/bench_extra.rules (38 more). The sets are nested: every set contains the smaller ones.
#   bench_5   one rule per category (SSH_FAIL_PW, SQLI, ERR_LEVEL, HIGH_LATENCY, PII_EMAIL)
#   bench_10  + 5 more default rules       bench_20  + 10 more default rules
#   bench_32  all 26 defaults + 6 extra     bench_64  26 defaults + all 38 extra
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
rules() { grep -v '^#' "$1"; }
by_id() { rules rules/default.rules | awk -F'\t' -v ids="$1" 'BEGIN{n=split(ids,a," ");for(i=1;i<=n;i++)w[a[i]]=i} ($1 in w){print w[$1]"\t"$0}' | sort -n | cut -f2-; }
hdr='# id	category	severity	base	flags	validator	extract	context	regex'
first5="SSH_FAIL_PW SQLI ERR_LEVEL HIGH_LATENCY PII_EMAIL"
next5="SSH_INVALID_USER XSS HTTP_5XX SLOW_QUERY PII_CARD"
next10="PATH_TRAVERSAL CMD_INJECTION JAVA_EXCEPTION CRASH_OOM TIMEOUT RESOURCE_PRESSURE PII_AADHAAR PII_PAN PII_SECRET_QS SSH_ACCEPTED"
emit() {   # emit N DEFAULT_IDS EXTRA_COUNT
    { echo "# bench rule set with $1 rules, built by tools/make_bench_rules.sh; do not edit"; echo "$hdr"
      if [ "$2" = all ]; then rules rules/default.rules; else by_id "$2"; fi
      rules rules/bench_extra.rules | head -n "$3"; } > "rules/bench_$1.rules"
}
emit 5 "$first5" 0
emit 10 "$first5 $next5" 0
emit 20 "$first5 $next5 $next10" 0
emit 32 all 6
emit 64 all 38
for n in 5 10 20 32 64; do echo "rules/bench_$n.rules: $(grep -vc '^#' rules/bench_$n.rules) rules"; done
