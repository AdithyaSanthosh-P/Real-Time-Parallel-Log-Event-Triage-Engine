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

- (none yet)

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
