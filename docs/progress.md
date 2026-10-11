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
