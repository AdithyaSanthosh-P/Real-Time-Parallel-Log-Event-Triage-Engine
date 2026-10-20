#!/usr/bin/env python3
"""Build docs/Final_Report_IEEE.pdf: the project report (IEEE two-column format) for the state of the
project at commit da15165 (compiler, classification, parallel pipeline, correlator, base-paper
reproduction).  Same structure and page machinery as make_review1_report.py, which it imports.

Structure follows the professor's template: Abstract, 1 Introduction, 2 Literature Survey,
3 Proposed System (architecture diagram, modules with explanation and pseudocode),
4 Results and Implementation, 5 Performance and Metrics, 6 Conclusion, References.

Every number is read from files in the repository:
  results/milestone2/*.json   own-engine measurements of commit da15165
                              (tools/report/measure_milestone1.py da15165 milestone2, measure_correlator.py)
  results/milestone1/*.json   same measurements at the earlier commit 8093e0a (before/after comparisons)
  results/basepaper/*         base-paper reproduction runs
  git show da15165:...        line counts of the modules
Needs: Graphviz `dot` and reportlab (python3 -m venv v && v/bin/pip install reportlab).
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import make_review1_report as R                      # page machinery, charts, references, base-paper data
from make_review1_report import (BLUE, COL_W, FULL_W, GREEN, GREY, ORANGE, P, RB1, RB2, RB4, S, SS, algorithm,
                                 body, body0, cite, fmt, hbar_err, hbar_log, item, kw, line_chart, si, table,
                                 vbars, figure, image, abstract, ParagraphStyle, Paragraph, KeepTogether,
                                 NextPageTemplate, PageBreak, FrameBreak, Spacer, ImageReader, cm, sec, ref)

ROOT = R.ROOT
COMMIT = "da15165"
R.COMMIT = COMMIT
OUT = os.path.join(ROOT, "docs", "Final_Report_IEEE.pdf")
R.OUT = OUT


def jload(p):
    return json.load(open(os.path.join(ROOT, p)))


M2 = {k: jload(f"results/milestone2/{k}.json") for k in ("state_sweep", "throughput", "scaling", "corpus", "tests",
                                                          "correlator")}
M1C = {c["file"]: c for c in jload("results/milestone1/corpus.json")}


def src(*names):
    return R.git_loc([f"src/{n}.{e}" for n in names for e in ("c", "h")])


GROUPS = [
    ("Regex front end", ["regex_lex", "regex_parse", "ast"]),
    ("NFA/DFA compiler", ["nfa", "dfa", "dot", "engine_dfa"]),
    ("Classification, confidence, masking", ["validators", "extract", "aggregate", "mask"]),
    ("Parallel runtime", ["spsc", "pool", "worker", "dispatch", "stats", "pipeline", "reader", "batch"]),
    ("Correlation and routing", ["correlate", "route", "json"]),
    ("Common (rules, CLI, engines, normaliser)", ["main", "cli", "rules", "normalize", "timestamp", "engine",
                                                  "engine_posix", "types"]),
]
LOC = {g: src(*f) for g, f in GROUPS}
assert sum(LOC.values()) == M2["tests"]["sizes"]["src_lines"], (LOC, M2["tests"]["sizes"])
TOOLS_BP = M2["tests"]["sizes"]["basepaper_tool_lines"]


def title_block():
    t = ParagraphStyle("t", fontName="Serif-Bold", fontSize=18, leading=21.5, alignment=1)
    st = ParagraphStyle("st", fontName="Serif-Italic", fontSize=10.5, leading=12.8, alignment=1)
    au = ParagraphStyle("au", fontName="Serif", fontSize=10.5, leading=12.5, alignment=1)
    af = ParagraphStyle("af", fontName="Serif-Italic", fontSize=9, leading=11, alignment=1)
    from reportlab.platypus import Table, TableStyle
    authors = Table([[Paragraph("Adithya Santhosh P<br/><font size=9>Reg. No. 24BCI0099</font>", au),
                      Paragraph("Anvesha Tripathi<br/><font size=9>Reg. No. 24BCT0055</font>", au)]],
                    colWidths=[FULL_W / 2] * 2)
    authors.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                                 ("RIGHTPADDING", (0, 0), (-1, -1), 0)]))
    return [Spacer(1, 0.1 * cm), P("Real-Time Parallel Log / Event Triage Engine", t),
            P("A Regex-to-DFA Compiler and Multi-Core Pipeline for Streaming Log Classification", st),
            Spacer(1, 0.4 * cm), authors, Spacer(1, 0.12 * cm),
            P("Team 1, Compiler Design Course Project, VIT  ·  Project Report  ·  October 2026", af), FrameBreak()]


def content():
    s = []
    tp, sc, T = M2["throughput"], M2["scaling"]["results"], M2["tests"]
    dfa_lps, pos_lps = tp["dfa"]["lines_per_s_median"], tp["posix"]["lines_per_s_median"]
    speed = tp["speedup_dfa_over_posix"]
    sw = {r["max_states"]: r for r in M2["state_sweep"]}
    single, grouped = sw[1000000]["dfa_states"], sw[20000]
    checks = sum(T["unit_test_checks"].values())
    dt = T["difftest"]
    sizes = T["sizes"]
    corp = {c["file"]: c for c in M2["corpus"]}
    cor = {c["file"]: c for c in M2["correlator"]}
    ssh, bgl = cor["OpenSSH_2k.log"], cor["BGL_2k.log"]
    sp = lambda e, w: sc[e][str(w)]["lines_per_s_median"] / sc[e]["1"]["lines_per_s_median"]

    def r2(batch, variant):
        return [x for x in RB2 if x["batch"] == batch and x["variant"] == variant][0]["inputs_per_s"]
    thr = {v: r2(10000, v) for v in ("fork", "seq-recompile", "seq-precompiled", "threads-pattern",
                                      "threads-data_t5", "dfa_t5")}
    fork_vs_rec = thr["seq-recompile"]["median"] / thr["fork"]["median"]
    fork_vs_pre = thr["seq-precompiled"]["median"] / thr["fork"]["median"]
    a, b = RB4["paper"], RB4["none"]
    lab = RB4["none"]["lines"]

    # ------------------------------------------------------------------------------ Abstract
    s.append(P("<b><i>Abstract</i></b>—Servers emit thousands of log lines per second, and the few that matter—"
               "repeated failed logins, injection attempts, crashes, leaked personal data—must be found as the "
               "lines arrive. This report presents a real-time log triage engine written in C11 around a "
               "purpose-built regular-expression compiler. Twenty-six detection rules are parsed by a "
               "recursive-descent parser, converted to a Thompson NFA and compiled by subset construction into "
               "combined DFAs that match every rule in one pass over a line. A classifier validates matched "
               "numbers with Luhn, Verhoeff and PAN checks, combines rule evidence by noisy-OR into per-category "
               "confidences, masks personal data and routes records to alert, store and quarantine files; a "
               "multi-threaded pipeline with lock-free queues and a correlator that detects brute force, password "
               "spraying and error storms complete the system. The DFA agrees with the C library's regex engine on "
               f"{dt['file_lines']:,} corpus lines and {dt['random_strings']:,} generated strings "
               f"({dt['mismatches']} mismatches) and runs {speed:.1f}× faster than the library on one thread "
               f"({si(dfa_lps)} against {si(pos_lps)} lines/s). Checksum validation removes "
               f"{M1C['HDFS_2k.log']['primary_label_counts']['PII'] - corp['HDFS_2k.log']['primary_label_counts']['PII']}"
               " of 119 false PII labels in an HDFS sample, and the correlator reports "
               f"{ssh['flags']['ESCALATED_BRUTE_FORCE']} brute-force and {ssh['flags']['ESCALATED_PASSWORD_SPRAY']} "
               "password-spray episodes in a real SSH log. The report also reproduces and measures the course's base "
               "paper, whose process-per-pattern design is slower than a sequential loop. Throughput does not yet "
               "scale with workers for the DFA engine, because serial stages become the bottleneck; this is "
               "analysed in Section 5.", abstract))
    s.append(P("<b><i>Index Terms</i></b>—regular expressions, finite automata, subset construction, log "
               "classification, multicore processing, lock-free queues, intrusion detection.",
               ParagraphStyle("it", parent=abstract, fontName="Serif", spaceAfter=2)))

    # ------------------------------------------------------------------------ 1 Introduction
    s.append(S("Introduction"))
    s.append(SS("1.1", "Motivation"))
    s.append(P("Modern services write logs faster than people can read them. Hidden in the stream are lines that "
               "need action: repeated authentication failures, SQL-injection or path-traversal requests, "
               "application crashes, slow queries, and personal data that must not be stored in clear. "
               "<i>Triage</i>—assigning every line a category and a severity, and routing urgent lines to an "
               "alert stream—has to keep up with the input rate. Hand-written regular-expression rules are the "
               f"traditional way to encode this knowledge {cite('J14')}, which makes the matching engine the "
               "performance-critical part, and a regex that only checks shape cannot tell a card number from a "
               "long identifier.", body0))
    s.append(P("The base paper given for this course classifies short text inputs by running one regular "
               "expression per forked process on separate cores. We treat its design and claims as hypotheses "
               "and test them. This project takes a different route: compile all rules into one deterministic "
               "automaton, parallelise across input lines rather than across patterns, and add the "
               "validation and correlation that raw pattern matching lacks."))
    s.append(SS("1.2", "Problem Statement and Objectives"))
    s.append(P("<i>Problem.</i> Given a stream of log lines and at most 64 rules, each a regular expression with a "
               "category, a severity and a base score, assign every line its categories, a confidence per "
               "category, a primary category and a severity, mask personal data, and detect attacks that only "
               "show across several lines—at a rate that keeps up with the input. The matching semantics must "
               "equal POSIX extended regular expressions on the supported subset, so that the C library serves as "
               "an independent reference, and a rule outside the subset must be reported, never silently "
               "approximated.", body0))
    s.append(P("<i>Objectives.</i> (O1) Build a regex-to-DFA compiler from first principles and verify it against "
               "the C library. (O2) Reduce false positives by validating matched numbers and combine evidence "
               "into calibrated multi-label output. (O3) Process lines on several cores with a clean "
               "hand-off between threads and output identical to the sequential run. (O4) Detect multi-line "
               "attacks with sliding windows. (O5) Measure the base paper's design and fairer alternatives "
               "under a documented protocol."))
    s.append(SS("1.3", "Contributions"))
    s.append(P("1)&nbsp;a <b>regex front end</b> for the regular POSIX-ERE subset that rejects everything "
               "outside it;", item))
    s.append(P("2)&nbsp;a <b>regex-to-DFA compiler</b>: Thompson NFA, subset construction with accept masks and "
               "automatic rule grouping, and a table-driven matcher;", item))
    s.append(P("3)&nbsp;a <b>classifier</b> with checksum validators, entity extraction, a noisy-OR confidence "
               "model and PII masking;", item))
    s.append(P("4)&nbsp;a <b>parallel pipeline</b> with pinned worker threads, lock-free single-producer "
               "single-consumer queues, a pre-allocated buffer pool and ordered output;", item))
    s.append(P("5)&nbsp;a <b>correlator</b> for brute force, password spraying, fail-then-success and error storms;",
               item))
    s.append(P("6)&nbsp;a faithful <b>reproduction and benchmark</b> of the base paper.", item))
    s.append(SS("1.4", "Organization"))
    s.append(P("Fig. 1 gives an overview: rules are compiled once at start-up; at run time a reader feeds batches to "
               "worker threads, an aggregator and a correlator classify and escalate, and a router writes the "
               "outputs; the evaluation harness measures the base-paper designs. Section 2 surveys related work, "
               "Section 3 describes each module with pseudocode, Section 4 gives implementation results, Section 5 "
               "compares existing and proposed methods, and Section 6 concludes.", body0))
    s.append(R.FIGSLOT)
    s.append(SS("1.5", "Scope and Limitations"))
    s.append(P(f"This report covers the project at commit <tt>{COMMIT}</tt>. The engine is complete for "
               "single-machine use; what is still open is evaluation on the full data sets (the Loghub samples "
               "used here have 2,000 lines each), hand-labelled precision and recall for the log rules, and "
               "the benchmark mode that runs the base-paper's forking design inside the engine. Accuracy "
               "figures therefore exist for the five-pattern task of the base paper, for which a labelled set was "
               "built; for the log rules we report agreement with annotated lines and with an independent "
               "reference implementation, and the effect of validation on identifiers. The correlator "
               "thresholds are the defaults of the fail2ban tool, not tuned values, and all absolute numbers "
               "depend on the machine described in Section 4.", body0))

    # ------------------------------------------------------------------ 2 Literature survey
    s.append(S("Literature Survey"))
    s.append(SS("2.1", "Automata Foundations and Regex Compilation"))
    s.append(P(f"Rabin and Scott introduced nondeterministic finite automata and the decision problems they "
               f"raise {cite('J1')}, which gives the subset construction its theoretical basis. McNaughton and "
               f"Yamada gave a procedure that turns a regular expression into an automaton {cite('J2')}. "
               f"Thompson described the search algorithm that compiles a regular expression into a "
               f"nondeterministic matcher {cite('J3')}; the construction named after him is the one used here. "
               f"For a fixed set of literal patterns, Aho and Corasick match all patterns in a single pass over "
               f"the text {cite('J4')}, the same one-pass idea that a combined DFA extends from literals to "
               f"regular expressions.", body0))
    s.append(SS("2.2", "Multi-Pattern Matching and DFA State Explosion"))
    s.append(P(f"Regular-expression matching for deep packet inspection (DPI) has a large literature. The survey "
               f"by Xu <i>et al.</i> covers applications, algorithms and hardware platforms and identifies the "
               f"growth in DFA size as the central difficulty {cite('J5')}. Responses fall into three groups. "
               f"<i>Compression:</i> D<super>2</super>FA reduces memory with default transitions "
               f"{cite('C2')}; differential encoding of DFAs keeps one transition per byte with smaller tables "
               f"{cite('J6')}; A-DFA bounds the number of default-transition hops {cite('J7')}. "
               f"<i>Hybrids and decomposition:</i> hybrid automata combine a DFA with an NFA for complex "
               f"intrusion-detection patterns {cite('C3')}, and the extensions to counting and back-references "
               f"are treated in {cite('C4')}; Liu and Norige decompose expressions to avoid the explosion "
               f"{cite('J8')}. <i>Prefiltering and engineering:</i> Bloom-filter-assisted matching for "
               f"intrusion detection {cite('J9')}, and Hyperscan, which splits each expression into literal and "
               f"automaton parts and uses SIMD matching, reporting an 8.7× speed-up for Snort on a real trace "
               f"{cite('C5')}. Backtracking engines have a different weakness: Davis <i>et al.</i> found "
               f"super-linear regular expressions in more than 10,000 JavaScript and Python modules "
               f"{cite('C6')}, which is the argument for an automaton engine whose cost is linear in the input. "
               f"Our grouping strategy (split the rule set in halves until each DFA fits a state cap) is "
               f"deliberately simpler than these techniques.", body0))
    s.append(SS("2.3", "Parallel Pattern Matching and Concurrency"))
    s.append(P(f"Luchaup <i>et al.</i> parallelise the matching of <i>one</i> input by splitting it into chunks "
               f"and speculating on the start state of each {cite('J10')}; Ko <i>et al.</i> study a speculative "
               f"parallel DFA membership test for multicore, SIMD and cloud settings {cite('J11')}, and "
               f"Mytkowicz <i>et al.</i> make finite-state machines data parallel by computing transitions for "
               f"all possible start states, reporting up to 21× on eight cores {cite('C1')}. Yang and Prasanna "
               f"show that large automata behave poorly in the cache of a multicore processor {cite('J12')}. "
               f"Log lines are already independent units, so line-level parallelism needs no speculation; this "
               f"is the option adopted here. Two performance-theory points frame the experiments: Gustafson's "
               f"scaled-speedup argument {cite('J22')}, and Lamport's work on specifying the interfaces of "
               f"concurrent program modules {cite('J23')}, which guided the hand-off between pipeline stages. "
               f"Baumann <i>et al.</i> argue that <tt>fork()</tt> has become a poor primitive for modern "
               f"systems {cite('C10')}, which is relevant to a base-paper design that forks per pattern per "
               f"input.", body0))
    s.append(SS("2.4", "Log Analysis, Datasets and Sensitive Data"))
    s.append(P(f"He <i>et al.</i> survey automated log analysis for reliability engineering {cite('J14')}. Log "
               f"parsers extract templates: Drain uses a fixed-depth tree for online parsing {cite('C7')}, and "
               f"POP parallelises parsing on a cluster {cite('J15')}. Landauer <i>et al.</i> survey clustering "
               f"for security log analysis and group approaches into overview and filtering, parsing and "
               f"signature extraction, outlier detection, and sequence anomaly detection {cite('J16')}; a "
               f"rule-based triage engine belongs to the filtering and signature group. Public data: Loghub "
               f"collects system logs from many software systems {cite('C8')}, and the AIT data sets add "
               f"labelled attack steps {cite('J17')} produced by a simulation testbed {cite('J18')}. Event "
               f"processing platforms are surveyed in {cite('J19')}. For personal data, the Text Anonymization "
               f"Benchmark provides an annotated corpus of 1,268 court cases {cite('J20')} and Lison "
               f"<i>et al.</i> review anonymisation models {cite('C9')}; check-digit schemes such as the one "
               f"studied by Gumm {cite('J21')} allow identifier-like numbers to be validated by arithmetic, "
               f"which is how the classifier here separates card and Aadhaar numbers from other digit strings.",
               body0))
    s.append(SS("2.5", "Benchmarking Methodology"))
    s.append(P(f"Hoefler and Belli reviewed 120 papers from three high-performance-computing conferences, found "
               f"that reporting of parallel performance was often inadequate, and propose rules for it "
               f"{cite('C11')}; Roodt <i>et al.</i> benchmark regular-expression engines built on Thompson and "
               f"Glushkov constructions {cite('C12')}. We follow their spirit: every data point is the median "
               f"of five runs with minimum and maximum reported, the machine is described, and nothing is "
               f"tuned to reproduce a published number.", body0))
    s.append(SS("2.6", "Summary and Research Gap"))
    s.append(table("Surveyed work and its relation to this project", [
        ["Work", "Technique", "Relation to this work"],
        [cite("J5"), "DPI regex survey", "State explosion is the key issue"],
        [cite("C2", "J6", "J7"), "DFA compression", "Smaller tables; not needed yet"],
        [cite("C3", "J8"), "Hybrid / decomposed automata", "More complex than our grouping"],
        [cite("C5"), "Literal prefilter + SIMD", "Industrial engine, comparison target"],
        [cite("C6"), "ReDoS study", "Motivates linear-time DFA"],
        [cite("J10", "J11", "C1"), "Parallel DFA on one input", "We split lines, not inputs"],
        [cite("J14", "J16"), "Log analysis surveys", "Rules = filtering / signatures"],
        [cite("J17", "C8"), "Log data sets", "Our test data"],
        [cite("J21", "J20"), "Check digits; anonymisation", "Validators; masking of PII"],
        [cite("C10", "C11"), "fork cost; benchmarking", "Base-paper analysis, method"]],
        [1.7 * cm, 3.3 * cm, 3.6 * cm]))
    s.append(P("In the surveyed work, regex automata research targets network payloads, while log research "
               "targets parsing, clustering and anomaly detection. A compiled multi-rule DFA applied to "
               "per-line triage of heterogeneous logs, combined with checksum validation, evidence fusion, "
               "line-parallel execution and windowed correlation, and evaluated against a process-parallel "
               "baseline, is the gap this project addresses.", body0))

    # ------------------------------------------------------------------ 3 Proposed system
    s.append(S("Proposed System"))
    s.append(P("The engine is one C11 program with two phases (Fig. 1). At start-up, the rule file is compiled "
               "into DFA tables by the regex front end and the DFA compiler. At run time a pipeline of threads "
               "reads log lines, normalises them, matches them with the DFA (or, for comparison, the C library), "
               "validates and extracts entities, fuses the evidence into labels, applies windowed correlation, "
               "masks personal data and writes JSON-lines records. The tables are read-only after "
               "construction and shared by all workers without locks. The evaluation harness reproduces the "
               "base paper and measures it together with our DFA. Table II gives the module split.", body0))
    rows = [["Module", "Source files", "Lines"]]
    for g, files in GROUPS:
        rows.append([g, ", ".join(files), fmt(LOC[g], 0)])
    rows.append(["Reproduction and benchmark", "tools/basepaper/ (fork, variants, runner, evaluation)",
                 fmt(TOOLS_BP, 0)])
    s.append(table("Module split", rows, [2.5 * cm, 4.8 * cm, 1.0 * cm], right=(2,)))
    s.append(P(f"The engine totals {sizes['src_lines']:,} lines of C in {sizes['src_files']} files. The front end "
               "and the DFA compiler are the compiler core; the classification module, the parallel runtime "
               "and the correlator are the new contributions of the run-time system, and all of them carry "
               "pseudocode below.", body0))

    s.append(SS("3.1", "Regex Front End"))
    s.append(P("The front end turns the text of a rule's regular expression into an abstract syntax tree (AST). The "
               "grammar is the POSIX-ERE subset—literals, <tt>.</tt>, bracket expressions, "
               "<tt>* + ? {n,m}</tt>, alternation, grouping and the anchors <tt>^ $</tt>—so the same text "
               "also runs on the C library, which makes differential testing possible. A lexer produces "
               "tokens and the parser has one function per non-terminal (Algorithm 1). Character classes are "
               "256-bit sets, and case-insensitive rules fold the set once at parse time. Every construct "
               "outside the regular subset is rejected with the byte offset of the offending character; "
               "nothing is approximated, because an approximation would silently change what a rule matches.",
               body0))
    s.append(algorithm("ParseRegex (recursive descent)", [
        (0, f"{kw('function')} Parse(<i>pattern</i>, <i>icase</i>):"),
        (1, "<i>ast</i> ← ParseAlt(); " + kw("if") + " input remains " + kw("then") + " Error(pos)"),
        (1, kw("return") + " <i>ast</i>"),
        (0, f"{kw('function')} ParseAlt():"),
        (1, "<i>kids</i> ← [ParseConcat()]"),
        (1, kw("while") + " next = '|' " + kw("do") + " append ParseConcat() to <i>kids</i>"),
        (1, kw("return") + " Alt(<i>kids</i>) if |<i>kids</i>| > 1 else <i>kids</i>[0]"),
        (0, f"{kw('function')} ParseConcat():"),
        (1, kw("while") + " next is not '|', ')' or END " + kw("do") + " append ParseRepeat()"),
        (1, kw("return") + " Concat(<i>kids</i>)  (Empty if none)"),
        (0, f"{kw('function')} ParseRepeat():"),
        (1, "<i>node</i> ← ParseAtom()"),
        (1, kw("while") + " next is a quantifier (*, +, ?, {n,m}) " + kw("do")),
        (2, kw("if") + " a '?' or '+' follows the quantifier " + kw("then") + " Error(lazy)"),
        (2, kw("if") + " {n,m}: need n ≤ m ≤ 1000, else Error"),
        (2, "<i>node</i> ← Repeat(<i>node</i>, n, m)"),
        (1, kw("return") + " <i>node</i>"),
        (0, f"{kw('function')} ParseAtom():"),
        (1, "'(' : " + kw("if") + " next = '?' " + kw("then") + " Error(lookaround);  depth ≤ 100;"),
        (2, "<i>node</i> ← ParseAlt(); expect ')'  (else Error(unbalanced))"),
        (1, "'[' : read class into a 256-bit set;  '.' : all bytes except \\n"),
        (1, "'\\' : only escaped metacharacters; \\d \\b \\1 ... → Error"),
        (1, "'^', '$' : only at pattern start / end, else Error"),
        (1, "other : literal byte (set folded if <i>icase</i>)")]))

    s.append(SS("3.2", "NFA and Combined-DFA Compiler"))
    s.append(P(f"The DFA compiler compiles the ASTs of all rules into one automaton. Thompson's construction "
               f"{cite('J3')} gives every AST node a small NFA fragment with one entry and one exit "
               f"(Algorithm 2); a bounded repetition <tt>x{{n,m}}</tt> is expanded into copies. The fragments of "
               f"all rules are joined under one start state, and each accept state is tagged with its rule "
               f"number. Subset construction {cite('J1')} then builds a DFA whose states are sets of NFA states "
               f"(Algorithm 3). Three design points matter. First, each DFA state stores an <i>accept mask</i>, "
               f"a 64-bit word whose bit <i>r</i> says that rule <i>r</i> has matched, so one pass over a line "
               f"reports every matching rule. Second, to find a match anywhere in the line, the set <i>U</i> of "
               f"start states of all non-anchored rules is added to every transition; anchors are handled by "
               f"assertion edges that only fire at offset 0 (<tt>^</tt>) or at the end of the line "
               f"(<tt>$</tt>). Third, bytes that no character set of the NFA distinguishes are merged into "
               f"byte classes, so the transition function is computed once per class. When a DFA would exceed "
               f"a state cap, the rule set is halved and each half is built separately, recursively; the group "
               f"results are OR-ed at match time (Algorithm 4).", body0))
    s.append(algorithm("Thompson construction (fragment = entry, exit)", [
        (0, f"{kw('function')} Build(<i>node</i>) → (<i>s</i>, <i>e</i>):"),
        (1, "Charset: new <i>s</i>, <i>e</i>;  <i>s</i> —set→ <i>e</i>"),
        (1, "Concat(<i>k</i><sub>1</sub>…<i>k</i><sub>n</sub>): link exit(<i>k</i><sub>i</sub>) —ε→ "
            "entry(<i>k</i><sub>i+1</sub>)"),
        (1, "Alt: split chain; <i>s</i> —ε→ entry(<i>k</i><sub>i</sub>); exit(<i>k</i><sub>i</sub>) —ε→ <i>e</i>"),
        (1, "Star: <i>s</i> —ε→ entry(<i>k</i>), <i>e</i>;  exit(<i>k</i>) —ε→ entry(<i>k</i>), <i>e</i>"),
        (1, "Plus: as Star, but <i>s</i> = entry(<i>k</i>)"),
        (1, "Quest: <i>s</i> —ε→ entry(<i>k</i>), <i>e</i>;  exit(<i>k</i>) —ε→ <i>e</i>"),
        (1, "Repeat{n,m}: n copies in sequence, then (m−n) nested optional copies"),
        (1, "^ , $ : assertion edge, usable at offset 0 / at line end only"),
        (0, f"{kw('for each')} rule <i>r</i>: (<i>s</i><sub>r</sub>, <i>e</i><sub>r</sub>) ← Build(<i>ast</i><sub>r</sub>);"
            f" tag <i>e</i><sub>r</sub> accept(<i>r</i>)"),
        (0, "start ← new state with ε-edges to every <i>s</i><sub>r</sub>")]))
    s.append(algorithm("Subset construction with accept masks and grouping", [
        (0, f"{kw('function')} BuildDfa(<i>rules</i>, <i>cap</i>):"),
        (1, "<i>U</i> ← ε-closure of the start states of rules not '^'-anchored"),
        (1, "<i>S</i><sub>0</sub> ← ε-closure of the start states of all rules, '^' allowed"),
        (1, "table ← {<i>S</i><sub>0</sub> : 0}; work ← [<i>S</i><sub>0</sub>]"),
        (1, kw("while") + " work not empty " + kw("do")),
        (2, "<i>S</i> ← pop(work)"),
        (2, kw("for each") + " byte class <i>c</i> " + kw("do")),
        (3, "<i>T</i> ← ε-closure(move(<i>S</i>, <i>c</i>)) " + kw("union") + " <i>U</i>"),
        (3, kw("if") + " <i>T</i> not in table " + kw("then")),
        (4, kw("if") + " |table| = <i>cap</i> " + kw("then return") + " TOO_BIG"),
        (4, "add <i>T</i> to table; push <i>T</i> on work"),
        (3, "next[<i>S</i>][bytes of <i>c</i>] ← id(<i>T</i>)"),
        (2, "accept[<i>S</i>] ← rules whose accept state is in <i>S</i>"),
        (2, "accept_end[<i>S</i>] ← rules reachable from <i>S</i> by ε / '$' edges"),
        (1, kw("return") + " tables"),
        (0, f"{kw('function')} BuildAll(<i>rules</i>):"),
        (1, kw("if") + " BuildDfa(<i>rules</i>) = TOO_BIG " + kw("then")),
        (2, kw("if") + " |<i>rules</i>| = 1 " + kw("then") + " Error"),
        (2, "BuildAll(first half); BuildAll(second half)")]))
    s.append(algorithm("DFA match loop", [
        (0, f"{kw('function')} Match(<i>line</i>) → mask:"),
        (1, "<i>mask</i> ← 0"),
        (1, kw("for each") + " group DFA <i>D</i> " + kw("do")),
        (2, "<i>st</i> ← 0; <i>m</i> ← accept[0]"),
        (2, kw("for each") + " byte <i>b</i> of <i>line</i> " + kw("do")),
        (3, "<i>st</i> ← next[<i>st</i>][<i>b</i>]; <i>m</i> ← <i>m</i> | accept[<i>st</i>]"),
        (2, "<i>mask</i> ← <i>mask</i> | <i>m</i> | accept_end[<i>st</i>]"),
        (1, kw("return") + " <i>mask</i>")]))

    s.append(SS("3.3", "Classification, Confidence and Masking"))
    s.append(P("A regular expression says that a span has the right <i>shape</i>; the classifier decides whether "
               "the line really belongs to the category. For rules with a validator, each candidate span is "
               "checked: the Luhn mod-10 sum for card numbers of 13–19 digits, the Verhoeff dihedral-group check "
               "for 12-digit Aadhaar numbers, and the structural rule of the Indian PAN (five letters, four "
               "digits, a letter, with a restricted fourth letter). A rule whose validator fails on every span "
               "is discarded. A surviving rule <i>r</i> contributes the evidence score "
               "<i>s<sub>r</sub></i> = base + 0.25 if a validator passed + 0.15 if a context keyword (for "
               "example <tt>card</tt> or <tt>payment</tt>) occurs on the line, clamped to [0, 1]; this follows the "
               "tiered pattern, checksum, keyword scheme of data-loss-prevention products. The confidence of a "
               "category is the noisy-OR of its rules, "
               "<i>conf</i>(<i>c</i>) = 1 − Π(1 − <i>s<sub>i</sub></i>): independent weak signals add up, one "
               "strong rule is enough. Categories with confidence ≥ τ (default 0.5) become labels, so a line "
               "can carry several; the primary label has the highest severity, then the higher confidence. "
               "Source address and user name are extracted from matched lines for the correlator, and spans "
               "of PII rules are masked before the record is written (<tt>XXXX XXXX 1234</tt> for Aadhaar, "
               "<tt>XXXX…1111</tt> for cards). Algorithms 5 and 6 give the validator and the fusion.", body0))
    s.append(algorithm("Luhn validation of a candidate span", [
        (0, f"{kw('function')} LuhnOk(<i>span</i>):"),
        (1, "<i>d</i> ← digits of <i>span</i> without ' ' and '-'; " + kw("if") + " |<i>d</i>| ∉ [13, 19] " +
            kw("return") + " false"),
        (1, "<i>sum</i> ← 0"),
        (1, kw("for") + " <i>i</i> ← 0…|<i>d</i>|−1, counting from the right " + kw("do")),
        (2, "<i>x</i> ← <i>d</i>[<i>i</i>]; " + kw("if") + " <i>i</i> odd " + kw("then") + " <i>x</i> ← 2<i>x</i>; " +
            kw("if") + " <i>x</i> > 9 " + kw("then") + " <i>x</i> ← <i>x</i> − 9"),
        (2, "<i>sum</i> ← <i>sum</i> + <i>x</i>"),
        (1, kw("return") + " <i>sum</i> mod 10 = 0")]))
    s.append(algorithm("Evidence fusion (aggregator)", [
        (0, f"{kw('function')} Decide(<i>mask</i>, <i>valid</i>, <i>ctx</i>):"),
        (1, kw("for each") + " rule <i>r</i> in <i>mask</i> " + kw("do")),
        (2, kw("if") + " <i>r</i> has a validator and <i>r</i> ∉ <i>valid</i> " + kw("then continue")),
        (2, "<i>s</i> ← base(<i>r</i>) + 0.25·[validator passed] + 0.15·[<i>ctx</i> contains a keyword of <i>r</i>]"),
        (2, "<i>c</i> ← category(<i>r</i>);  prod[<i>c</i>] ← prod[<i>c</i>]·(1 − min(<i>s</i>, 1))"),
        (2, "sev[<i>c</i>] ← max(sev[<i>c</i>], severity(<i>r</i>))"),
        (1, kw("for each") + " category <i>c</i> " + kw("do") + " conf[<i>c</i>] ← 1 − prod[<i>c</i>]"),
        (1, "labels ← {<i>c</i> : conf[<i>c</i>] ≥ τ}"),
        (1, "primary ← argmax over labels by (severity, confidence, fixed order SEC>PII>AUTH>ERR>PERF)"),
        (1, kw("return") + " (labels, primary, severity(primary))")]))

    s.append(SS("3.4", "Parallel Pipeline"))
    s.append(P("The pipeline is a chain of threads connected by lock-free single-producer single-consumer ring "
               "buffers (Fig. 1), each with exactly one writer and one reader, so a hand-off needs only acquire "
               "and release atomic operations. A reader thread splits the input into lines, fills batches (up to "
               "1,024 lines) and, as dispatcher, hands each batch to a worker by round robin or to the least "
               "loaded queue. Each worker is pinned to a core (one thread per physical core before hyper-thread "
               "siblings), owns its engine state—for the POSIX engine its own compiled <tt>regex_t</tt> set, "
               "for the DFA engine a read-only share of the tables—and classifies the batch in place. All "
               "batch buffers come from a pool allocated before the threads start and are recycled through a "
               "return ring, so no memory is allocated per line (an allocation-counting test checks this). The "
               "aggregator collects worker results, the correlator restores input order before it applies its "
               "windows, and the router writes the three output files. Backpressure is blocking by default; "
               "<tt>--drop</tt> discards and counts batches instead. Shutdown sends an end-of-input sentinel "
               "through every ring and joins all threads; any failure raises a shared stop flag so that no "
               "thread blocks forever. Algorithm 7 gives the worker and the ring.", body0))
    s.append(algorithm("SPSC ring hand-off and worker loop", [
        (0, f"{kw('function')} Push(<i>ring</i>, <i>x</i>):   // producer only"),
        (1, kw("while") + " tail − load_acquire(head) = capacity " + kw("do") + " wait (or fail if stopped)"),
        (1, "slot[tail mod capacity] ← <i>x</i>;  store_release(tail, tail + 1)"),
        (0, f"{kw('function')} Pop(<i>ring</i>):   // consumer only"),
        (1, kw("while") + " head = load_acquire(tail) " + kw("do") + " wait (or fail if stopped)"),
        (1, "<i>x</i> ← slot[head mod capacity];  store_release(head, head + 1);  " + kw("return") + " <i>x</i>"),
        (0, f"{kw('function')} Worker(<i>in</i>, <i>out</i>):"),
        (1, "pin to core; build own engine state"),
        (1, kw("loop")),
        (2, "<i>job</i> ← Pop(<i>in</i>); " + kw("if") + " <i>job</i> = EOF " + kw("then") + " Push(<i>out</i>, EOF); " +
            kw("break")),
        (2, kw("for each") + " line in <i>job</i> " + kw("do")),
        (3, "mask ← Match(normalised line);  valid ← validators on spans;  extract IP, user;  mask PII text"),
        (2, "Push(<i>out</i>, <i>job</i>)")]))

    s.append(SS("3.5", "Attack Correlation"))
    s.append(P("Some attacks are invisible in a single line: one failed login is noise, five from one address in a "
               "minute is a brute-force attack. The correlator runs on one thread behind the aggregator, so its "
               "state needs no locks. It keeps a hash table keyed by source IPv4 address; each entry holds a ring "
               "of the last 64 authentication failures (time and a hash of the user name). The time is the "
               "timestamp parsed from the log, so replays of old logs behave as real time. Four detectors "
               "use these windows: <i>brute force</i> (at least 5 failures from one address within 60 s), "
               "<i>password spraying</i> (failures against at least 5 distinct users within 300 s), "
               "<i>fail-then-success</i> (a successful login from an address that is over the brute-force "
               "threshold, severity CRITICAL) and <i>error storm</i> (at least 100 ERROR lines within 60 s, "
               "reported once). The parameter names and defaults of the first two follow fail2ban. "
               "Because workers finish out of order, the correlator holds early batches until the next expected "
               "sequence number arrives; output is therefore in input order whatever the worker count. An "
               "escalation raises the category confidence to 0.95 and reruns the label decision (Algorithm 8).",
               body0))
    s.append(algorithm("Correlator", [
        (0, f"{kw('function')} OnLine(<i>line</i>):   // lines arrive in input order"),
        (1, kw("if") + " <i>line</i> is an AUTH failure with source address <i>ip</i> " + kw("then")),
        (2, "<i>e</i> ← entry(<i>ip</i>);  push (time, hash(user)) into <i>e</i>.ring"),
        (2, "<i>n</i> ← failures in <i>e</i>.ring within 60 s of now"),
        (2, kw("if") + " <i>n</i> ≥ 5 and not already reported in this window " + kw("then") +
            " escalate(BRUTE_FORCE, HIGH, 0.95)"),
        (2, "<i>u</i> ← distinct users in <i>e</i>.ring within 300 s"),
        (2, kw("if") + " <i>u</i> ≥ 5 and not reported " + kw("then") + " escalate(PASSWORD_SPRAY, HIGH)"),
        (1, kw("if") + " <i>line</i> is a successful login from <i>ip</i> and <i>ip</i> over the brute-force " +
            "threshold " + kw("then") + " escalate(FAIL_THEN_SUCCESS, CRITICAL)"),
        (1, kw("if") + " <i>line</i> labelled ERROR " + kw("then") + " count it in a 60 s window;"),
        (2, kw("if") + " count ≥ 100 and no storm active " + kw("then") + " escalate(ERROR_STORM, HIGH)")]))

    s.append(SS("3.6", "Base-Paper Reproduction and Benchmark"))
    s.append(P("The evaluation harness rebuilds the base paper's program as described and measures it. For each input "
               "the program creates a pipe, forks one child per pattern, pins each child to a core, and the child "
               "compiles its regular expression, matches it and reports the result through the pipe "
               "(Algorithm 9); the parent collects the five records and reaps the children. The paper's "
               "input-normalisation pseudocode is reproduced as an option, and its inefficiencies are kept on "
               "purpose. For comparison, one benchmark binary offers six variants on the same five patterns: "
               "the forking design, a sequential loop that recompiles per input, a sequential loop with "
               "precompiled patterns, five threads (one pattern each), data-parallel threads (inputs split "
               "across threads), and our combined DFA with split inputs. A runner executes every experiment "
               "five times and stores the raw JSON.", body0))
    s.append(algorithm("Base-paper classification (fork per pattern)", [
        (0, f"{kw('function')} ClassifyFork(<i>text</i>):"),
        (1, "create pipe; " + kw("for") + " <i>i</i> in 0…4 " + kw("do")),
        (2, "<i>pid</i><sub>i</sub> ← fork()"),
        (2, kw("if") + " child " + kw("then")),
        (3, "pin to CPU (<i>i</i> mod ncpu); regcomp(<i>P</i><sub>i</sub>)"),
        (3, "<i>ok</i> ← regexec(<i>text</i>); regfree"),
        (3, "write (<i>i</i>, <i>ok</i>, t<sub>compile</sub>, t<sub>exec</sub>) to pipe; exit"),
        (1, kw("for") + " <i>k</i> in 1…5 " + kw("do")),
        (2, "read one record; waitpid(<i>pid</i><sub>k</sub>)"),
        (1, kw("return") + " bit mask of matched patterns")]))

    # ------------------------------------------------------------------ 4 Results
    s.append(S("Results and Implementation"))
    s.append(P(f"All results come from commit <tt>{COMMIT}</tt>, built with gcc 15.2 and "
               f"<tt>-O2 -Wall -Wextra -Wpedantic -pthread</tt> without warnings, on a 13th-generation Intel "
               f"Core i7-13650HX (14 cores) under Linux. The code base has {sizes['src_lines']:,} lines of "
               f"engine source, {sizes['test_lines']:,} lines of tests, {sizes['basepaper_tool_lines']:,} "
               f"lines of base-paper tooling and {sizes['other_tool_lines']:,} lines of other tools. "
               f"{T['unit_test_programs']} unit-test programs run {checks:,} checks with no failure, and the code "
               f"runs without reports under ThreadSanitizer and AddressSanitizer. Every measurement is stored "
               f"under <tt>results/</tt>.", body0))
    s.append(SS("4.1", "Dataset Description"))
    s.append(table("Data sets", [
        ["Data set", "Source", "Lines", "Use"],
        ["OpenSSH_2k", "Loghub sample", f"{corp['OpenSSH_2k.log']['lines']:,}", "AUTH rules, correlator"],
        ["Apache_2k", "Loghub sample", f"{corp['Apache_2k.log']['lines']:,}", "web-server log"],
        ["Linux_2k", "Loghub sample", f"{corp['Linux_2k.log']['lines']:,}", "syslog / PAM, PII"],
        ["BGL_2k", "Loghub sample", f"{corp['BGL_2k.log']['lines']:,}", "ERROR rules, error storm"],
        ["HDFS_2k", "Loghub sample", f"{corp['HDFS_2k.log']['lines']:,}", "identifiers vs. PII"],
        ["synthetic_small", "hand-written", f"{corp['synthetic_small.log']['lines']}",
         f"{T['annotated_lines_synthetic_small']} annotated lines: positives, near-misses, correlator cases"],
        ["labelled set", "generated, fixed seed", f"{lab:,}", "five base-paper categories"]],
        [2.3 * cm, 2.3 * cm, 1.0 * cm, 3.0 * cm], right=(2,)))
    s.append(P(f"The first five are the 2,000-line samples published with Loghub {cite('C8')}. They carry no "
               f"labels, so for them we report what the engine finds. The hand-written corpus annotates each "
               f"line with the exact set of rules that must match it, including look-alikes such as a "
               f"12-digit order number or the word “errors”, and contains scripted attacks for the correlator. "
               f"The labelled set of {lab:,} inputs is generated from templates with a fixed seed, balanced over "
               f"e-mail, phone, question, greeting and name plus none, multi-category and near-miss lines; its "
               f"ground truth describes what a line <i>is</i>, not what the regexes match.", body0))

    s.append(SS("4.2", "Regex Front End"))
    rej = [("a\\d+", r"\d shorthand not supported (write [0-9])", 1), ("a\\bb", r"\b word boundary not supported", 1),
           ("(a)\\1", "backreference \\1 (not a regular language)", 3),
           ("(?=a)b", "\"(?\" groups: lookaround not supported", 0),
           ("a*?b", "lazy / possessive quantifier", 2), ("a{,3}", "{,n} not supported; write {0,n}", 1),
           ("a^b", "'^' only allowed at pattern start", 1), ("(ab", "unbalanced '('", 0),
           ("a{2,1}", "invalid interval, m &lt; n", 1)]
    s.append(table("Rejected syntax (message and byte offset)", [["Pattern", "Compiler message", "Offset"]] +
                   [[f"<font name='Mono' size='7'>{R.escape(p)}</font>", R.escape(m).replace("&amp;lt;", "&lt;"),
                     str(o)] for p, m, o in rej],
                   [1.5 * cm, 6.0 * cm, 1.0 * cm], right=(2,)))
    s.append(P(f"All 26 default rules parse. The parser test has {T['unit_test_checks']['test_parse']} checks over "
               f"valid and invalid patterns. Table IV shows that every unsupported construct is refused with a "
               f"message that names a regular alternative where one exists. <i>Inference:</i> the front end "
               f"enforces the regular subset strictly; a rule author is told how to rewrite the rule instead of "
               f"getting silently different behaviour.", body0))

    s.append(SS("4.3", "NFA and DFA Compiler"))
    s.append(P(f"The 26 rules give a combined NFA of {M2['state_sweep'][0]['nfa_states']:,} states. Table V and "
               f"Fig. 2 show what the state cap does. Without a cap the combined DFA has {single:,} states and "
               f"takes {sw[1000000]['build_ms']:.0f} ms to build; with the default cap of 20,000 it is "
               f"split into {grouped['groups']} groups with {grouped['dfa_states']:,} states in total "
               f"({grouped['build_ms']:.0f} ms), a {single / grouped['dfa_states']:.1f}× reduction in table "
               f"states. Tighter caps give more groups: at 500 states the DFA needs "
               f"{sw[500]['groups']} groups of {sw[500]['dfa_states']:,} states in total, built in "
               f"{sw[500]['build_ms']:.0f} ms.", body0))
    s.append(table("DFA size versus state cap (26 rules)", [["Cap", "Groups", "DFA states", "Build (ms)"]] + [
        [f"{r['max_states']:,}", str(r["groups"]), f"{r['dfa_states']:,}", f"{r['build_ms']:.0f}"]
        for r in M2["state_sweep"] if r["max_states"] != 100000],
        [2.0 * cm, 1.6 * cm, 2.2 * cm, 2.0 * cm], right=(0, 1, 2, 3)))
    order = [500, 1000, 2000, 5000, 20000, 1000000]
    grp = [(("1M" if k == 1000000 else f"{k // 1000}K" if k >= 1000 else str(k)),
            [sw[k]["dfa_states"] / 1000]) for k in order]
    s.append(figure(vbars(grp, [BLUE], COL_W, 100, 50, "DFA states (thousands)", "{:.1f}", legend=False,
                          top_text=[f"{sw[k]['groups']} grp" for k in order]),
                    "Total DFA states against the state cap; the label above each bar is the number of groups."))
    s.append(P(f"<i>Correctness.</i> The differential tester compares the DFA's rule mask with the C library's "
               f"for every rule on every line of the {dt['files']} corpus files ({dt['file_lines']:,} lines) "
               f"and on {dt['random_strings']:,} generated strings: <b>{dt['mismatches']} mismatches</b>. The "
               f"DFA test ({T['unit_test_checks']['test_dfa']:,} checks) additionally compares the DFA "
               f"with an NFA simulator, and the NFA test ({T['unit_test_checks']['test_nfa']:,} checks) "
               f"compares that simulator with <tt>regexec</tt>. <i>Inference:</i> the combined DFA is "
               f"semantically equal to the reference, and grouping trades a few more table lookups per byte "
               f"(one pass per group) for much smaller tables.", body0))

    s.append(SS("4.4", "Classification, Confidence and Masking"))
    rows = [["File", "Lines", "Rule hits", "Primary label counts"]]
    short = {"AUTH_FAILURE": "AUTH", "SECURITY_THREAT": "SEC", "ERROR": "ERR", "PERF_WARNING": "PERF", "PII": "PII"}
    for c in M2["corpus"]:
        pl = ", ".join(f"{short[k]} {v}" for k, v in sorted(c["primary_label_counts"].items(),
                                                          key=lambda kv: -kv[1])) or "none"
        rows.append([c["file"].replace(".log", ""), f"{c['lines']:,}", f"{c['lines_with_rule_match']:,}", pl])
    s.append(table("Classification output", rows, [2.4 * cm, 1.0 * cm, 1.3 * cm, 3.9 * cm], right=(1, 2)))
    s.append(P(f"Table VI shows the distribution on real logs: OpenSSH is dominated by authentication failures "
               f"({corp['OpenSSH_2k.log']['primary_label_counts']['AUTH_FAILURE']:,} of "
               f"{corp['OpenSSH_2k.log']['lines']:,} lines) and BGL by errors. The Apache sample produces no "
               f"hit: its lines are routine requests and none matches a rule, which the rule set is designed to do. "
               f"The classifier agrees with an independent Python reimplementation of the same specification on "
               f"primary label, label set, severity, confidence and flags for every line of all six files, and "
               f"with the annotations of all {T['annotated_lines_synthetic_small']} annotated lines of the "
               f"hand-written corpus. Validation is the main change from a pattern-only classifier. Table VII "
               f"compares the number of lines labelled PII before validators existed (commit 8093e0a) and now.",
               body0))
    pii = [["File", "Pattern only", "With validation"]]
    for f in ("HDFS_2k.log", "Linux_2k.log", "BGL_2k.log"):
        pii.append([f.replace(".log", ""), str(M1C[f]["primary_label_counts"].get("PII", 0)),
                    str(corp[f]["primary_label_counts"].get("PII", 0))])
    s.append(table("Lines labelled PII, before and after validation", pii, [2.4 * cm, 2.4 * cm, 2.6 * cm],
                   right=(1, 2)))
    s.append(P(f"<i>Inference.</i> HDFS block identifiers such as <tt>blk_278357163850888</tt> are long digit "
               f"strings that match the card and Aadhaar shapes; the checksum rejects almost all of them "
               f"({M1C['HDFS_2k.log']['primary_label_counts']['PII']} → "
               f"{corp['HDFS_2k.log']['primary_label_counts']['PII']} lines). The few survivors are expected: a "
               f"random 13–19 digit string passes the Luhn test with probability about 1 in 10, and an all-zero BIOS "
               f"address in the Linux sample passes trivially. The Linux e-mail hit is a build-system address in a "
               f"kernel banner; it is a correct match of the PII pattern, which the policy treats as PII. The "
               f"validator therefore removes false positives but cannot reach zero, which is why confidence is "
               f"reported (a pattern-only card match scores 0.35, validated 0.60, with a keyword 0.75). Masked "
               f"values reach the quarantine file and the store; the raw digits are never written.", body0))

    s.append(SS("4.5", "Parallel Pipeline"))
    s.append(P(f"Correctness comes first. A comparison script (<tt>make compare</tt>) runs every corpus file with "
               f"both engines at 1, 2, 4 and 8 workers, with a small batch size, with the least-loaded "
               f"dispatcher and with input from a pipe, and requires all three output files to be equal, record "
               f"by record, to the single-worker run: 72 runs, 0 differences. The single-worker output is in turn "
               f"equal to the independent Python reference. ThreadSanitizer reports no data race on the test suite, the "
               f"end-to-end tests or the comparison sweep, and an allocation-counting test shows that the number of "
               f"allocations does not depend on the input size (it is the same for 100,000 and 500,000 lines at "
               f"one worker). Table VIII gives the throughput against the worker count on {tp['lines']:,} "
               f"lines.", body0))
    rows = [["Workers", "DFA (lines/s)", "× vs 1", "POSIX (lines/s)", "× vs 1"]]
    for w in (1, 2, 4, 8):
        rows.append([str(w), si(sc["dfa"][str(w)]["lines_per_s_median"]), f"{sp('dfa', w):.2f}",
                     si(sc["posix"][str(w)]["lines_per_s_median"]), f"{sp('posix', w):.2f}"])
    s.append(table("Throughput against worker count (median of 5 runs)", rows,
                   [1.4 * cm, 2.2 * cm, 1.2 * cm, 2.4 * cm, 1.2 * cm], right=(0, 1, 2, 3, 4)))
    ser = [("POSIX engine", [(w, sp("posix", w)) for w in (1, 2, 4, 8)], ORANGE, None),
           ("DFA engine", [(w, sp("dfa", w)) for w in (1, 2, 4, 8)], BLUE, None)]
    s.append(figure(line_chart(ser, COL_W, 98, [1, 2, 4, 8], 8, "workers", "speed-up vs. 1 worker"),
                    "Speed-up of the whole pipeline against one worker (median of 5 runs); dotted line = ideal."))
    s.append(P(f"<i>Inference.</i> The POSIX engine scales: {sp('posix', 8):.1f}× at eight workers, because "
               f"each line costs 26 library calls and the matching dominates. The DFA engine does not: its single "
               f"worker already runs at {si(sc['dfa']['1']['lines_per_s_median'])} lines/s, and adding workers "
               f"leaves it flat or slightly lower ({sp('dfa', 8):.2f}× at eight). Matching is so cheap that the "
               f"serial stages—the reader, the aggregator, the correlator and the router, each on one "
               f"thread—limit the pipeline at roughly a million lines per second. This is the expected shape "
               f"of Amdahl's law, not a defect in the queues: the figure also shows that the DFA engine at one "
               f"worker is faster than the POSIX engine at eight "
               f"({si(sc['dfa']['1']['lines_per_s_median'])} against "
               f"{si(sc['posix']['8']['lines_per_s_median'])} lines/s). Moving work out of the serial "
               f"stages (for example batch-level masking and output formatting) is the next optimisation, "
               f"and we do not claim DFA scaling beyond what is measured.", body0))

    s.append(SS("4.6", "Attack Correlation"))
    s.append(table("Correlator results on the real samples", [
        ["File", "Brute force", "Spray", "Error storm", "Alerts"],
        ["OpenSSH_2k", str(ssh["flags"].get("ESCALATED_BRUTE_FORCE", 0)), str(ssh["flags"].get("ESCALATED_PASSWORD_SPRAY", 0)),
         "0", str(ssh["alerts"])],
        ["BGL_2k", "0", "0", str(bgl["flags"].get("ERROR_STORM", 0)), str(bgl["alerts"])],
        ["synthetic_small", str(cor["synthetic_small.log"]["flags"].get("ESCALATED_BRUTE_FORCE", 0)),
         str(cor["synthetic_small.log"]["flags"].get("ESCALATED_PASSWORD_SPRAY", 0)), "0",
         str(cor["synthetic_small.log"]["alerts"])]],
        [2.5 * cm, 1.5 * cm, 1.1 * cm, 1.6 * cm, 1.2 * cm], right=(1, 2, 3, 4)))
    s.append(P(f"Table IX lists the detections. The unit tests (<tt>{T['unit_test_checks']['test_correlate']:,}</tt> checks) cover the cases of "
               f"the specification: 4 failures give nothing, 5 inside the window give exactly one escalation, 5 "
               f"spread over more than the window give nothing, and a success after a burst is CRITICAL. On the "
               f"real OpenSSH sample the correlator reports {ssh['flags']['ESCALATED_BRUTE_FORCE']} brute-force and "
               f"{ssh['flags']['ESCALATED_PASSWORD_SPRAY']} password-spray episodes, all of which appear in the "
               f"alert file, and one error storm in the BGL sample; a second implementation of the windows in "
               f"Python agrees on every line. <i>Inference:</i> without the correlator these lines are individual "
               f"MEDIUM authentication failures; escalation turns a pattern over time into one HIGH alert per "
               f"episode. Whether each episode is a true attack cannot be said without labels; the Loghub "
               f"OpenSSH sample is known to contain a long scan, and the next step is to score the alerts against "
               f"the labelled AIT data set.", body0))

    s.append(SS("4.7", "Base-Paper Reproduction"))

    def p50(variant):
        by_in = {}
        for x in RB1:
            if x["variant"] == variant:
                by_in[x["input_index"]] = (x["input"], x["p50_ns"]["median"] / 1000.0)
        return by_in
    v = {n: p50(n) for n in ("fork", "seq-recompile", "seq-precompiled", "threads-pattern", "dfa_t1")}
    rows = [["Input", "fork", "seq-rec.", "seq-pre.", "thr-pat.", "DFA"]]
    for i in sorted(v["fork"]):
        name = v["fork"][i][0]
        name = (name[:14] + "…") if len(name) > 15 else name
        rows.append([f"<font name='Mono' size='6.8'>{R.escape(name)}</font>"] +
                    [(f"{v[k][i][1]:.0f}" if v[k][i][1] >= 100 else f"{v[k][i][1]:.3g}")
                     for k in ("fork", "seq-recompile", "seq-precompiled", "threads-pattern", "dfa_t1")])
    s.append(table("Latency per input, median (µs)", rows,
                   [2.6 * cm, 1.1 * cm, 1.2 * cm, 1.2 * cm, 1.2 * cm, 1.1 * cm], right=(1, 2, 3, 4, 5)))
    fk = [v["fork"][i][1] for i in v["fork"]]
    s.append(P(f"The reproduced program classifies one input in {min(fk):.0f}–{max(fk):.0f} µs, while a "
               f"sequential loop that recompiles the five patterns needs "
               f"{min(v['seq-recompile'][i][1] for i in v['fork']):.0f}–"
               f"{max(v['seq-recompile'][i][1] for i in v['fork']):.0f} µs (Table X). Inside each child, "
               f"<tt>regcomp</tt> takes about 20 µs and <tt>regexec</tt> about 3–5 µs, so process management "
               f"is most of the {min(fk):.0f} µs. The paper's input module also rejects "
               f"{RB4['paper']['rejected_by_input_module']} of the {lab:,} labelled lines (for example those "
               f"containing '+' or an apostrophe), because its character whitelist is narrower than the "
               f"inputs. <i>Inference:</i> the paper's central hypothesis—that process overhead dominates—is "
               f"confirmed, and it works against the paper's own speed-up claim.", body0))

    # ------------------------------------------------------------------ 5 Performance
    s.append(S("Performance and Metrics"))
    s.append(SS("5.1", "Matching Engine: Existing versus Proposed"))
    spread = max((tp[e]["lines_per_s_max"] - tp[e]["lines_per_s_min"]) / tp[e]["lines_per_s_median"]
                 for e in ("dfa", "posix")) * 100
    extra = tp["dfa"]["median_startup_s"] - tp["posix"]["median_startup_s"]
    breakeven = extra / (1 / pos_lps - 1 / dfa_lps)
    s.append(P(f"We compare the C library's POSIX engine (one <tt>regexec</tt> per rule, the approach of the "
               f"base paper's matching step) with the proposed combined DFA. Both run the same 26 rules inside "
               f"the same pipeline with one worker on {tp['lines']:,} log lines ({tp['bytes'] / 1e6:.0f} MB, the "
               f"six corpus files repeated 100 times), output written to memory-backed files; the start-up "
               f"cost of each engine (rule compilation) is measured separately on an empty input and "
               f"subtracted. Five runs per engine were interleaved.", body0))
    s.append(figure(hbar_err([("POSIX regexec", pos_lps, tp["posix"]["lines_per_s_min"],
                               tp["posix"]["lines_per_s_max"], ORANGE),
                              ("Combined DFA", dfa_lps, tp["dfa"]["lines_per_s_min"],
                               tp["dfa"]["lines_per_s_max"], BLUE)],
                             COL_W, 62, "lines per second (median of 5 runs, whiskers = min–max)", 1.2e6),
                    "End-to-end throughput of the two engines on real log rules, one worker."))
    s.append(P(f"The DFA engine sustains {si(dfa_lps)} lines/s and the POSIX engine {si(pos_lps)} lines/s, "
               f"a factor of <b>{speed:.1f}</b>; the spread between the five runs is at most {spread:.0f} % of the "
               f"median. <i>Inference:</i> in a loop of 26 library calls every line is scanned once per rule, and "
               f"the library's cost grows with the rule count; the DFA scans the line once whatever the rule count. "
               f"The factor is a lower bound on the matching speed-up, because the measurement includes the "
               f"unchanged reader, classifier and output. The price is the start-up cost: building the DFAs "
               f"takes {tp['dfa']['median_startup_s'] * 1000:.0f} ms against "
               f"{tp['posix']['median_startup_s'] * 1000:.0f} ms for compiling the library's regexes, which "
               f"is repaid after about {breakeven / 1000:.0f} thousand lines.", body0))

    s.append(SS("5.2", "Base-Paper Design versus Alternatives"))
    sp3 = {}
    for x in R.RB3:
        sp3.setdefault(x["variant"], {})[x["k"]] = x["speedup_vs_same_variant_k1"]
    spreads = {x["variant"]: (x["inputs_per_s"]["max"] - x["inputs_per_s"]["min"]) / x["inputs_per_s"]["median"] * 100
               for x in RB2 if x["batch"] == 10000}
    low = sorted(q for q in spreads.values() if q <= 10)
    high = sorted(q for q in spreads.values() if q > 10)
    s.append(P(f"Fig. 5 compares six designs on the five-pattern task for a batch of 10,000 inputs (median of "
               f"five runs). The run-to-run spread (maximum minus minimum, relative to the median) is at most "
               f"{max(low):.0f} % for {len(low)} designs and {high[0]:.0f} % and {high[1]:.0f} % for the two "
               f"fastest, whose 10,000-input runs last only a few milliseconds or less and are dominated by "
               f"timing noise; the scaling runs therefore use longer runs, up to 10 million inputs.", body0))
    rows = [("fork (paper)", thr["fork"], ORANGE), ("sequential, recompile", thr["seq-recompile"], GREY),
            ("5 threads, one per pattern", thr["threads-pattern"], GREY),
            ("sequential, precompiled", thr["seq-precompiled"], GREY),
            ("threads over inputs (5)", thr["threads-data_t5"], GREEN),
            ("DFA, threads over inputs (5)", thr["dfa_t5"], BLUE)]
    s.append(figure(hbar_log([(a_, b_["median"], b_["min"], b_["max"], c_) for a_, b_, c_ in rows], COL_W, 100,
                             "inputs per second (log scale)", 1e3, 2e8,
                             [(1e3, "1K"), (1e4, "10K"), (1e5, "100K"), (1e6, "1M"), (1e7, "10M"), (1e8, "100M")]),
                    "Throughput of six designs for 10,000 inputs (existing: first row; proposed: last row)."))
    kmax = lambda var: max(sp3[var][k] for k in sp3[var])
    tdata_vs_pre = thr["threads-data_t5"]["median"] / thr["seq-precompiled"]["median"]
    dfa_vs_pre = thr["dfa_t5"]["median"] / thr["seq-precompiled"]["median"]
    s.append(P(f"The forking design reaches {si(thr['fork']['median'])} inputs/s, which is "
               f"{fork_vs_rec:.1f}× slower than a sequential loop that recompiles and {fork_vs_pre:.0f}× slower "
               f"than one with precompiled patterns. Splitting the <i>inputs</i> across five "
               f"threads gives {tdata_vs_pre:.1f}× the sequential precompiled loop, and our DFA with the same "
               f"split {dfa_vs_pre:.1f}×. The fork design peaks at {kmax('fork'):.2f}× and per-pattern threads "
               f"at {kmax('threads-pattern'):.2f}× when cores are added, because five patterns cannot give more "
               f"than five-fold parallelism and the setup cost is paid per input; data-parallel threads reach "
               f"{max(sp3['threads-data'].values()):.2f}× and the DFA variant {max(sp3['dfa'].values()):.2f}× at "
               f"six cores. <i>Inference:</i> the base paper's design cannot deliver near-linear scaling on this "
               f"hardware; parallelism over input items does, which is the model our pipeline uses.", body0))

    s.append(SS("5.3", "Accuracy: Existing versus Proposed"))
    s.append(P(f"On the labelled set of {lab:,} inputs we compare the base paper's pipeline (its input module "
               f"followed by <tt>regexec</tt>; <i>existing</i>) with matching on the unmodified text with the "
               f"same five patterns, as done by the DFA variant (<i>proposed</i>). The DFA gives the same masks as "
               f"<tt>regexec</tt> on every line (asserted by a test), so any difference comes from the paper's "
               f"input module. Table XI and Fig. 6 give precision (P), recall (R) and F1 per category.", body0))
    cats = ["EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"]
    rows = [["Category", "P<br/>exist.", "R<br/>exist.", "F1<br/>exist.", "P<br/>prop.", "R<br/>prop.", "F1<br/>prop."]]
    for c in cats:
        pa, pb = a["per_category"][c], b["per_category"][c]
        nz = lambda q: "—" if q is None else f"{q:.2f}"
        rows.append([c.title()] + [nz(pa[k]) for k in ("precision", "recall", "f1")] +
                    [nz(pb[k]) for k in ("precision", "recall", "f1")])
    rows.append(["<b>Exact match</b>", "", "", f"{a['overall']['exact_match_accuracy']:.3f}", "", "",
                 f"{b['overall']['exact_match_accuracy']:.3f}"])
    rows.append(["<b>Label accuracy</b>", "", "", f"{a['overall']['label_accuracy']:.3f}", "", "",
                 f"{b['overall']['label_accuracy']:.3f}"])
    s.append(table("Accuracy on the labelled set", rows,
                   [1.9 * cm, 1.1 * cm, 1.1 * cm, 1.1 * cm, 1.1 * cm, 1.1 * cm, 1.1 * cm], right=(1, 2, 3, 4, 5, 6)))
    f1 = [(c.title(), [a["per_category"][c]["f1"], b["per_category"][c]["f1"]]) for c in cats]
    s.append(figure(vbars(f1, [ORANGE, BLUE], COL_W, 92, 1.0, "F1 score", "{:.2f}",
                          series_names=["existing (input module + regexec)", "proposed (DFA, raw text)"]),
                    "F1 per category, existing pipeline versus proposed matching."))
    s.append(P(f"With the proposed matching, exact-set accuracy is {b['overall']['exact_match_accuracy']:.3f} "
               f"against {a['overall']['exact_match_accuracy']:.3f} for the existing pipeline, and label "
               f"accuracy {b['overall']['label_accuracy']:.3f} against {a['overall']['label_accuracy']:.3f}. "
               f"The existing input module lowercases text, so the name pattern, which needs capital letters, "
               f"can never match (recall {a['per_category']['NAME']['recall']:.2f}), and it rejects "
               f"{a['rejected_by_input_module']} inputs. Without it, e-mail is nearly perfect (F1 "
               f"{b['per_category']['EMAIL']['f1']:.2f}) while question, greeting and name are lower "
               f"({b['per_category']['QUESTION']['f1']:.2f}, {b['per_category']['GREETING']['f1']:.2f}, "
               f"{b['per_category']['NAME']['f1']:.2f}): structured formats are easier for regular "
               f"expressions than natural language, as the paper claims (absolute values depend on our "
               f"templates). For the log rules themselves, precision against human labels is not yet measured; "
               f"the evidence so far is the validator effect of Table VII (119 to 1 PII labels on HDFS), "
               f"agreement with the reference classifier on all "
               f"{sum(c['lines'] for c in M2['corpus']):,} lines, and exact agreement with the "
               f"{T['annotated_lines_synthetic_small']} annotated lines. <i>Inference:</i> the proposed approach does not "
               f"lose accuracy relative to the library; any accuracy gain comes from not applying a lossy input "
               f"stage and from validating matched numbers.", body0))
    s.append(KeepTogether([SS("5.4", "Summary of Metrics")] + table("Existing versus proposed", [
        ["Metric", "Existing", "Proposed"],
        ["Log-rule throughput, 1 worker", f"{si(pos_lps)} lines/s", f"{si(dfa_lps)} lines/s"],
        ["Throughput, 8 workers", f"{si(sc['posix']['8']['lines_per_s_median'])} lines/s (POSIX)",
         f"{si(sc['dfa']['8']['lines_per_s_median'])} lines/s (DFA)"],
        ["Inputs/s, 5 patterns, batch 10K", f"{si(thr['fork']['median'])} (fork)",
         f"{si(thr['dfa_t5']['median'])} (DFA)"],
        ["Mask agreement with regexec", "(reference)", f"{dt['mismatches']} mismatches"],
        ["PII labels on HDFS_2k", f"{M1C['HDFS_2k.log']['primary_label_counts']['PII']} (pattern only)",
         f"{corp['HDFS_2k.log']['primary_label_counts']['PII']} (validated)"],
        ["Exact-set accuracy, 5 categories", f"{a['overall']['exact_match_accuracy']:.3f}",
         f"{b['overall']['exact_match_accuracy']:.3f}"]],
        [3.2 * cm, 2.6 * cm, 2.6 * cm], flat=True)))

    # ------------------------------------------------------------------ 6 Conclusion
    s.append(S("Conclusion"))
    s.append(P(f"We built a real-time log triage engine around a regex-to-DFA compiler written from first "
               f"principles. The compiler agrees with the C library on {dt['file_lines']:,} corpus lines and "
               f"{dt['random_strings']:,} random strings; on 26 rules the DFA engine is {speed:.1f}× faster than "
               f"the library, grouping keeps the tables at {grouped['dfa_states']:,} states instead of "
               f"{single:,}, checksum validation removes nearly all false PII labels from identifier-heavy logs, "
               f"the multi-threaded pipeline reproduces the sequential output exactly on 72 comparison runs, and "
               f"the correlator turns {ssh['flags']['ESCALATED_BRUTE_FORCE']} bursts of failed logins in a "
               f"real SSH log into single alerts. The measured reproduction of the base paper shows that its "
               f"process-per-pattern design is {fork_vs_rec:.0f}× slower than a sequential loop, while "
               f"parallelism over inputs scales.", body0))
    s.append(P(f"The measurements also show limits. DFA start-up takes {tp['dfa']['median_startup_s'] * 1000:.0f} ms; "
               f"the DFA engine's pipeline throughput is capped near a million lines per second by the serial "
               f"stages and does not rise with workers; and accuracy on the log rules is supported by "
               f"agreement tests, not yet by human-labelled precision and recall. The remaining work is the "
               f"in-binary benchmark mode and generator, the full data sets (Loghub, AIT) with hand-labelled "
               f"samples, and the evaluation experiments that give precision, recall, calibration and the "
               f"effect of the rule count.", body))

    s.append(Paragraph("REFERENCES", sec))
    for i, k in enumerate(R.ORDER, 1):
        s.append(Paragraph(f"[{i}]&nbsp;&nbsp;{R.REFS[k]}", ref))
    return s


def main():
    with tempfile.TemporaryDirectory() as tmp:
        arch = os.path.join(tmp, "arch.png")
        subprocess.run(["dot", "-Tpng", "-Gdpi=300", os.path.join(ROOT, "tools/report/r2_arch.dot"), "-o", arch],
                       check=True)
        iw, ih = ImageReader(arch).getSize()
        fig_h = FULL_W * ih / iw + 1.4 * cm
        doc = R.build_doc(fig_h)
        doc.title = "Real-Time Parallel Log / Event Triage Engine: Project Report"
        doc.subject = "Compiler Design project report (IEEE format), state at commit " + COMMIT
        arch_fig = figure(image(arch, FULL_W), "System architecture: start-up compilation, parallel triage "
                          "pipeline and evaluation harness.", wide=True)
        body_flow = content()
        idx = body_flow.index(R.FIGSLOT)
        full = [NextPageTemplate("figpage"), PageBreak(), arch_fig, FrameBreak(), NextPageTemplate("later")]
        story = title_block() + [NextPageTemplate("later")] + body_flow[:idx] + full + body_flow[idx + 1:]
        doc.build(story)
    print(OUT, "references:", len(R.ORDER))


if __name__ == "__main__":
    main()
