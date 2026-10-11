# Project Design — Real-Time Parallel Log / Event Triage Engine

*Compiler Design course project, Team 1 (24BCI0099 Adithya Santhosh P, 24BCT0055 Anvesha Tripathi).*
*Single source of truth for implementation. Merged from `research_claude.md` and `research_gemini.docx`; see Appendix B for what came from where and what was corrected. Background: `base_paper.md` (professor's base paper), `review1_submission.md` (our Review 1).*

---

## 1. Goal and contributions

Extend the base paper's parallel regex text classifier into a streaming engine that classifies log lines in real time into five categories — **AUTH_FAILURE, SECURITY_THREAT, ERROR, PERF_WARNING, PII** — and routes them by severity.

Headline contributions over the base paper:

1. **A regex compiler** (regex → AST → Thompson NFA → subset construction → combined multi-pattern DFA, optional Hopcroft minimization) that evaluates *all* rules in one linear-time pass per line, independent of rule count, and is ReDoS-immune by construction.
2. **Data-parallel streaming runtime**: persistent pinned worker threads, batched SPSC ring buffers, bounded queues with backpressure — replacing fork-per-pattern-per-input.
3. **Multi-label classification** with severity-based primary label and **evidence-based, calibrated confidence** (replacing the arbitrary 0.4/0.3/0.3 single-winner formula).
4. **Algorithmic validators** (Luhn, Verhoeff, PAN structure) to cut PII false positives.
5. **Stateful sliding-window correlation** (brute force, password spraying, failure-then-success).
6. **Reproducible evaluation** on public datasets with scripted benchmarks.

Language/platform: **C11 on Linux, POSIX + pthreads**, gcc, Make. No external runtime dependencies for the core.

---

## 2. Architecture

```
 input (stdin / file / tail -F / generator socket)
        │
   [Reader]       split lines, assign seq_id, URL-decode copy for matching,
        │         parse timestamp if present, build batches
        ▼
 [Dispatcher]     round-robin (default) or least-loaded → per-worker SPSC ring
        │
   ┌────┼────┐
   ▼    ▼    ▼
 [Worker 0..N-1]  pinned pthreads; engine = DFA | POSIX; validators;
   │    │    │    entity extraction on matched lines → MatchResult
   └────┼────┘    (per-worker SPSC ring back to aggregator)
        ▼
 [Aggregator]     rules → categories, confidence, multi-label, primary, severity
        ▼
 [Correlator]     sliding windows keyed by source IP / user; escalations
        ▼
 [Router]         alerts.jsonl / pii_quarantine.jsonl (masked) / store.jsonl
                  + 1 Hz stats line on stderr
```

Reader, dispatcher, aggregator, correlator, router each run on their own thread (reader+dispatcher may share one). Only workers scale with `-w`.

### 2.1 Engine modes (one binary, selectable — needed for experiments)

| `--engine` | Meaning |
|---|---|
| `dfa` | Combined multi-pattern DFA from our compiler (default) |
| `posix` | Loop over rules calling `regexec` per rule per line; **each worker thread compiles its own `regex_t`** (glibc `regexec` takes an internal lock, so sharing one `regex_t` across threads contends) |
| `fork` | Reproduction of the base paper: per line, `fork()` one child per rule, child runs `regcomp`+`regexec`, reports over a pipe. Single dispatcher, no pool. Only for comparison. |

`-w 1` with `--engine posix` is the sequential baseline.

### 2.2 Worker model

- Pool created once at startup **after** rules are compiled. DFA tables are read-only and shared without locks.
- Pin with `pthread_setaffinity_np` (`--pin` flag; default on).
- Process-based variant (fork workers after compile + `shm_open`/`mmap` rings) is a **stretch goal**, only if the professor insists on process-level parallelism.

### 2.3 Queues, batching, backpressure

- One **SPSC ring** dispatcher→worker and one worker→aggregator per worker. C11 `stdatomic.h`; producer publishes with `memory_order_release`, consumer reads with `memory_order_acquire`. Head and tail on **separate 64-byte cache lines** to avoid false sharing:
  ```c
  typedef struct {
      _Alignas(64) atomic_size_t head;
      _Alignas(64) atomic_size_t tail;
      size_t capacity;            // power of two
      void **slots;               // Batch* or ResultBatch*
  } SpscRing;
  ```
  Start with a mutex+condvar queue if the SPSC ring is buggy; keep the same interface so it can be swapped.
- Items are **batches** (`-b`, default 256 lines). Partial batches flush after `--batch-delay` ms (default 5) so latency stays bounded at low rates.
- Rings are bounded. When the chosen worker's ring is full, the dispatcher blocks, which blocks the reader (no data loss). `--drop` mode instead drops and counts.
- Dispatch: `--dispatch rr` (default) or `--dispatch least` (pick ring with lowest occupancy).
- Every line has a `seq_id`. Output is unordered by default; `--ordered` (stretch) adds a reorder buffer in the router.

### 2.4 Core data structures

Lines live in a shared batch buffer referenced by offsets (no per-line fixed-size copy, so long access-log lines are not truncated).

```c
typedef struct {
    uint64_t seq_start;
    uint32_t count;
    uint32_t off[BATCH_MAX + 1];     // line i = raw[off[i] .. off[i+1]-1]
    uint32_t moff[BATCH_MAX + 1];    // same for match-normalized copy
    int64_t  ts[BATCH_MAX];          // parsed log timestamp (epoch s) or ingest time
    uint64_t ingest_ns[BATCH_MAX];   // CLOCK_MONOTONIC, for latency
    char    *raw;                    // original bytes (for output)
    char    *norm;                   // URL-decoded / control-stripped (for matching)
} Batch;

typedef struct {
    uint64_t seq;
    uint64_t rule_mask;              // bit r: rule r matched
    uint64_t valid_mask;             // bit r: rule r's validator passed (or has none)
    uint64_t ctx_mask;               // bit r: rule r's context keyword present
    uint32_t src_ip;                 // extracted if any rule requests it, else 0
    char     user[32];               // extracted username if requested
} MatchResult;

typedef struct {
    uint64_t seq;
    uint32_t label_mask;             // CAT_* bits, multi-label
    uint8_t  primary;                // CAT index
    uint8_t  severity;               // SEV_*
    float    confidence;             // of primary
    uint64_t rule_mask;              // for explainability
    uint32_t flags;                  // e.g. ESCALATED_BRUTE_FORCE
} Classified;

typedef enum { CAT_AUTH = 1<<0, CAT_SEC = 1<<1, CAT_ERR = 1<<2,
               CAT_PERF = 1<<3, CAT_PII = 1<<4 } Category;
typedef enum { SEV_INFO, SEV_LOW, SEV_MEDIUM, SEV_HIGH, SEV_CRITICAL } Severity;
```

---

## 3. The regex compiler (core compiler-design component)

### 3.1 Supported regex subset

Rules use a **POSIX-ERE-compatible subset**, so the exact same rule string runs in our compiler and in `regcomp(REG_EXTENDED | (icase ? REG_ICASE : 0))`. That makes differential testing trivial.

Supported: literals; `.`; bracket expressions `[abc]`, `[a-z]`, `[^...]`; `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}`; `|`; `( )`; `^` only at rule start; `$` only at rule end; backslash escapes of metacharacters (`\.`, `\(`, `\[`, `\\`, `\+`, `\?`, `\*`, `\|`, `\{`, `\$`, `\^`); case-insensitive per-rule flag.

**Not supported, by design:** backreferences, lookaround, lazy quantifiers, `\d \s \w \b` shorthands (write `[0-9]`, `[ \t]`, explicit boundaries). These are either non-regular or non-ERE; excluding them is what guarantees linear time.

Grammar (recursive descent, one function per nonterminal):

```
regex   := alt
alt     := concat ( '|' concat )*
concat  := repeat+
repeat  := atom ( '*' | '+' | '?' | '{' INT ( ',' INT? )? '}' )*
atom    := CHAR | '.' | class | '(' alt ')' | ESCAPE
class   := '[' '^'? ']'? item+ ']'          // leading ']' is literal
item    := CHAR ( '-' CHAR )?
```
`^`/`$` are stripped by the rule loader and recorded as flags (`anchored_start`, `anchored_end`).

### 3.2 Pipeline

1. **Lexer + parser** → AST (`NODE_CHARSET` with a 256-bit set, `NODE_CONCAT`, `NODE_ALT`, `NODE_STAR`, `NODE_PLUS`, `NODE_QUEST`). `{n,m}` is expanded into copies during parsing or NFA build. Case-insensitive rules fold each charset to include both cases. `.` = all bytes except `\n`.
2. **Thompson NFA**: states with at most two ε-edges or one charset edge; each rule's accept state tagged with its rule id.
3. **Combined NFA**: new start with ε-edges to every rule's start.
4. **Subset construction → combined DFA**:
   - `start0 = ε-closure(starts of ALL rules)`.
   - **Unanchored search**: every DFA state also includes `U = ε-closure(starts of rules without anchored_start)`, i.e. `δ(S, c) = ε-closure(move(S, c)) ∪ U`. Anchored-start rules therefore only begin at position 0.
   - `accept_mask[S]` = OR of rule bits for accept states in S of rules **without** `anchored_end`; `accept_end_mask[S]` = same for rules **with** `anchored_end`.
   - State cap `--max-states` (default 20000). If exceeded, the compiler splits rules into groups and builds one DFA per group (worker runs all groups). Report state counts.
5. **Optional Hopcroft minimization** (initial partition by `(accept_mask, accept_end_mask)` pair, not just accept/non-accept, so rule identity is preserved).
6. **Code generation**: `int32_t next[nstates][256]`, `uint64_t accept_mask[nstates]`, `uint64_t accept_end_mask[nstates]`. Stretch: byte equivalence classes to shrink the table.
7. **`--dump-dot`**: write NFA/DFA as Graphviz DOT for small rule sets (report figures).

### 3.3 Matching

```c
uint64_t dfa_match(const DFA *d, const char *s, uint32_t n) {
    int32_t st = 0; uint64_t m = d->accept_mask[0];
    for (uint32_t i = 0; i < n; i++) {
        st = d->next[st * 256 + (unsigned char)s[i]];
        m |= d->accept_mask[st];
    }
    return m | d->accept_end_mask[st];
}
```
Optional early exit: if all rules have matched, stop (rare; skip).

### 3.4 Post-match steps (only on lines with `rule_mask != 0`)

- **Validators** on the rule's matched span. The DFA gives no span, so for rules with a validator, re-run that single rule's `regexec` with `pmatch` on the line to get candidate spans (cheap, only on already-matched lines), then validate each span.
- **Entity extraction** (`src_ip`, `user`) the same way, for rules flagged `extract=`.
- **Context keywords**: per-rule keyword list, simple case-insensitive substring search.

### 3.5 Correctness: differential testing

`make difftest`: for every line in the test corpus, compare `dfa_match` against the per-rule `regexec` bitmask (with matching `REG_ICASE`). Any mismatch prints the rule id and line. Run in CI-style script before every benchmark. Plus unit tests for parser, NFA construction, and small hand-built DFAs.

---

## 4. Rules

### 4.1 Rule file format (`rules/default.rules`)

One rule per line, tab-separated:

```
# id	category	severity	base	flags	validator	extract	context	regex
SSH_FAIL_PW	AUTH	MEDIUM	0.6	-	-	ip,user	-	Failed password for (invalid user )?[^ ]+ from [0-9.]+
```

- `base`: prior precision tier (0.35 weak / 0.6 strong), later tuned by calibration.
- `flags`: `i` = case-insensitive, `-` none.
- `validator`: `luhn`, `verhoeff`, `pan`, or `-`.
- `extract`: comma list of `ip`, `user`, or `-`.
- `context`: comma list of keywords, or `-`.
- Max 64 rules (fits `uint64_t` masks).

### 4.2 Starter rule set

All in the supported subset. `(^|[^0-9])` / `([^0-9]|$)` are explicit digit boundaries (no `\b`). Tune against data.

Contents of `rules/default.rules` (tab-separated; copy verbatim — the regex column is exactly what both engines compile):

```
# id	category	severity	base	flags	validator	extract	context	regex
SSH_FAIL_PW	AUTH	MEDIUM	0.6	-	-	ip,user	-	Failed password for (invalid user )?[^ ]+ from [0-9.]+
SSH_INVALID_USER	AUTH	MEDIUM	0.6	-	-	ip,user	-	Invalid user [^ ]* from [0-9.]+
PAM_AUTH_FAIL	AUTH	MEDIUM	0.6	-	-	user	-	pam_unix\([a-z-]+:auth\): authentication failure
SUDO_BAD_PW	AUTH	MEDIUM	0.6	-	-	user	-	incorrect password attempts?
SSH_ACCEPTED	AUTH*	INFO	0.6	-	-	ip,user	-	Accepted (password|publickey) for [^ ]+ from [0-9.]+
HTTP_401_403	AUTH	LOW	0.35	-	-	-	-	" (401|403) [0-9-]+
SQLI	SEC	CRITICAL	0.6	i	-	-	select,union,where	(union[ +]+(all[ +]+)?select|' *or *'?[0-9]+'? *= *'?[0-9]+|or[ +]+1=1|sleep\([0-9]+\)|benchmark\(|information_schema)
XSS	SEC	HIGH	0.6	i	-	-	-	(<script|javascript:|onerror *=|onload *=)
PATH_TRAVERSAL	SEC	HIGH	0.6	-	-	-	-	(\.\./|\.\.\\|/etc/passwd|/etc/shadow)
CMD_INJECTION	SEC	CRITICAL	0.6	i	-	-	-	(;|\|\||&&|\$\()[ +]*(wget|curl|nc|bash|sh)[ +/]
SCANNER_UA	SEC	MEDIUM	0.6	i	-	-	-	(sqlmap|nikto|nmap|masscan|gobuster|dirbuster)
ERR_LEVEL	ERR	MEDIUM	0.6	-	-	-	-	(^|[^A-Za-z])(ERROR|FATAL|CRITICAL|SEVERE)([^A-Za-z]|$)
JAVA_EXCEPTION	ERR	MEDIUM	0.6	-	-	-	-	[A-Za-z_][A-Za-z0-9_.]*(Exception|Error): 
JAVA_STACK	ERR	MEDIUM	0.6	-	-	-	-	^[ 	]+at [A-Za-z0-9_.$]+\(
HTTP_5XX	ERR	HIGH	0.6	-	-	-	-	" 5[0-9][0-9] [0-9-]+
CRASH_OOM	ERR	HIGH	0.6	-	-	-	-	(segfault at|Kernel panic|Out of memory: Kill(ed)? process|oom-killer)
SLOW_QUERY	PERF	LOW	0.6	i	-	-	-	(slow query|Query_time: [0-9]+)
TIMEOUT	PERF	MEDIUM	0.35	i	-	-	-	(timed out|timeout|deadline exceeded)
HIGH_LATENCY	PERF	LOW	0.6	-	-	-	-	(took|duration|elapsed)[=: ]+[0-9]{4,}(\.[0-9]+)? ?ms
RESOURCE_PRESSURE	PERF	MEDIUM	0.6	i	-	-	-	(connection pool exhausted|queue (is )?full|disk usage 9[0-9]%|high (cpu|memory) usage)
PII_EMAIL	PII	LOW	0.6	-	-	-	-	[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}
PII_CARD	PII	CRITICAL	0.35	-	luhn	-	card,cc,payment	(^|[^0-9])[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{1,4}([^0-9]|$)
PII_AADHAAR	PII	HIGH	0.35	-	verhoeff	-	aadhaar,aadhar,uid	(^|[^0-9])[2-9][0-9]{3}[ -]?[0-9]{4}[ -]?[0-9]{4}([^0-9]|$)
PII_PAN	PII	HIGH	0.6	-	pan	-	pan	[A-Z]{3}[ABCFGHJLPT][A-Z][0-9]{4}[A-Z]
PII_PHONE_IN	PII	MEDIUM	0.35	-	-	-	phone,mobile,tel	(^|[^0-9])(\+91[ -]?)?[6-9][0-9]{9}([^0-9]|$)
PII_SECRET_QS	PII	CRITICAL	0.6	i	-	-	-	(password|passwd|pwd|token|api_key|secret)=[^& "]+
```

All 26 regexes were checked to compile with `regcomp(REG_EXTENDED)` and to match their example lines. Expected overlap: `card=4111 1111 1111 1111` matches both PII_CARD and PII_AADHAAR (its first 12 digits look like an Aadhaar); the Verhoeff validator is what removes the false Aadhaar — a good worked example for the report.

`AUTH*`: `SSH_ACCEPTED` produces no label on its own (INFO); it exists only to feed the correlator's failure-then-success rule.

Rule notes:
- Matching runs on the **normalized copy**: URL-decoded (`%xx`, `+`→space in query strings) and control characters stripped. No lowercasing — case-insensitivity is a per-rule flag (fixes the base paper's normalization bug).
- **PAN** 4th letter = holder type (P individual, C company, H HUF, F firm, A AOP, T trust, B BOI, L local authority, J artificial juridical person, G government). The `pan` validator only re-checks length/structure; the class in the regex already enforces the 4th letter.
- **Aadhaar**: 12 digits, first digit 2–9, last digit Verhoeff check. **Card**: Luhn after stripping spaces/hyphens, length 13–19.

### 4.3 Validators

```c
// Verhoeff (tables verified against the standard dihedral-group D5 tables)
static const uint8_t V_D[10][10] = {
 {0,1,2,3,4,5,6,7,8,9},{1,2,3,4,0,6,7,8,9,5},{2,3,4,0,1,7,8,9,5,6},
 {3,4,0,1,2,8,9,5,6,7},{4,0,1,2,3,9,5,6,7,8},{5,9,8,7,6,0,4,3,2,1},
 {6,5,9,8,7,1,0,4,3,2},{7,6,5,9,8,2,1,0,4,3},{8,7,6,5,9,3,2,1,0,4},
 {9,8,7,6,5,4,3,2,1,0}};
static const uint8_t V_P[8][10] = {
 {0,1,2,3,4,5,6,7,8,9},{1,5,7,6,2,8,3,0,9,4},{5,8,0,3,7,9,6,1,4,2},
 {8,9,1,6,0,4,3,5,2,7},{9,4,5,3,1,2,6,8,7,0},{4,2,8,6,5,7,3,9,0,1},
 {2,7,9,3,8,0,6,4,1,5},{7,0,4,6,9,1,3,2,5,8}};

int verhoeff_ok(const char *d, size_t n) {          // n digits, no separators
    int c = 0;
    for (size_t i = 0; i < n; i++)
        c = V_D[c][V_P[i % 8][d[n - 1 - i] - '0']];
    return c == 0;
}

int luhn_ok(const char *d, size_t n) {
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        int x = d[n - 1 - i] - '0';
        if (i & 1) { x *= 2; if (x > 9) x -= 9; }
        sum += x;
    }
    return n >= 13 && n <= 19 && sum % 10 == 0;
}
```
Unit-test with known values: Aadhaar `234123412346` valid / `234123412347` invalid; card `4111111111111111` valid.

---

## 5. Classification, confidence, severity

### 5.1 Per-rule evidence score

For each matched rule r:
- If r has a validator and it **fails** on every candidate span → discard r.
- `s = base(r)`; `+0.25` if validator passed; `+0.15` if a context keyword is present; clamp to [0, 1].

(Modeled on tiered PII detection used by DLP products: pattern only < pattern + checksum < pattern + checksum + nearby keyword.)

### 5.2 Per-category confidence

Noisy-OR over surviving rules in the category: `conf(c) = 1 − Π(1 − s_i)`. Correlator escalations set the escalated category to 0.95.

### 5.3 Labels, primary, severity

- `label_mask` = categories with `conf ≥ τ` (`--tau`, default 0.5).
- `severity(c)` = max severity among its surviving rules.
- **Primary** = label with highest severity; tie → higher confidence; tie → fixed order SEC > PII > AUTH > ERR > PERF.
- Within a category, if one rule's span fully contains another's, the outer rule is kept for explanation (reporting only; doesn't change the mask).
- Line severity = primary's severity; no labels → INFO.

Rejected from base paper: the "completeness = matched length / line length" factor — it penalizes short, high-severity signatures (a 15-byte SQLi in a 500-byte request).

### 5.4 Calibration

On labeled data: 10 confidence bins, plot observed precision per bin (reliability diagram) and report **Expected Calibration Error** `ECE = Σ_b (|B_b|/N)·|acc(B_b) − conf(B_b)|`. Adjust `base` values if bins are off.

---

## 6. Correlator

Single thread, after the aggregator (no locks). Open-addressing hash map keyed by `src_ip` (uint32) and a second keyed by `(src_ip)` → set of usernames. Each entry keeps a ring of recent failure timestamps (capacity 64).

| Rule | Trigger | Effect |
|---|---|---|
| BRUTE_FORCE | ≥ `maxretry` (default 5) AUTH failures from one IP within `findtime` (default 60 s) | Add CAT_SEC, sev HIGH, conf 0.95, flag `ESCALATED_BRUTE_FORCE`; emit once per window |
| PASSWORD_SPRAY | Failures from one IP against ≥ K (default 5) distinct users within T (default 300 s) | CAT_SEC, sev HIGH |
| FAIL_THEN_SUCCESS | `SSH_ACCEPTED` from an IP currently over BRUTE_FORCE threshold | CAT_SEC, sev CRITICAL |
| ERROR_STORM | ≥ N ERR lines within T (global) | One aggregated alert, suppress repeats |

Time source: parsed log timestamp (syslog `Mon DD HH:MM:SS` assumed current year; ISO-8601 if present), falling back to ingest time. Using log time makes dataset replays behave like real time. Parameter names mirror fail2ban (`maxretry`, `findtime`).

---

## 7. Routing and output

| Condition | Sink |
|---|---|
| severity ≥ HIGH (any category) | `out/alerts.jsonl` (+ colored line on stderr with `--tty`) |
| PII in labels | `out/pii_quarantine.jsonl`, matched values masked (Aadhaar `XXXX XXXX 1234`, card `XXXX…1111`, PAN `XXXXX1234X`, secrets `***`) |
| everything | `out/store.jsonl` (raw line included unless PII, then masked) |

Record:
```json
{"seq":1042,"ts":"2026-10-11T10:14:22","primary":"SECURITY_THREAT","severity":"CRITICAL",
 "confidence":0.85,"labels":["SECURITY_THREAT","PII"],"rules":["SQLI","PII_EMAIL"],
 "flags":[],"line":"GET /item?id=1+UNION+SELECT ..."}
```

Stats line on stderr every second: lines/s, per-worker queue depth, counts per category, dropped.

---

## 8. CLI

```
triage [-w N] [-b BATCH] [--batch-delay MS] [--engine dfa|posix|fork]
       [--dispatch rr|least] [--pin|--no-pin] [--drop] [--ordered]
       [-r rules/default.rules] [-o out/] [--tau 0.5] [--max-states N]
       [--dump-dot FILE] [--bench] [--quiet] [INPUT|-]
```
`--bench`: disable output sinks, report throughput and latency percentiles at exit (as JSON on stdout) for the benchmark scripts.

Companion tools: `gen` (synthetic generator), `difftest`, `label_eval` (computes P/R/F1, ECE from ground truth).

---

## 9. Repository layout

```
triage/
  Makefile                 # targets: all, test, difftest, bench, clean; -O2 -Wall -Wextra -pthread
  CLAUDE.md                # points to docs/, build/test commands
  docs/ base_paper.md review1_submission.md project_design.md research_*.md
  src/
    main.c cli.c
    regex_lex.c regex_parse.c ast.c nfa.c dfa.c minimize.c dot.c   # compiler
    rules.c                      # rule file loader
    engine_dfa.c engine_posix.c engine_fork.c
    reader.c normalize.c dispatch.c worker.c spsc.c
    aggregate.c correlate.c route.c validators.c extract.c stats.c
  tools/ gen.c difftest.c label_eval.py plot.py run_bench.sh
  rules/ default.rules
  tests/ test_parse.c test_nfa_dfa.c test_validators.c test_spsc.c corpus/
  data/ (gitignored: downloaded datasets)
  results/ (benchmark JSON + plots)
```

---

## 10. Evaluation

### 10.1 Datasets

| Dataset | Size | Labels | Use |
|---|---|---|---|
| Loghub OpenSSH | 655,146 lines, 28.4 days | **None** | AUTH rules, brute-force correlator, throughput on real lines |
| Loghub Apache (error log) | 56,481 lines | None | ERR rules |
| Loghub Linux | 25,567 lines | None | auth/PAM/kernel lines |
| Loghub BGL | 4,747,963 lines | Alert vs non-alert per line | ERR precision/recall; large-volume throughput |
| AIT-LDS v2.0 (Zenodo) | Multi-host testbed: Apache access/error, auth, DNS, VPN, audit, Suricata, syslog | **Line-level attack labels (JSON)** | SEC/AUTH ground truth |
| Synthetic (`tools/gen`) | Configurable | Exact by construction | PII (valid + invalid checksums), injected attack payloads from public lists (e.g. OWASP CRS test payloads, SecLists), latency-vs-rate runs |

Hand-label a stratified sample (≈1,000–2,000 lines across categories) for categories without labels; both teammates label independently; report Cohen's κ. Cite Loghub and AIT-LDS in the report.

### 10.2 Benchmark hygiene

- Preload input into RAM (generator or `--bench` reads whole file first) so disk I/O isn't measured.
- Generator can pace output at a fixed rate (`clock_nanosleep`, `CLOCK_MONOTONIC`) over a pipe or UNIX socket for latency experiments.
- Record CPU model, physical cores, RAM, kernel, gcc version/flags. ≥5 runs each; report median and min/max. Never quote a single run or numbers not measured.

### 10.3 Experiments

| # | Experiment | Varies | Metrics | Hypothesis to test |
|---|---|---|---|---|
| E1 | Architecture comparison | C1 `fork`, C2 `posix -w1`, C3 `posix -wN`, C4 `dfa -wN` | lines/s, MB/s | C1 ≪ C2 < C3 < C4; C1 dominated by process creation |
| E2 | Scaling | `-w` 1 → #physical cores (C3 and C4) | throughput, speedup S=T1/Tp, efficiency S/p | near-linear until serial stages (reader/aggregator) bound it |
| E3 | **Rule-count sensitivity** (headline) | 5 → 64 rules, C2 vs `dfa -w1` | throughput, DFA states, build time | POSIX falls ~linearly with rules; DFA stays roughly flat |
| E4 | Batch size | `-b` 1…4096 | throughput, p99 latency | throughput rises then plateaus; latency grows |
| E5 | Latency under load | input rate 25/50/75/90% of max | p50/p90/p99/p99.9 | flat until saturation, then queueing tail |
| E6 | Accuracy | — | P/R/F1 per category; confusion matrix | — |
| E7 | Validator gain | validators on/off | PII precision | validators raise precision |
| E8 | Calibration | — | reliability diagram, ECE | — |
| E9 | Load balancing *(stretch)* | `rr` vs `least`, skewed line lengths | per-worker utilization variance | `least` lowers variance |
| E10 | ReDoS *(stretch)* | `(a+)+b`-style rule on PCRE2 vs our DFA, growing input | time vs length | exponential vs linear |
| E11 | DFA size *(stretch)* | rule groups, minimization on/off | states, table bytes | minimization/grouping trade-offs |

Tools: `perf stat` (IPC, cache/branch misses), `perf record -g` (hot spots), `hyperfine`, `valgrind --tool=memcheck`, `-fsanitize=thread` and `-fsanitize=address` builds, `taskset`.

---

## 11. Roadmap (2 people, ~5–6 weeks; adjust to real review dates)

| Week | A — compiler & rules | B — runtime & evaluation |
|---|---|---|
| 1 | Rule loader; regex lexer + recursive-descent parser → AST; parser tests | `engine_posix` sequential baseline; reader + normalizer; synthetic generator; download datasets |
| 2 | Thompson NFA; combined NFA; subset construction with unanchored/anchored handling; `dump-dot` | SPSC ring + tests (TSan); batch structs; worker pool + pinning; dispatcher |
| 3 | `difftest` vs `regexec` on all corpora — fix until zero mismatches; state-count reporting; `engine_fork` | Aggregator, router, JSONL sinks, stats line; `--bench` mode |
| 4 | Validators + extraction + context keywords; confidence model; rule tuning on data | Correlator (brute force, spray, fail-then-success); hand-labeling; `label_eval` |
| 5 | Hopcroft minimization (if time); E3, E11 | E1, E2, E4–E8 runs; plots |
| 6 | Report: compiler sections (grammar, constructions, DOT figures, difftest) | Report: architecture, scaling, latency, accuracy, calibration; slides |

**Must have:** compiler for the subset → combined DFA; difftest = 0 mismatches; three engines; pinned worker pool with batching and bounded queues; full rule set; validators; multi-label + confidence; brute-force correlator; JSONL routing with masking; E1–E3, E6, E7.

**Stretch:** Hopcroft minimization; byte equivalence classes; least-loaded dispatch (E9); latency study (E5); calibration (E8); ordered output; ReDoS demo (E10); process-based worker variant; Aho-Corasick literal prefilter in front of the DFA; SIGHUP rule reload; small live dashboard.

**Out of scope:** SIMD/AVX scanning, work stealing, ML classifiers, GPU, distributed processing, PagerDuty/Slack/S3/SIEM integrations.

---

## Appendix A — Related work for the report (verified)

1. K. Thompson, "Regular Expression Search Algorithm," CACM 11(6), 1968 — Thompson construction.
2. A. Aho, M. Corasick, "Efficient String Matching: An Aid to Bibliographic Search," CACM 18(6), 1975 — multi-literal matching.
3. Aho, Lam, Sethi, Ullman, *Compilers: Principles, Techniques, and Tools*, ch. 3 — NFA/DFA, subset construction, minimization, lex.
4. R. Cox, "Regular Expression Matching Can Be Simple And Fast," swtch.com, 2007 — backtracking vs Thompson NFA; ReDoS argument. (Web article, not a journal paper.)
5. X. Wang et al., "Hyperscan: A Fast Multi-pattern Regex Matcher for Modern CPUs," USENIX NSDI 2019 — regex decomposition + SIMD; literal prefiltering. (Hyperscan is proprietary after 5.4; Vectorscan is the BSD fork.)
6. M. Becchi, P. Crowley, "A Hybrid Finite Automaton for Practical Deep Packet Inspection," ACM CoNEXT 2007 — DFA state explosion and hybrid automata.
7. S. Kumar et al., "Algorithms to Accelerate Multiple Regular Expressions Matching for Deep Packet Inspection," ACM SIGCOMM 2006 — D²FA transition compression.
8. T. Mytkowicz, M. Musuvathi, W. Schulte, "Data-Parallel Finite-State Machines," ASPLOS 2014 — parallelizing a single FSM run; contrast with our line-level parallelism.
9. P. He, J. Zhu, Z. Zheng, M. Lyu, "Drain: An Online Log Parsing Approach with Fixed Depth Tree," IEEE ICWS 2017 — streaming log template mining.
10. J. Zhu et al., "Loghub: A Large Collection of System Log Datasets for AI-driven Log Analytics," ISSRE 2023 — datasets.
11. M. Landauer et al., AIT Log Data Set v2.0 (Zenodo) — labeled attack logs.
12. M. Roesch, "Snort: Lightweight Intrusion Detection for Networks," USENIX LISA 1999 — signature IDS with fast-path prefilters.
13. OWASP Core Rule Set; SigmaHQ rules and Sigma correlation spec; fail2ban — practitioner rule sources.
14. re2c — regex-to-DFA lexer generator (comparison point; DOT dumps).

## Appendix B — Merge decisions and corrections

**Taken from Gemini's report:** cache-line-padded SPSC ring with acquire/release ordering; four-configuration architecture comparison (E1); critique of the base paper's completeness factor; subsumption rule; Hopcroft minimization detail; ECE metric; RAM preloading and rate-paced generator; attack-payload augmentation of datasets; Aho-Corasick prefilter (kept as stretch); Becchi & Crowley, Kumar et al., Aho-Corasick, Thompson, Roesch citations; Verhoeff C implementation (tables checked against the standard ones).

**Taken from Claude's report:** ERE-compatible rule syntax and differential testing against `regexec`; unanchored/anchored subset-construction details and accept masks; state-cap + rule grouping; per-thread `regex_t` for the POSIX engine; URL-decoding normalization; offsets-into-buffer batches; noisy-OR evidence confidence; AIT-LDS labeled dataset; rule-count sensitivity experiment (E3); password-spray and failure-then-success correlations; PII masking in routing; Mytkowicz et al. citation.

**Corrected or dropped from Gemini's report:**
- Its regexes use PCRE syntax (`\d \s \b (?: (?i)` lazy `*?`, `[\s\S]`), which conflicts with its own recommendation of a custom DFA compiler; some were also garbled in the export. Rewritten in the ERE subset.
- "PCRE2 fallback for complex patterns" dropped from core: it reintroduces backtracking and undermines the ReDoS claim.
- Dataset table errors: Loghub Linux is 25,567 lines (not ~4.3M); Loghub OpenSSH and Apache have **no** ground-truth labels.
- Cox's article is a swtch.com web article, not ACM Queue 5(8).
- Hypothesized latency figures (e.g. "sub-100 µs p50") and the "100–500 µs per fork" figure were unsourced; replaced by measuring them (E1, E5).
- PAN regex `[A-Z]{5}[0-9]{4}[A-Z]` tightened to enforce the 4th-letter holder type; BIN-specific card regex replaced by a separator-tolerant pattern + Luhn.
- Fixed 512-byte `raw_text` per event replaced by offsets (avoids truncating long lines).
- 10,000 hand-labeled lines reduced to ~1,000–2,000 (realistic for two people).
- PagerDuty/Slack/S3/firewall routing replaced by file sinks.
- Aho-Corasick moved from core to stretch (redundant for correctness once all rules are in one DFA).
