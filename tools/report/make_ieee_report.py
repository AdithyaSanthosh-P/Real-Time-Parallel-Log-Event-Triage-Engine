#!/usr/bin/env python3
"""Build docs/Progress_Report_IEEE.pdf: a two-column, IEEE-style progress report.

Needs Graphviz (`dot`) and reportlab (e.g. in a venv: pip install reportlab):
    /tmp/venv/bin/python tools/report/make_ieee_report.py
Figures come from tools/report/ieee_compiler.dot and ieee_runtime.dot. Every number
in the text is taken from the repository: test output, the saved runs in
results/basepaper/, and line counts (wc -l over src/, tests/, tools/).
"""
import os
import subprocess
import tempfile

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import cm
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, Frame, FrameBreak, Image, KeepTogether,
                                NextPageTemplate, PageTemplate, Paragraph, Spacer, Table, TableStyle)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "docs", "Progress_Report_IEEE.pdf")

LIB = "/usr/share/fonts/truetype/liberation/"
for name, f in [("Serif", "LiberationSerif-Regular"), ("Serif-Bold", "LiberationSerif-Bold"),
                ("Serif-Italic", "LiberationSerif-Italic"), ("Serif-BoldItalic", "LiberationSerif-BoldItalic"),
                ("Mono", "LiberationMono-Regular")]:
    pdfmetrics.registerFont(TTFont(name, LIB + f + ".ttf"))
pdfmetrics.registerFontFamily("Serif", normal="Serif", bold="Serif-Bold", italic="Serif-Italic",
                              boldItalic="Serif-BoldItalic")

PAGE_W, PAGE_H = A4
LM = RM = 1.6 * cm
TOP, BOT = 1.9 * cm, 2.2 * cm
GUTTER = 0.6 * cm
COL_W = (PAGE_W - LM - RM - GUTTER) / 2

body = ParagraphStyle("body", fontName="Serif", fontSize=9.5, leading=11.6, alignment=TA_JUSTIFY,
                      firstLineIndent=0.45 * cm, spaceAfter=0, hyphenationLang=None)
body0 = ParagraphStyle("body0", parent=body, firstLineIndent=0)
abstract = ParagraphStyle("abs", parent=body, fontName="Serif-Bold", fontSize=9, leading=10.8,
                          firstLineIndent=0, spaceAfter=3)
sec = ParagraphStyle("sec", fontName="Serif", fontSize=9.5, leading=12, alignment=TA_CENTER,
                     spaceBefore=9, spaceAfter=4)
sub = ParagraphStyle("sub", fontName="Serif-Italic", fontSize=9.5, leading=12, alignment=TA_LEFT,
                     spaceBefore=5, spaceAfter=2)
cap = ParagraphStyle("cap", fontName="Serif", fontSize=8, leading=9.6, alignment=TA_JUSTIFY, spaceBefore=3,
                     spaceAfter=7)
tcap = ParagraphStyle("tcap", fontName="Serif", fontSize=8, leading=9.6, alignment=TA_CENTER, spaceBefore=5,
                      spaceAfter=3)
cell = ParagraphStyle("cell", fontName="Serif", fontSize=7.8, leading=9.2, alignment=TA_LEFT)
cellh = ParagraphStyle("cellh", parent=cell, fontName="Serif-Bold")
ref = ParagraphStyle("ref", fontName="Serif", fontSize=8, leading=9.6, alignment=TA_JUSTIFY, leftIndent=0.75 * cm,
                     firstLineIndent=-0.75 * cm, spaceAfter=2)
item = ParagraphStyle("item", parent=body0, alignment=TA_LEFT, leftIndent=0.5 * cm, firstLineIndent=-0.5 * cm, spaceAfter=1.5)

SEC = 0


def S(title):
    global SEC
    SEC += 1
    roman = ["I", "II", "III", "IV", "V", "VI", "VII", "VIII"][SEC - 1]
    return Paragraph(f"{roman}. {title.upper()}", sec)


def SS(letter, title):
    return Paragraph(f"{letter}. {title}", sub)


def P(t, st=body):
    return Paragraph(t, st)


def mono(t):
    return f'<font name="Mono" size="8.3">{t}</font>'


def ieee_table(num, title, rows, widths, align=None):
    data = [[Paragraph(c, cellh if i == 0 else cell) for c in r] for i, r in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle([
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 2.5), ("RIGHTPADDING", (0, 0), (-1, -1), 2.5),
        ("TOPPADDING", (0, 0), (-1, -1), 1.6), ("BOTTOMPADDING", (0, 0), (-1, -1), 1.6),
        ("LINEABOVE", (0, 0), (-1, 0), 0.9, colors.black), ("LINEBELOW", (0, 0), (-1, 0), 0.5, colors.black),
        ("LINEBELOW", (0, -1), (-1, -1), 0.9, colors.black),
    ]))
    head = Paragraph(f"TABLE {num}<br/>{title.upper()}", tcap)
    return KeepTogether([head, t, Spacer(1, 7)])


def figure(path, width_cm, number, text):
    iw, ih = ImageReader(path).getSize()
    w = width_cm * cm
    img = Image(path, width=w, height=w * ih / iw)
    img.hAlign = "CENTER"
    return KeepTogether([img, Paragraph(f"Fig. {number}. {text}", cap)])


def equation(expr, number):
    t = Table([[Paragraph(expr, ParagraphStyle("eq", parent=body0, alignment=TA_CENTER, fontName="Serif-Italic")),
                Paragraph(f"({number})", ParagraphStyle("en", parent=body0, alignment=2))]],
              colWidths=[COL_W - 1.1 * cm, 1.1 * cm])
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "MIDDLE"), ("TOPPADDING", (0, 0), (-1, -1), 2),
                           ("BOTTOMPADDING", (0, 0), (-1, -1), 3)]))
    return t


# ---- page templates -----------------------------------------------------------------------
def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Serif", 8)
    canvas.drawCentredString(PAGE_W / 2, 1.2 * cm, str(doc.page))
    canvas.restoreState()


def build_doc():
    doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=LM, rightMargin=RM, topMargin=TOP, bottomMargin=BOT,
                          title="Real-Time Parallel Log / Event Triage Engine: Progress Report",
                          author="Adithya Santhosh P, Anvesha Tripathi",
                          subject="Compiler Design project progress report (IEEE format)")
    title_h = 5.9 * cm
    y0 = BOT
    col_h = PAGE_H - TOP - BOT
    x1, x2 = LM, LM + COL_W + GUTTER
    first = PageTemplate(id="first", onPage=footer, frames=[
        Frame(LM, PAGE_H - TOP - title_h, PAGE_W - LM - RM, title_h, id="title", leftPadding=0, rightPadding=0,
              topPadding=0, bottomPadding=0),
        Frame(x1, y0, COL_W, col_h - title_h, id="c1", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
        Frame(x2, y0, COL_W, col_h - title_h, id="c2", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)])
    later = PageTemplate(id="later", onPage=footer, frames=[
        Frame(x1, y0, COL_W, col_h, id="l1", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
        Frame(x2, y0, COL_W, col_h, id="l2", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)])
    doc.addPageTemplates([first, later])
    return doc


def title_block():
    t = ParagraphStyle("t", fontName="Serif-Bold", fontSize=19, leading=22.5, alignment=TA_CENTER)
    st = ParagraphStyle("st", fontName="Serif-Italic", fontSize=11, leading=13.5, alignment=TA_CENTER)
    au = ParagraphStyle("au", fontName="Serif", fontSize=10.5, leading=12.5, alignment=TA_CENTER)
    af = ParagraphStyle("af", fontName="Serif-Italic", fontSize=9, leading=11, alignment=TA_CENTER)
    authors = Table([[Paragraph("Adithya Santhosh P<br/><font size=9>Reg. No. 24BCI0099</font>", au),
                      Paragraph("Anvesha Tripathi<br/><font size=9>Reg. No. 24BCT0055</font>", au)]],
                    colWidths=[(PAGE_W - LM - RM) / 2] * 2)
    authors.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                                 ("RIGHTPADDING", (0, 0), (-1, -1), 0)]))
    return [Spacer(1, 0.1 * cm),
            P("Real-Time Parallel Log / Event Triage Engine", t),
            P("A Regex-to-DFA Compiler and Lock-Free Multi-Core Pipeline for Streaming Log Classification", st),
            Spacer(1, 0.45 * cm), authors, Spacer(1, 0.15 * cm),
            P("Team 1, Compiler Design Course Project, VIT  ·  Progress report, October 2026", af),
            FrameBreak()]


# ---- text ------------------------------------------------------------------------------------
def content(figs):
    s = []
    s.append(P("<b><i>Abstract</i></b>—Servers emit thousands of log lines per second, and the few that matter—"
               "repeated failed logins, injection attempts, crashes, leaked card or national-ID numbers—must be "
               "found as the lines arrive. This report presents the design and the current implementation of a "
               "real-time triage engine that classifies each log line into five categories (authentication failure, "
               "security threat, error, performance warning, personal data) with a severity and a confidence score. "
               "The core is a purpose-built regular-expression compiler: 26 detection rules are parsed by a "
               "recursive-descent parser, converted to a Thompson NFA and compiled by subset construction into one "
               "combined DFA, so every line is matched against all rules in a single linear pass. Checksum "
               "validators, a noisy-OR confidence model, PII masking and a sliding-window correlator are layered on "
               "the matcher, and a pipeline of threads connected by lock-free rings scales the engine across cores. "
               "The compiled DFA agrees with the C library's regex engine on all test lines and 100,000 generated "
               "strings, and parallel output is identical to sequential output. A faithful reproduction of the base "
               "paper's process-per-pattern design is also reported. At this milestone the compiler, the "
               "classification logic, the correlator and the parallel runtime are complete (32 of the 41 required "
               "tasks); benchmarking and accuracy evaluation remain.", abstract))
    s.append(P("<b><i>Index Terms</i></b>—regular expressions, finite automata, subset construction, log "
               "classification, intrusion detection, lock-free queues, multi-core processing.",
               ParagraphStyle("it", parent=abstract, fontName="Serif", spaceAfter=2)))

    # I. Introduction
    s.append(S("Introduction"))
    s.append(P("Modern services produce logs faster than people can read them. Hidden in the stream are lines "
               "that need action: repeated authentication failures, SQL-injection or path-traversal requests, "
               "application crashes, slow queries, and personal data such as card or Aadhaar numbers that must not "
               "be stored in clear. Triage—assigning each line a category, a severity and a confidence, and "
               "routing urgent lines to an alert stream—has to keep up with the input rate.", body0))
    s.append(P("The base paper for this course project [5] classifies short text inputs by running one regular "
               "expression per process on separate cores. Our measurements (Section VI) show that this design "
               "is dominated by process-creation cost. This work keeps the goal—parallel regex classification—but "
               "changes the method: all rules are compiled into one deterministic automaton, and parallelism is "
               "applied across input lines rather than across patterns. The contributions are:"))
    s.append(P("1)&nbsp;a regex compiler (lexer, recursive-descent parser, Thompson NFA, subset-construction DFA) "
               "restricted to the regular POSIX-ERE subset, checked against the C library's engine;", item))
    s.append(P("2)&nbsp;a classification layer with checksum validators, noisy-OR confidence, multi-label output "
               "and masking of personal data;", item))
    s.append(P("3)&nbsp;a correlator that detects multi-line attacks (brute force, password spraying) in sliding "
               "time windows;", item))
    s.append(P("4)&nbsp;a multi-threaded pipeline with lock-free single-producer/single-consumer (SPSC) rings; and", item))
    s.append(P("5)&nbsp;a faithful reproduction and measurement of the base paper's design.", item))

    # II. Background
    s.append(S("Background and Related Work"))
    s.append(SS("A", "From Regular Expressions to Automata"))
    s.append(P("Thompson's construction [1] converts a regular expression to an NFA with ε-transitions; subset "
               "construction [2] converts the NFA to a DFA whose states are sets of NFA states. A DFA reads one "
               "input symbol per table lookup, so matching takes time linear in the input with no backtracking, "
               "at the price of a possibly exponential number of states. Combining many rules into one automaton "
               "makes the per-line cost independent of the number of rules.", body0))
    s.append(SS("B", "The Base Paper"))
    s.append(P("The base paper [5] classifies short strings as e-mail, phone number, name, question or greeting "
               "using five regular expressions, one per forked process, and reports a 2.4× speed-up over a "
               "sequential implementation and near-linear scaling to six cores. Because the paper is an "
               "unverified student work, we treat its claims as hypotheses and test them (Section VI-C).", body0))
    s.append(SS("C", "Log Data and Detection Practice"))
    s.append(P("Loghub [3] provides real system logs; we use its OpenSSH, Apache, Linux, BGL and HDFS samples. "
               "Tools such as fail2ban [6] flag an address after a number of failures within a time window; "
               "our correlator uses the same parameter names (<i>maxretry</i>, <i>findtime</i>).", body0))

    # III. Architecture
    s.append(S("System Architecture"))
    s.append(P("The system has a start-up phase and a run-time phase. At start-up the rule file is compiled into "
               "read-only DFA tables (Fig. 1). At run time a pipeline of threads streams lines through matching, "
               "classification, correlation and output (Fig. 2).", body0))
    s.append(P("The reader splits the input into lines, decodes URL escapes for matching, parses timestamps and "
               "fills batches (256 lines by default). The dispatcher hands each batch to a worker, round-robin or "
               "to the least-loaded one. Workers share the DFA tables without locks; each runs the matcher and, "
               "for matched lines only, the validators, entity extraction and PII masking. The aggregator computes "
               "labels and confidence; the correlator restores input order and applies the time windows; the "
               "router writes three JSON-lines sinks and prints a status line every second. Stages are connected "
               "by SPSC rings, and finished batches return to the reader through a free ring so that the steady "
               "state allocates no memory.", body0))
    s.append(figure(figs["compiler"], 7.6, 1, "Rule compilation, performed once at start-up."))
    s.append(figure(figs["runtime"], 5.6, 2, "Run-time pipeline. Arrows between threads are lock-free SPSC rings; "
                    "the DFA tables are shared read-only by all workers."))

    # IV. Implementation
    s.append(S("Implementation"))
    s.append(SS("A", "Regex Front End"))
    s.append(P("The grammar is the POSIX-ERE subset: literals, <font name='Mono'>.</font>, bracket expressions, "
               "<font name='Mono'>* + ? {n,m}</font>, alternation, grouping and the line anchors "
               "<font name='Mono'>^ $</font>. A lexer feeds a recursive-descent parser with one function per "
               "non-terminal. Character classes are 256-bit sets, so case-insensitive rules fold the set. "
               "Constructs outside the regular subset (shorthand classes, word boundaries, backreferences, "
               "lookaround, lazy quantifiers) are rejected with their position instead of being approximated; this "
               "guarantees linear-time matching and identical semantics to the C library.", body0))
    s.append(SS("B", "NFA and DFA Construction"))
    s.append(P("Each AST node maps to a Thompson fragment; the 26 rules are joined under one start state and "
               "each accept state carries its rule number (1,762 NFA states). Subset construction uses an "
               "explicit-stack ε-closure and a hash table of state sets. Every DFA state stores a 64-bit accept "
               "mask whose bit <i>r</i> is set when rule <i>r</i> has matched. To find matches anywhere in a "
               "line, the start set is added at every step; anchored rules are handled with assertion edges. "
               "Transitions are indexed by byte class. When the state count would exceed a cap, the rules are "
               "split into groups and one DFA is built per group; the default rules give 2 groups and 7,593 "
               "states in total.", body0))
    s.append(SS("C", "Validation and Classification"))
    s.append(P("A regular expression cannot tell a card number from any other 16-digit string, so matched "
               "spans are validated: the Luhn checksum [7] for cards, the Verhoeff checksum [8] for Aadhaar "
               "numbers, and a format check for PAN. A rule whose validator fails on every candidate span is "
               "discarded. Each surviving rule <i>r</i> receives a score", body0))
    s.append(equation("s<sub>r</sub> = min(1, b<sub>r</sub> + 0.25·v<sub>r</sub> + 0.15·k<sub>r</sub>)", 1))
    s.append(P("where <i>b<sub>r</sub></i> is the rule's base precision, <i>v<sub>r</sub></i> = 1 if it has a "
               "validator that passed and <i>k<sub>r</sub></i> = 1 if a context keyword is present. The "
               "confidence of a category with surviving scores <i>s</i><sub>1</sub>…<i>s</i><sub>m</sub> combines "
               "them by noisy-OR:", body0))
    s.append(equation("c = 1 − (1 − s<sub>1</sub>)(1 − s<sub>2</sub>)…(1 − s<sub>m</sub>)", 2))
    s.append(P("A category becomes a label when <i>c</i> ≥ τ = 0.5, so a line can carry several labels. The "
               "primary label is the most severe one, ties being broken by confidence and then by a fixed order. "
               "Rules with a weak base (for example a bare HTTP 401) need a second signal to reach τ.", body0))
    s.append(SS("D", "PII Masking"))
    s.append(P("Before any output is written, validator-passed values are masked in the line: cards as "
               "<font name='Mono'>XXXX…1111</font>, Aadhaar as <font name='Mono'>XXXX XXXX 1234</font>, PAN as "
               "<font name='Mono'>XXXXX1234X</font>, secrets as <font name='Mono'>key=***</font>, plus e-mail "
               "addresses and phone numbers. Masked text goes to every sink, including the alert sink.", body0))
    s.append(SS("E", "Correlator"))
    s.append(P("Some attacks are invisible in one line. The correlator keeps, per source IPv4 address, a ring "
               "of the last 64 failures (time and user) in an open-addressing hash map, and works on log "
               "timestamps. Table I lists the patterns. An escalation raises the category confidence to 0.95, "
               "sets a flag, and is emitted once per window.", body0))
    s.append(ieee_table("I", "Correlation patterns", [
        ["Pattern", "Trigger", "Result"],
        ["Brute force", "≥ 5 failures, one IP, 60 s", "SECURITY, HIGH"],
        ["Password spray", "≥ 5 distinct users, one IP, 300 s", "SECURITY, HIGH"],
        ["Fail then success", "login from an IP currently in brute force", "SECURITY, CRITICAL"],
        ["Error storm", "≥ 100 ERROR lines, 60 s", "ERROR raised to HIGH"]],
        [2.2 * cm, 3.7 * cm, 2.7 * cm]))
    s.append(SS("F", "Parallel Runtime"))
    s.append(P("An SPSC ring is a power-of-two array with a head index written only by the consumer and a tail "
               "index written only by the producer, each on its own 64-byte cache line. The producer publishes a "
               "slot with a release store of the tail; the consumer observes it with an acquire load, so no "
               "locks or read-modify-write operations are needed. Batches are preallocated in a pool and recycled "
               "through a free ring. A full ring blocks the stage before it (back-pressure) or, with "
               "<font name='Mono'>--drop</font>, drops and counts batches. Workers can be pinned to separate "
               "physical cores. Since workers finish batches out of order, the correlator releases batches in "
               "input order, which makes the output independent of the worker count. End of input travels as a "
               "sentinel through every stage, and a shared stop flag unblocks all threads when a stage fails.", body0))

    # V. Status
    s.append(S("Project Status"))
    s.append(P("Work follows an ordered plan in which every task has a testable completion criterion; a "
               "correct single-threaded baseline was built first, and each later stage was compared with the "
               "output of the simpler one before it. Table II gives the status; Table III the module-wise split.",
               body0))
    s.append(ieee_table("II", "Status by phase", [
        ["Phase", "Scope", "Status"],
        ["0", "Build system, tests, sample logs", "Done"],
        ["1", "Single-thread baseline (C regex library)", "Done"],
        ["2", "Regex compiler and DFA engine", "Done"],
        ["R", "Base-paper reproduction (6 claims)", "Done*"],
        ["3", "Validators, confidence, PII masking", "Done"],
        ["4", "Parallel runtime", "Done"],
        ["5", "Correlator", "Done"],
        ["6", "Benchmark tooling", "Pending"],
        ["7", "Evaluation on full datasets", "Pending"]],
        [1.2 * cm, 5.9 * cm, 1.5 * cm]))
    s.append(P("*One optional task (conflict-resolution scoring) is not done. In total 32 of the 41 required tasks "
               "(78 %) are complete.", ParagraphStyle("fn", parent=cap, spaceBefore=0)))
    s.append(ieee_table("III", "Modules and source size", [
        ["Module", "Source files", "Lines"],
        ["CLI, driver", "main, cli", "321"],
        ["Rule loader", "rules", "417"],
        ["Input", "reader, normalize, timestamp, batch", "683"],
        ["Regex compiler", "regex_lex, regex_parse, ast, nfa, dfa, dot", "2,305"],
        ["Matching engines", "engine, engine_dfa, engine_posix", "312"],
        ["Validation, extraction", "validators, extract", "500"],
        ["Classification", "aggregate", "213"],
        ["PII masking", "mask", "160"],
        ["Correlator", "correlate", "327"],
        ["Output", "route, json", "501"],
        ["Parallel runtime", "spsc, pool, worker, dispatch, pipeline, stats", "1,354"],
        ["Shared types", "types", "80"],
        ["<b>Engine total</b>", "29 source files (C11)", "<b>7,173</b>"]],
        [2.3 * cm, 5.2 * cm, 1.1 * cm]))

    # VI. Verification
    s.append(S("Verification and Results"))
    s.append(SS("A", "Correctness"))
    s.append(P("Table IV summarises the checks. The DFA is compared with the C library's <i>regexec</i> per "
               "rule; classification and correlation are compared with an independent Python implementation of "
               "the same specification; parallel runs are compared with the single-worker run record by record.",
               body0))
    s.append(ieee_table("IV", "Verification", [
        ["Check", "Result"],
        ["19 unit-test programs (incl. 10M-item ring stress test)", "pass"],
        ["DFA vs. regexec: corpus + 100,000 random strings", "0 mismatches"],
        ["C engine vs. Python reference, 10,000+ lines, 6 logs", "all fields equal"],
        ["1, 2, 4, 8 workers vs. 1 worker, 2 engines, 6 logs", "72 runs equal"],
        ["ThreadSanitizer, AddressSanitizer on tests and runs", "no reports"],
        ["Allocations at 100k vs. 500k input lines", "constant"]],
        [6.2 * cm, 2.4 * cm]))
    s.append(SS("B", "Attack Detection on a Real Log"))
    s.append(P("On the Loghub OpenSSH sample (2,000 lines) the correlator raises 34 brute-force and 5 "
               "password-spray escalations; every escalated record appears in the alert sink and agrees with "
               "the Python reference.", body0))
    s.append(SS("C", "Reproduction of the Base Paper"))
    s.append(P("The paper's program was rebuilt as described (one <i>fork</i> per pattern per input, with "
               "<i>regcomp</i> and <i>regfree</i> in every child) and compared with sequential, thread-based and "
               "DFA variants on the same five patterns (5 runs per point, 14-core Intel i7-13650HX). Table V gives "
               "selected results; no value was tuned to match the paper.", body0))
    s.append(ieee_table("V", "Base-paper design versus alternatives", [
        ["Measurement", "Result"],
        ["fork design, time per input", "0.19–0.21 ms (paper: 3.7 ms)"],
        ["fork vs. sequential, recompiling", "12–13× slower"],
        ["fork vs. sequential, precompiled", "about 500× slower"],
        ["Peak speed-up, fork, 8 cores", "1.72× (paper: 5.62×)"],
        ["Peak speed-up, split by input, 6 cores", "4.83×"],
        ["Combined DFA, 6 cores, vs. sequential", "5.29× (own scaling)"]],
        [5.0 * cm, 3.6 * cm]))
    s.append(P("The per-pattern fork design is dominated by process creation (about 200 µs per input against "
               "well under 1 µs of matching) and does not reproduce the paper's claims; splitting the "
               "<i>inputs</i> across cores scales, which motivates the present design.", body0))

    # VII. Remaining work
    s.append(S("Remaining Work and Conclusion"))
    s.append(P("Phase 6 adds the fork engine inside the triage binary, a benchmark mode with latency "
               "percentiles, a synthetic log generator with exact ground truth, rule sets of 5–64 rules and an "
               "experiment runner. Phase 7 evaluates throughput and scaling of the DFA, POSIX and fork engines, "
               "and measures accuracy on full datasets with a stratified sample labelled independently by both "
               "authors (precision, recall, F1, Cohen's κ, confidence calibration). Optional extensions are DFA "
               "minimisation and ordered output. No throughput or accuracy figures for the engine are reported "
               "here, because they will be taken only from saved benchmark runs.", body0))
    s.append(P("The regex compiler, the classification logic, the correlator and the parallel runtime are "
               "implemented and verified, giving a working system that classifies, masks and correlates real "
               "log data on multiple cores with output identical to the sequential run.", body))

    # References
    s.append(Paragraph("REFERENCES", sec))
    refs = [
        "K. Thompson, “Programming techniques: Regular expression search algorithm,” <i>Commun. ACM</i>, vol. 11, "
        "no. 6, pp. 419–422, 1968.",
        "A. V. Aho, M. S. Lam, R. Sethi, and J. D. Ullman, <i>Compilers: Principles, Techniques, and Tools</i>, "
        "2nd ed. Boston, MA, USA: Addison-Wesley, 2006.",
        "J. Zhu, S. He, P. He, J. Liu, and M. R. Lyu, “Loghub: A large collection of system log datasets for "
        "AI-driven log analytics,” in <i>Proc. IEEE Int. Symp. Softw. Rel. Eng. (ISSRE)</i>, 2023.",
        "<i>IEEE Standard for Information Technology—Portable Operating System Interface (POSIX)</i>, "
        "IEEE Std 1003.1-2017, Regular Expressions.",
        "“Parallel text classification system using regex pattern matching on multi-core architectures,” "
        "base paper provided by the course faculty, unpublished.",
        "fail2ban: intrusion prevention software. [Online]. Available: https://www.fail2ban.org",
        "H. P. Luhn, “Computer for verifying numbers,” U.S. Patent 2 950 048, Aug. 23, 1960.",
        "J. Verhoeff, <i>Error Detecting Decimal Codes</i>, Mathematical Centre Tracts 29. Amsterdam, The "
        "Netherlands: Mathematisch Centrum, 1969.",
    ]
    for i, r in enumerate(refs, 1):
        s.append(Paragraph(f"[{i}]&nbsp;&nbsp;{r}", ref))
    return s


def main():
    with tempfile.TemporaryDirectory() as tmp:
        figs = {}
        for key, name in (("compiler", "ieee_compiler"), ("runtime", "ieee_runtime")):
            out = os.path.join(tmp, name + ".png")
            subprocess.run(["dot", "-Tpng", "-Gdpi=300", os.path.join(ROOT, "tools/report", name + ".dot"),
                            "-o", out], check=True)
            figs[key] = out
        doc = build_doc()
        story = title_block() + content(figs)
        # pages after the first use the plain two-column template
        story.insert(len(title_block()), NextPageTemplate("later"))
        doc.build(story)
    print(OUT)


if __name__ == "__main__":
    main()
