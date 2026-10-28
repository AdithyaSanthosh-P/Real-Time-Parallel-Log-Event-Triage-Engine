# Project guide: Real-Time Parallel Log / Event Triage Engine

A single document that explains the whole project: what it is, why it is built the way it is, how every
part works, how to run and test it, what has been measured, and what is honestly still open. Written for
a teammate, an examiner, or yourself in a viva. Numbers quoted here come from files saved under
`results/`; where a number is not saved yet, the text says so.

Team 1, Compiler Design course project, VIT: Adithya Santhosh P (24BCI0099), Anvesha Tripathi (24BCT0055).

---

## 1. The idea in one page

**Problem.** A server writes thousands of log lines per second. A few of them matter: repeated failed
logins, SQL-injection attempts, crashes, slow queries, leaked card or Aadhaar numbers. We want a program
that reads the stream, gives every line categories, a confidence and a severity, hides personal data, and
raises one alert when a pattern spreads over many lines (a brute-force attack).

**Approach.** Rules are regular expressions. Instead of running 26 regexes one after another per line
(slow), our own compiler turns all rules into **one deterministic automaton (DFA)** that finds every
matching rule in a single pass over the line. Lines are independent, so the work is spread over several
**worker threads** connected by **lock-free queues**. A classifier checks numbers with checksums, fuses
evidence into confidences, and a **correlator** watches sliding time windows.

**Why it is a compiler-design project.** The core is a regex → NFA → DFA compiler written from scratch:
lexer, recursive-descent parser, Thompson construction, subset construction, and a table-driven matcher.
Everything else (classifier, threads, correlator) is the system built around that compiler.

**Base paper.** The professor's paper classifies short texts by forking one process per regex per input
and claims near-linear speed-up. We rebuilt it faithfully (inefficiencies included), measured every claim,
and then built our design as the alternative. Result: the paper's design is slower than a plain sequential
loop; splitting the *inputs* across cores scales; our DFA is the fastest variant.

---

## 2. Architecture

```
 input (file / stdin / pipe)
        |
 [Reader]        splits lines, assigns seq ids, makes a URL-decoded copy for matching,
        |        parses the timestamp, fills batches (default 256 lines)
        v
 [Dispatcher]    round-robin or least-loaded -> one SPSC ring per worker
        |
   +----+----+
   v    v    v
 [Worker 0..N-1]  pinned pthreads; each has its own engine (dfa | posix | fork);
   |    |    |    match -> validators -> entity extraction (IP, user) -> PII masking
   +----+----+
        v
 [Aggregator]    rule hits -> category confidence (noisy-OR), labels, primary, severity
        v
 [Correlator]    puts batches back in input order; sliding windows per source IP;
        |        escalations (brute force, spray, fail-then-success, error storm)
        v
 [Router]        alerts.jsonl / store.jsonl / pii_quarantine.jsonl  (+ 1 Hz stats line on stderr)
```

Each arrow is a **single-producer single-consumer ring buffer**: exactly one thread writes, one reads, so
a hand-off needs only acquire/release atomics, no locks. The DFA tables are read-only after construction
and shared by all workers without locks. Batch buffers come from a pool created before the threads start
and recycled through a return ring, so no memory is allocated per line (an allocation-counting test checks
this).

Two phases:

1. **Start-up** (once): load rules, compile them (parse → NFA → DFA tables).
2. **Run time**: stream lines through the pipeline above.

---

## 3. The compiler (the core, `src/regex_*.c`, `nfa.c`, `dfa.c`)

### 3.1 Front end: lexer and parser
Rules use only a **regular subset of POSIX ERE**: literals, `.`, bracket classes, `* + ? {n,m}`,
alternation, grouping, anchors `^ $`. The parser is recursive descent, one function per grammar level
(`alt → concat → repeat → atom`). Anything outside the subset is **rejected with its byte offset**, never
approximated: `\d` ("write [0-9]"), `\b`, backreferences, lookaround `(?=`, lazy quantifiers `*?`. Reason:
those are not regular (or not DFA-friendly), and silent approximation would change what a rule matches. A
side benefit: the same text also runs on the C library's `regexec`, so the C library is an independent
oracle.

### 3.2 Thompson construction (AST → NFA)
Every AST node becomes a small NFA fragment with one entry and one exit joined by ε-edges: concat links
exit to entry, alternation splits and merges, `*` loops back, `{n,m}` is expanded into copies. All rules
are joined under one start state; each accept state is tagged with its **rule number**.

### 3.3 Subset construction (NFA → DFA)
Standard subset construction, with the project-specific parts:

- **Accept masks.** Each DFA state stores a 64-bit word; bit *r* means rule *r* has matched. One pass
  over a line gives the set of all matching rules. (Hence at most 64 rules.)
- **Match anywhere.** The start states of every non-anchored rule are added to every transition, so a
  match can begin at any position. `^` is an assertion that only fires at offset 0, `$` only at line end.
- **Byte classes.** Bytes no character set distinguishes are merged, so transitions are computed once per
  class and tables are smaller.
- **Grouping under a state cap.** The 26 default rules in one DFA would need 39,764 states. When a DFA
  would exceed the cap (default 20,000), the rule set is halved recursively and each half is built
  separately; at match time every group is run and the masks are OR-ed. With the default cap: 2 groups,
  7,593 states in total.

### 3.4 Matching
```
state = 0; mask = accept[0]
for each byte b of the line:  state = next[state][b]; mask |= accept[state]
mask |= accept_end[state]       // rules that match at end of line ($)
```
Cost per line is linear in its length and does not depend on the number of rules (it grows by one pass per
*group*, see E3).

### 3.5 Correctness evidence
`make difftest`: for every rule, on every line of the sample corpus and on 100,000 generated strings, the
DFA mask must equal glibc `regexec`. Result: 0 mismatches (also for all bench rule sets). Unit tests add an
NFA simulator as a second oracle.

---

## 4. The engines (`--engine dfa|posix|fork`)

| Engine | What it does | Why it exists |
|---|---|---|
| `dfa` | Our combined DFA (default). | The proposed method. |
| `posix` | One glibc `regexec` per rule per line; each worker compiles its own `regex_t` set (glibc locks a shared one). | Sequential baseline, difftest oracle. |
| `fork` | The base paper's design on our rules: per line, fork one child per rule; child does `regcomp` + `regexec`, reports through a pipe; parent `waitpid`s. Single-threaded (`-w` is ignored with a warning). | Experiment baseline (E1). |

All three produce identical rule masks (tested).

---

## 5. Classification (`aggregate.c`, `validators.c`, `extract.c`, `mask.c`)

1. **Validators.** A regex only checks *shape*. For rules with a validator, each matched span is checked:
   Luhn (cards, 13–19 digits), Verhoeff (12-digit Aadhaar), PAN structure. A rule whose validator fails on
   every span is discarded. This removes false positives such as HDFS block ids that look like card numbers.
2. **Per-rule evidence.** `s = base` (0.35 weak / 0.6 strong), `+0.25` if a validator passed, `+0.15` if a
   context keyword (e.g. "card", "payment") is on the line, clamped to [0,1].
3. **Per-category confidence = noisy-OR:** `conf(c) = 1 − Π(1 − s_i)`. Independent weak signals add up; one
   strong rule is enough.
4. **Labels** = categories with `conf ≥ τ` (`--tau`, default 0.5); a line can have several labels
   (multi-label). **Primary** = highest severity, ties by confidence, then SEC > PII > AUTH > ERR > PERF.
5. **Entity extraction** pulls the source IP and user from matched lines (for the correlator) and the
   spans of PII (for masking).
6. **Masking** happens before anything is written: Aadhaar `XXXX XXXX 1234`, card `XXXX…1111`, PAN, secrets
   `***`, e-mail and phone. A test greps every output file for every PII value of the corpus.

Categories: `AUTH_FAILURE`, `SECURITY_THREAT`, `ERROR`, `PERF_WARNING`, `PII`. Severities: INFO, LOW,
MEDIUM, HIGH, CRITICAL.

---

## 6. Correlator (`correlate.c`)

Runs on one thread, so its state needs no locks. A hash table keyed by source IPv4 holds a ring of the last
64 authentication failures (time, hash of user). Time comes from the **log timestamp**, so replays of old
logs behave like real time.

| Detector | Trigger (defaults) | Effect |
|---|---|---|
| BRUTE_FORCE | ≥ 5 AUTH failures from one IP within 60 s | SEC, HIGH, confidence 0.95, flag; once per window |
| PASSWORD_SPRAY | failures against ≥ 5 distinct users within 300 s | SEC, HIGH |
| FAIL_THEN_SUCCESS | a successful login from an IP over the brute-force threshold | SEC, CRITICAL |
| ERROR_STORM | ≥ 100 ERROR lines within 60 s | one aggregated alert |

Names and defaults of the first two follow fail2ban. The error-storm numbers are our own choice (the spec
gives none). Because workers finish batches out of order, the correlator holds early batches until the next
expected sequence number arrives, so windows see lines in log order and **output is in input order for any
`-w`**.

On the real Loghub OpenSSH 2k sample: 34 brute-force and 5 password-spray escalations; BGL 2k: one error
storm.

---

## 7. Parallel runtime (`spsc.c`, `pool.c`, `worker.c`, `dispatch.c`, `pipeline.c`, `stats.c`)

- **SPSC ring:** power-of-two capacity, head and tail on separate cache lines, release/acquire ordering.
  Stress-tested with 10M items under ThreadSanitizer.
- **Workers:** one pinned pthread each (one thread per physical core before hyper-thread siblings; `--pin`
  default on), own engine instance and own capture regexes.
- **Dispatcher:** `rr` (round robin) or `least` (fewest queued jobs); blocking backpressure by default;
  `--drop` discards and counts batches instead.
- **Shutdown:** an end-of-input sentinel travels through every ring; all threads join; any failure sets a
  shared stop flag so nothing blocks forever.
- **Stats:** once per second on stderr: lines/s, queue depth per worker, counts per category, dropped.
- **Equivalence:** `make compare` runs every corpus file with both engines at 1, 2, 4, 8 workers, small
  batches, least-loaded dispatch and stdin input, and requires all three output files to equal the
  single-worker run (72 runs, 0 differences). ThreadSanitizer and AddressSanitizer runs are clean.

---

## 8. Using the program

```
make                                   # build bin/triage and tools
bin/triage tests/corpus/OpenSSH_2k.log                 # outputs in out/
bin/triage -w 8 --engine dfa big.log                   # 8 workers
bin/triage --engine posix -w 1 big.log                 # sequential baseline
bin/triage --bench -w 4 big.log                        # one JSON line: throughput + latency, no output files
bin/triage -r rules/bench_64.rules big.log             # another rule set
bin/triage -r my.rules --dump-dot g.dot                # NFA + DFA as Graphviz
tail -F app.log | bin/triage --batch-delay 5 -         # live stream
```
Options: `-w N`, `-b BATCH` (max 1024), `--batch-delay MS`, `--engine dfa|posix|fork`,
`--dispatch rr|least`, `--pin/--no-pin`, `--drop`, `--tau T`, `--max-states N`, `-r RULES`, `-o DIR`,
`--dump-dot FILE`, `--bench`, `--quiet`. (`--ordered` is a stretch item and is not implemented; output is
already in input order.)

**Rule file** (`rules/default.rules`, 26 rules, tab-separated):
`id  category  severity  base  flags  validator  extract  context  regex`, e.g.
`SSH_FAIL_PW  AUTH  MEDIUM  0.6  -  -  ip,user  -  Failed password for (invalid user )?[^ ]+ from [0-9.]+`.

**Output record** (one JSON per line):
`{"seq":1042,"ts":"…","primary":"SECURITY_THREAT","severity":"CRITICAL","confidence":0.75,"labels":[…],"rules":[…],"flags":[…],"line":"…"}`
Alerts (severity ≥ HIGH) go to `alerts.jsonl`, PII lines (masked) to `pii_quarantine.jsonl`, everything to
`store.jsonl`.

---

## 9. Repository map

| Path | Contents |
|---|---|
| `src/` | The engine, one `.c/.h` pair per module (regex_lex/parse, ast, nfa, dfa, dot, engine*, validators, extract, aggregate, mask, correlate, route, json, reader, batch, normalize, timestamp, spsc, pool, worker, dispatch, stats, pipeline, bench, cli, main). |
| `rules/` | `default.rules` (26), `bench_{5,10,20,32,64}.rules` (nested), `bench_extra.rules`, `novalidators.rules` (E7). |
| `tests/` | Unit tests (`test_*.c`), `corpus/` (Loghub 2k samples + hand-annotated `synthetic_small.log`), `expected/`, `basepaper/`. |
| `tools/` | `gen.c` (synthetic generator + ground truth), `difftest.c`, `run_bench.sh`, `run_eval.sh`, `plot.py`, `label_eval.py`, `sample_for_labeling.py`, `fetch_*`, `compare_runs.sh`, `demo.sh`, `basepaper/` (Phase R), `report/` (report generators). |
| `data/` | Full datasets (gitignored; `tools/fetch_datasets.sh`). |
| `results/` | Saved measurements: `basepaper/`, `milestone1/`, `milestone2/`, `e1`–`e4`, `e6`, `e7`, `SUMMARY.md`. |
| `docs/` | `project_design.md` (spec), `implementation_plan.md`, `progress.md`, reports (PDF), `demo_guide.md`, this file. |

---

## 10. Testing and verification

| Command | What it proves |
|---|---|
| `make test` | 20+ unit-test programs (hundreds of thousands of checks), Phase R tests, allocation test, generator test, label-eval test |
| `make difftest` | DFA == glibc regexec on corpus + 100,000 random strings, for the default and every bench rule set |
| `make e2e` | End-to-end on `synthetic_small.log` against expected output, both engines plus fork; PII leak check; OpenSSH escalations; `--bench` JSON; drop accounting; write-error handling |
| `make compare` | Parallel output == single-thread output (72 runs) |
| `make gen-test` | Generator is reproducible (same hash for 1M lines), exact rates, its truth equals the engine's labels |
| `make SAN=thread` / `SAN=address` | Same tests under ThreadSanitizer / AddressSanitizer |
| `make demo` | The review demo (see `docs/demo_guide.md`) |

Independent reference: `tools/ref_classify.py` re-implements the classifier and correlator in Python; C and
Python agree on every line of all corpus files.

---

## 11. Data sets

| Data | Size | Labels | Used for |
|---|---|---|---|
| Loghub 2k samples (`tests/corpus/`) | 5 × 2,000 lines (OpenSSH, Apache, Linux, BGL, HDFS) | none | tests, demo, difftest |
| Loghub full (`data/`) | OpenSSH 655,146; Apache 56,481; Linux 25,567; BGL 4,747,963 lines | none | label distributions, correlator on real data |
| AIT-LDS v2.0, scenario russellmitchell (`data/ait/`) | 3 labelled logs (8,530 + 36 + 272 lines) | per-line attack labels | attack-detection recall |
| `synthetic_small.log` | 218 lines | 99 hand-annotated | exact rule-mask tests |
| Generator (`bin/gen`) | any size, fixed seed | exact by construction | throughput inputs, accuracy, validator study |
| Base-paper labelled set | 1,250 generated inputs | by construction | Phase R accuracy |

Real: Loghub, AIT. Synthetic: the generator, `synthetic_small`, the base-paper set. Be explicit about this
when presenting.

---

## 12. Experiments and what they showed

Phase R (base paper, `results/basepaper/SUMMARY.md`): the fork-per-pattern design handles one input in about
193–205 µs; at a batch of 10,000 it is 12.6× slower than a sequential loop that recompiles and 424× slower
than one that precompiles. Peak speed-ups: fork 1.72×, per-pattern threads 2.54×, data-parallel threads
4.83× (6 cores), our DFA 5.29×. The paper's central hypothesis (process overhead dominates) is confirmed;
its near-linear-scaling claim is not reproduced. Its input module also rejects 146 of 1,250 lines.

Engine measurements (`results/milestone2/`, commit da15165): DFA 998,003 lines/s vs POSIX 76,868 lines/s
on one worker (13.0×, 1,021,400 lines, 5 runs). The DFA engine does **not** scale with workers (about
924k → 833k lines/s from 1 to 8) because the serial stages (reader, aggregator, correlator, router) cap the
pipeline near a million lines/s; the POSIX engine scales 4.4× at 8 workers.

Experiment matrix (`tools/run_bench.sh`, `tools/run_eval.sh`; results and exact commands in `results/`,
tables in `results/SUMMARY.md`, generated from the saved JSON):

| # | Experiment | Question |
|---|---|---|
| E1 | fork vs posix -w1 vs posix -wN vs dfa -wN | is the paper's design dominated by process creation? |
| E2 | scaling with workers | where do serial stages bound the speed-up? |
| E3 | rule count 5 → 64 | does the DFA stay flat while POSIX falls? (it makes one pass per group, so cost steps up when groups are added) |
| E4 | batch size 1 → 1,024 | throughput vs latency |
| E6 | accuracy | P/R/F1 per category on generator truth and AIT; label distribution on full Loghub |
| E7 | validators on/off | do checksums raise PII precision? (HDFS 2k: 119 PII-labelled lines before validation, 1 after) |

Notable honest findings: rules do not cover most AIT attacker requests (low recall; not tuned afterwards);
`PWD=` in sudo lines is flagged as a secret by the `PII_SECRET_QS` rule; confidences are conservative
(correct labels often score 0.6 – observed precision is higher), shown by the reliability diagram.

---

## 13. Reports and demo

| File | What |
|---|---|
| `docs/Review1_Report_IEEE.pdf` | Review 1 (state at commit 8093e0a: compiler, baseline pipeline, base paper) |
| `docs/Review2_Report.pdf`, `docs/Final_Report_IEEE.pdf` | Later-state reports (IEEE two-column, professor's structure) |
| `docs/demo_guide.md`, `make demo` | Step-by-step demo with talking points and likely questions |
| `docs/progress.md` | Session-by-session log with decisions and open questions |

Report generators are in `tools/report/` and read numbers from `results/` only.

---

## 14. Status and what is open

All required engineering tasks of `docs/implementation_plan.md` are implemented. What humans must still do:

- **Hand-labelling** (Phase 7): both teammates label the same stratified sample *independently*
  (`tools/sample_for_labeling.py` makes the blank CSVs; `tools/label_eval.py --annot` reports P/R/F1 and
  Cohen's κ). This gives real-data precision/recall; until then no such number is claimed.
- **Decisions** logged in `docs/progress.md` ("Open questions"): correlator thresholds as CLI options,
  error-storm defaults, weak rules (bare HTTP 401/403, timeouts) that produce no label alone.
- **Optional / stretch:** R5 (reproduce the paper's conflict-resolution scoring, ask first), Phase 8 items
  (Hopcroft minimisation, `--ordered`, ReDoS demo, …) are not started.

Known limits: DFA start-up is about half a second for 26 rules and grows with the rule set; the DFA engine's
pipeline throughput is bounded by serial stages; at most 64 rules; IPv4 only for correlation; the DFA
matches bytes (C locale), not Unicode characters.

---

## 15. Questions you may be asked

- **Why a DFA instead of regexec in a loop?** One pass per line for all rules, no backtracking, linear time,
  tables shared across threads without locks; the library engine is kept as baseline and oracle.
- **How do you know the DFA is correct?** Differential tests against glibc (0 mismatches) plus an NFA
  simulator; every bench rule set is covered too.
- **Why not support `\d` or lookahead?** Not regular / not DFA-compatible; the parser rejects them with a
  position so no rule is silently changed.
- **What about state explosion?** Measured: 39,764 states unsplit; automatic halving under a state cap gives
  7,593 states in 2 groups. Cost: one pass per group.
- **Why does the DFA engine not scale with threads?** Matching is about a microsecond per line, so the
  single-threaded reader/aggregator/correlator/router are the limit (Amdahl); measured and reported.
- **Why is the paper's design slow?** A process per pattern per input plus `regcomp` in every child: process
  creation dominates the 3–5 µs a match costs.
- **How is the correlator tested?** Unit tests with crafted sequences (4 failures → nothing, 5 in window →
  one escalation, 5 spread out → nothing, burst then success → CRITICAL), a Python reference, and the real
  OpenSSH sample.
- **What is synthetic and what is real?** See section 11.
