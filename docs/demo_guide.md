# Review 2 demo guide (project at about 78 % of the required tasks)

## How to run

```
cd /home/adi/Desktop/Projects/CD
make                      # build once (bin/triage); the demo does not rebuild
make demo                 # all steps, pauses after each one: press Enter to continue
```

Other ways: `tools/demo.sh --no-pause` runs straight through (about 25 s, 64 lines of output);
`tools/demo.sh correlate` runs one step. Steps, in order: `status compiler classify pii correlate
parallel speed basepaper` (`build` is an optional extra). Everything the demo writes goes to
`out/demo/` (gitignored). Dry-run once on the demo machine first. Optional: `sudo apt install graphviz`
so step 1 also renders the DFA picture `out/demo/tiny.png`.

Not part of the demo, but the evidence behind it: `make test` (19 test programs), `make difftest`
(0 mismatches against glibc), `make e2e`, `make compare` (72 runs), and the same under
`make SAN=thread` / `make SAN=address`.

## What each step shows and what the output means

**0. Status.** Reads `docs/implementation_plan.md` and counts the required tasks that are ticked:
32 of 41 (78 %). Optional R5 and the stretch tasks (Phase 8) are not counted. Say: the engine is
complete; what is open is benchmark tooling (Phase 6: the in-binary fork engine, `--bench`,
generator, rule sets, runner) and the full datasets with hand-labelled evaluation (Phase 7).

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

**7. Base paper.** Three of the professor's paper's six claims, with the verdict from our measured
reproduction (`results/basepaper/SUMMARY.md`): per-pattern parallelism, near-linear scaling to 6
cores and "3.7 ms vs 8.9 ms" are all *not reproduced*: the fork-per-pattern design is about 12.6x
slower than a sequential loop that recompiles and 424x slower than one that precompiles, and only
splitting the *inputs* across cores scales.

## Sample output (one real run, this machine)

Timings and lines/s vary from run to run; everything else is deterministic.

```

== 0. Status: tasks of docs/implementation_plan.md ==
  required tasks done: 32 of 41  (78 %)
  open: T6.1 T6.2 T6.3 T6.4 T6.5 T7.1 T7.2 T7.3 T7.4
Done: compiler, classifier, parallel pipeline, correlator, base-paper study. Open: benchmark tooling (Phase 6), full datasets and labelled evaluation (Phase 7).

== 1. Compiler: rules -> NFA -> one combined DFA ==
$ bin/triage <26 rules>   (startup line only)
triage: dfa: 26 rules, NFA 1762 states, 2 DFA groups, 7593 DFA states, built in 468.1 ms
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
triage: 498 lines/s | queues 0 0 0 0 | AUTH 252 SEC 13 ERR 2 PERF 0 PII 0 | dropped 0
triage: 0 lines/s | queues 0 0 0 0 | AUTH 252 SEC 13 ERR 2 PERF 0 PII 0 | dropped 0

== 6. Speed: DFA engine vs C library regexec (1 worker, same 26 rules) ==
204280 log lines; each run includes rule compilation, so absolute numbers are lower than in the report:
  dfa     0.69 s      295026 lines/s
  posix   2.52 s       81182 lines/s

== 7. Base paper: reproduced and measured ==
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
- *Is it really about 80 % done?* 32 of 41 required tasks, shown live by step 0. The rest is
  evaluation: benchmark tooling, full datasets, hand-labelled precision/recall.
- *Why does the DFA not get faster with more workers?* Matching costs about a microsecond per line,
  so the single-threaded reader, aggregator, correlator and router set the limit (Amdahl's law);
  measured and reported, not hidden. Next optimisation: move more work out of the serial stages.
- *Accuracy of the log rules?* Checked against annotations, an independent Python reference and
  the effect of validators (HDFS sample: 119 to 1 PII labels); precision/recall against human
  labels is Phase 7.
- *Report?* `docs/Final_Report_IEEE.pdf` (IEEE format); earlier `docs/Review2_Report.pdf`.
