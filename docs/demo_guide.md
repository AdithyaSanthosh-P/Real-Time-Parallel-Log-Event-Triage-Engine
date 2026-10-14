# Review 2 demo guide

Run `make demo` (or `tools/demo.sh`) from the repository root. It pauses after each step; press
Enter to go on. `tools/demo.sh --no-pause` runs straight through, and `tools/demo.sh correlate`
runs a single step (steps: `compiler classify pii correlate parallel basepaper`; `build` is optional). Outputs go
to `out/demo/`. Takes about 20 seconds without pauses (about 40 lines of output). Do a dry run on the demo machine first.

Optional: `sudo apt install graphviz` lets step 2 render the DFA picture (`out/demo/tiny.png`);
without it the step writes the `.dot` file only.

## What to say at each step

Tests and sanitizer runs (not part of the demo): `make test`, `make e2e`, `make compare`, all also under AddressSanitizer and ThreadSanitizer.

**1. Compiler — the compiler-design core.** The 26 rules are parsed by a recursive-descent
parser into an AST, turned into one Thompson NFA, then subset construction builds a combined
DFA: every rule is matched in a single pass over the line, so the cost does not grow with the
number of rules. The first line shows NFA states, DFA states, groups (the DFA is split when it
would exceed the state cap) and build time. A `\d` is rejected with its offset: only the
regular, POSIX-ERE subset is accepted, which is what guarantees linear-time matching. Every DFA
answer is checked against glibc `regexec` (`make difftest`: 0 mismatches on the corpus and
100,000 random strings). Code to show: `src/regex_parse.c`, `src/nfa.c`, `src/dfa.c`.

**2. Classification.** A line can have several labels (`SECURITY_THREAT,ERROR` on #50).
Confidence = noisy-OR of rule scores (base, +0.25 if a checksum validator passed, +0.15 for a
context keyword); a label needs confidence ≥ τ = 0.5. The two look-alikes get no label:
`4111 1111 1111 1112` fails Luhn, and `523456789012` fails the Aadhaar Verhoeff check —
regex alone would flag both.

**3. PII masking.** Cards, Aadhaar, PAN, secrets, e-mail and phone numbers are masked before
anything is written, in every output file; PII lines are copied to `pii_quarantine.jsonl`.
`make e2e` greps all output files for every PII value of the corpus and fails if one survives.

**4. Correlator.** Sliding windows on log time, per source IP: 5 failures within 60 s →
brute force; 5 distinct users within 300 s → password spray; a successful login during a brute
force → CRITICAL. On the real Loghub OpenSSH sample it finds 34 brute-force and 5 spray
escalations. Each is checked line by line against an independent Python implementation.

**5. Parallel pipeline.** Reader → N worker threads (lock-free single-producer/single-consumer
rings, cache-line-separated indices, release/acquire ordering) → aggregator → correlator →
router. The DFA tables are read-only and shared by all workers without locks. `-w 1` and
`-w 8` produce byte-identical output. The status line updates once per second while lines
stream in from a pipe.

**6. Base paper.** We rebuilt the professor's base paper faithfully (one `fork` per pattern
per input, `regcomp` in every child) and measured each claim on this machine. The per-pattern
`fork` design is slower than a plain sequential loop; splitting the *inputs* across threads
scales, and our combined DFA is the fastest variant. All numbers come from the saved runs in
`results/basepaper/` (`SUMMARY.md`, `figures/*.svg`).

## Likely questions

- *Why a DFA and not just regexec in a loop?* One pass per line for all rules, no backtracking,
  tables shared across threads without locks. The `posix` engine is kept as the baseline.
- *How do you know the DFA is correct?* Differential testing against glibc on every corpus line
  and 100,000 generated strings, plus an NFA simulator as a second oracle.
- *Throughput numbers for your engine?* Not yet: the benchmark mode (`--bench`) and runner are
  the next tasks (Phase 6). Only numbers from saved runs are reported.
- *What is left?* Benchmark tooling (Phase 6), full datasets, hand-labeled evaluation with
  precision/recall/F1 and Cohen's κ (Phase 7).
