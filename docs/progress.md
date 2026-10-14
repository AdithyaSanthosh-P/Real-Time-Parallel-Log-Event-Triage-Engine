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

## 2026-10-12 — T3.2 spans and extraction, T3.3 context keywords
Done:      T3.2 and T3.3. `src/extract.c/.h`: `Extractor` (per worker; compiles a capture `regex_t` only for rules with a validator or an extract flag), `extractor_spans` (REG_STARTEND, REG_NOTBOL after the first search; the next search restarts one byte after the previous span's start so two numbers one space apart are both found), `extract_line` (fills `valid_mask` — a validator must pass on at least one span, §5.1 — plus `src_ip` and `user`), `parse_ipv4`, `extract_ip`, `extract_user`, and `context_mask` (case-insensitive substring per rule, T3.3). `MatchResult` (§2.4) added to `types.h` exactly as the spec has it. `tests/test_extract.c` (2,177 checks): IPv4 parsing edge cases; every AUTH line of `synthetic_small.log` (8 lines, all five extract rules) gives the expected IP/user, and the test fails if a corpus AUTH line has no expectation; 450+ "Failed password"/"Invalid user" lines of the real OpenSSH sample agree with an independent sscanf-style reading; validator results for the PII lines (card, Aadhaar, PAN, the timestamp that looks like a card, the order id that looks like Aadhaar); span iteration and cap; context keywords. Mutating `extract_user` makes 249 checks fail.
State:     `make test` passes (15 programs + bp-test); ASan build of test_extract clean. No change to `src/` code used by difftest, so difftest not re-run.
Next:      T3.4 confidence + multi-label (aggregate.c, §5.1–5.3), which uses `valid_mask` and `ctx_mask`. This is the task recommended for Opus.
Decisions:
  - Extraction uses the rule's span first and falls back to the line: PAM's `user=root` and sudo's login name are outside the span the rule's regex matches (the regex ends at "failure" / "attempts"), so `extract_user` tries, in order: "invalid user NAME" and " for NAME" inside the span, "sudo: NAME :" on the line, then a case-sensitive token `user=NAME` (so `ruser=` and sudo's `USER=root` target are ignored). The plan says "from the span"; this is the minimal widening the default rules need.
  - IP = last space-delimited token of the span that parses as a strict IPv4 (so `from 10.1.2`, which the rule's `[0-9.]+` accepts, gives no IP). A rule flagged only `user` (PAM, sudo) leaves `src_ip` at 0, as the rule file says.
  - Real Loghub OpenSSH lines can contain stray NUL bytes and garbled hostnames; tests terminate buffers explicitly.

## 2026-10-13 — T3.4 confidence + multi-label
Done:      T3.4. `aggregate.c/.h` rewritten for spec §5.1–5.3: rules whose validator failed on every span are discarded; per-rule score (base, +0.25 validator, +0.15 context, clamped); noisy-OR per category; labels at conf ≥ `--tau` (now implemented; removed from `cli_unimplemented`); primary by severity, then confidence, then SEC > PII > AUTH > ERR > PERF. Two-step API so the correlator can escalate in between: `aggregator_evidence` → `aggregator_escalate` (hook for T5.1: conf ≥ 0.95, severity raised) → `aggregator_decide`; `aggregator_classify` = both without escalation. Subsumption (§5.3) is `extract_subsumed` in `extract.c` (first span of a kept rule inside another kept rule's first span, same category) and only shrinks the explanation (`Classified.rule_mask`). The extractor now compiles a capture regex for every rule (needed for subsumption). `pipeline.c` runs extraction, context and subsumption on matched lines only.
           New `tools/ref_classify.py`: an independent Python implementation of §3.4/§5 (own normalizer, `re` matching, validators, scoring). `tests/expected/synthetic_small.expected` is now derived with it (changes listed in its header and reviewed); `tools/run_e2e.sh` re-derives it (fails if stale) and also compares every line's severity with it. On all six corpus files (10,179 lines) the C pipeline and the Python reference agree on primary, labels and severity for every line.
           `tests/test_aggregate.c` (rewritten): the plan's worked examples through the real post-match path (card line keeps PII_CARD and drops PII_AADHAAR; SQLi + e-mail gives SEC|PII with primary SEC; Verhoeff-failing Aadhaar gives no label), scores, noisy-OR values, the exact-τ case, τ = 0 / 0.9, discard, no-label rules, confidence tie-break before the fixed order, escalation, subsumption, and random invariants.
State:     `make test` (15 programs + bp-test), `make e2e` (posix and dfa), ASan test + e2e, TSan e2e all pass.
Next:      T3.5 masking + PII sink (Sonnet-level).
Decisions:
  - Confidence comparisons use `CONF_EPS = 1e-6`: base values are floats, and 0.35f + 0.15 is just under 0.5, so PII_PHONE_IN with its keyword would otherwise miss the spec's τ = 0.5 by rounding.
  - Effect of the spec's weak bases (0.35, τ 0.5): HTTP_401_403 and TIMEOUT alone no longer produce a label (seq 31, 33, 89–92 in the synthetic corpus). This follows §4.2/§5 as written; if the team wants bare 401/403 or timeouts labelled, raise their base or add context keywords (calibration, §5.4).
  - `Classified.rule_mask` now means "explanation": surviving rules minus subsumed ones (validator-failed rules are no longer listed). The correlator should use `MatchResult` (which keeps the raw rule mask) — e.g. SSH_ACCEPTED stays in both.
  - Escalation sets confidence to max(current, 0.95) rather than overwriting, so it never lowers a category.
Observation (for rule tuning, not changed): Apache_2k yields no labels and Linux_2k only 2 — Linux's PAM lines read `sshd(pam_unix)[pid]: authentication failure`, which `PAM_AUTH_FAIL` (`pam_unix\([a-z-]+:auth\)`) does not match.

## 2026-10-13 — T3.5 masking + PII sink
Done:      T3.5. `src/mask.c/.h`: replaces the values of surviving PII-rule spans (validator-passed only; boundary characters trimmed) in the line before output — card `XXXX…1111`, Aadhaar `XXXX XXXX 1234`, PAN `XXXXX1234X`, `key=value` secrets `key=***` (spec §7), plus e-mail `***@domain` and 10+ digit numbers `XXXXXX1234`. `router_emit`/`route_format` take an optional replacement line; `pii_quarantine.jsonl` is written for every line labelled PII; the router now has three sinks. The pipeline masks any line with a surviving PII rule (PII labelled or not), and the masked text goes to every sink, `alerts.jsonl` included (a CRITICAL card line would otherwise leak there). `record` already had `confidence`, `rules`, `flags` (T1.6/T3.4). `tests/test_mask.c` (84 checks) covers every format, non-PII lookalikes (Luhn/Verhoeff failures stay readable), several values on a line, adjacent numbers, URL-encoded values. `tools/run_e2e.sh` now (a) takes every PII value found by `tools/ref_classify.py --pii-values` (span as written, separator-stripped form, e-mail local part, secret value) and fails if any appears anywhere under the output directory, and (b) checks that `pii_quarantine.jsonl` is exactly the PII-labelled lines. Disabling the masking makes it fail.
State:     `make test` (16 programs + bp-test), `make e2e` (posix and dfa, three input modes each), ASan test + e2e pass.
Next:      Phase 4 (parallel pipeline): T4.1 SPSC queue, T4.3–T4.6 are Opus-level; I will say so before T4.1.
Decisions:
  - Masking works on the NORMALIZED line (spans are exact there), so a masked record's `line` is the URL-decoded form, e.g. `%2B`/`+` become spaces (the log timestamp `+0530` appears as ` 0530`). Unmasked lines keep the raw text.
  - E-mail addresses and phone numbers are masked too, beyond the four formats the spec lists, so that "no PII value of the corpus survives in any sink" holds for every PII rule. Say if the team wants them left readable.
  - A decoded `&` ends a secret value (`password=a%26b` masks as `password=***&b`) because the rule's regex does.

## 2026-10-13 — T4.1 SPSC ring
Done:      T4.1. `src/spsc.c/.h`: bounded single-producer/single-consumer ring of pointers (spec §2.3). `head` (consumer) and `tail` (producer) are monotonic `atomic_size_t` counters on separate 64-byte lines, the read-only `capacity`/`slots` on a third; capacity rounded up to a power of two. `spsc_try_push`/`spsc_try_pop` publish with release and observe with acquire (no RMW atomics, no locks); blocking `spsc_push`/`spsc_pop` spin (pause) 64 rounds, yield 16, then nanosleep from 1 µs doubling to 50 µs, and take an optional `atomic_bool *stop` so a stage can be unblocked when another fails. `spsc_size` for `--dispatch least` and the stats line (clamped snapshot from third threads). `tests/test_spsc.c` (751 checks): capacity rounding/alignment/cache-line layout, FIFO/full/empty, capacity 1, indices wrapping past SIZE_MAX, the stop flag (incl. waking a blocked thread), 10M-item producer/consumer stress checking order and checksum, a capacity-1 stress, a spinning try_* stress, a pool of non-atomic payload objects recycled through a return ring (the T4.2 pattern), and a third thread reading the size.
State:     `make test` passes (17 programs + bp-test). test_spsc clean under `make SAN=thread` (8.5 s) and `make SAN=address`. Mutation check: making the tail publish `relaxed` gives 6 TSan data-race reports in the payload test.
Next:      T4.2 batch pool.
Decisions: no condvar fallback (the plan makes it optional); the stop flag replaces it for shutdown on error. Normal shutdown will use EOF sentinel batches (T4.6).

## 2026-10-13 — T4.2–T4.6 parallel runtime
Done:      T4.2–T4.6 (one commit: the pieces only work together).
  - T4.2 `src/pool.c/.h`: `Job` = a `Batch` plus its `ResultBatch` (per-line `MatchResult`, subsumed mask, `Classified`, masked text). `pool_size_for(w, depth)` = w × depth + w + 4 jobs, allocated before any thread starts and recycled router → reader through an SPSC free ring. Job buffers are pre-sized (64 KiB line buffers as before, 64 KiB masked text; new `sb_reserve`). Check: `tests/test_alloc.sh` (`make alloc-test`, also run by `make test`) preloads `tests/alloc_count.c`, an allocation counter, and requires the number of allocations outside glibc's `regexec` to be equal (± the pool size) on a 100k- and a 500k-line input, for dfa/posix and -w 1/4. Measured: identical counts at both sizes with -w 1 (dfa and posix), one more allocation at 500k with -w 4. `make SAN=address alloc-test` runs -w 4 on the 100k-line file under LSan: no leaks.
  - T4.3 `src/worker.c/.h`: one pthread per worker with its own `Engine` (DFA shares the tables; POSIX compiles its own `regex_t` set) and its own `Extractor`; per line the same steps as the Phase 3 loop (match, extraction, context, subsumption, masking). Pinning with `pthread_setaffinity_np` (`--pin`, default on) using `worker_cpu_order`: allowed CPUs, one logical CPU per physical core first (sysfs topology), SMT siblings after — on this machine 14 cores first, then the 6 siblings.
  - T4.4 `src/dispatch.c/.h`: `rr` and `least` (fewest queued jobs, ties rotate), blocking backpressure, `--drop` (try-push, dropped lines/batches counters, the reader reuses the job). `--batch-delay` is implemented in the reader (`reader_set_flush_ms`): on a pipe/tty/socket a partial batch is returned `ms` after its first line arrived if no further line is complete (ppoll); regular files never wait.
  - T4.5 `pipeline.c` threads: reader+dispatcher (calling thread), workers, aggregator (polls all worker output rings round-robin, classifies), router (sinks, stats counters, returns jobs to the pool), and `src/stats.c/.h` for the 1 Hz stderr line `lines/s | queues … | AUTH … PII … | dropped …` (not with --quiet). The final summary line now also gives the dropped count.
  - T4.6 shutdown: `WORKER_EOF` sentinel to every worker, forwarded; the aggregator forwards one to the router after all N; all threads joined, sinks closed. On any failure (write error, read error, thread start) a shared `stop` flag makes every blocked ring operation give up; the run exits 1 with the error and does not hang.
  - CLI: `-w N`, `--batch-delay`, `--dispatch`, `--pin/--no-pin`, `--drop` removed from `cli_unimplemented` (still unimplemented: `--engine fork`, `--ordered`, `--bench`).
  - Tests: `tests/test_runtime.c` (pool, dispatcher incl. drop/stop/EOF, `worker_process` + classification reproduce `synthetic_small.expected` for dfa/posix and batch sizes 256/7/1, the worker thread hand-off and stop, CPU order, stats line); reader batch-delay test in `test_reader.c`; `tools/run_e2e.sh` adds -w 4 -b 5 runs for both engines, `--drop` accounting (written + dropped = lines, no duplicate seq), a write error (sink symlinked to /dev/full: exit 1, message, no hang) and the stats line.
  - `tools/compare_runs.sh` (`make compare`): for every corpus file, posix and dfa, -w 1/2/4/8, plus `-b 3 --dispatch least` and stdin `--no-pin --batch-delay 0` at -w 8, all three sinks sorted must equal the single-threaded Phase 3 binary (built from commit 4f2e1ca into `build/ref-4f2e1ca/`) record for record — 72 runs.
State:     `make test` (18 programs + bp-test + alloc-test), `make e2e`, `make compare` pass in the plain, `SAN=thread` and `SAN=address` builds; no TSan reports (TSan compare sweep of all 72 runs: about 8.5 min).
Next:      T5.1 correlator (Opus-level).
Decisions:
  - `-w 1` also runs the threaded pipeline (reader, 1 worker, aggregator, router), as the spec says only workers scale with -w; the reference for equivalence is the Phase 3 binary, rebuilt from git, not a second code path.
  - compare_runs blanks `ts` on both sides: lines without a parseable timestamp carry the ingest wall-clock time (spec §6), which differs between any two runs. Everything else is compared byte for byte.
  - QUEUE_DEPTH = 4 jobs per worker ring; with the default -b 256 that is up to 1,024 lines queued per worker.
  - Output order is unordered across batches with -w > 1 (spec §2.3); `--ordered` is still the stretch item.
  - The masked text is computed in the worker (it needs the worker's capture regexes), not in the router.
Observation: glibc `regexec` allocates and frees scratch memory inside many calls: with the posix engine nearly all of the run's allocation calls happen inside `regexec` (millions for a 1M-line input; outside it the count is constant). Worth keeping in mind for the posix-vs-dfa scaling experiments (malloc arena contention with many workers).
Open for T5.1: with -w > 1 the aggregator sees batches out of seq order, so the correlator's sliding windows (keyed on log timestamps) receive lines out of order within a few batches; T5.1 must either tolerate that or reorder.

## 2026-10-13 — T5.1 correlator
Done:      T5.1. `src/correlate.c/.h` (spec §6): BRUTE_FORCE (≥ maxretry AUTH failures from one IP within findtime → SEC, HIGH, conf 0.95, once per window per IP), PASSWORD_SPRAY (≥ K distinct users from one IP within T → SEC, HIGH, once per T), FAIL_THEN_SUCCESS (a matched no-label AUTH rule — SSH_ACCEPTED — from an IP over the BRUTE_FORCE threshold → SEC, CRITICAL), ERROR_STORM (≥ N ERROR lines within T → ERROR raised to HIGH, one alert per storm). Open-addressing map keyed by IPv4, each entry a ring of the last 64 failures (time + FNV hash of the user), used for both counts. Escalation goes through the T3.4 hook (`aggregator_escalate`, then `aggregator_decide` again with the flags).
           Pipeline: a correlator thread between aggregator and router. The aggregator now keeps the per-line `Evidence` in the job. The correlator restores input order (it holds early batches until the next expected `seq_start` arrives; batches dropped by `--drop` reach it on a reader→correlator ring so their range is not awaited), so the windows see lines in log order whatever -w is, and the sinks are now written in input order. Summary line gains "N escalations".
           Tests: `tests/test_correlate.c` (the plan's cases — 4 failures → nothing, 5 in the window → exactly one escalation, 5 spread over > window → nothing, burst then Accepted → CRITICAL — plus window boundaries, per-IP windows, IP-less failures ignored, re-escalation after a window, spray vs brute force, users missing, error storm start/suppression/end, 5,000 IPs (map growth), time going backwards, config validation); mutating the threshold or the once-per-window rule fails it. `tools/ref_classify.py` now models §6 independently (own timestamp parser, IP/user extraction, windows); C and Python agree on primary, labels, severity, confidence and flags for every line of all six corpus files (OpenSSH_2k: 34 BRUTE_FORCE + 5 PASSWORD_SPRAY; BGL_2k: 1 ERROR_STORM). `synthetic_small.log` gained a correlator section (seq 179–217: brute force + fail-then-success, spray, an under-threshold series); the expected file was regenerated (rows 0–178 unchanged, verified by diff) and its header lists the new escalations. `run_e2e.sh` compares flags with the reference and runs the plan's OpenSSH_2k check (escalations present, all in alerts.jsonl, every field equal to the reference). `compare_runs.sh` now uses `-w 1` of the binary under test as the reference and checks that run against `ref_classify.py` (the Phase 3 binary no longer applies once escalations exist; `REF_COMMIT=4f2e1ca` still selects it).
State:     `make test` (19 programs + bp-test + alloc-test), `make e2e`, `make compare` pass; TSan and ASan test/e2e clean.
Next:      Phase 6: T6.1 fork engine (Sonnet-level), T6.2 --bench, T6.3 generator, …
Decisions:
  - No CLI flags for the correlator parameters: spec §8 lists none and CLI changes need approval. They are a `CorrelatorConfig` with the spec's defaults (maxretry 5, findtime 60 s, K 5, T 300 s).
  - The spec gives no N/T for ERROR_STORM: chosen N = 100 ERROR lines within 60 s. The storm alert raises ERROR to HIGH (so it reaches alerts.jsonl) rather than adding SEC; its flag is named `ERROR_STORM` (as the router has named it since T1.6), the others `ESCALATED_*`.
  - "AUTH failure" = a line labelled AUTH_FAILURE (after τ) with an extracted source IP; PAM and sudo lines carry no IP and do not count.
  - "Emit once per window": after an escalation at t, the same kind for the same IP can fire again at t + window or later.
  - The spec's "second map ip → set of usernames" is folded into the per-IP failure ring (user hash next to each time); distinct users are counted among the last 64 failures.
  - Phase R test fix: the R1 zombie check in `tests/basepaper/test_bp.c` failed about 1 run in 3 under TSan because it treated a zombie seen on two consecutive /proc polls as a leak, while the paper's design reaps only after reading all 5 records. It now checks for accumulation instead (never more than 5 children of the bp_fork process at once, none left at the end). bp_fork itself is unchanged.
Open questions:
  - Should the correlator parameters (maxretry, findtime, spray K/T, storm N/T) become CLI options? They would extend spec §8.
  - ERROR_STORM N/T defaults are ours, not the spec's.
  - The IP map never forgets an IP (≈1 KB each); fine for the datasets of §10.1, unbounded for an endless stream.

## 2026-10-14 — Review 2 demo
Done:      `tools/demo.sh` (`make demo`): 7 paused steps on the committed sample logs — build; compiler (NFA/DFA sizes, `\d` rejected with its offset, DOT dump, rendered to PNG only if Graphviz is installed); classification (multi-label, confidence, checksum look-alikes not flagged); PII masking; correlator on OpenSSH_2k; -w 1 vs -w 8 identical output plus the live stats line; base-paper verdicts from `results/basepaper/SUMMARY.md`. `--no-pause`, or step names to run a subset. Talking points and likely questions in `docs/demo_guide.md`. No new numbers: the demo prints what the program outputs and the saved Phase R results.
State:     demo runs clean end to end; `make test` passes.
Next:      Phase 6 (T6.1 fork engine, T6.2 --bench, …).

## 2026-10-14 — T2.6 DOT dump (done-when met)
Done:      Graphviz 14.1.2 is now installed. `bin/triage -r tests/fixtures/tiny.rules --dump-dot tiny.dot && dot -Tpng tiny.dot -o tiny.png` succeeds; the picture shows the 18-state Thompson NFA (ε-edges dashed, `^`/`$` anchor edges) and the 7-state combined DFA with collapsed byte-range labels. `tools/demo.sh` step 2 now renders `out/demo/tiny.png`.
State:     every task of Phases 0–5 and R is done except R5 (optional).
