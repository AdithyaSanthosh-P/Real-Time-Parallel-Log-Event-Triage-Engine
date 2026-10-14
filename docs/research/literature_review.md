# Literature Review — Parallel Regex Classification of Log Streams (merged)

*Compiler Design project, Team 1. Merges two independent searches: Claude's (Oct 12, 2026) and Gemini's "Parallel_Regex_Research_Review" (Oct 2026). Every entry Gemini found that Claude's search did not was re-checked; Section 6 lists what was dropped and why. Use this file for the report's Related Work section and for the base-paper reproduction study (Phase R).*

**Legend:** ✔ = venue and year confirmed against a publisher page, DBLP, or a citing paper during the merge. ◐ = canonical or plausible, details not re-checked — confirm the DOI once before final citation. "Found by": C = Claude, G = Gemini, C+G = both.

---

## 1. Journal articles

### 1A. Automata foundations (the compiler core)

| # | Reference | ✔/◐ | Found by | Contribution → how we use it |
|---|---|---|---|---|
| J1 | M. O. Rabin, D. Scott, "Finite automata and their decision problems," *IBM J. Research and Development*, 3(2):114–125, 1959. DOI 10.1147/rd.32.0114 | ✔ | C+G | Nondeterministic automata and the subset construction → cite for T2.3. |
| J2 | R. McNaughton, H. Yamada, "Regular expressions and state graphs for automata," *IRE Trans. Electronic Computers*, EC-9(1):39–47, 1960. DOI 10.1109/TEC.1960.5221603 | ✔ | C+G | Regex → automaton by position marking → background, alternative to Thompson. |
| J3 | K. Thompson, "Regular expression search algorithm," *Communications of the ACM*, 11(6):419–422, 1968. DOI 10.1145/363347.363387 | ✔ | C+G | Thompson NFA construction, linear-time simulation → cite for T2.2; basis of the ReDoS-immunity argument. |
| J4 | A. V. Aho, M. J. Corasick, "Efficient string matching: an aid to bibliographic search," *Communications of the ACM*, 18(6):333–340, 1975. DOI 10.1145/360825.360855 | ✔ | C+G | Multi-pattern literal matching in one pass → related work; optional prefilter (T8.9). |

### 1B. Multi-pattern regex matching and DFA engineering

| # | Reference | ✔/◐ | Found by | Contribution → how we use it |
|---|---|---|---|---|
| J5 | C. Xu, S. Chen, J. Su, S. M. Yiu, L. C. K. Hui, "A survey on regular expression matching for deep packet inspection: applications, algorithms, and hardware platforms," *IEEE Communications Surveys & Tutorials*, 18(4):2991–3029, 2016. DOI 10.1109/COMST.2016.2565660 | ✔ | C+G | Survey; names DFA state explosion as the central problem → main survey citation; justifies our state cap and rule grouping. |
| J6 | D. Ficara, A. Di Pietro, S. Giordano, G. Procissi, F. Vitucci, G. Antichi, "Differential encoding of DFAs for fast regular expression matching," *IEEE/ACM Trans. Networking*, 19(3):683–694, 2011. DOI 10.1109/TNET.2010.2089639 | ✔ | C+G | δFA: smaller tables, still one transition per byte → related work for table-size reduction (T8.2). |
| J7 | M. Becchi, P. Crowley, "A-DFA: A time- and space-efficient DFA compression algorithm for fast regular expression evaluation," *ACM Trans. Architecture and Code Optimization*, 10(1), Article 4, 2013. | ✔ | G (corrected) | DFA compression from the authors of the hybrid-FA work → related work on state explosion. *Gemini listed a different title under this journal; that title is actually a 2008 conference paper (see §6).* |
| J8 | A. X. Liu, E. Torng, "A de-compositional approach to regular expression matching for network security," *IEEE/ACM Trans. Networking*, 2019. DOI 10.1109/TNET.2019.2941920 | ✔ (venue/DOI) | C | Decomposes regexes to fight state explosion → contrast with our simpler halving strategy. |
| J9 | S. Dharmapurikar, J. Lockwood, "Fast and scalable pattern matching for network intrusion detection systems," *IEEE J. Selected Areas in Communications*, 24(10):1781–1792, 2006. | ✔ | C | Bloom-filter-assisted multi-pattern matching for NIDS → signature matching at scale. |

### 1C. Parallel pattern matching on multicore (most relevant to the base paper)

| # | Reference | ✔/◐ | Found by | Contribution → how we use it |
|---|---|---|---|---|
| J10 | D. Luchaup, R. Smith, C. Estan, S. Jha, "Speculative parallel pattern matching," *IEEE Trans. Information Forensics and Security*, 6(2):438–451, 2011. DOI 10.1109/TIFS.2011.2112647 | ✔ | C+G | Splits **one** input into chunks matched in parallel with speculation → contrast: logs are already split into lines, so we need no speculation. |
| J11 | Y. Ko, M. Jung, Y.-S. Han, B. Burgstaller, "A speculative parallel DFA membership test for multicore, SIMD and cloud computing environments," *Int. J. Parallel Programming*, 42:456–489, 2014. DOI 10.1007/s10766-013-0258-5 | ✔ | C | Names the two straightforward options: one DFA over many input streams, or several DFAs in parallel → **key citation for our design choice** (ours = first option, base paper = second). |
| J12 | Y.-H. E. Yang, V. K. Prasanna, "Robust and scalable string pattern matching for deep packet inspection on multicore processors," *IEEE Trans. Parallel and Distributed Systems*, 24:2283–2292, 2013. DOI 10.1109/TPDS.2012.217 | ✔ | C | Large automata suffer poor cache behaviour on multicore; reports scaling on 32 cores → supports memory-effects claim; motivates `perf stat` cache counters. |

### 1D. Log analysis, parsing, datasets, and stream processing

| # | Reference | ✔/◐ | Found by | Contribution → how we use it |
|---|---|---|---|---|
| J14 | S. He, P. He, Z. Chen, T. Yang, Y. Su, M. R. Lyu, "A survey on automated log analysis for reliability engineering," *ACM Computing Surveys*, 54(6), Article 130, 2021. DOI 10.1145/3460345 | ✔ | C+G | Survey of the full log-analysis pipeline; regex rules described as the traditional, manual approach → main log-side survey citation. |
| J15 | P. He, J. Zhu, S. He, J. Li, M. R. Lyu, "Towards automated log parsing for large-scale log data analysis," *IEEE Trans. Dependable and Secure Computing*, 2018 (POP). | ✔ (venue) | C | Parallel log parser on Spark → closest parallel-log work; differs (cluster template mining vs. single-machine per-line triage). |
| J16 | M. Landauer, F. Skopik, M. Wurzenberger, A. Rauber, "System log clustering approaches for cyber security applications: a survey," *Computers & Security*, 92, 101739, 2020. DOI 10.1016/j.cose.2020.101739 | ✔ | C+G | Groups security log analysis into filtering, signature extraction, outlier and anomaly detection → places our system in the signature/filtering group. *Check the author list on the DOI page.* |
| J17 | M. Landauer, F. Skopik, M. Frank, W. Hotwagner, M. Wurzenberger, A. Rauber, "Maintainable log datasets for evaluation of intrusion detection systems," *IEEE Trans. Dependable and Secure Computing*, 20(4):3466–3482, 2023 (AIT-LDS v2.0). | ✔ | C | Public log datasets with per-line attack labels → our SEC/AUTH ground truth. |
| J18 | M. Landauer, F. Skopik, M. Wurzenberger, W. Hotwagner, A. Rauber, "Have it your way: generating customized log datasets with a model-driven simulation testbed," *IEEE Trans. Reliability*, 70(1):402–415, 2021. | ✔ (venue) | G | How the AIT testbed injects and labels attacks → methodology reference for our synthetic generator (T6.3). |
| J19 | M. Dayarathna, S. Perera, "Recent advancements in event processing," *ACM Computing Surveys*, 51(2):33:1–33:36, 2018. DOI 10.1145/3170432 | ✔ | G | Survey of event/stream processing platforms (Storm, Spark, Flink) → context for positioning a single-machine streaming pipeline. |
| J20 | I. Pilán, P. Lison, L. Øvrelid, A. Papadopoulou, D. Sánchez, M. Batet, "The Text Anonymization Benchmark (TAB): a dedicated corpus and evaluation framework for text anonymization," *Computational Linguistics*, 48(4), 2022. | ✔ (venue) | G (substituted) | Journal reference for detecting and masking personal data in text → cite for the PII category and masking. *Replaces Gemini's Lison et al. entry, which is a conference paper (see §6).* |

### 1E. Check digits and parallel performance theory

| # | Reference | ✔/◐ | Found by | Contribution → how we use it |
|---|---|---|---|---|
| J21 | H. P. Gumm, "A new class of check-digit methods for arbitrary number systems," *IEEE Trans. Information Theory*, 31(1):102–105, 1985. | ✔ | G | Theory of group-based check digits (the family Verhoeff belongs to) → background for T3.1 validators. |
| J22 | J. L. Gustafson, "Reevaluating Amdahl's law," *Communications of the ACM*, 31(5):532–533, 1988. | ✔ | C | Scaled speedup → frames the base paper's "≥10 inputs" observation and our scaling experiment. |
| J23 | L. Lamport, "Specifying concurrent program modules," *ACM Trans. Programming Languages and Systems*, 5(2):190–222, 1983. | ✔ | C | Classic single-producer/single-consumer queue → cite for T4.1. |

Primary sources that are not journal articles (cite as such): J. Verhoeff, *Error Detecting Decimal Codes*, Mathematical Centre Tracts 29, Amsterdam, 1969 (monograph); H. P. Luhn, US Patent 2,950,048, 1960.

---

## 2. Conference papers and other sources (no journal equivalent found)

| # | Reference | Found by | Use |
|---|---|---|---|
| C1 | T. Mytkowicz, M. Musuvathi, W. Schulte, "Data-parallel finite-state machines," ASPLOS 2014. | C | Parallelizing one FSM run; contrast with line-level parallelism. |
| C2 | S. Kumar, S. Dharmapurikar, F. Yu, P. Crowley, J. Turner, "Algorithms to accelerate multiple regular expressions matching for deep packet inspection," SIGCOMM 2006. DOI 10.1145/1159913.1159952 | C+G | D²FA transition compression. |
| C3 | M. Becchi, P. Crowley, "A hybrid finite automaton for practical deep packet inspection," CoNEXT 2007. | C+G | Unanchored wildcards as a main cause of state explosion; DFA/NFA hybrid. |
| C4 | M. Becchi, P. Crowley, "Extending finite automata to efficiently match Perl-compatible regular expressions," CoNEXT 2008. DOI 10.1145/1544012.1544037 | G (corrected venue) | Handling counting and back-references in automata; explains what our subset deliberately excludes. |
| C5 | X. Wang et al., "Hyperscan: a fast multi-pattern regex matcher for modern CPUs," NSDI 2019. | C+G | Industrial engine; literal prefiltering. |
| C6 | J. C. Davis, C. A. Coghlan, F. Servant, D. Lee, "The impact of regular expression denial of service (ReDoS) in practice," ESEC/FSE 2018, pp. 246–256. | C | Super-linear regexes are common in practice; supports the DFA safety argument. |
| C7 | P. He, J. Zhu, Z. Zheng, M. R. Lyu, "Drain: an online log parsing approach with fixed depth tree," ICWS 2017. DOI 10.1109/ICWS.2017.13 | C+G | Streaming log template mining. |
| C8 | J. Zhu et al., "Loghub: a large collection of system log datasets for AI-driven log analytics," ISSRE 2023. | C | Loghub datasets. |
| C9 | P. Lison, I. Pilán, D. Sánchez, M. Batet, L. Øvrelid, "Anonymisation models for text data: state of the art, challenges and future directions," ACL-IJCNLP 2021, pp. 4188–4203. | G (corrected venue) | Text anonymisation survey; pairs with J20. |
| C10 | A. Baumann, J. Appavoo, O. Krieger, T. Roscoe, "A fork() in the road," HotOS 2019. | C | Cost and problems of `fork()`; context for the base paper's overhead. |
| C11 | T. Hoefler, R. Belli, "Scientific benchmarking of parallel computing systems: twelve ways to tell the masses when reporting performance results," SC 2015. DOI 10.1145/2807591.2807644 | C+G | Rules for reporting parallel performance → our benchmark methodology (§4). |
| C12 | A. Roodt et al., "Benchmarking regular expression matching," CIAA 2024. | C | Recent regex-engine benchmarking methodology. |
| C13 | R. Cox, "Regular expression matching can be simple and fast," swtch.com, 2007. | C+G | Thompson NFA vs backtracking; web article, not peer-reviewed. |

---

## 3. What the literature says about the base paper's claims

The professor asked us to support the base paper where possible. "Supported in principle" means the general idea is established — not that the paper's numbers are correct. Both searches agreed on rows 2, 4 and 6; on rows 1 and 5 Gemini took a harsher line, and the wording below keeps the more defensible version.

| Base-paper claim | Literature position | How we test it (Phase R) |
|---|---|---|
| 1. Running regex matchers in parallel on several cores speeds up classification (§1, §3.3). | **Supported in principle, superseded in practice.** Running several automata in parallel is a recognised parallelization (J11); multicore scaling of pattern matching is documented (J12). But compiling all patterns into one automaton checks every rule in a single pass (J3, J4, J5), so per-pattern parallelism is no longer needed. Gemini called this claim "contradicted"; that is too strong — it is true relative to a sequential loop of separate regexes. | Reproduce their design; measure speedup vs a sequential loop of `regexec` calls **and** vs a single combined DFA. |
| 2. Per-input process creation limits single-input speedup; batches of ~10+ work better (§5.3). | **Supported.** `fork()` carries significant fixed cost (C10); amortising fixed overhead over more work is the scaled-speedup argument (J22). | Batch-size sweep (1, 10, 100, 1000 inputs); split kernel time (fork, pipe, waitpid) from user time (regexec) with `perf`. |
| 3. Near-linear scaling to 6 cores, 5.62× at 8 cores (§4.3). | **Qualified.** Memory and cache effects limiting multicore matching are reported (J12). There is also an internal problem: with one process per pattern and only 5 patterns, at most 5 matchers can run at once per input, so a 5.62× speedup on 8 cores is hard to explain from the described design. | Core-count sweep with `taskset`; record cache misses; check whether speedup can exceed the pattern count. |
| 4. Structured formats (email, phone) are classified more accurately than natural-language ones (§4.2). | **Supported in principle.** Regular languages describe rigid formats well; log and anonymisation surveys treat free-text detection as harder (J14, J20). Note the paper's own accuracy figures are inconsistent with each other. | Accuracy on a labeled set of their 5 categories. |
| 5. Single-winner conflict resolution with 0.4/0.3/0.3 weights and a confidence score (§3.4). | **Not supported as designed.** Security rule engines combine all firing rules rather than picking one winner (OWASP CRS anomaly scoring); log lines can belong to several categories. No source found for the specific weights. | Compare their single winner with multi-label output on the same inputs. |
| 6. 8.9 ms sequential / 3.7 ms parallel for a 16-character email (§4.1). | **Not supported.** Thompson-style matching is linear in input length (J3); no source suggests milliseconds for one short match. Both searches attribute the figure to per-input `fork()` and per-call `regcomp`/`regfree`, but that is a hypothesis until measured. | Time `regcomp`, `regexec` and `fork` separately, with many repetitions. |

Suggested framing for the report: *"We reproduced the base paper's design. Its qualitative findings — parallel matching helps, process overhead limits small inputs, structured formats are easier — agree with the literature and with our measurements. Its absolute timings could not be reproduced on our hardware; we report our own."* Adjust the last sentence to what the measurements actually show.

---

## 4. Benchmarking and reproduction methodology (from C11 and both reports)

These rules apply to Phase R and to the main experiments (spec §10.2):

- **Fair baseline:** report speedup against the fastest sequential version on the same machine (a single thread with precompiled `regexec` calls), not against a slowed-down single-thread variant of the parallel program.
- **Separate compile and scan:** time pattern compilation (`regcomp` or our subset construction) separately from matching. The base paper recompiles on every input, so this split is the key measurement for claim 6.
- **Warm-up and repetition:** discard warm-up runs; repeat each point (≥ 5 runs; more for short timings) and report median with spread, plus p50/p90/p99 for latency.
- **Realistic input:** stream from a large, preloaded input pool rather than matching the same buffer in a tight loop, which keeps data in L1 cache and inflates throughput.
- **Stage breakdown for the base design:** use `perf` to separate kernel time (fork, pipe, waitpid, page-table copying) from user time (matching).
- **Throughput units:** report both lines/s and MB/s, since line lengths vary.

---

## 5. Gap our project addresses

Both searches reached the same conclusion independently:

- **Regex-matching research (J5–J13)** targets network packets: long single streams, hardware acceleration, table compression. It rarely studies classifying many short, independent lines on a general-purpose multicore CPU.
- **Log-analysis research (J14–J18)** centres on template mining and ML anomaly detection; rule-based triage is treated as the traditional baseline rather than studied as a system.
- **Parallel log and event processing (J15, J19)** relies on distributed frameworks (Spark, Flink), not a single-machine pipeline.
- **PII detection research (J20, C9)** focuses on NLP-based anonymisation; checksum validation inside a matching pipeline is not its subject.

To our knowledge, no journal paper combines (a) a multi-pattern DFA compiled by our own compiler, (b) line-level data parallelism on one multicore machine, (c) checksum validators in the post-match step, and (d) in-process windowed correlation for log triage. Say "to our knowledge" / "we did not find" in the report — neither search was exhaustive.

---

## 6. Gemini entries dropped or corrected during the merge

Each Gemini-only journal entry was checked. These did not survive:

| Gemini entry | Problem | Action |
|---|---|---|
| Becchi & Crowley, "Extending finite automata…," *ACM TACO* 10(4), 2013 | That title is a CoNEXT 2008 conference paper (DOI 10.1145/1544012.1544037). Their TACO 2013 paper is A-DFA, 10(1). | Corrected → J7 and C4. |
| Lison et al., "Anonymisation models for text data," *Computational Linguistics* 47(4), 2021 | It is an ACL-IJCNLP 2021 conference paper (pp. 4188–4203). | Moved → C9; journal slot taken by TAB (J20). |
| Smith, Estan, Jha, XFA, *ACM TOCS* 30(4), 2012 | No journal version found; XFA work is published as a technical report, conference papers and a patent. | Dropped. |
| Vaarandi et al., "Event log mining and aggregation for SIEM," *Computers & Security* 140, 2024 | Not found. | Dropped. |
| Chapman & Stolee, "Exploring regular expression comprehension and usage in practice," *IEEE TSE* 46(3), 2020 | Not found; their regex studies are conference papers (ISSTA 2016, ASE 2017). | Dropped. |
| Verhoeff, "Error detecting decimal codes," *ZAMM* 51(3), 1971 | Verhoeff's work is a 1969 monograph; the ZAMM item is likely a book review, not his article. | Replaced by the monograph (§1E note). |
| Chen, Jones, Becchi, Wolf, "Picking pesky parameters…," *IEEE TPDS* 2016 (J13) | Re-checked 14 Oct 2026 by web search: no record of this title, authors or venue found. | Dropped. |
| Ben-Porat, Bremler-Barr, Levy, *IEEE/ACM ToN* 21(3), 2013 | Title could not be matched to a known paper; weak relevance. | Dropped. |
| Jeong et al., REACT, *IEEE TPDS* 2020; Wurzenberger et al., *Computers & Security* 2024; Skopik et al., *IEEE Security & Privacy* 2022; Sánchez et al., *Information Sciences* 2013 and *JASIST* 2016; Purtzel & Weidlich, *PACMMOD* 2025 | Not verified in this merge, and only loosely related (storage accelerators, statistical anomaly detection, ML review, document sanitisation, stream aggregation). | Left out. Add one later only if a specific report section needs it, after checking its DOI. |

**Other Gemini content not to reuse** (it repeated several problems from its first report):

- **Unsourced numbers:** fork costing 100–500 µs, regex matches of 100–800 ns, DFAs under 128 KB, PII precision rising from 40–60% to above 99%, throughput and latency projections. None are sourced; measure them instead.
- **Dataset errors:** it again says Loghub OpenSSH has ground-truth labels (it has none), lists Linux as 4,322,296 lines (Loghub lists 25,567), and Apache as 56,460 (Loghub lists 56,481).
- **Rule syntax:** its regex table uses PCRE features (`\b`, `(?i)`, `\s`, lazy `.*?`) that our compiler rejects by design, a PAN regex without the holder-type check, and a card regex limited to Visa/Mastercard without separators. `rules/default.rules` stays as is.
- **Data structures:** its 512-byte fixed `raw_text` per event truncates long lines; spec §2.4 offsets stay.
- **Overclaimed consensus:** phrases like "universally favored" attached to citations that don't say that.

---

## 7. Still to check before the final report

- Re-checked 14 Oct 2026 by web search: J1, J2, J3, J21, J22, J23 confirmed (venue, volume, pages, DOI); J17 (20(4):3466–3482) and J18 (70(1):402–415) pages added from DBLP; J13 not found and dropped.
- Still open: J15 (volume and pages of the TDSC 2018 paper), J16 (author list).
- Optional: a database search (IEEE Xplore, ACM DL, Scopus) with *parallel regular expression matching multicore*, *multi-pattern DFA state explosion*, *log classification regular expressions*, *security log analysis rule-based*.
