# CLAUDE.md — Real-Time Parallel Log / Event Triage Engine

Compiler Design course project (VIT, Team 1). A C11 engine that classifies streaming log lines into AUTH_FAILURE, SECURITY_THREAT, ERROR, PERF_WARNING and PII using a home-built regex → NFA → DFA compiler and a data-parallel pthread pipeline. It extends the professor's base paper (parallel regex text classifier).

## Read first

1. `docs/project_design.md` — **the spec**. Architecture, data structures, rule format, compiler algorithms, confidence model, CLI, evaluation. Follow it; section numbers are referenced in the plan.
2. `docs/implementation_plan.md` — ordered tasks with "done when" checks. Work on the first unchecked task unless told otherwise.
3. `docs/progress.md` — what previous sessions did, what's broken, what's next. Read the latest entry before starting.

Background only (read when relevant, not by default): `docs/base_paper.md` (what we're extending), `docs/review1_submission.md` (our Review 1). Don't read `docs/research/` unless asked — it's input for the written report, and everything needed for code is already in the spec.

## Build and test

```
make                  # bin/triage and tools
make test             # unit tests (tests/test_*.c)
make difftest         # DFA engine vs POSIX regexec on tests/corpus — must report 0 mismatches
make e2e              # end-to-end: triage on tests/corpus/synthetic_small.log vs tests/expected/
make SAN=address      # ASan/UBSan build;  make SAN=thread  for TSan
make clean
```
(Targets are created in Phase 0 and later phases; if one doesn't exist yet, its task isn't done.)

## Hard constraints

- C11 (`-std=c11 -D_GNU_SOURCE`), gcc, Linux, POSIX + pthreads. Flags: `-O2 -Wall -Wextra -Wpedantic -pthread`. Code must compile warning-free.
- **No external libraries in the core** (no PCRE2, RE2, Hyperscan). glibc `regex.h` is allowed — it is the `posix` engine and the difftest oracle. Python is allowed only in `tools/` for evaluation and plotting.
- Rules use only the regex subset in spec §3.1 (POSIX ERE compatible). The parser must **reject** anything outside it (`\d`, `\b`, backreferences, lookaround, lazy quantifiers) with a clear error — never silently approximate.
- One binary, three engines: `--engine dfa|posix|fork` (spec §2.1). Don't remove `posix` or `fork`; they are experiment baselines.
- DFA tables are read-only after build and shared across workers without locks. Each worker using the POSIX engine compiles its **own** `regex_t` set.
- Never fabricate benchmark numbers or results in any doc. Numbers only come from runs saved under `results/`.

## Working rules

- A task is done only when its "done when" check passes **and** `make test` passes. For any change touching `src/regex_*`, `nfa.c`, `dfa.c`, `minimize.c` or `rules/`, `make difftest` must also show 0 mismatches.
- Concurrency code (`spsc.c`, `worker.c`, `dispatch.c`, `aggregate.c`, `pipeline.c`) must run clean under `make SAN=thread` before being marked done.
- Tick the checkbox in `docs/implementation_plan.md` when a task is done. At the end of every session, append an entry to `docs/progress.md` (template inside).
- Commit after each completed task: `git commit -m "T2.3: subset construction with accept masks"`.
- Ask before changing architecture, the data structures in spec §2.4, the rule file format, the CLI, or anything marked out of scope in spec §11. Small internal helpers don't need approval.
- Don't edit `docs/project_design.md`, `docs/base_paper.md` or `docs/review1_submission.md`. If the spec looks wrong or ambiguous, note it under "Open questions" in `progress.md` and ask.
- Compiler code (`regex_parse.c`, `nfa.c`, `dfa.c`, `minimize.c`) gets comments explaining the algorithm step being implemented (Thompson case, ε-closure, subset construction, partition refinement), citing the spec section. These files will be walked through in the viva and the report.
- Keep functions small and modules separated as in spec §9; every `src/x.c` has a matching `src/x.h`. No global mutable state except the parsed config.

## Layout

```
src/        engine + pipeline (module list in spec §9)
tools/      gen.c, difftest.c, run_bench.sh, label_eval.py, plot.py, fetch_*.sh
rules/      default.rules (26 rules, spec §4.2) and bench_*.rules
tests/      test_*.c, corpus/ (small sample logs), expected/
data/       full datasets — gitignored, fetched by scripts
results/    benchmark JSON and plots
docs/       spec, plan, progress log, background
```
