# Implementation Plan

Spec: `docs/project_design.md` (§ numbers below refer to it). Work top to bottom; each task depends on the ones before it unless noted. Tick `[x]` when the **Done when** check passes and `make test` is green. Commit per task with the task ID in the message.

The order is deliberate: get a correct, single-threaded, end-to-end pipeline on the POSIX engine first (Phase 1), then replace the engine with our compiler (Phase 2), then add classification logic (Phase 3), and only then parallelize (Phase 4). Every later phase is checked against the output of the earlier, simpler version.

**Update (2026-10-12, professor's direction):** the base paper was written by students and is not verified, so before continuing our own design we first reproduce and benchmark the base paper's approach (**Phase R**, inserted after Phase 2), then build the rest of our design. Phase R reuses our compiler (Phase 2) for one comparison variant. The supporting literature review is `docs/research/literature_review.md` (read it only for R6). Phases 3–8 are unchanged except T6.1, which now reuses R1's fork/collect code.

---

## Phase 0 — Setup

- [x] **T0.1 Repo skeleton and build.** `git init`; directories per spec §9; `.gitignore` (`bin/`, `build/`, `data/`, `out/`, `results/*.json` optional, `*.o`). Makefile with targets `all`, `test`, `difftest`, `e2e`, `clean`, and `SAN=address|thread` variants (adds `-fsanitize=...  -g -O1`). Minimal `src/main.c` that parses `--help`. Tiny test runner (`tests/test_main.h` with `CHECK()` macros; no framework).
  *Done when:* `make && make test && make SAN=address test` succeed with zero warnings; `bin/triage --help` prints the CLI from spec §8.
- [x] **T0.2 Test corpus.** `tools/fetch_samples.sh` downloads Loghub's 2k-line samples (OpenSSH, Apache, Linux, BGL, HDFS — check exact paths in github.com/logpai/loghub) into `tests/corpus/`. Hand-write `tests/corpus/synthetic_small.log`: at least one positive line per rule in `rules/default.rules` (use the example lines from spec §4.2), plus ≥ 20 negative/near-miss lines (e.g. invalid Aadhaar checksum, 12-digit order ID, the word "errors" in normal text, `select` in a benign URL). Commit the corpus (small files only).
  *Done when:* corpus files exist; each rule id appears in a comment next to its positive line in `synthetic_small.log`.

## Phase 1 — Correct single-threaded baseline (POSIX engine)

- [x] **T1.1 Rule loader** (`rules.c/.h`, §4.1). Parse the TSV format into `Rule` structs (id, category, severity, base, icase, anchored flags, validator, extract list, context keywords, regex string). Reject: wrong column count, unknown category/severity/validator, > 64 rules, duplicate ids — with file:line in the error.
  *Done when:* `tests/test_rules.c` loads `rules/default.rules` as 26 rules with correct fields, and each malformed fixture in `tests/fixtures/bad_*.rules` fails with the expected message.
- [x] **T1.2 Normalizer** (`normalize.c/.h`, §4.2 notes). Produce the match copy: decode `%xx` (invalid escapes left as-is), `+`→space, strip control chars except `\t`. No lowercasing. Output length ≤ input length.
  *Done when:* `tests/test_normalize.c` covers `%27`, `%2e%2e%2f`, `%zz`, `%`, trailing `%4`, `+`, mixed control chars.
- [x] **T1.3 Reader** (`reader.c/.h`). Read from file or stdin in large chunks; split on `\n`; handle `\r\n`, final line without newline, empty lines, and lines longer than the chunk size. Assign `seq_id`. Parse timestamp per §6 (syslog `Mon DD HH:MM:SS`, ISO-8601), else ingest time. Fill `Batch` (§2.4) with raw + normalized copies.
  *Done when:* `tests/test_reader.c` passes on crafted inputs (CRLF, 1 MB single line, no trailing newline, empty file) and line counts on every corpus file equal `wc -l` (+1 if no trailing newline).
- [x] **T1.4 POSIX engine** (`engine_posix.c/.h`, §2.1). An `EnginePosix` instance holds its own compiled `regex_t[]` (`REG_EXTENDED|REG_NOSUB`, plus `REG_ICASE` per rule). `uint64_t posix_match(EnginePosix*, line, len)` returns the rule mask. Lines are not NUL-terminated in the batch — copy to a per-instance scratch buffer or use `REG_STARTEND`.
  *Done when:* on `synthetic_small.log`, every positive line's mask includes its rule (test asserts it from the comments added in T0.2).
- [x] **T1.5 Aggregator v0** (`aggregate.c/.h`). Rule mask → `label_mask` (any matched rule's category), primary = highest severity with tie order from §5.3, line severity. No confidence, validators or context yet (set confidence = 1.0).
  *Done when:* unit test with hand-built masks covers ties and multi-label cases.
- [x] **T1.6 Router v0** (`route.c/.h`, §7). JSONL writer with correct JSON string escaping; sinks `out/alerts.jsonl` (severity ≥ HIGH), `out/store.jsonl` (all). PII masking comes later (T3.5).
  *Done when:* output of a test run parses with `python3 -m json.tool` line by line (script in `tools/check_jsonl.py`).
- [x] **T1.7 Single-thread pipeline + CLI** (`main.c`, `cli.c`). Wire reader → posix engine → aggregator → router in one thread. Implement CLI parsing for all flags in §8 (unimplemented ones print "not yet implemented" and exit 2).
  *Done when:* `make e2e` runs `bin/triage --engine posix -w 1 tests/corpus/synthetic_small.log` and compares `(seq, primary, labels)` against `tests/expected/synthetic_small.expected` (create this file by hand from the corpus comments, then review it). Diff is empty.

## Phase 2 — Regex compiler and DFA engine (core contribution)

- [x] **T2.1 Regex lexer + recursive-descent parser → AST** (`regex_lex.c`, `regex_parse.c`, `ast.c`, §3.1–3.2). Grammar exactly as §3.1. Charsets as 256-bit sets; `.` = all bytes except `\n`; icase folds charsets; `^`/`$` only at ends (recorded as flags by the loader). Reject unsupported syntax with position: `\d`, `\s`, `\w`, `\b`, `\1`, `(?`, `*?`, `+?`, `??`, `{,n}`, `^`/`$` in the middle, unbalanced parens/brackets. Add `ast_print()` for debugging.
  *Done when:* `tests/test_parse.c` has ≥ 40 cases (valid → expected AST print; invalid → expected error position), and all 26 default rules parse.
- [x] **T2.2 Thompson NFA** (`nfa.c/.h`, §3.2 step 2–3). One construction per AST node type; `{n,m}` expanded; accept states tagged with rule id; combined NFA with a fresh start state. Include `nfa_simulate(nfa, line, len) → mask` (set-of-states simulation with the same unanchored/anchored semantics as §3.2 step 4) — this is a second oracle and the reference for the DFA.
  *Done when:* `tests/test_nfa.c`: for each default rule and ~200 strings (corpus lines + synthetic edge strings), `nfa_simulate` mask == per-rule `regexec` mask.
- [x] **T2.3 Subset construction → combined DFA** (`dfa.c/.h`, §3.2 step 4). ε-closure with an explicit stack; NFA-state sets as sorted int arrays (or bitsets) hashed into a state table; `U` set added to every transition for unanchored rules; `accept_mask` and `accept_end_mask`; transition table `int32_t next[n][256]`. Enforce `--max-states`; on overflow, split rules into groups (halve recursively) and build one DFA per group. Report state count and build time (`--verbose`).
  *Done when:* DFA mask == `nfa_simulate` mask on the same test set as T2.2; building the 26 default rules succeeds under the default cap.
- [x] **T2.4 DFA engine** (`engine_dfa.c/.h`, §3.3). Match loop exactly as §3.3, iterating over all groups. Hook into `--engine dfa`.
  *Done when:* `make e2e` passes with `--engine dfa` (same expected file as T1.7).
- [x] **T2.5 Differential test tool** (`tools/difftest.c`, §3.5). For every line in `tests/corpus/*.log` (and optionally a `--random N` mode generating strings biased toward rule fragments), compare DFA mask vs per-rule `regexec` mask. Print rule id, line number and line on mismatch; exit non-zero if any.
  *Done when:* `make difftest` reports 0 mismatches on all corpus files and `--random 100000`.
- [x] **T2.6 DOT dump** (`dot.c`, §3.2 step 7). `--dump-dot FILE` writes the NFA and DFA (and minimized DFA later) for the loaded rules; collapse byte ranges into labels like `[a-z]` so diagrams are readable.
  *Done when:* `dot -Tpng` renders the DFA for a 2-rule file in `tests/fixtures/tiny.rules` without errors.

## Phase R — Base-paper reproduction and benchmark (spec §10.4)

Goal: rebuild the base paper's program as described, measure each of its claims on our machine, and compare it against fairer baselines and our design on the **same five-pattern task**. All code lives in `tools/basepaper/` and `tests/basepaper/`; nothing in `src/` changes except where noted. Two rules apply to every R task (also in CLAUDE.md):

- **Faithful, not fixed.** Reproduce the paper's design including its inefficiencies (fork per pattern per input, `regcomp`/`regfree` inside every child). Don't optimise or correct it; differences from the paper are findings to record, not bugs to fix.
- **No fitting.** Never tune anything so our numbers match the paper's. Report what we measure.

The five patterns, verbatim from base paper §3.3, as C string literals (compile with `REG_EXTENDED`; index = core number in the paper's screenshots):

```c
/* 0 EMAIL    */ "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}"
/* 1 PHONE    */ "[6-9][0-9]{9}"
/* 2 QUESTION */ ".*\\?$"
/* 3 GREETING */ "^(hello|hi|hey|good morning|good afternoon)"
/* 4 NAME     */ "^[A-Z][a-z]+ [A-Z][a-z]+$"
```

Keep them in `tools/basepaper/patterns.h` (one shared definition used by every variant).

- [x] **R1 Faithful reproduction** (`tools/basepaper/bp_fork.c`). Per input: one pipe; `fork()` one child per pattern; child calls `sched_setaffinity` to core `i % ncores`, then `regcomp` → `regexec` → `regfree`, writes a fixed-size record `{idx, matched, t_regcomp_ns, t_regexec_ns}` to the pipe and `_exit(0)`; parent reads 5 records, then `waitpid`s each child. Modes: interactive (prompt and per-core ✅/❌ lines in arrival order, like base paper §6), and batch (`-f FILE`, one input per line, `--quiet`). Normalisation switch `--normalize=none|paper`: `paper` follows the §3.2 pseudocode exactly (length ≤ 256, `ALLOWED_CHARS` check, lowercase, trim, collapse spaces/tabs, drop newlines, with its error messages); default `none`, which is what the §6 screenshots imply. Add Makefile targets `bp` (build everything in `tools/basepaper/`) and `bp-test` (run the R tests); make `make test` run `bp-test` too.
  *Done when:* (a) the 4 inputs in `tests/basepaper/screenshots.txt` give exactly the matched sets shown in base paper §6 under `--normalize=none` (compare as sets — arrival order varies); (b) `tests/basepaper/input_module.txt` runs the §4.4 Input Processing inputs under `--normalize=paper`, and the test output lists which rows the pseudocode contradicts (e.g. the apostrophe in `What's your name?` is not in `ALLOWED_CHARS`; the pseudocode has no empty-input check) — recorded, not fixed; (c) a 10,000-input batch run leaves no zombie processes and is clean under `make SAN=address`.
- [x] **R2 Comparison variants** (`tools/basepaper/bp_bench.c`, one binary, `--variant V`). Same 5 patterns, same inputs, same output mask:
  - `fork` — R1's mechanics in timing mode;
  - `seq-recompile` — one process; per input, loop the 5 patterns with `regcomp`/`regexec`/`regfree` (the children's work without `fork`);
  - `seq-precompiled` — compile once; per input, 5 `regexec` calls (the fair sequential baseline);
  - `threads-pattern` — persistent pool of 5 pthreads, one pattern each, own precompiled `regex_t`, a barrier per input (the paper's pattern parallelism without `fork`);
  - `threads-data` — `-t N` pthreads, each with its own 5 precompiled `regex_t`, inputs split into contiguous chunks (data parallelism);
  - `dfa` — the 5 patterns compiled by our compiler (Phase 2) into one combined DFA; `-t N` splits inputs across threads. Use the existing compiler API (build `Rule` structs in code or call the builder directly); don't change the rule-file format to admit these categories.
  Each run prints one JSON object: variant, inputs, threads, CPU list (from `sched_getaffinity`), total seconds, inputs/s, per-input latency p50/p90/p99 (ns), and for `fork` also mean fork-to-collect time and mean child `regcomp` and `regexec` times.
  *Done when:* all variants give identical masks on every file in `tests/basepaper/` and on the R3 labeled set (a test asserts it); JSON validates; thread variants clean under `make SAN=thread`.
- [x] **R3 Inputs and labeled set.** `tests/basepaper/table41.txt` (the 5 inputs of base paper §4.1) and `matching_module.txt` (§4.4 matching-module inputs, including `hello john@test.com`); `screenshots.txt` and `input_module.txt` already exist from R1. `tools/basepaper/gen_bp.py`: fixed-seed generator writing `tests/basepaper/labeled.tsv` with about 1,250 lines (the paper's stated sample count) balanced across EMAIL, PHONE, QUESTION, GREETING, NAME, plus NONE lines, multi-category lines and near-misses (malformed emails, 11-digit numbers, numbers starting 1–5, lowercase names, greetings mid-sentence, questions without `?`). Ground truth describes what each line **is** (by construction from its template), not what the regexes match. Document every template in the script header. The paper's own dataset isn't available, so absolute accuracy won't be comparable — only trends (structured vs natural-language categories).
  *Done when:* two runs with the same seed give identical files (same hash); the script prints per-category counts; the file is committed (it is small).
- [x] **R4 Benchmark runner** (`tools/basepaper/run_bp.sh`, `tools/basepaper/bp_eval.py`). Each point ≥ 5 runs; raw JSON to `results/basepaper/<exp>/`; machine info (`lscpu`, physical core count, `uname -a`, `gcc --version`, flags) to `results/basepaper/machine.txt`. Experiments (spec §10.4):
  - **RB1 single-input latency** (paper §4.1): the 5 `table41` inputs, 1,000 repetitions each, every variant; median and p99 per input; `fork` breakdown.
  - **RB2 batch size** (paper §5.3): batches of 1, 10, 100, 1,000, 10,000 inputs from the labeled set; inputs/s per variant.
  - **RB3 core scaling** (paper §4.3): `taskset -c 0..k-1` for k = 1, 2, 4, 6, 8 (stop at the physical core count and record it); variants `fork`, `threads-pattern`, `threads-data -t k`, `dfa -t k`; speedup vs the same variant at k = 1 **and** vs `seq-precompiled`. Record whether any variant's speedup exceeds 5 (the pattern count).
  - **RB4 accuracy** (paper §4.2): labeled set under `--normalize=none` and `--normalize=paper`; per-category precision, recall, F1 and overall accuracy (`bp_eval.py`). Expect the NAME pattern to fail after lowercasing — report it, don't fix it.
  - **RB5 kernel vs user time**: `fork` vs `seq-recompile` on 10,000 inputs with `perf stat -e task-clock,context-switches,cpu-migrations,page-faults` and user/sys time (`/usr/bin/time -v`; fall back to `getrusage` if `perf` is unavailable).
  Create a minimal `tools/plot.py` (median with min/max bars) for these figures; T6.5 extends it later.
  *Done when:* a smoke run at small sizes produces valid JSON for every experiment and one figure per experiment.
- [ ] **R5 (optional — ask before starting) Conflict-resolution reproduction.** Implement base paper §3.4 `resolve_conflicts` and `calculate_confidence` with a written interpretation of each undefined function (specificity, completeness, historical accuracy, base confidence), then compare with the §4.4 Output Generation table. No tuning to make the table match.
  *Done when:* a table lists each row of §4.4's output table as reproduced / not reproduced with the reason.
- [x] **R6 Full runs and write-up.** Run RB1–RB5 at full size. Write `results/basepaper/SUMMARY.md`: for each claim in `docs/research/literature_review.md` §3, the paper's figure, our measurement (median with min/max), a verdict (reproduced / qualitatively supported / not reproduced) and a one-line explanation; then exact commands and machine info. Numbers only from the saved JSON.
  *Done when:* every claim row has a measured entry, and each number in `SUMMARY.md` can be traced to a JSON file under `results/basepaper/`.

## Phase 3 — Classification logic

- [x] **T3.1 Validators** (`validators.c/.h`, §4.3). `luhn_ok`, `verhoeff_ok`, `pan_ok`, plus a helper that strips spaces/hyphens from a span. Use the tables in §4.3 exactly.
  *Done when:* `tests/test_validators.c` passes the known values in §4.3 plus ≥ 5 more valid/invalid cases each.
- [x] **T3.2 Spans and extraction** (`extract.c/.h`, §3.4). For matched rules that need a validator or extraction, run that rule's `regexec` with `pmatch` on the normalized line to get spans (iterate for multiple occurrences). Extract IPv4 (dotted quad → `uint32_t`) and username tokens from the span for rules flagged `ip`/`user`. Each worker owns its own capture `regex_t` set.
  *Done when:* unit tests extract the right IP/user from every AUTH example line in the corpus, and spans for PII lines contain exactly the number (plus boundary chars).
- [x] **T3.3 Context keywords.** Case-insensitive substring check per rule's context list → `ctx_mask`.
  *Done when:* unit test.
- [x] **T3.4 Confidence + multi-label** (`aggregate.c`, §5.1–5.3). Discard rules whose validator fails on all spans; per-rule score; noisy-OR per category; `--tau`; correlator escalations hook (stub until T5.1); subsumption only affects the explanation list.
  *Done when:* `tests/test_aggregate.c` checks worked examples: card line gives PII_CARD kept + PII_AADHAAR discarded (spec §4.2 overlap note); SQLi + email line gives labels SEC|PII, primary SEC; a validator-failing Aadhaar yields no PII label. `tests/expected/synthetic_small.expected` updated and `make e2e` passes for both engines.
- [x] **T3.5 Masking + PII sink** (`route.c`, §7). Mask Aadhaar/card/PAN/secret spans in `pii_quarantine.jsonl` and in `store.jsonl` for PII lines; add `confidence`, `rules`, `flags` fields.
  *Done when:* e2e check script asserts no unmasked PII value from the corpus appears anywhere under `out/`.

## Phase 4 — Parallel runtime

- [x] **T4.1 SPSC ring** (`spsc.c/.h`, §2.3). Power-of-two capacity, cache-line-separated head/tail, release/acquire ordering; `try_push`, `try_pop`, blocking variants with short spin then `nanosleep`/futex-free backoff (or condvar fallback behind a compile flag).
  *Done when:* `tests/test_spsc.c` stress test (10M items, producer/consumer threads, checks order and checksum) passes under `make SAN=thread`.
- [x] **T4.2 Batch pool.** Fixed pool of preallocated `Batch` and result-batch objects recycled through a return ring (router → reader), so steady state does no `malloc`. Pool size = workers × queue depth + slack.
  *Done when:* valgrind/ASan run on a 100k-line file shows no leaks and allocation count does not grow with input size (count allocations with a debug counter).
- [x] **T4.3 Workers** (`worker.c/.h`, §2.2). N pthreads, optional pinning (`pthread_setaffinity_np`, `--pin`/`--no-pin`), each with its own engine instance (DFA: shared tables; POSIX: own `regex_t` set) and own capture regexes. Produces `MatchResult` batches.
- [x] **T4.4 Dispatcher** (`dispatch.c/.h`, §2.3). `rr` and `least`; batch-delay flush of partial batches; blocking backpressure; `--drop` with a dropped counter.
- [x] **T4.5 Aggregator + router threads, stats** (`pipeline.c`, `stats.c`). Aggregator consumes result rings from all workers; router writes sinks; 1 Hz stderr stats line (lines/s, per-worker queue depth, per-category counts, dropped).
- [x] **T4.6 Shutdown and equivalence.** EOF propagates as a sentinel batch; all threads join; sinks flushed.
  *Done when (covers T4.3–T4.6):* for every corpus file, engines `posix` and `dfa`, and `-w 1,2,4,8`, the set of `(seq, primary, labels, confidence)` records equals the single-thread result from Phase 3 (`tools/compare_runs.sh`, sorts by seq). Clean under `make SAN=thread` and `make SAN=address`.

## Phase 5 — Correlator

- [x] **T5.1 Correlator** (`correlate.c/.h`, §6). Single thread between aggregator and router. Open-addressing hash maps keyed by source IP; ring of timestamps per key; distinct-user tracking for spraying; BRUTE_FORCE, PASSWORD_SPRAY, FAIL_THEN_SUCCESS, ERROR_STORM with parameters from CLI/config (`maxretry`, `findtime`, etc.). Escalations add CAT_SEC with confidence 0.95 and a flag; emit each escalation once per window.
  *Done when:* `tests/test_correlate.c` replays crafted sequences (4 failures → nothing; 5 within window → one escalation; 5 spread over > window → nothing; burst then Accepted → CRITICAL). Running on the Loghub OpenSSH 2k sample produces escalations, and the e2e check confirms they appear in `alerts.jsonl`.

## Phase 6 — Baselines and benchmarking infrastructure

- [x] **T6.1 Fork engine** (`engine_fork.c/.h`, §2.1). Reproduce the base paper: per line, `fork()` one child per rule; child runs `regcomp` + `regexec` and writes the result to a pipe; parent collects and `waitpid`s. Single-threaded only (`-w` ignored with a warning). Reuse the fork/pipe/collect code from R1 (move the shared part into `src/` if needed); here it runs our log rules instead of the paper's five patterns.
  *Done when:* masks equal `posix -w 1` on `synthetic_small.log`.
- [x] **T6.2 `--bench` mode** (§8, §10.2). Preload the whole input into memory before timing; disable sinks; record per-line latency (ingest → aggregated) into a log-scale histogram; print one JSON object: engine, workers, batch, lines, seconds, lines/s, MB/s, p50/p90/p99/p99.9, DFA states, build time.
  *Done when:* output validates as JSON and line counts match the input.
- [x] **T6.3 Synthetic generator** (`tools/gen.c`, §10.1). Templates for normal traffic per log type (sshd, Apache access/error, app logs); configurable injection rates per category; valid and invalid Luhn/Verhoeff numbers; attack payloads from a payload file; optional rate pacing (`clock_nanosleep`, `CLOCK_MONOTONIC`) to stdout. Writes a ground-truth file (`seq`, true labels) alongside.
  *Done when:* generating 1M lines with a fixed seed is reproducible (same hash) and ground-truth counts match configured rates within 1%.
- [x] **T6.4 Bench rule sets.** `rules/bench_5.rules` … `bench_64.rules` (subsets of the defaults plus realistic extra rules — more keywords, more PII/attack variants — all in the §3.1 subset) for the rule-count experiment.
  *Done when:* all pass `make difftest` and build under the state cap.
- [x] **T6.5 Bench runner** (`tools/run_bench.sh`). Runs a named experiment matrix ≥ 5 times per point, saves raw JSON to `results/<exp>/`, records machine info (`lscpu`, `uname -a`, `gcc --version`, flags) to `results/<exp>/machine.txt`. `tools/plot.py` produces the figures (median with min/max bars).
  *Done when:* a smoke run of E1 at small size produces JSON and a plot.

## Phase 7 — Evaluation (§10)

- [x] **T7.1 Dataset fetch.** `tools/fetch_datasets.sh` downloads full Loghub OpenSSH, Apache, Linux, BGL (Zenodo links in the Loghub README) and one AIT-LDS v2.0 scenario into `data/` (gitignored); converts AIT JSON labels into the ground-truth format used by `label_eval.py`. Print sizes and checksums.
- [ ] **T7.2 Labeling helper.** `tools/sample_for_labeling.py` draws a stratified sample (by predicted category + unmatched) into a CSV with empty label columns for two annotators. *Humans do the labeling* — Claude Code only builds the tool.
- [ ] **T7.3 `tools/label_eval.py`.** Multi-label P/R/F1 per category, confusion matrix, Cohen's κ between annotators, reliability diagram + ECE (§5.4).
  *Done when:* runs on generator ground truth and produces numbers + plots.
- [ ] **T7.4 Run must-have experiments.** E1, E2, E3, E6, E7 from §10.3 via `run_bench.sh`; results and plots in `results/`. Summarize in `results/SUMMARY.md` with exact commands used — numbers only from the saved JSON.

## Phase 8 — Stretch (only after Phase 7 is done; ask before starting)

- [ ] T8.1 Hopcroft minimization (`minimize.c`, §3.2 step 5) + E11
- [ ] T8.2 Byte equivalence classes to shrink transition tables
- [ ] T8.3 Calibration study E8 and base-weight tuning
- [ ] T8.4 Latency-under-load E5 with paced generator
- [ ] T8.5 Least-loaded dispatch study E9
- [ ] T8.6 ReDoS demo E10 (PCRE2 in `tools/` only, never in `src/`)
- [ ] T8.7 `--ordered` reorder buffer
- [ ] T8.8 Process-based worker variant with shared-memory rings
- [ ] T8.9 Aho-Corasick literal prefilter in front of the DFA
- [ ] T8.10 SIGHUP rule reload
- [ ] T8.11 Live dashboard (ncurses stats view)
