# Results summary

Every number below is computed by `tools/make_results_summary.py` from the raw runs saved under `results/`
(benchmark runs: `results/e1`…`e4/raw/*.json`; accuracy runs: `results/e6`, `results/e7`). Throughput is
`triage --bench` lines per second (input preloaded, sinks to /dev/null, rule compilation excluded); every point
is run several times, interleaved, and reported as the median with the min–max range. Figures: `results/figures/`.
The exact command of every point is in `results/<exp>/commands.txt`; the machine in `results/machine.txt`.

Machine: 13th Gen Intel(R) Core(TM) i7-13650HX; full, runs per point: 5, physical cores: 14; gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0; commit db79a8576ee6; kernel `Linux zerotrace 7.0.0-38-generic #38-Ubuntu SMP PREEMPT_DYNA…`.

## E1 Architecture comparison

| config | engine | workers | lines | lines/s (median) | min–max | runs | vs C2 |
|---|---|---|---|---|---|---|---|
| c1-fork | fork | 1 | 2,000 | 358 | 343–362 | 5 | 0.00485× |
| c2-posix-w1 | posix | 1 | 1,000,000 | 73,844 | 73,613–74,315 | 5 | 1× |
| c3-posix-w14 | posix | 14 | 1,000,000 | 521,485 | 494,869–526,899 | 5 | 7.06× |
| c4-dfa-w14 | dfa | 14 | 1,000,000 | 884,119 | 873,848–897,005 | 5 | 12× |

Hypothesis C1 ≪ C2 < C3 < C4: **holds** (C1 358, C2 73,844, C3 521,485, C4 884,119 lines/s). The fork design (C1) is 206× slower than the sequential POSIX engine (C2) and C4 (dfa, same workers as C3) is 2467× faster than C1 and 1.70× faster than C3.

Note: C1 is run on fewer lines than the others (it processes about 358 lines/s); the throughput per line is what is compared.

## E2 Scaling with workers

Speed-up S = T(1)/T(p) of the whole pipeline (reader, p workers, aggregator, correlator, router), efficiency S/p.

| workers | posix lines/s | posix S | posix S/p | dfa lines/s | dfa S | dfa S/p |
|---|---|---|---|---|---|---|
| 1 | 74,126 | 1.00 | 1.00 | 971,394 | 1.00 | 1.00 |
| 2 | 143,578 | 1.94 | 0.97 | 944,128 | 0.97 | 0.49 |
| 4 | 243,226 | 3.28 | 0.82 | 850,069 | 0.88 | 0.22 |
| 6 | 335,315 | 4.52 | 0.75 | 892,285 | 0.92 | 0.15 |
| 8 | 341,089 | 4.60 | 0.58 | 863,661 | 0.89 | 0.11 |
| 10 | 406,776 | 5.49 | 0.55 | 848,410 | 0.87 | 0.09 |
| 12 | 463,006 | 6.25 | 0.52 | 878,398 | 0.90 | 0.08 |
| 14 | 507,421 | 6.85 | 0.49 | 897,982 | 0.92 | 0.07 |

The POSIX engine peaks at 507,421 lines/s with 14 workers (S = 6.85). The DFA engine peaks at 971,394 lines/s with 1 worker (S = 1.00); its single-worker run is already at 971,394 lines/s, which is 1.91× the best POSIX result.
Hypothesis "near-linear until the serial stages bound it": the POSIX engine keeps efficiency ≥ 0.8 up to 4 workers; the DFA engine is bound by the serial stages from the start (S = 0.97 at 2 workers).

## E3 Rule-count sensitivity (headline)

| rules | posix lines/s | dfa lines/s | dfa / posix | DFA groups | DFA states | DFA build (ms) |
|---|---|---|---|---|---|---|
| 5 | 259,850 | 886,235 | 3.4× | 1 | 703 | 4 |
| 10 | 154,796 | 884,769 | 5.7× | 1 | 2,554 | 24 |
| 20 | 81,204 | 880,042 | 10.8× | 2 | 4,954 | 487 |
| 32 | 61,592 | 863,779 | 14.0× | 2 | 20,061 | 816 |
| 64 | 36,829 | 329,323 | 8.9× | 7 | 26,517 | 1,761 |

From 5 to 64 rules the POSIX engine falls to 14 % of its throughput (259,850 → 36,829 lines/s) and the DFA engine to 37 % (886,235 → 329,323). Hypothesis "POSIX falls about linearly with the rule count, DFA stays roughly flat": POSIX: holds (a perfectly linear fall would be to 8 %); DFA: **does not hold**. Up to 32 rules (at most 2 DFA groups) the DFA engine keeps 97 % of its 5-rule throughput; the drop comes with the 7-group set. The DFA engine makes one pass per DFA group, and the number of groups grows with the rule count (table above), so its cost rises in steps when a rule set needs another group; the DFA engine stays 8.9× faster than POSIX at 64 rules.

## E4 Batch size (dfa, 4 workers)

| batch (-b) | lines/s | min–max | latency p50 (µs) | latency p99 (µs) |
|---|---|---|---|---|
| 1 | 191,318 | 182,986–222,536 | 23,727 | 61,539 |
| 4 | 563,614 | 558,279–588,283 | 8,389 | 25,874 |
| 16 | 737,277 | 714,951–757,598 | 6,468 | 19,952 |
| 64 | 765,928 | 756,542–775,356 | 5,932 | 16,777 |
| 256 | 787,649 | 769,922–801,894 | 6,468 | 15,385 |
| 1024 | 806,878 | 798,939–812,975 | 7,054 | 14,108 |

Latency here is ingest → aggregated for a preloaded input that the reader pushes in as fast as the queues accept it, so it measures queueing behind the bounded rings, not a paced stream (that is E5, not run). Throughput goes from 191,318 lines/s at -b 1 to 806,878 at its best (-b 1024); the largest batch the build accepts is 1,024 lines.

## E6 Accuracy of the labels

**Generator ground truth** (200,000 lines, `bin/gen --seed 11`). The generator was written together with the rule set, so this checks that the engine and the generator's by-construction labels agree (and that the checksum look-alikes are rejected); it is not an estimate of accuracy on real logs.

| category | precision | recall | F1 | support |
|---|---|---|---|---|
| AUTH_FAILURE | 1.0000 | 1.0000 | 1.0000 | 10,000 |
| SECURITY_THREAT | 1.0000 | 1.0000 | 1.0000 | 6,000 |
| ERROR | 1.0000 | 1.0000 | 1.0000 | 10,000 |
| PERF_WARNING | 1.0000 | 1.0000 | 1.0000 | 6,000 |
| PII | 1.0000 | 1.0000 | 1.0000 | 6,000 |

Exact label-set match 1.0000. Calibration of the primary label: ECE = 0.373 (reliability diagram `results/e6/synthetic/reliability.svg`). Confidences are the noisy-OR of the base scores in `rules/default.rules` (0.35 weak, 0.6 strong, +0.25 validated, +0.15 keyword), so a correct label with confidence 0.6 shows up as under-confident when its observed precision is 1.0: bin 0.6–0.7: mean confidence 0.60, observed precision 1.00, bin 0.7–0.8: mean confidence 0.75, observed precision 1.00, bin 0.9–1.0: mean confidence 0.99, observed precision 1.00.

**AIT-LDS v2.0 (scenario russellmitchell), labelled web and auth logs.** AIT labels every request of the attacker's scan and exploitation steps; a line counts as detected if the engine labels it SECURITY_THREAT or AUTH_FAILURE.

| log | lines | attack lines | detected (recall) | false alarms | precision |
|---|---|---|---|---|---|
| auth.log | 272 | 8 | 1 (0.1250) | 0 | 1.0000 |
| intranet.smith.russellmitchell.com-access.log.2 | 8,530 | 7,695 | 6 (0.0008) | 0 | 1.0000 |
| intranet.smith.russellmitchell.com-error.log.2 | 36 | 36 | 0 (0.0000) | 0 | n/a |

The rule set has no rule for most of these attacker requests (WordPress scanning and an upload exploit with ordinary-looking URLs), so recall is low; the few lines it flags are correct. The rules were not changed after seeing these numbers (no tuning on the evaluation data).

**Full Loghub logs** (no ground truth; what the engine labels):

| log | lines | AUTH | SEC | ERR | PERF | PII | none | brute-force / spray / storm escalations |
|---|---|---|---|---|---|---|---|---|
| OpenSSH | 655,147 | 353,185 | 13,671 | 184 | 0 | 0 | 288,107 | 13412 / 284 / 0 |
| Apache | 56,482 | 0 | 553 | 0 | 0 | 0 | 55,929 | 0 / 0 / 0 |
| Linux | 25,567 | 0 | 2 | 0 | 0 | 40 | 25,525 | 0 / 0 / 0 |
| BGL | 4,747,963 | 0 | 892 | 985,785 | 0 | 1,175 | 3,760,111 | 0 / 0 / 1 |
| HDFS_2k | 2,000 | 0 | 0 | 0 | 0 | 1 | 1,999 | 0 / 0 / 0 |

**AI-annotator check on 100 real log lines** (not human ground truth). A stratified sample of 100 lines from the full OpenSSH, Apache and Linux logs (20 per stratum: AUTH, SEC, ERROR, PII, none) was labelled by Claude, the model that also wrote the rules, from the log lines and the written criteria only (`docs/llm_labelling_instructions.md`), without seeing the engine's answers. Strata are weighted back to the full logs. Details and judgement calls: `results/e6/ai_100/README.md`.

| category | precision | recall | F1 |
|---|---|---|---|
| AUTH_FAILURE | 1.0000 | 0.6882 | 0.8153 |
| SECURITY_THREAT | 0.4000 | 0.0251 | 0.0473 |
| ERROR | 1.0000 | 0.0017 | 0.0033 |
| PERF_WARNING | n/a | n/a | n/a |
| PII | 0.8000 | 1.0000 | 0.8889 |

Exact label-set match 0.588; ECE of the primary label 0.406. Reading: failed-login precision is perfect but about a third of failed-login lines are missed (for example "Auth fail" disconnects); the ERROR recall is near zero because the ERROR rules look for the uppercase level word and miss sshd `error:` lines; PII precision is below 1 because kernel boot lines with an all-zero memory address pass the card checksum; security-threat numbers depend on the labelling criteria (repeated failures stated in the line count as threats). With 100 lines the weighted figures are rough.

Human-labelled precision and recall on a stratified sample of real log lines need two people labelling independently (`tools/sample_for_labeling.py`, then `tools/label_eval.py --annot`); that has not been done, so no human-labelled number is reported. The AI-annotator check above is the stand-in until it is, and a second LLM annotator can be added with `docs/llm_labelling_instructions.md`.

## E7 Validator gain

PII precision and recall on the generator truth (which contains Luhn- and Verhoeff-failing look-alikes), with the default rules and with `rules/novalidators.rules` (validators off, validator rules scored 0.6 so a pattern-only match is a label):

| | PII precision | PII recall | false PII labels | PII true positives |
|---|---|---|---|---|
| validators off | 0.6000 | 1.0000 | 4,000 | 6,000 |
| validators on | 1.0000 | 1.0000 | 0 | 6,000 |

Hypothesis "validators raise precision": **holds** (0.6000 → 1.0000) at unchanged recall (1.0000 → 1.0000).

Lines labelled PII on the real logs (no truth; every line that drops out was a pattern match the checksum rejected):

| log | lines | validators off | validators on |
|---|---|---|---|
| OpenSSH | 655,147 | 163 | 0 |
| Apache | 56,482 | 0 | 0 |
| Linux | 25,567 | 105 | 40 |
| BGL | 4,747,963 | 12,279 | 1,175 |
| HDFS_2k | 2,000 | 119 | 1 |

