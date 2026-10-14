#!/usr/bin/env python3
"""Build docs/Review2_Report.pdf (Compiler Design, Review 2).

Needs Graphviz (`dot`) and the reportlab package, e.g.
    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install reportlab
    /tmp/venv/bin/python tools/report/make_review2.py
Figures: tools/report/compiler.dot, tools/report/runtime.dot, and the NFA/DFA of
tests/fixtures/tiny.rules exported by `bin/triage --dump-dot` (run `make` first).
Every number in the text comes from the repository (test output, saved results in
results/basepaper/, line counts); nothing is estimated.
"""
import os
import subprocess
import sys
import tempfile

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import cm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph, SimpleDocTemplate,
                                Spacer, Table, TableStyle)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "docs", "Review2_Report.pdf")

# ---- fonts and styles -------------------------------------------------------------
LIB = "/usr/share/fonts/truetype/liberation/"
pdfmetrics.registerFont(TTFont("Sans", LIB + "LiberationSans-Regular.ttf"))
pdfmetrics.registerFont(TTFont("Sans-Bold", LIB + "LiberationSans-Bold.ttf"))
pdfmetrics.registerFont(TTFont("Sans-Italic", LIB + "LiberationSans-Italic.ttf"))
pdfmetrics.registerFont(TTFont("Sans-BoldItalic", LIB + "LiberationSans-BoldItalic.ttf"))
pdfmetrics.registerFont(TTFont("Mono", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"))
pdfmetrics.registerFontFamily("Sans", normal="Sans", bold="Sans-Bold", italic="Sans-Italic",
                              boldItalic="Sans-BoldItalic")

NAVY = colors.HexColor("#1f2d4d")
ACCENT = colors.HexColor("#3b5b9a")
LIGHT = colors.HexColor("#eef2f8")
RULE = colors.HexColor("#c5cddc")
GREY = colors.HexColor("#5a6270")

body = ParagraphStyle("body", fontName="Sans", fontSize=10.2, leading=14.2, alignment=TA_JUSTIFY,
                      spaceAfter=6)
small = ParagraphStyle("small", parent=body, fontSize=8.8, leading=11.4, alignment=0, spaceAfter=0)
cell = ParagraphStyle("cell", parent=body, fontSize=8.8, leading=11.2, alignment=0, spaceAfter=0)
cellb = ParagraphStyle("cellb", parent=cell, fontName="Sans-Bold")
head = ParagraphStyle("head", parent=cell, fontName="Sans-Bold", textColor=colors.white)
h1 = ParagraphStyle("h1", fontName="Sans-Bold", fontSize=15, leading=19, textColor=NAVY,
                    spaceBefore=10, spaceAfter=8)
h2 = ParagraphStyle("h2", fontName="Sans-Bold", fontSize=11.5, leading=15, textColor=ACCENT,
                    spaceBefore=8, spaceAfter=4)
caption = ParagraphStyle("caption", parent=body, fontName="Sans-Italic", fontSize=8.8, leading=11,
                         alignment=TA_CENTER, textColor=GREY, spaceBefore=3, spaceAfter=10)
bullet = ParagraphStyle("bullet", parent=body, leftIndent=14, bulletIndent=3, spaceAfter=3)


def P(text, style=body):
    return Paragraph(text, style)


def bullets(items, style=bullet):
    return [Paragraph(t, style, bulletText="•") for t in items]


def code(t):
    return f'<font name="Mono" size="8.6">{t}</font>'


def table(rows, widths, header=True, zebra=True, bold_first_col=False):
    data = []
    for i, r in enumerate(rows):
        out = []
        for j, c in enumerate(r):
            if isinstance(c, str):
                st = head if (header and i == 0) else (cellb if bold_first_col and j == 0 else cell)
                c = Paragraph(c, st)
            out.append(c)
        data.append(out)
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    style = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 3.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 3.5),
        ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
        ("BOX", (0, 0), (-1, -1), 0.6, RULE),
    ]
    if header:
        style += [("BACKGROUND", (0, 0), (-1, 0), NAVY)]
    if zebra:
        for i in range(1 if header else 0, len(rows)):
            if i % 2 == 0:
                style.append(("BACKGROUND", (0, i), (-1, i), LIGHT))
    t.setStyle(TableStyle(style))
    return t


def figure(path, width, text):
    from reportlab.lib.utils import ImageReader
    iw, ih = ImageReader(path).getSize()
    img = Image(path, width=width, height=width * ih / iw)
    return KeepTogether([img, P(text, caption)])


# ---- figures ------------------------------------------------------------------------
def render_figures(tmp):
    figs = {}
    for name in ("compiler", "runtime"):
        out = os.path.join(tmp, name + ".png")
        subprocess.run(["dot", "-Tpng", "-Gdpi=250", os.path.join(ROOT, "tools/report", name + ".dot"),
                        "-o", out], check=True)
        figs[name] = out
    dotf = os.path.join(tmp, "tiny.dot")
    subprocess.run([os.path.join(ROOT, "bin/triage"), "--quiet", "-r",
                    os.path.join(ROOT, "tests/fixtures/tiny.rules"), "--dump-dot", dotf], check=True)
    figs["tiny"] = os.path.join(tmp, "tiny.png")
    subprocess.run(["dot", "-Tpng", "-Gdpi=220", dotf, "-o", figs["tiny"]], check=True)
    return figs


# ---- page decoration -------------------------------------------------------------------
def on_page(canvas, doc):
    canvas.saveState()
    w, h = A4
    canvas.setStrokeColor(RULE)
    canvas.setLineWidth(0.6)
    canvas.line(2 * cm, 1.55 * cm, w - 2 * cm, 1.55 * cm)
    canvas.setFont("Sans", 8)
    canvas.setFillColor(GREY)
    canvas.drawString(2 * cm, 1.1 * cm, "Real-Time Parallel Log / Event Triage Engine  ·  Review 2  ·  Team 1")
    canvas.drawRightString(w - 2 * cm, 1.1 * cm, f"Page {doc.page}")
    canvas.restoreState()


def on_first_page(canvas, doc):
    canvas.saveState()
    w, h = A4
    canvas.setFillColor(NAVY)
    canvas.rect(0, h - 1.2 * cm, w, 1.2 * cm, stroke=0, fill=1)
    canvas.rect(0, 0, w, 0.6 * cm, stroke=0, fill=1)
    canvas.restoreState()


# ---- content ---------------------------------------------------------------------------
def title_page():
    s = []
    s.append(Spacer(1, 3.2 * cm))
    s.append(P("COMPILER DESIGN  ·  PROJECT REVIEW 2",
               ParagraphStyle("k", fontName="Sans-Bold", fontSize=10, textColor=ACCENT, alignment=TA_CENTER,
                              leading=14)))
    s.append(Spacer(1, 0.5 * cm))
    s.append(P("Real-Time Parallel Log / Event<br/>Triage Engine",
               ParagraphStyle("t", fontName="Sans-Bold", fontSize=26, leading=32, textColor=NAVY,
                              alignment=TA_CENTER)))
    s.append(Spacer(1, 0.45 * cm))
    s.append(P("A regex → NFA → DFA compiler and a lock-free multi-core pipeline for classifying "
               "streaming logs into authentication failures, security threats, errors, performance "
               "warnings and personal data",
               ParagraphStyle("st", fontName="Sans-Italic", fontSize=11.5, leading=16, textColor=GREY,
                              alignment=TA_CENTER)))
    s.append(Spacer(1, 1.6 * cm))
    t = Table([[P("<b>Team 1</b>", ParagraphStyle("c", parent=body, alignment=TA_CENTER, fontSize=11))],
               [P("24BCI0099 — Adithya Santhosh P", ParagraphStyle("c2", parent=body, alignment=TA_CENTER,
                                                                  fontSize=11))],
               [P("24BCT0055 — Anvesha Tripathi", ParagraphStyle("c3", parent=body, alignment=TA_CENTER,
                                                                fontSize=11))]],
              colWidths=[9 * cm])
    t.setStyle(TableStyle([("BOX", (0, 0), (-1, -1), 0.8, RULE), ("BACKGROUND", (0, 0), (-1, 0), LIGHT),
                           ("TOPPADDING", (0, 0), (-1, -1), 6), ("BOTTOMPADDING", (0, 0), (-1, -1), 6)]))
    s.append(t)
    s.append(Spacer(1, 1.8 * cm))
    meta = ParagraphStyle("m", parent=body, alignment=TA_CENTER, textColor=GREY, fontSize=9.5)
    s.append(P("Review 2 report  ·  14 October 2026", meta))
    s.append(P("Source code: github.com/AdithyaSanthosh-P/Real-Time-Parallel-Log-Event-Triage-Engine", meta))
    s.append(PageBreak())
    return s


def contents():
    rows = [["Section", ""],
            ["1. Abstract", ""], ["2. Summary of progress", ""], ["3. System architecture", ""],
            ["4. Implementation steps", ""], ["5. Module-wise split and work done", ""],
            ["6. Verification and results so far", ""], ["7. Remaining work", ""], ["References", ""]]
    s = [P("Contents", h1)]
    t = Table([[P(r[0], cell)] for r in rows[1:]], colWidths=[17 * cm])
    t.setStyle(TableStyle([("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
                           ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 4)]))
    s.append(t)
    s.append(Spacer(1, 0.6 * cm))
    return s


def abstract():
    return [
        P("1. Abstract", h1),
        P("Servers and network services emit thousands of log lines per second, and the few that matter "
          "— repeated failed logins, injection attempts, crashes, slow queries, leaked card or Aadhaar "
          "numbers — must be found as they arrive. This project builds a real-time triage engine that "
          "classifies every incoming log line into <b>AUTH_FAILURE</b>, <b>SECURITY_THREAT</b>, "
          "<b>ERROR</b>, <b>PERF_WARNING</b> and <b>PII</b>, with a severity and a confidence score, and "
          "routes urgent lines to an alert stream."),
        P("The core is a compiler built for the purpose: the 26 detection rules, written as POSIX-style "
          "regular expressions, are lexed and parsed by a recursive-descent parser, turned into a Thompson "
          "NFA and compiled by subset construction into one combined DFA whose states carry a bit mask of "
          "the rules matched. Every line is therefore checked against all rules in a single linear pass, "
          "with no backtracking, and the read-only DFA tables are shared by all worker threads without "
          "locks. Around this matcher the engine adds checksum validators (Luhn, Verhoeff) to suppress false "
          "positives, a noisy-OR confidence model with multi-label output, masking of personal data, a "
          "correlator that detects attacks spread over many lines (brute force, password spraying), and a "
          "multi-threaded pipeline connected by lock-free single-producer/single-consumer rings."),
        P("The project extends the base paper on parallel regex text classification. We first reproduced "
          "that paper's design and measured its claims: spawning one process per pattern per input costs "
          "far more than the matching it parallelises, while parallelism over input lines — the approach "
          "taken here — scales. At this review the compiler, the classification logic, the correlator and "
          "the parallel runtime are complete and verified; benchmarking and accuracy evaluation follow."),
    ]


def summary():
    rows = [["Phase", "Scope", "Status"],
            ["0", "Repository, build system (warning-free C11, sanitizer builds), test runner, sample logs", "Complete"],
            ["1", "Single-threaded baseline: rule loader, normaliser, reader, POSIX regex engine, output", "Complete"],
            ["2", "Regex compiler: lexer, parser, Thompson NFA, subset-construction DFA, DFA engine, "
                  "differential tester, Graphviz export", "Complete"],
            ["R", "Faithful reproduction and benchmark of the base paper (6 claims evaluated)", "Complete (optional R5 open)"],
            ["3", "Validators, entity extraction, confidence model, multi-label, PII masking", "Complete"],
            ["4", "Parallel runtime: lock-free rings, job pool, workers, dispatcher, stats, shutdown", "Complete"],
            ["5", "Correlator: brute force, password spray, fail-then-success, error storm", "Complete"],
            ["6", "Benchmark tooling: fork engine, --bench mode, log generator, rule sets, runner", "Next"],
            ["7", "Evaluation: full datasets, hand-labelled accuracy (P/R/F1, Cohen's κ), experiments", "Planned"]]
    t = table(rows, [1.6 * cm, 11.9 * cm, 3.5 * cm])
    stats = [["Measure", "Value"],
             ["Planned tasks completed", "35 of 45 (78 %)"],
             ["Engine features implemented", "12 of 15 planned features"],
             ["Engine source (src/, C11)", "7,173 lines in 29 modules"],
             ["Tests (unit, end-to-end, scripts)", "5,076 lines; 19 unit-test programs"],
             ["Base-paper reproduction (tools/basepaper/)", "2,099 lines"],
             ["Detection rules", "26 rules, 5 categories"]]
    return [
        PageBreak(),
        P("2. Summary of progress", h1),
        P("Work follows an ordered implementation plan in which every task has an explicit, testable "
          "completion criterion. A correct single-threaded pipeline was built first on the C library's "
          "regex engine; our compiler then replaced the matcher and was checked against it; classification "
          "logic was added next, and only then was the pipeline parallelised, each stage being compared "
          "with the output of the simpler one before it."),
        t, Spacer(1, 8),
        KeepTogether([P("Key facts at this review", h2), table(stats, [8.5 * cm, 8.5 * cm], bold_first_col=True)]),
        Spacer(1, 6),
        P("Highlights", h2),
        *bullets([
            "The compiled DFA agrees with the C library's <i>regexec</i> on every rule for every sample log "
            "line and 100,000 generated strings (0 mismatches).",
            "The C engine and an independent Python re-implementation agree on primary label, labels, "
            "severity, confidence and escalation flags for every line of six sample logs (10,000+ lines).",
            "Output with 1, 2, 4 and 8 worker threads is identical to the single-threaded output "
            "(72 comparison runs); all concurrent code runs clean under ThreadSanitizer.",
            "On the public Loghub OpenSSH sample the correlator raises 34 brute-force and 5 password-spray "
            "escalations.",
            "Base paper: its fork-per-pattern design measured 12–13× slower than a sequential loop "
            "that recompiles the patterns; data-parallel variants scale (results in "
            + code("results/basepaper/") + ")."]),
    ]


def architecture(figs):
    return [
        PageBreak(),
        P("3. System architecture", h1),
        P("The system has two parts. At start-up the rule set is compiled into DFA tables (Figure 1). At "
          "run time a pipeline of threads streams log lines through matching, classification, correlation "
          "and output (Figure 2). Stages exchange <i>batches</i> of lines (256 by default) through bounded "
          "single-producer/single-consumer rings; only the workers scale with <font name='Mono'>-w N</font>."),
        figure(figs["compiler"], 17 * cm, "Figure 1. Rule compilation (once at start-up): the front end of a "
               "compiler, from regular-expression source to DFA transition tables."),
        P("<b>Compilation.</b> A lexer and a recursive-descent parser (one function per grammar "
          "non-terminal) build an abstract syntax tree; constructs outside the regular POSIX-ERE subset "
          "(<font name='Mono'>\\d</font>, <font name='Mono'>\\b</font>, backreferences, lookaround, lazy "
          "quantifiers) are rejected with their position. Thompson's construction gives one ε-NFA for all "
          "rules, each accept state tagged with its rule. Subset construction (ε-closure over NFA-state "
          "sets) yields a combined DFA in which every state stores a 64-bit accept mask; to match anywhere "
          "in a line the start set is re-injected at every step. If the DFA would exceed the state cap, "
          "the rules are split into groups with one DFA each (the 26 default rules: 1,762 NFA states, "
          "2 DFA groups, 7,593 DFA states)."),
        figure(figs["runtime"], 13.2 * cm, "Figure 2. Run-time architecture: one thread per stage, lock-free "
               "SPSC rings between stages, worker threads sharing the read-only DFA tables."),
        P("<b>Run time.</b> The reader splits the input into lines, decodes URL escapes for matching, "
          "parses timestamps and fills preallocated batches, which the dispatcher hands to workers "
          "(round-robin or least-loaded). Each worker matches lines with the DFA (or, as a baseline, one "
          "<i>regexec</i> per rule) and, for matched lines only, runs validators, extracts the source IP "
          "and user name, and masks personal data. The aggregator turns rule matches into per-category "
          "confidence and labels; the correlator restores input order and applies sliding time windows "
          "per source IP; the router writes JSON-lines records to three sinks and prints a status line "
          "every second. Finished batches return to the reader through a free ring, so the steady state "
          "allocates no memory. Full rings block the stage before them (back-pressure, no data loss), or "
          "drop and count batches with <font name='Mono'>--drop</font>."),
        figure(figs["tiny"], 17 * cm, "Figure 3. Output of the engine's own <font name='Mono'>--dump-dot</font> "
               "for a two-rule file (<font name='Mono'>ab+c</font> and <font name='Mono'>^x[0-9]$</font>): the "
               "Thompson NFA (bottom, dashed ε-edges) and the 7-state combined DFA (top) with collapsed "
               "byte-range labels."),
    ]


def steps():
    rows = [["#", "Step", "What was built", "How it was verified"],
            ["1", "Baseline pipeline", "TSV rule loader with validation; normaliser (URL-decoding, control "
             "characters); chunked reader with batching and timestamp parsing; POSIX engine; JSONL router; "
             "command-line interface", "Hand-annotated corpus (every rule has a positive line and near-misses); "
             "expected-output file"],
            ["2", "Regex front end", "Lexer and recursive-descent parser to an AST with 256-bit character sets; "
             "precise errors for unsupported syntax", "97 parser cases (valid and invalid); all 26 rules parse"],
            ["3", "Thompson NFA", "One construction per AST node, anchors as assertion edges, combined NFA; "
             "set-of-states simulator", "NFA result = regexec on every rule and test string"],
            ["4", "Subset construction", "ε-closure, hashed state sets, accept masks, byte classes, "
             "state cap with rule-group splitting", "DFA result = NFA simulator"],
            ["5", "DFA engine + difftest", "Table-driven matcher behind <font name='Mono'>--engine dfa</font>; "
             "differential test tool", "0 mismatches on all corpus lines and 100,000 random strings"],
            ["6", "Base-paper reproduction", "Paper's fork-per-pattern program, five comparison variants, "
             "labelled set of 1,250 inputs, benchmark runner, report", "5 runs per data point; verdict per claim"],
            ["7", "Classification", "Luhn, Verhoeff and PAN validators; span and entity extraction; context "
             "keywords; noisy-OR confidence, τ threshold, multi-label, primary and severity", "Property tests; "
             "independent Python reference classifier"],
            ["8", "PII masking", "Format-preserving masks; quarantine sink", "End-to-end test searches all "
             "output for every PII value"],
            ["9", "Parallel runtime", "SPSC rings, job pool, workers with CPU pinning, dispatcher (rr/least, "
             "batch delay, drop), aggregator and router threads, stats, error shutdown", "10-million-item ring "
             "stress test; ThreadSanitizer; parallel = sequential on all logs"],
            ["10", "Correlator", "Per-IP hash map with failure rings; brute force, password spray, "
             "fail-then-success, error storm; in-order release of batches", "Crafted sequences; agreement with "
             "Python reference on real OpenSSH log"],
            ["11", "Demo and tooling", "Scripted review demo, Graphviz export, comparison and allocation checks",
             "Runs end to end on sample logs"]]
    return [
        PageBreak(),
        P("4. Implementation steps", h1),
        P("The steps below are in the order they were carried out. Each was finished only when its "
          "verification check and the full test suite passed."),
        table(rows, [0.8 * cm, 3.1 * cm, 7.6 * cm, 5.5 * cm]),
    ]


def modules():
    rows = [["Module", "Source files", "Responsibility", "Work done", "Lines"],
            ["CLI and driver", "main, cli", "Command-line options, help, exit codes",
             "All options of the specification parsed and validated; engine, workers, batching, dispatch, "
             "pinning, drop, τ, state cap, DOT export implemented", "321"],
            ["Rule loader", "rules", "Load and validate the rule file", "26 rules; column, category, severity, "
             "validator and duplicate checks with file:line errors", "417"],
            ["Input", "reader, normalize, timestamp, batch", "Read stream, split lines, normalise, timestamps, "
             "batches", "CRLF, long lines, empty input; URL-decoding; syslog and ISO-8601 time; batch-delay "
             "flush for pipes", "683"],
            ["Regex compiler", "regex_lex, regex_parse, ast, nfa, dfa, dot", "Rules to DFA tables; diagrams",
             "Recursive-descent parser with error positions; Thompson NFA and simulator; subset construction "
             "with accept masks and group splitting; DOT export", "2,305"],
            ["Matching engines", "engine, engine_dfa, engine_posix", "Uniform interface; per-thread instances",
             "DFA engine on shared tables; POSIX baseline with per-thread compiled regexes", "312"],
            ["Validation and extraction", "validators, extract", "Checksums, spans, IP and user, context",
             "Luhn, Verhoeff, PAN; capture spans; IPv4 and user-name extraction; context keywords; subsumption",
             "500"],
            ["Classification", "aggregate", "Confidence, labels, severity", "Rule scores, noisy-OR per category, "
             "τ = 0.5, multi-label, primary by severity and tie order, escalation hook", "213"],
            ["PII masking", "mask", "Mask personal data in output", "Card, Aadhaar, PAN, secrets, e-mail, phone; "
             "only validator-passed values", "160"],
            ["Correlator", "correlate", "Multi-line attack detection", "Per-IP windows on log time; brute force, "
             "password spray, fail-then-success, error storm; once-per-window emission", "327"],
            ["Output", "route, json", "JSONL records and sinks", "alerts, store and PII-quarantine sinks; "
             "strict JSON escaping including invalid UTF-8", "501"],
            ["Parallel runtime", "spsc, pool, worker, dispatch, pipeline, stats", "Threads, queues, scheduling, "
             "shutdown", "Lock-free rings (release/acquire, cache-line separated); preallocated job pool; "
             "pinned workers; rr and least-loaded dispatch; back-pressure and drop; ordered correlation; "
             "1 Hz statistics; stop-on-error", "1,354"],
            ["Base-paper reproduction", "tools/basepaper/", "Rebuild and measure the base paper",
             "Faithful fork program, five variants, labelled data, runner, figures, verdict report", "2,099"],
            ["Verification tools", "tests/, tools/", "Correctness checks", "19 unit-test programs, differential "
             "tester, Python reference classifier, end-to-end and run-equivalence scripts, demo", "6,613"]]
    return [
        PageBreak(),
        P("5. Module-wise split and work done", h1),
        P("The engine is divided into modules with one header per source file and no shared mutable state "
          "apart from the parsed configuration. Line counts include headers."),
        table(rows, [2.6 * cm, 3.0 * cm, 3.3 * cm, 6.6 * cm, 1.5 * cm], bold_first_col=True),
        Spacer(1, 8),
        KeepTogether([
            P("Features not yet implemented", h2),
            *bullets([
                "<b>Fork engine inside the triage binary</b> (<font name='Mono'>--engine fork</font>): the base "
                "paper's mechanism applied to the log rules, as a baseline for experiments.",
                "<b>Benchmark mode</b> (<font name='Mono'>--bench</font>): preloaded input, throughput and "
                "latency percentiles as JSON.",
                "<b>Synthetic log generator</b> with exact ground truth and controllable attack and PII rates."]),
        ]),
    ]


def verification():
    rows = [["Check", "What it establishes", "Result"],
            ["Unit tests (make test)", "Each module in isolation, incl. 10M-item ring stress test", "19 programs pass"],
            ["Differential test (make difftest)", "DFA = C library regexec per rule", "0 mismatches"],
            ["End-to-end (make e2e)", "Labels, severity, flags vs. Python reference; no PII in output; "
             "alerts = HIGH/CRITICAL; clean exit on write error", "pass"],
            ["Run equivalence (make compare)", "-w 1, 2, 4, 8 identical, both engines, all sample logs", "72 runs equal"],
            ["Sanitizers", "AddressSanitizer and ThreadSanitizer on tests and end-to-end runs", "no reports"],
            ["Allocation check", "No memory growth with input size (100k vs 500k lines)", "constant"]]
    bp = [["Base-paper claim", "Paper", "Our measurement", "Verdict"],
          ["Parallel matching speeds up classification", "2.4×", "fork design 0.08× of a sequential "
           "recompiling loop; data-parallel threads 3.1×, combined DFA 25× (5 threads)", "Not reproduced for "
           "the paper's design"],
          ["Near-linear scaling to 6 cores", "4.87× at 6", "fork 1.65× at 6; threads over inputs 4.83× at 6",
           "Not reproduced for the paper's design"],
          ["8.9 ms sequential vs 3.7 ms parallel", "3.7 ms", "fork 0.19–0.21 ms per input, but 12–13× slower "
           "than the sequential baseline", "Not reproduced"],
          ["Structured formats classified more accurately", "98.7 % vs 93.2 %", "0.979 vs 0.952 (per-label "
           "accuracy)", "Qualitatively supported"]]
    return [
        PageBreak(),
        P("6. Verification and results so far", h1),
        table(rows, [4.2 * cm, 9.3 * cm, 3.5 * cm], bold_first_col=True),
        Spacer(1, 10),
        P("Base-paper reproduction", h2),
        P("The base paper was rebuilt as described — one <i>fork()</i> per pattern per input, with "
          "<i>regcomp</i> and <i>regfree</i> in every child — and compared with sequential, thread-based and "
          "DFA variants on the same five patterns (5 runs per point, Intel i7-13650HX). Selected results; the "
          "complete tables and figures are in <font name='Mono'>results/basepaper/SUMMARY.md</font>."),
        table(bp, [4.4 * cm, 2.2 * cm, 6.6 * cm, 3.8 * cm], bold_first_col=True),
        Spacer(1, 6),
        P("The finding motivates this project's design: process creation per pattern (about 200 µs per input) "
          "dwarfs the matching itself (under a microsecond for five precompiled patterns), whereas splitting "
          "the <i>inputs</i> across cores scales, and one combined DFA is the fastest variant."),
        P("Demonstration", h2),
        P("<font name='Mono'>make demo</font> runs a scripted walk-through on the sample logs: rule compilation "
          "and rejection of unsupported syntax, the NFA/DFA diagram, multi-label classification with checksum "
          "look-alikes left unflagged, masked PII, correlator escalations on the real OpenSSH log, identical "
          "output with 1 and 8 workers with the live statistics line, and the base-paper verdicts."),
    ]


def remaining():
    rows = [["Next step", "Content"],
            ["Benchmark tooling (Phase 6)", "Fork engine in the triage binary; --bench mode with latency "
             "histogram; synthetic generator with ground truth; rule sets of 5–64 rules; benchmark runner"],
            ["Experiments (Phase 7)", "Throughput and scaling of DFA vs POSIX vs fork engines over worker "
             "count, rule count and batch size; latency under load"],
            ["Accuracy evaluation (Phase 7)", "Full Loghub and AIT-LDS datasets; stratified sample labelled "
             "independently by both members; precision, recall, F1, Cohen's κ, confidence calibration"],
            ["Optional extensions", "DFA minimisation (Hopcroft's algorithm); ordered output; reproduction of the "
             "base paper's conflict-resolution scoring"]]
    refs = [
        "K. Thompson, “Programming Techniques: Regular expression search algorithm,” <i>Communications of the "
        "ACM</i>, 11(6), 1968.",
        "A. V. Aho, M. S. Lam, R. Sethi, J. D. Ullman, <i>Compilers: Principles, Techniques, and Tools</i>, "
        "2nd ed., Addison-Wesley, 2006 (ch. 3: lexical analysis, NFA/DFA construction).",
        "J. Zhu, S. He, P. He, J. Liu, M. R. Lyu, “Loghub: A Large Collection of System Log Datasets for "
        "AI-driven Log Analytics,” <i>IEEE ISSRE</i>, 2023.",
        "IEEE Std 1003.1 (POSIX), Regular Expressions; GNU C Library <i>regcomp/regexec</i>.",
        "Base paper: “Parallel Text Classification System Using Regex Pattern Matching on Multi-Core "
        "Architectures,” provided by the course faculty.",
        "fail2ban — intrusion prevention software (parameters <i>maxretry</i>, <i>findtime</i>).",
    ]
    return [
        KeepTogether([P("7. Remaining work", h1),
                      table(rows, [4.6 * cm, 12.4 * cm], bold_first_col=True)]),
        Spacer(1, 14),
        P("References", h1),
        *[Paragraph(r, bullet, bulletText=f"[{i}]") for i, r in enumerate(refs, 1)],
    ]


def main():
    if not os.path.exists(os.path.join(ROOT, "bin/triage")):
        sys.exit("run `make` first (bin/triage is needed for Figure 3)")
    with tempfile.TemporaryDirectory() as tmp:
        figs = render_figures(tmp)
        doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=2 * cm, rightMargin=2 * cm,
                                topMargin=2 * cm, bottomMargin=2.2 * cm,
                                title="Real-Time Parallel Log / Event Triage Engine — Review 2",
                                author="Team 1: Adithya Santhosh P, Anvesha Tripathi",
                                subject="Compiler Design project review 2")
        story = (title_page() + contents() + abstract() + summary() + architecture(figs) + steps()
                 + modules() + verification() + remaining())
        doc.build(story, onFirstPage=on_first_page, onLaterPages=on_page)
    print(OUT)


if __name__ == "__main__":
    main()
