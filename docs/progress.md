# Progress Log

Append one entry per session at the **bottom** (newest last). Keep entries short. Next session reads the latest entry first.

## Template

```
## YYYY-MM-DD — <tasks touched, e.g. T2.2–T2.3>
Done:      what was completed (task IDs ticked in the plan)
State:     what works now; test status (make test / difftest / SAN=thread)
Broken:    known failures or partial work, with file/function names
Next:      the exact next step
Decisions: anything decided that isn't in the spec (and why)
```

## Open questions

Spec ambiguities or decisions that need the team's input. Remove an item once answered, and record the answer under the session entry where it was resolved.

- (none open)
---

## 2026-10-11 — Project docs set up
Done:      Spec (`project_design.md`), plan, CLAUDE.md, rules/default.rules (26 rules; all compile with `regcomp(REG_EXTENDED)` and match their example lines).
State:     No code yet.
Broken:    —
Next:      T0.1 repo skeleton and Makefile.
Decisions: Build order is POSIX single-thread baseline → own DFA compiler → classification → parallel runtime, so each stage is checked against the previous one.

## 2026-10-11 — T0.1
Done:      T0.1 ticked. `git init`; dirs per spec §9 (src, tools, rules, tests/{corpus,expected,fixtures}, results; data/ and out/ gitignored); `.gitignore`; Makefile; `src/main.c` (only `--help`), `src/cli.c/.h` (`cli_print_usage`, text from spec §8); `tests/test_main.h` (`CHECK`, `CHECK_EQ_INT`, `CHECK_STR_EQ`, `TEST_DONE`); `tests/test_cli.c`.
State:     `make`, `make test`, `make SAN=address test`, `make SAN=thread test` all pass with zero warnings. `bin/triage --help` prints the §8 CLI. `make difftest` and `make e2e` exist but fail with "not implemented yet" until T2.5 / T1.7 (deliberate: no false pass).
Broken:    —
Next:      T0.2 test corpus (`tools/fetch_samples.sh`, `tests/corpus/synthetic_small.log`).
Decisions: Sanitizer builds use separate dirs (`build/<SAN>/`, `bin/<SAN>/`) so they never mix with the normal build; plain build still outputs `bin/triage`. `SAN=address` also enables UBSan (`address,undefined`). Tests and tools link all `src/*.o` except `main.o`, so any module is unit-testable. `tests/*.c` are picked up by wildcard (`tests/test_*.c`); `tools/gen.c` and `tools/difftest.c` are built automatically once they exist. `results/` is tracked (benchmark JSON must be committed); `.gitignore` does not exclude it.

## 2026-10-11 — T0.2
Done:      T0.2 ticked. `tools/fetch_samples.sh` (idempotent, `--force` to refetch) downloaded Loghub OpenSSH/Apache/Linux/BGL/HDFS 2k samples into `tests/corpus/`. Hand-written `tests/corpus/synthetic_small.log` (179 lines, 83 of them annotations' targets): every one of the 26 rules has a positive line; 30 lines are expected to match no rule (near-misses). Verified with a throw-away script (Python `re` on the normalized line) that every annotation equals the real rule-mask.
State:     `make test` unchanged and green. Corpus is data only.
Broken:    —
Next:      T1.1 rule loader.
Decisions: Annotation format: a line `#@ rules: A,B` (or `none`) precedes the line it describes; optional free text after ` -- `. The ids are the exact *regex-level* rule mask on the NORMALIZED line (so validators are not applied: invalid Luhn/Verhoeff lines still list PII_CARD/PII_AADHAAR; some annotations say "needs url decoding" = only matches after T1.2 normalization). Annotation and `# ...` header lines are real stream lines and match no rule, EXCEPT an annotation naming TIMEOUT, which matches rule TIMEOUT (case-insensitive keyword). Consumers (T1.4 test, T1.7 expected file) must skip lines starting with `#`, or account for this; the e2e expected file should skip them.
           Facts the later tasks need: (1) All five Loghub 2k files are CRLF (reader must strip `\r`) and each holds 2000 lines; Apache/BGL/Linux/OpenSSH have no trailing newline (`wc -l` = 1999), HDFS has one (`wc -l` = 2000). (2) JAVA_STACK's regex contains a literal TAB inside `[ <tab>]`, so the rule loader must split on only the first 8 tabs; the 9th column is the rest of the line. (3) A 14-digit timestamp matches PII_CARD at regex level (13–16 digit shape): Luhn is what rejects it.

## 2026-10-11 — T1.1–T1.7 (Phase 1 complete)
Done:      T1.1 rule loader, T1.2 normalizer, T1.3 reader (+ batch buffers, timestamp parser), T1.4 POSIX engine, T1.5 aggregator v0, T1.6 JSONL router (+ JSON escaping), T1.7 single-thread pipeline + full CLI. All ticked and committed one commit per task.
State:     `make`, `make test` (8 programs), `make e2e` (3 runs: file, stdin, `-b 7`) pass; so do `make SAN=address test e2e` and `make SAN=thread test e2e`; zero compiler warnings. `bin/triage --engine posix -w 1 FILE` writes `out/alerts.jsonl` + `out/store.jsonl`. Also ran it under ASan on the five Loghub 2k samples: no errors, all output valid JSONL (sanity run only; no numbers recorded).
Broken:    Nothing known. `make difftest` fails by design ("not implemented yet", T2.5).
Next:      T2.1 regex lexer + recursive-descent parser -> AST (core compiler work: suggest switching to Opus for T2.1-T2.3 and T2.5).
Decisions: (all small internal choices; none touch the spec's architecture)
  - Added to `Batch` (§2.4) only capacity fields `raw_cap`/`norm_cap` so buffers grow for arbitrarily long lines; added `Classified` and flag bits (`FLAG_*`) to `src/types.h` exactly as in §2.4/§6. `Classified.primary` is a category *index* (0 AUTH,1 SEC,2 ERR,3 PERF,4 PII, `PRIMARY_NONE`=0xFF), per the "CAT index" comment in §2.4.
  - Rule loader keeps the FULL regex text (with `^`/`$`) because the posix engine compiles it as written; it records `anchored_start/end` flags for the DFA compiler to use in T2.1 (the parser must strip them there). Regex column = everything after the 8th tab (JAVA_STACK has a tab inside `[ <tab>]`; JAVA_EXCEPTION ends in a space).
  - Normalizer: single-pass decoding (`%2541` -> `%41`); decoded control characters are dropped too (so `%00` cannot split a keyword); `+` -> space only for a literal `+`.
  - Reader: strips one `\r` before `\n` and a lone trailing `\r` at EOF; lines over 16 MiB are split into pieces of 16 MiB (bounded memory); batches close early at 16 MiB. Timestamps only recognized at line start (syslog with current UTC year, ISO-8601 with optional zone); everything else (Apache access log, HDFS, BGL) falls back to ingest time. Time-based partial-batch flush (`--batch-delay`) is T4.4 work and will need a poll()-style read.
  - Router: JSON `line` uses `�` for bytes that are not valid UTF-8; `primary` is `null` and `labels` empty for unlabeled lines; labels list primary first. `confidence` is 1.00 for labeled lines and 0.00 otherwise in v0.
  - Added `engine.c/.h` (small facade over the three engines so the pipeline is engine-agnostic) and `pipeline.c/.h` (`pipeline_run(Options*)`, T4.5 will replace the body).
  - CLI parses every flag in §8; flags whose features do not exist yet (`--engine dfa|fork`, `-w >1`, `--batch-delay`, `--dispatch`, `--pin/--no-pin`, `--drop`, `--ordered`, `--tau`, `--max-states`, `--dump-dot`, `--bench`) exit 2 with "not yet implemented" (see `cli_unimplemented()`, shrink it as tasks land). `-w` defaults to 1 and `--engine` to `dfa` (so a bare `triage FILE` says dfa is not implemented until T2.4).
  - `tests/expected/synthetic_small.expected` is v0: it still labels the validator-failing PII lines (seq 111, 117, 119, 168) because validators arrive in T3.4; its header lists what must change then. `tools/run_e2e.sh` honours `E2E_ENGINES="posix dfa"`; add dfa when T2.4 lands.
  - Test helper `tests/annot.h` (header-only) loads the annotated corpus; reuse it for the NFA/DFA tests (T2.2, T2.3) and difftest.

## 2026-10-11 — T2.1–T2.6 (Phase 2; T2.6 awaiting the Graphviz check)
Done:      T2.1 lexer + recursive-descent parser + AST, T2.2 Thompson NFA + `nfa_simulate`, T2.3 subset construction (+ rule groups), T2.4 DFA engine (`--engine dfa`, now the working default; `--max-states` works), T2.5 `tools/difftest.c` + `make difftest`. T2.6 `--dump-dot` is implemented and unit-tested but NOT ticked: its check needs `dot -Tpng`, and Graphviz is not installed on this machine.
State:     `make` warning-free; `make test` 13 programs pass; `make e2e` passes for posix AND dfa (file, stdin, `-b 7`); `make difftest`: 10,179 corpus lines + 100,000 generated strings, 0 mismatches (also 0 for seeds 2 and 3 at 1,000,000 strings each); ASan/UBSan and TSan builds clean. Each test oracle was checked with a planted bug (EOL handling, dropped ∪U, dropped accept_end) and caught it.
Broken:    —
Next:      Install Graphviz (`sudo apt install graphviz`), run `bin/triage -r tests/fixtures/tiny.rules --dump-dot tiny.dot && dot -Tpng tiny.dot -o tiny.png`, tick T2.6. Then T3.1 validators.
Decisions:
  - `^`/`$` are assertions (see Open questions). `nfa.rule_anchored[r]` (every path starts with `^`, e.g. JAVA_STACK) replaces the loader flag for excluding a rule from U; the loader's `anchored_start/end` flags are now informational.
  - Parser rejects, with byte offset: `\d \s \w \b` (+ upper case), `\1`..`\9`, any escape not in §3.1's list (`\n`, `\t`, `\-`, ...; `\)` `\]` `\}` ARE allowed), `(?`, `*?` `+?` `??` and `+`/`?` after any quantifier (lazy/possessive look-alikes; `a**`, `a{2}*` stay legal as in POSIX), `{,n}`, bad/oversized intervals (max 1000), `]`/`}` unescaped outside brackets, backslash inside `[...]` (POSIX would read it literally), `[:class:]`/`[.x.]`/`[=x=]`, `-` not first/last in a class, reversed ranges, empty alternatives/groups, nesting > 100.
  - `.` excludes only `\n` (spec); glibc's `.` also excludes NUL. Neither byte can reach a matcher (the normalizer drops control bytes), and difftest's generator only emits bytes that survive normalization.
  - icase: charsets are folded before `[^...]` is complemented (matches glibc).
  - AST keeps `NODE_REPEAT{min,max}`; the NFA builder expands it (x{n,m} = n copies + nested optionals).
  - DFA: states keyed by kernel (CHAR/ACCEPT/EOL NFA states) minus U, start state flagged unique (exact `^$` on the empty line); byte classes by partition refinement; groups built from the single combined NFA so masks keep global rule bits; halving on overflow. 26 rules: NFA 1,762 states; per-rule DFAs 8-78 states.
  - `engine.h` now has two layers: `EnginePrepared` (built once, read-only, holds the shared DFA tables) and per-thread `Engine` (posix compiles its own regex_t set). No `--verbose` flag was added (CLI is fixed by §8); the DFA build summary (groups, states, build time) is printed on stderr unless `--quiet`.
  - `make SAN=address` now uses `-fno-sanitize-recover=all`, so any UBSan finding fails the run (an empty-kernel memcpy(NULL, 0) was found and fixed this way).
  - `make difftest` runs `--random $(DIFFTEST_RANDOM)` (default 100000); difftest prints per-rule match counts for the random strings and warns if a rule was never hit. Use `-r` to check other rule files (T6.4).

## 2026-10-12 — Planning update (no code changes)
Done:      Docs only. Professor's direction: the base paper is an unverified student project; benchmark its approach first, review journal papers, then continue our design. Added Phase R (R1–R6) to the plan after Phase 2; spec gained §10.4 (base-paper reproduction study), the §3.1 anchor clarification, §9 layout entries and an Appendix A pointer; added `docs/research/literature_review.md` (merged Claude + Gemini journal review, verified entries only).
State:     Unchanged from the previous entry: Phases 0–2 done except T2.6 (needs Graphviz for its check).
Broken:    —
Next:      R1 faithful reproduction of the base paper (`tools/basepaper/bp_fork.c`). T2.6 can be ticked whenever Graphviz is installed.
Decisions: Both open questions answered by the team:
  - `^`/`$` as zero-width assertions (allowed only where nothing can be consumed before `^` / after `$`): accepted as implemented; spec §3.1 now says this.
  - DFA state cap: keep the default `--max-states 20000` (default rules build as 2 groups); raising it or regrouping is studied in E3/E11, not changed now.


## 2026-10-12 — R1 faithful reproduction of the base paper
Done:      R1. `tools/basepaper/`: `patterns.h` (the paper's 5 patterns, verbatim), `bp_lib.c/.h` (`bp_classify_fork` = paper §3.3 parallel_classify: pipe, fork per pattern, child pins to a CPU then regcomp/regexec/regfree, fixed-size record over the pipe, parent reads one record then waitpid per child; `bp_normalize_paper` = §3.2 process_input), `bp_fork.c` (interactive mode like §6, and `-f FILE [--quiet]` batch; `--normalize=none|paper`). Makefile: `make bp`, `make bp-test` (also run by `make test`, works with `SAN=`). Tests in `tests/basepaper/test_bp.c` with fixtures `screenshots.txt`, `input_module.txt`.
State:     `make test` (13 programs + bp-test) passes; `make SAN=address bp-test` clean; zombie test over 10,000 inputs: no zombie persists between /proc polls (momentary ones between a child's exit and the parent's waitpid are normal). Core code untouched, so difftest not re-run for this task.
Broken:    T2.6 still waits for Graphviz (`sudo apt install graphviz`).
Next:      R2 comparison variants (`bp_bench.c`), then R3 inputs/labeled set, R4 runner.
Decisions (not in the spec):
  - Pinning: child i pins to the (i % n)-th CPU of `sched_getaffinity`, not of the whole machine, so `taskset` in RB3 is respected. Same idea as the paper's `i % available_cores`.
  - `ALLOWED_CHARS = "a-zA-Z0-9@.-_?! "` is read as a character list (`.`, `-`, `_` separately). Read as a regex class `.-_` would be a range admitting `/ : ; < = > [ \ ] ^` too. The apostrophe is rejected either way.
  - Collect loop follows the paper's pseudocode: receive one record, then waitpid, per child; records are reported in arrival order.
  - The timings in the pipe record (regcomp/regexec ns) are our instrumentation, not in the paper.
  - The exact wording of the per-core lines in the interactive mode isn't visible in the transcribed screenshots; we print `Core i (NAME): ✅/❌`.
  - Shared fork code lives in `tools/basepaper/bp_lib.c`; T6.1 will move what it needs into `src/`.
Findings (recorded, not fixed), from `tests/basepaper/test_bp.c` output for the §4.4 input-module table vs the §3.2 pseudocode:
  - `"What's your name?"`: paper says PASS and keeps it; pseudocode rejects it ('  is not in ALLOWED_CHARS).
  - `""`: paper says "Error: Empty input"; pseudocode has no empty check and accepts it.
  - 300 characters: both reject, but the pseudocode says "Input exceeds maximum length", not "Input too long".
  - The tab and newline replacement steps can never run (tabs/newlines are rejected by the character check first).
  - Lowercasing makes the NAME pattern `^[A-Z][a-z]+ [A-Z][a-z]+$` unmatchable under `--normalize=paper`.
  - §6 screenshots are reproduced under `--normalize=none` (the 4 inputs give exactly the paper's matched sets), including `Hello what is your name?` not matching GREETING.
Note for RB3: this machine is an i7-13650HX (6 P-cores with SMT + 8 E-cores = 20 CPUs); record which CPUs each taskset run uses.

## 2026-10-12 — R2 comparison variants
Done:      R2. `tools/basepaper/bp_bench.c` (`--variant fork|seq-recompile|seq-precompiled|threads-pattern|threads-data|dfa`, `-f FILE`, `-n N`, `-t T`, `--warmup W`, `--no-latency`, `--dump-masks FILE`), one JSON object per run (validated by `tools/basepaper/check_bench_json.py`). `bp_lib` now exports `bp_allowed_cpus` / `bp_pin_to_cpu`. The `dfa` variant builds the five patterns through the ordinary rule loader (placeholder category/severity; rule format unchanged) and our NFA/DFA code. `make bp` builds `bin/bp_fork` and `bin/bp_bench` (bp_bench links the core objects). Test: `tests/basepaper/test_variants.sh` (part of `make bp-test` / `make test`) checks that every variant's masks equal `bp_fork --quiet` for every fixture in `tests/basepaper/` (`*.txt`, `*.tsv`, first tab column, `#` lines skipped; new fixtures such as R3's `labeled.tsv` are picked up automatically), including cycling past the pool size with `-n`. New fixture `edge_cases.txt` (empty line, bare `?`, 10- vs 11-digit numbers, bytes ≥ 0x80, …); the DFA agrees with glibc on all of it.
State:     `make test` (incl. bp-test) passes; `make SAN=address bp-test` and `make SAN=thread bp-test` clean. Smoke runs only; no benchmark results saved yet (those come with R4/R6).
Broken:    T2.6 still waits for Graphviz.
Next:      R3 (table41.txt, matching_module.txt, gen_bp.py, labeled.tsv), then R4.
Decisions: timed region = the loop over inputs only; thread creation and regex/DFA compilation are reported separately as `setup_ns` (variants that compile per input by design, `fork` and `seq-recompile`, include it in the loop). `threads-pattern` and the data variants pin worker j to the (j mod n)-th allowed CPU, as the paper pins child i. Per-input latency uses two clock reads per input (a visible cost for `dfa`); `--no-latency` drops them for pure throughput runs. `fork` JSON reports per-child mean regcomp/regexec, mean fork-to-collect time and any inputs that got fewer than 5 records.

## 2026-10-12 — R3 inputs and labeled set
Done:      R3. `tests/basepaper/table41.txt` (the five §4.1 inputs), `matching_module.txt` (the six §4.4 matching-module inputs), `tools/basepaper/gen_bp.py` (fixed seed 20261012, templates documented in its header) and the committed `tests/basepaper/labeled.tsv` (1,250 lines: text, true labels, template). Counts by label set: EMAIL 200, PHONE 180, QUESTION 180, GREETING 150, NAME 150, multi-category 120, NONE 270. `tests/basepaper/test_gen.sh` (in `make bp-test`) checks the generator is deterministic and the committed file is its exact output. `test_variants.sh` now also covers `labeled.tsv`, `table41.txt`, `matching_module.txt`: all six variants give the same masks as `bp_fork` on all 1,250 lines.
State:     `make test` passes. Nothing in `src/` changed.
Broken:    T2.6 still waits for Graphviz.
Next:      R4 benchmark runner (`run_bp.sh`, `bp_eval.py`, `plot.py`).
Decisions: ground truth is by construction and describes what a line IS, so some labels disagree with the paper's regexes on purpose (capitalised `Hello everyone` = GREETING, `john smith` = NAME, `what is your name` = QUESTION, `98765 43210` = PHONE). Two-word capitalised non-names (`New Delhi`) are NONE (place, not a person). 11-digit numbers are NONE even though they contain a valid 10-digit substring. These are what RB4 measures; the template column lets RB4 break errors down by cause.

## 2026-10-12 — R4 benchmark runner
Done:      R4. `tools/basepaper/run_bp.sh [--smoke|--full] [--out DIR] [--exp RB1,..] [--runs N]` runs RB1–RB5 (runs interleaved across variants; raw JSON under `OUT/rbN/`, `machine.txt` with lscpu, per-CPU max MHz, core order, uname, gcc, exact flags via the new `make print-flags`, git hash, perf_event_paranoid, governor), then `bp_eval.py summarize` (median/min/max over runs → `rbN/summary.json`) and `tools/plot.py` (one SVG per experiment, plus `rb3_vs_seq.svg`). `bp_eval.py accuracy` does RB4 (per-category precision/recall/F1/accuracy, exact-match and label accuracy, structured-vs-natural-language groups, per-template breakdown, rejected-by-input-module count). `bp_bench` JSON now also carries `rusage` (user/sys, children, context switches, faults). Test: `tests/basepaper/test_smoke.sh` (in `make bp-test`) runs the smoke mode and checks valid JSON for every experiment, one figure per experiment, and RB4/RB3 sanity.
State:     `make test` incl. bp-test passes; `make SAN=address bp-test` and `make SAN=thread bp-test` clean. RB4's masks were cross-checked against Python's `re` on all 1,250 lines (0 differences). Smoke outputs are in `out/` (gitignored) and are not results. No full run has been done yet (R6).
Broken:    T2.6 still waits for Graphviz. `perf stat` is not usable here (`perf_event_paranoid=4`), so RB5 falls back to `/usr/bin/time -v` and getrusage and records why in `rb5/perf_unavailable.txt` (cpu-migrations is not available that way).
Next:      R5 is optional ("ask before starting"); R6 is the full run and `SUMMARY.md`. Phase 3 (T3.1…) follows.
Decisions:
  - RB3 CPU choice: the plan says `taskset -c 0..k-1`; on this machine CPUs 0/1, 2/3, … are SMT siblings, so 0..k-1 would pair siblings and not measure k cores. The runner uses the first logical CPU of each physical core, in core order (here 0,2,4,6,8,10 = six P-cores, then 12,13 = E-cores). So k=8 includes two slower E-cores; machine.txt lists the max MHz per CPU and each JSON records its CPU list. k up to 8 ≤ 14 physical cores.
  - Input counts per variant in RB3 are fixed in the script (fork 2,000; seq-recompile 20,000; threads-pattern 100,000; seq-precompiled and threads-data 1,000,000; dfa 10,000,000) so each run lasts long enough to time; speedups are ratios of inputs/s, which is size-independent. Throughput runs (RB2, RB3, RB5) use `--no-latency`.
  - RB1 repeats one input 1,000 times (that is the paper's table: one input per row); the pool-of-inputs rule applies to RB2–RB5, which draw from the labeled set.
  - RB4 is deterministic, so one run; accuracy is reported two ways (exact set match per line, and per-label decision accuracy) because the paper doesn't say which it means. F1 is 0 (not undefined) for a pattern with no true positives.
  - `tools/plot.py` writes plain SVG with the standard library (matplotlib is not installed here). Figures were checked by rendering with headless Firefox.
  - Full runs should be done from a clean, committed tree so `machine.txt` doesn't say "dirty", with nothing else running.

## 2026-10-12 — R6 full runs and write-up (R5 skipped)
Done:      R6. Full run (`tools/basepaper/run_bp.sh --full`, 5 runs per point, from the clean commit 3358f98) saved under `results/basepaper/` (raw JSON, `machine.txt`, per-experiment `summary.json`, `figures/*.svg`). `tools/basepaper/make_summary.py` writes `results/basepaper/SUMMARY.md` from those files: a verdict per claim of `docs/research/literature_review.md` §3 and detail tables for RB1–RB5. Every statement the verdict text relies on is an `assert` in the script, so the text cannot disagree with the data.
Results (all from the saved JSON, see SUMMARY.md): the paper's fork design takes about 0.19–0.21 ms per input here (paper: 3.7 ms) and is 12–13× slower than a sequential loop that recompiles, and ~500× slower than precompiled `regexec` (it was meant to be 2.4× faster); per-pattern parallelism never exceeds 2.54× on 8 cores (cannot exceed 5 with 5 patterns); data-parallel variants scale near-linearly to 4–6 cores (threads-data 4.83× at 6) and the combined DFA is ~25× the sequential baseline at 5 threads; the paper's input module rejects 146 of 1,250 labeled lines and makes NAME unmatchable; `fork` costs ~1.8 s of kernel time per 10,000 inputs against ~0 for the same work without `fork`.
State:     `make test` passes. Claim 5 (conflict resolution) is "not tested": **R5 was not done** because the plan says to ask before starting it; the SUMMARY records the multi-label counts instead.
Broken:    T2.6 still waits for Graphviz. `perf stat` not permitted here (`perf_event_paranoid=4`; `sudo sysctl kernel.perf_event_paranoid=1` would allow it), so RB5 uses `/usr/bin/time -v` and getrusage; cpu-migrations and cache misses are not available.
Next:      Phase 3 (T3.1 validators …). Optional: R5, and re-run RB5 with perf if the sysctl is changed.
Decisions: `make_summary.py` asserts its claims; if a re-run changes a result enough to break a sentence the script fails instead of printing it.

## 2026-10-12 — T3.1 validators
Done:      T3.1. `src/validators.c/.h`: `luhn_ok` (also enforces 13–19 digits), `verhoeff_ok`, `pan_ok`, `strip_separators`, and `validate_span(Validator, span, n)` which trims the regex's boundary characters, strips spaces/hyphens and runs the rule's validator (Aadhaar additionally needs exactly 12 digits starting 2–9). Tables copied from spec §4.3. `tests/test_validators.c` (5,488 checks): the §4.3 known values, 10 valid / 13 invalid Luhn cases, 5 known Verhoeff values, 11 valid / 13 invalid PANs, and properties that do not rely on remembered values (exactly one valid check digit per prefix; every single-digit error detected by Luhn and Verhoeff; every adjacent transposition detected by Verhoeff).
State:     `make test` passes (14 programs + bp-test).
Next:      T3.2 spans and extraction.
Decisions: both checksums return false on non-digits (the spec's snippet would index the tables with garbage); `validate_span` treats the leading/trailing non-alphanumerics of a span as the regex's boundary group.
