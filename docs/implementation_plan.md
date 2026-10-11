# Implementation Plan

Spec: `docs/project_design.md` (§ numbers below refer to it). Work top to bottom; each task depends on the ones before it unless noted. Tick `[x]` when the **Done when** check passes and `make test` is green. Commit per task with the task ID in the message.

The order is deliberate: get a correct, single-threaded, end-to-end pipeline on the POSIX engine first (Phase 1), then replace the engine with our compiler (Phase 2), then add classification logic (Phase 3), and only then parallelize (Phase 4). Every later phase is checked against the output of the earlier, simpler version.

---

## Phase 0 — Setup

- [x] **T0.1 Repo skeleton and build.** `git init`; directories per spec §9; `.gitignore` (`bin/`, `build/`, `data/`, `out/`, `results/*.json` optional, `*.o`). Makefile with targets `all`, `test`, `difftest`, `e2e`, `clean`, and `SAN=address|thread` variants (adds `-fsanitize=...  -g -O1`). Minimal `src/main.c` that parses `--help`. Tiny test runner (`tests/test_main.h` with `CHECK()` macros; no framework).
  *Done when:* `make && make test && make SAN=address test` succeed with zero warnings; `bin/triage --help` prints the CLI from spec §8.
- [x] **T0.2 Test corpus.** `tools/fetch_samples.sh` downloads Loghub's 2k-line samples (OpenSSH, Apache, Linux, BGL, HDFS — check exact paths in github.com/logpai/loghub) into `tests/corpus/`. Hand-write `tests/corpus/synthetic_small.log`: at least one positive line per rule in `rules/default.rules` (use the example lines from spec §4.2), plus ≥ 20 negative/near-miss lines (e.g. invalid Aadhaar checksum, 12-digit order ID, the word "errors" in normal text, `select` in a benign URL). Commit the corpus (small files only).
  *Done when:* corpus files exist; each rule id appears in a comment next to its positive line in `synthetic_small.log`.

## Phase 1 — Correct single-threaded baseline (POSIX engine)

- [ ] **T1.1 Rule loader** (`rules.c/.h`, §4.1). Parse the TSV format into `Rule` structs (id, category, severity, base, icase, anchored flags, validator, extract list, context keywords, regex string). Reject: wrong column count, unknown category/severity/validator, > 64 rules, duplicate ids — with file:line in the error.
  *Done when:* `tests/test_rules.c` loads `rules/default.rules` as 26 rules with correct fields, and each malformed fixture in `tests/fixtures/bad_*.rules` fails with the expected message.
- [ ] **T1.2 Normalizer** (`normalize.c/.h`, §4.2 notes). Produce the match copy: decode `%xx` (invalid escapes left as-is), `+`→space, strip control chars except `\t`. No lowercasing. Output length ≤ input length.
  *Done when:* `tests/test_normalize.c` covers `%27`, `%2e%2e%2f`, `%zz`, `%`, trailing `%4`, `+`, mixed control chars.
- [ ] **T1.3 Reader** (`reader.c/.h`). Read from file or stdin in large chunks; split on `\n`; handle `\r\n`, final line without newline, empty lines, and lines longer than the chunk size. Assign `seq_id`. Parse timestamp per §6 (syslog `Mon DD HH:MM:SS`, ISO-8601), else ingest time. Fill `Batch` (§2.4) with raw + normalized copies.
  *Done when:* `tests/test_reader.c` passes on crafted inputs (CRLF, 1 MB single line, no trailing newline, empty file) and line counts on every corpus file equal `wc -l` (+1 if no trailing newline).
- [ ] **T1.4 POSIX engine** (`engine_posix.c/.h`, §2.1). An `EnginePosix` instance holds its own compiled `regex_t[]` (`REG_EXTENDED|REG_NOSUB`, plus `REG_ICASE` per rule). `uint64_t posix_match(EnginePosix*, line, len)` returns the rule mask. Lines are not NUL-terminated in the batch — copy to a per-instance scratch buffer or use `REG_STARTEND`.
  *Done when:* on `synthetic_small.log`, every positive line's mask includes its rule (test asserts it from the comments added in T0.2).
- [ ] **T1.5 Aggregator v0** (`aggregate.c/.h`). Rule mask → `label_mask` (any matched rule's category), primary = highest severity with tie order from §5.3, line severity. No confidence, validators or context yet (set confidence = 1.0).
  *Done when:* unit test with hand-built masks covers ties and multi-label cases.
- [ ] **T1.6 Router v0** (`route.c/.h`, §7). JSONL writer with correct JSON string escaping; sinks `out/alerts.jsonl` (severity ≥ HIGH), `out/store.jsonl` (all). PII masking comes later (T3.5).
  *Done when:* output of a test run parses with `python3 -m json.tool` line by line (script in `tools/check_jsonl.py`).
- [ ] **T1.7 Single-thread pipeline + CLI** (`main.c`, `cli.c`). Wire reader → posix engine → aggregator → router in one thread. Implement CLI parsing for all flags in §8 (unimplemented ones print "not yet implemented" and exit 2).
  *Done when:* `make e2e` runs `bin/triage --engine posix -w 1 tests/corpus/synthetic_small.log` and compares `(seq, primary, labels)` against `tests/expected/synthetic_small.expected` (create this file by hand from the corpus comments, then review it). Diff is empty.

## Phase 2 — Regex compiler and DFA engine (core contribution)

- [ ] **T2.1 Regex lexer + recursive-descent parser → AST** (`regex_lex.c`, `regex_parse.c`, `ast.c`, §3.1–3.2). Grammar exactly as §3.1. Charsets as 256-bit sets; `.` = all bytes except `\n`; icase folds charsets; `^`/`$` only at ends (recorded as flags by the loader). Reject unsupported syntax with position: `\d`, `\s`, `\w`, `\b`, `\1`, `(?`, `*?`, `+?`, `??`, `{,n}`, `^`/`$` in the middle, unbalanced parens/brackets. Add `ast_print()` for debugging.
  *Done when:* `tests/test_parse.c` has ≥ 40 cases (valid → expected AST print; invalid → expected error position), and all 26 default rules parse.
- [ ] **T2.2 Thompson NFA** (`nfa.c/.h`, §3.2 step 2–3). One construction per AST node type; `{n,m}` expanded; accept states tagged with rule id; combined NFA with a fresh start state. Include `nfa_simulate(nfa, line, len) → mask` (set-of-states simulation with the same unanchored/anchored semantics as §3.2 step 4) — this is a second oracle and the reference for the DFA.
  *Done when:* `tests/test_nfa.c`: for each default rule and ~200 strings (corpus lines + synthetic edge strings), `nfa_simulate` mask == per-rule `regexec` mask.
- [ ] **T2.3 Subset construction → combined DFA** (`dfa.c/.h`, §3.2 step 4). ε-closure with an explicit stack; NFA-state sets as sorted int arrays (or bitsets) hashed into a state table; `U` set added to every transition for unanchored rules; `accept_mask` and `accept_end_mask`; transition table `int32_t next[n][256]`. Enforce `--max-states`; on overflow, split rules into groups (halve recursively) and build one DFA per group. Report state count and build time (`--verbose`).
  *Done when:* DFA mask == `nfa_simulate` mask on the same test set as T2.2; building the 26 default rules succeeds under the default cap.
- [ ] **T2.4 DFA engine** (`engine_dfa.c/.h`, §3.3). Match loop exactly as §3.3, iterating over all groups. Hook into `--engine dfa`.
  *Done when:* `make e2e` passes with `--engine dfa` (same expected file as T1.7).
- [ ] **T2.5 Differential test tool** (`tools/difftest.c`, §3.5). For every line in `tests/corpus/*.log` (and optionally a `--random N` mode generating strings biased toward rule fragments), compare DFA mask vs per-rule `regexec` mask. Print rule id, line number and line on mismatch; exit non-zero if any.
  *Done when:* `make difftest` reports 0 mismatches on all corpus files and `--random 100000`.
- [ ] **T2.6 DOT dump** (`dot.c`, §3.2 step 7). `--dump-dot FILE` writes the NFA and DFA (and minimized DFA later) for the loaded rules; collapse byte ranges into labels like `[a-z]` so diagrams are readable.
  *Done when:* `dot -Tpng` renders the DFA for a 2-rule file in `tests/fixtures/tiny.rules` without errors.

## Phase 3 — Classification logic

- [ ] **T3.1 Validators** (`validators.c/.h`, §4.3). `luhn_ok`, `verhoeff_ok`, `pan_ok`, plus a helper that strips spaces/hyphens from a span. Use the tables in §4.3 exactly.
  *Done when:* `tests/test_validators.c` passes the known values in §4.3 plus ≥ 5 more valid/invalid cases each.
- [ ] **T3.2 Spans and extraction** (`extract.c/.h`, §3.4). For matched rules that need a validator or extraction, run that rule's `regexec` with `pmatch` on the normalized line to get spans (iterate for multiple occurrences). Extract IPv4 (dotted quad → `uint32_t`) and username tokens from the span for rules flagged `ip`/`user`. Each worker owns its own capture `regex_t` set.
  *Done when:* unit tests extract the right IP/user from every AUTH example line in the corpus, and spans for PII lines contain exactly the number (plus boundary chars).
- [ ] **T3.3 Context keywords.** Case-insensitive substring check per rule's context list → `ctx_mask`.
  *Done when:* unit test.
- [ ] **T3.4 Confidence + multi-label** (`aggregate.c`, §5.1–5.3). Discard rules whose validator fails on all spans; per-rule score; noisy-OR per category; `--tau`; correlator escalations hook (stub until T5.1); subsumption only affects the explanation list.
  *Done when:* `tests/test_aggregate.c` checks worked examples: card line gives PII_CARD kept + PII_AADHAAR discarded (spec §4.2 overlap note); SQLi + email line gives labels SEC|PII, primary SEC; a validator-failing Aadhaar yields no PII label. `tests/expected/synthetic_small.expected` updated and `make e2e` passes for both engines.
- [ ] **T3.5 Masking + PII sink** (`route.c`, §7). Mask Aadhaar/card/PAN/secret spans in `pii_quarantine.jsonl` and in `store.jsonl` for PII lines; add `confidence`, `rules`, `flags` fields.
  *Done when:* e2e check script asserts no unmasked PII value from the corpus appears anywhere under `out/`.

## Phase 4 — Parallel runtime

- [ ] **T4.1 SPSC ring** (`spsc.c/.h`, §2.3). Power-of-two capacity, cache-line-separated head/tail, release/acquire ordering; `try_push`, `try_pop`, blocking variants with short spin then `nanosleep`/futex-free backoff (or condvar fallback behind a compile flag).
  *Done when:* `tests/test_spsc.c` stress test (10M items, producer/consumer threads, checks order and checksum) passes under `make SAN=thread`.
- [ ] **T4.2 Batch pool.** Fixed pool of preallocated `Batch` and result-batch objects recycled through a return ring (router → reader), so steady state does no `malloc`. Pool size = workers × queue depth + slack.
  *Done when:* valgrind/ASan run on a 100k-line file shows no leaks and allocation count does not grow with input size (count allocations with a debug counter).
- [ ] **T4.3 Workers** (`worker.c/.h`, §2.2). N pthreads, optional pinning (`pthread_setaffinity_np`, `--pin`/`--no-pin`), each with its own engine instance (DFA: shared tables; POSIX: own `regex_t` set) and own capture regexes. Produces `MatchResult` batches.
- [ ] **T4.4 Dispatcher** (`dispatch.c/.h`, §2.3). `rr` and `least`; batch-delay flush of partial batches; blocking backpressure; `--drop` with a dropped counter.
- [ ] **T4.5 Aggregator + router threads, stats** (`pipeline.c`, `stats.c`). Aggregator consumes result rings from all workers; router writes sinks; 1 Hz stderr stats line (lines/s, per-worker queue depth, per-category counts, dropped).
- [ ] **T4.6 Shutdown and equivalence.** EOF propagates as a sentinel batch; all threads join; sinks flushed.
  *Done when (covers T4.3–T4.6):* for every corpus file, engines `posix` and `dfa`, and `-w 1,2,4,8`, the set of `(seq, primary, labels, confidence)` records equals the single-thread result from Phase 3 (`tools/compare_runs.sh`, sorts by seq). Clean under `make SAN=thread` and `make SAN=address`.

## Phase 5 — Correlator

- [ ] **T5.1 Correlator** (`correlate.c/.h`, §6). Single thread between aggregator and router. Open-addressing hash maps keyed by source IP; ring of timestamps per key; distinct-user tracking for spraying; BRUTE_FORCE, PASSWORD_SPRAY, FAIL_THEN_SUCCESS, ERROR_STORM with parameters from CLI/config (`maxretry`, `findtime`, etc.). Escalations add CAT_SEC with confidence 0.95 and a flag; emit each escalation once per window.
  *Done when:* `tests/test_correlate.c` replays crafted sequences (4 failures → nothing; 5 within window → one escalation; 5 spread over > window → nothing; burst then Accepted → CRITICAL). Running on the Loghub OpenSSH 2k sample produces escalations, and the e2e check confirms they appear in `alerts.jsonl`.

## Phase 6 — Baselines and benchmarking infrastructure

- [ ] **T6.1 Fork engine** (`engine_fork.c/.h`, §2.1). Reproduce the base paper: per line, `fork()` one child per rule; child runs `regcomp` + `regexec` and writes the result to a pipe; parent collects and `waitpid`s. Single-threaded only (`-w` ignored with a warning).
  *Done when:* masks equal `posix -w 1` on `synthetic_small.log`.
- [ ] **T6.2 `--bench` mode** (§8, §10.2). Preload the whole input into memory before timing; disable sinks; record per-line latency (ingest → aggregated) into a log-scale histogram; print one JSON object: engine, workers, batch, lines, seconds, lines/s, MB/s, p50/p90/p99/p99.9, DFA states, build time.
  *Done when:* output validates as JSON and line counts match the input.
- [ ] **T6.3 Synthetic generator** (`tools/gen.c`, §10.1). Templates for normal traffic per log type (sshd, Apache access/error, app logs); configurable injection rates per category; valid and invalid Luhn/Verhoeff numbers; attack payloads from a payload file; optional rate pacing (`clock_nanosleep`, `CLOCK_MONOTONIC`) to stdout. Writes a ground-truth file (`seq`, true labels) alongside.
  *Done when:* generating 1M lines with a fixed seed is reproducible (same hash) and ground-truth counts match configured rates within 1%.
- [ ] **T6.4 Bench rule sets.** `rules/bench_5.rules` … `bench_64.rules` (subsets of the defaults plus realistic extra rules — more keywords, more PII/attack variants — all in the §3.1 subset) for the rule-count experiment.
  *Done when:* all pass `make difftest` and build under the state cap.
- [ ] **T6.5 Bench runner** (`tools/run_bench.sh`). Runs a named experiment matrix ≥ 5 times per point, saves raw JSON to `results/<exp>/`, records machine info (`lscpu`, `uname -a`, `gcc --version`, flags) to `results/<exp>/machine.txt`. `tools/plot.py` produces the figures (median with min/max bars).
  *Done when:* a smoke run of E1 at small size produces JSON and a plot.

## Phase 7 — Evaluation (§10)

- [ ] **T7.1 Dataset fetch.** `tools/fetch_datasets.sh` downloads full Loghub OpenSSH, Apache, Linux, BGL (Zenodo links in the Loghub README) and one AIT-LDS v2.0 scenario into `data/` (gitignored); converts AIT JSON labels into the ground-truth format used by `label_eval.py`. Print sizes and checksums.
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
