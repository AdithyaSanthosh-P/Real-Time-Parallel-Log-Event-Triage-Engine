# Demo guide (finished project: all 41 required tasks)

## How to run

```
cd /home/adi/Desktop/Projects/CD
make                      # build once (bin/triage); the demo does not rebuild
make demo                 # all steps, pauses after each one: press Enter to continue
```

Other ways: `tools/demo.sh --no-pause` runs straight through (about 35 s, about 120 lines of output);
`tools/demo.sh correlate` runs one step. Steps, in order: `status compiler classify pii correlate
parallel speed generator accuracy basepaper` (`build` is an optional extra). Everything the demo writes goes to
`out/demo/` (gitignored). Dry-run once on the demo machine first. Optional: `sudo apt install graphviz`
so step 1 also renders the DFA picture `out/demo/tiny.png`.

Not part of the demo, but the evidence behind it: `make test` (19 test programs), `make difftest`
(0 mismatches against glibc), `make e2e`, `make compare` (72 runs), and the same under
`make SAN=thread` / `make SAN=address`.

## Live dashboard (optional, after the steps)

`make dashboard`, open http://127.0.0.1:8765/, choose "OpenSSH sample" and press Start: the strip chart slides, the feed fills,
and the brute-force and password-spray alerts appear after a few seconds. The Experiments and Accuracy tabs show the saved
results as charts. The Palette button changes the colour scheme (light by default). Do one dry run first and close other programs.

## What each step shows and what the output means

**0. Status.** Reads `docs/implementation_plan.md` and counts the required tasks that are ticked: 41 of 41.
Optional R5 and the stretch tasks (Phase 8) are not counted. Say: the engine, the benchmark tooling and the
evaluation scripts are complete and tested; what is not done is the hand-labelled evaluation, which needs the
two of us to label a sample independently (the blank sheets and the evaluation tool are ready).

**1. Compiler (the compiler-design core).** The 26 rules go through a recursive-descent parser
(AST), Thompson construction (NFA) and subset construction (DFA). The first line reads
`NFA 1762 states, 2 DFA groups, 7593 DFA states, built in ~470 ms`: the NFA has 1,762 states; the DFA
is split into 2 groups because one DFA would exceed the 20,000-state cap (39,764 states uncapped), and
the groups total 7,593 states. Next, the rule `failed \d+ times` is refused with its position
(`offset 7`) and a hint (`write [0-9]`): the parser accepts only the regular POSIX-ERE subset, which
is what guarantees linear-time matching and lets glibc `regexec` act as an independent oracle.
Last, a 2-rule file is dumped as Graphviz (`out/demo/tiny.png`): dashed edges are epsilon moves.
Code: `src/regex_parse.c`, `src/nfa.c`, `src/dfa.c`.

**2. Classification.** `synthetic_small.log` is classified with 4 workers. Each record shows
`#seq SEVERITY conf X labels flags`. #42 is a URL-encoded SQL injection (SECURITY_THREAT, CRITICAL,
confidence 0.75). #50 carries two labels (SECURITY_THREAT and ERROR): lines are multi-label.
Confidence is a noisy-OR of rule scores (base score, +0.25 if a checksum passed, +0.15 for a context
keyword); a label needs confidence of at least 0.5. #111 (card number with a bad Luhn checksum) and
#119 (12-digit number failing the Aadhaar Verhoeff check) get no label (`INFO`, conf 0.00): a regex
alone would have flagged both.

**3. PII masking.** The e-mail in #22, the card in #107 and the Aadhaar number in #113 are masked
(`***@example.com`, `XXXX...1111`, `XXXX XXXX 2346`) before anything is written, and the records are
copied to `pii_quarantine.jsonl`. `make e2e` greps every output file for every PII value of the
corpus and fails if one survives.

**4. Correlator.** The real Loghub OpenSSH log (2,000 lines) gives 34 `ESCALATED_BRUTE_FORCE` (at
least 5 failures from one IP within 60 s) and 5 `ESCALATED_PASSWORD_SPRAY` (at least 5 distinct users
within 300 s): the individual lines are MEDIUM, the episode is one HIGH alert (confidence 0.95). The
last record is the synthetic case failures followed by a successful login from the same address:
`ESCALATED_FAIL_THEN_SUCCESS`, CRITICAL. Windows use the log's own timestamps. A separate Python
implementation agrees on every line.

**5. Parallel pipeline.** `-w 1` and `-w 8` give byte-identical output (2,000 records): workers
finish out of order, but the correlator restores input order. The two `triage:` lines are the live
status line (once per second): lines/s, backlog per worker queue, counts per category, dropped lines.
The first shows about 500 lines/s because the input is a pipe fed at that pace; the second is 0 once the
input stops. Say: reader, workers, aggregator, correlator, router are separate threads joined by
lock-free single-producer single-consumer rings; the DFA tables are read-only and shared without locks.

**6. Speed.** The same 204,280 lines (the six corpus files 20 times) run on one worker with the DFA
engine and with the glibc `regexec` engine on the same 26 rules. The line shows seconds and lines/s
per engine; expect the DFA to be about 3-4 times faster here. This run includes rule compilation
(about 0.5 s for the DFA), so it is lower than the report's figure of 13.0x, which subtracts
start-up on a 1,021,400-line input (results/milestone2/throughput.json). Quote the report's figure
as "measured in results/", this step as a live sanity check. Do not claim the DFA scales with
workers: in `results/milestone2/scaling.json` it stays flat near a million lines/s because the serial
stages limit it (the C library engine scales 4.4x at 8 workers).

**7. Generator, ground truth and --bench.** `bin/gen` writes 50,000 log lines (four log types) and a truth file
(seq, true labels, kind) with exactly the configured number of injected lines per category. The engine's labels
are compared with the truth line by line: 0 differ. This is a consistency check (generator and rules were written
together), not accuracy on real logs. `--bench` preloads the input, turns the sinks off and prints one JSON line:
lines/s, MB/s, latency p50/p90/p99/p99.9, DFA size. The last table is the saved rule-count experiment E3
(`results/SUMMARY.md`): POSIX falls with the rule count (259,850 to 36,829 lines/s from 5 to 64 rules), the DFA
stays near 880,000 up to 32 rules and falls at 64, where the rule set needs 7 DFA groups (one pass per group).

**8. Accuracy.** Three pieces of evidence, each labelled for what it is. (a) Validators: on the generated set with
look-alike numbers PII precision goes from 0.60 (no validators, 4,000 false labels) to 1.00; on the real logs the
PII-labelled lines drop (BGL 12,279 to 1,175, OpenSSH 163 to 0, HDFS 2k 119 to 1). (b) 100 real log lines labelled by
an AI annotator (Claude): this is NOT human ground truth and it also wrote the rules; failed-login precision is 1.00
but recall 0.69, error recall is near 0 (the rule misses sshd `error:` lines), PII precision 0.80 (all-zero memory
addresses pass the card checksum). (c) AIT-LDS, real attack-labelled logs: only 7 of 7,739 attack lines detected, 0
false alarms: the rules have no rule for those requests. Say plainly: human labelling is the open item, sheets are in
`data/labeling_100/`.

**9. Base paper.** Three of the professor's paper's six claims, with the verdict from our measured
reproduction (`results/basepaper/SUMMARY.md`): per-pattern parallelism, near-linear scaling to 6
cores and "3.7 ms vs 8.9 ms" are all *not reproduced*: the fork-per-pattern design is about 12.6x
slower than a sequential loop that recompiles and 424x slower than one that precompiles, and only
splitting the *inputs* across cores scales.

## Sample output (one real run, this machine)

Timings and lines/s vary from run to run; everything else is deterministic.

```

== 0. Status: tasks of docs/implementation_plan.md ==
  required tasks done: 41 of 41  (100 %)
  open: none
All required tasks are done. Not done: hand-labelled evaluation (two people label a sample), optional and stretch items.

== 1. Compiler: rules -> NFA -> one combined DFA ==
$ bin/triage <26 rules>   (startup line only)
triage: dfa: 26 rules, NFA 1762 states, 2 DFA groups, 7593 DFA states, built in 507.3 ms
Unsupported syntax is rejected with its position (rule: failed \d+ times):
$ bin/triage -r out/demo/bad.rules -o out/demo/empty /dev/null
triage: rule BAD: offset 7: \d shorthand is not supported; write a bracket expression such as [0-9]
$ bin/triage --quiet -r tests/fixtures/tiny.rules --dump-dot out/demo/tiny.dot
NFA + DFA picture of a 2-rule file: out/demo/tiny.png  (open it now)

== 2. Classification: labels, confidence, checksum validation ==
$ bin/triage --quiet -w 4 -o out/demo/synthetic tests/corpus/synthetic_small.log
A URL-encoded SQL injection, and a line with two labels:
  #42    CRITICAL conf 0.75  SECURITY_THREAT                
         203.0.113.50 - - [11/Oct/2026:10:08:00 +0530] "GET /products?id=1+UNION+ALL+SELEC...
  #50    CRITICAL conf 0.75  SECURITY_THREAT,ERROR          
         2026-10-11 10:20:05,311 ERROR [db-pool-3] query failed: select * from users where...
Look-alikes get no label: bad Luhn checksum, bad Verhoeff checksum:
  #111   INFO     conf 0.00  -                              
         2026-10-11 10:51:10,100 INFO  PaymentService - payment declined card=4111 1111 11...
  #119   INFO     conf 0.00  -                              
         2026-10-11 10:52:15,100 INFO  OrderService - order id 523456789012 shipped

== 3. PII masking (e-mail, card, Aadhaar) ==
  #22    MEDIUM   conf 0.60  AUTH_FAILURE,PII               
         Oct 11 10:06:20 srv1 sshd[2250]: Failed password for ***@example.com from 203.0.1...
  #107   CRITICAL conf 0.75  PII                            
         2026-10-11 10:51:00,100 INFO  PaymentService - payment captured card=XXXX…1111 am...
  #113   HIGH     conf 0.75  PII                            
         2026-10-11 10:52:00,100 INFO  KycService - uid XXXX XXXX 2346 verified

== 4. Correlator: attacks spread over many lines (real OpenSSH log) ==
$ bin/triage --quiet -w 4 -o out/demo/ssh tests/corpus/OpenSSH_2k.log
     34 ESCALATED_BRUTE_FORCE
      5 ESCALATED_PASSWORD_SPRAY
First brute force, and failures followed by a successful login (CRITICAL):
  #46    HIGH     conf 0.95  SECURITY_THREAT,AUTH_FAILURE   ESCALATED_BRUTE_FORCE
         Dec 10 07:28:03 LabSZ sshd[24243]: Failed password for root from 112.95.230.3 por...
  #194   CRITICAL conf 0.95  SECURITY_THREAT                ESCALATED_FAIL_THEN_SUCCESS
         Oct 11 12:00:40 srv1 sshd[4006]: Accepted password for root from 192.0.2.66 port ...

== 5. Parallel: 1 worker vs 8 workers, then a live stream ==
-w 1 and -w 8: identical output (2000 records)
Live stream, status line every second (queues = backlog per worker):
triage: 497 lines/s | queues 0 0 0 0 | AUTH 252 SEC 13 ERR 2 PERF 0 PII 0 | dropped 0
triage: 0 lines/s | queues 0 0 0 0 | AUTH 252 SEC 13 ERR 2 PERF 0 PII 0 | dropped 0

== 6. Speed: DFA engine vs C library regexec (1 worker, same 26 rules) ==
204280 log lines; each run includes rule compilation, so absolute numbers are lower than in the report:
  dfa     0.71 s      288784 lines/s
  posix   2.68 s       76277 lines/s

== 7. Generator, ground truth and --bench ==
$ bin/gen -n 50000 --seed 5 --truth out/demo/gen.truth.tsv > out/demo/gen.log
gen: 50000 lines, seed 5, auth 2500, sec 1500, err 2500, perf 1500, pii 1500, lookalike 1000, normal 39500
First lines of the generated log and of its ground truth (seq, true labels, kind):
2025-10-09 13:53:20,016 INFO  [pool-4] OrderService - cache hit for key k33593
Oct  9 13:53:20 web01 sshd[16845]: Received disconnect from 71.229.176.20 port 47783:11: disconnected by user
Oct  9 13:53:20 web01 sshd[8290]: pam_unix(sshd:session): session opened for user frank by (uid=0)
0	-	normal_app
1	-	normal_sshd
2	-	normal_sshd
19	PII	pii_aadhaar_valid
20	-	normal_app
21	-	normal_app
Engine labels vs the generator's truth, line by line:
check_gen: 50000 lines, 0 differ
Benchmark mode: one JSON line (input preloaded, sinks off):
$ bin/triage --bench -w 4 out/demo/gen.log
{"engine":"dfa","workers":4,"batch":256,"lines":50000,"bytes":5600373,"seconds":0.061052,"lines_per_s":818975.0,"mb_per_s":91.731,"latency_us":{"p50":4194.30,"p90":10878.
Rule-count experiment E3 (saved run, results/SUMMARY.md):
| rules | posix lines/s | dfa lines/s | dfa / posix | DFA groups | DFA states | DFA build (ms) |
|---|---|---|---|---|---|---|
| 5 | 259,850 | 886,235 | 3.4× | 1 | 703 | 4 |
| 10 | 154,796 | 884,769 | 5.7× | 1 | 2,554 | 24 |
| 20 | 81,204 | 880,042 | 10.8× | 2 | 4,954 | 487 |
| 32 | 61,592 | 863,779 | 14.0× | 2 | 20,061 | 816 |
| 64 | 36,829 | 329,323 | 8.9× | 7 | 26,517 | 1,761 |

== 8. Accuracy: validators, AI-annotated real lines, attack logs ==
Validators on/off on the generated truth (saved run E7), PII labels on real logs:
  PII precision without validators 0.60 (4000 false labels), with validators 1.00
  OpenSSH    655,147 lines: PII-labelled    163 without,      0 with
  Apache      56,482 lines: PII-labelled      0 without,      0 with
  Linux       25,567 lines: PII-labelled    105 without,     40 with
  BGL      4,747,963 lines: PII-labelled 12,279 without,  1,175 with
  HDFS_2k      2,000 lines: PII-labelled    119 without,      1 with
100 real log lines labelled by an AI annotator (NOT human ground truth; results/e6/ai_100):
== vs_A   (100 lines, weighted 737196)
  category         precision   recall      F1    support
  AUTH_FAILURE         1.000    0.688   0.815     533897
  SECURITY_THREAT      0.400    0.025   0.047     226608
  ERROR                1.000    0.002   0.003     111052
  PERF_WARNING           n/a      n/a     n/a          0
  PII                  0.800    1.000   0.889         32
  exact label-set match: 0.5882   ECE of the primary label: 0.4058
AIT-LDS attack-labelled logs (real, saved run): the rules have no rule for most attacker requests:
  auth.log                                         attack lines      8  detected    1  false alarms 0
  intranet.smith.russellmitchell.com-access.log.2  attack lines  7,695  detected    6  false alarms 0
  intranet.smith.russellmitchell.com-error.log.2   attack lines     36  detected    0  false alarms 0
Human labelling of a sample is the open item (data/labeling_100/ for two annotators).

== 9. Base paper: reproduced and measured ==
  claim 1: per-pattern parallelism speeds up classification -> Not reproduced
  claim 3: near-linear scaling to 6 cores                   -> Not reproduced
  claim 6: 3.7 ms parallel vs 8.9 ms sequential             -> Not reproduced
All six claims, tables and figures: results/basepaper/SUMMARY.md, results/basepaper/figures/

== Done - outputs in out/demo/ ==
```

## Likely questions

- *Why a DFA and not regexec in a loop?* One pass per line for all rules, no backtracking, tables
  shared across threads without locks. The `posix` engine stays as the baseline.
- *How do you know the DFA is correct?* Differential test against glibc on 10,218 corpus lines and
  100,000 random strings (0 mismatches), plus an NFA simulator as a second oracle.
- *Is it complete?* All 41 required tasks of the plan, shown live by step 0. Not done: hand-labelled
  precision/recall on real lines (two annotators), and the optional and stretch items.
- *Why does the DFA not get faster with more workers?* Matching costs about a microsecond per line,
  so the single-threaded reader, aggregator, correlator and router set the limit (Amdahl's law);
  measured and reported, not hidden. Next optimisation: move more work out of the serial stages.
- *Accuracy of the log rules?* Checked against annotations, an independent Python reference and
  the effect of validators (HDFS sample: 119 to 1 PII labels); precision/recall against human
  labels is Phase 7.
- *Report?* `docs/Final_Report_IEEE.pdf` (IEEE format); earlier `docs/Review2_Report.pdf`.
