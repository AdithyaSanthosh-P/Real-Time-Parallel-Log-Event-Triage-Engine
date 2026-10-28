#!/usr/bin/env python3
"""Build docs/Review1_Report_IEEE.pdf: the Review 1 report (IEEE two-column format) for the
state of the project at commit 8093e0a (regex compiler, baseline pipeline, base-paper
reproduction and benchmark).

Structure follows the professor's template: Abstract, 1 Introduction, 2 Literature Survey,
3 Proposed System (architecture diagram, modules with explanation and pseudocode),
4 Results and Implementation, 5 Performance and Metrics, 6 Conclusion, References.

Every number is read from files in the repository:
  results/milestone1/*.json   own-engine measurements (tools/report/measure_milestone1.py)
  results/basepaper/*         base-paper reproduction runs (tools/basepaper/run_bp.sh)
  git show 8093e0a:...        line counts of the modules at that commit
  docs/research/references_ieee.md   the reference list (papers only)
Needs: Graphviz `dot` and reportlab (python3 -m venv v && v/bin/pip install reportlab).
"""
import json
import os
import re
import subprocess
import tempfile
from xml.sax.saxutils import escape

from reportlab.graphics.shapes import Circle, Drawing, Line, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import cm
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, CondPageBreak, Frame, FrameBreak, Image, KeepTogether,
                                NextPageTemplate, PageBreak, PageTemplate, Paragraph, Spacer, Table,
                                TableStyle)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COMMIT = "8093e0a"
OUT = os.path.join(ROOT, "docs", "Review1_Report_IEEE.pdf")

# ---------------------------------------------------------------------------------- fonts / styles
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
FULL_W = PAGE_W - LM - RM
COL_W = (FULL_W - GUTTER) / 2

body = ParagraphStyle("body", fontName="Serif", fontSize=9.5, leading=11.6, alignment=TA_JUSTIFY,
                      firstLineIndent=0.45 * cm)
body0 = ParagraphStyle("body0", parent=body, firstLineIndent=0)
abstract = ParagraphStyle("abs", parent=body, fontName="Serif-Bold", fontSize=9, leading=10.8,
                          firstLineIndent=0, spaceAfter=3)
sec = ParagraphStyle("sec", fontName="Serif", fontSize=9.5, leading=12, alignment=TA_CENTER, spaceBefore=9,
                     spaceAfter=4, keepWithNext=1)
sub = ParagraphStyle("sub", fontName="Serif-Italic", fontSize=9.5, leading=12, alignment=TA_LEFT,
                     spaceBefore=5, spaceAfter=2, keepWithNext=1)
cap = ParagraphStyle("cap", fontName="Serif", fontSize=8, leading=9.6, alignment=TA_JUSTIFY, spaceBefore=3,
                     spaceAfter=7)
capc = ParagraphStyle("capc", parent=cap, alignment=TA_CENTER)
tcap = ParagraphStyle("tcap", fontName="Serif", fontSize=8, leading=9.6, alignment=TA_CENTER, spaceBefore=5,
                      spaceAfter=3)
cell = ParagraphStyle("cell", fontName="Serif", fontSize=7.6, leading=8.9, alignment=TA_LEFT)
cellr = ParagraphStyle("cellr", parent=cell, alignment=2)
cellh = ParagraphStyle("cellh", parent=cell, fontName="Serif-Bold")
cellhr = ParagraphStyle("cellhr", parent=cellh, alignment=2)
ref = ParagraphStyle("ref", fontName="Serif", fontSize=7.8, leading=8.7, alignment=TA_JUSTIFY,
                     leftIndent=0.75 * cm, firstLineIndent=-0.75 * cm, spaceAfter=1.4)
item = ParagraphStyle("item", parent=body0, alignment=TA_LEFT, leftIndent=0.5 * cm, firstLineIndent=-0.5 * cm,
                      spaceAfter=1.5)
alg_line = ParagraphStyle("alg", fontName="Serif", fontSize=8, leading=9.4, alignment=TA_LEFT)
alg_title = ParagraphStyle("algt", fontName="Serif-Bold", fontSize=8.2, leading=10, alignment=TA_LEFT)

BLUE, ORANGE, GREEN, GREY = (colors.HexColor("#1f4e79"), colors.HexColor("#c55a11"),
                             colors.HexColor("#548235"), colors.HexColor("#7f7f7f"))

# ------------------------------------------------------------------------------- data from the repo


def jload(path):
    return json.load(open(os.path.join(ROOT, path)))


def git_loc(files):
    n = 0
    for f in files:
        out = subprocess.run(["git", "show", f"{COMMIT}:{f}"], cwd=ROOT, capture_output=True)
        if out.returncode == 0:
            n += out.stdout.count(b"\n")
    return n


M1 = {k: jload(f"results/milestone1/{k}.json") for k in ("state_sweep", "throughput", "corpus", "tests")}
RB1 = jload("results/basepaper/rb1/summary.json")
RB2 = jload("results/basepaper/rb2/summary.json")
RB3 = jload("results/basepaper/rb3/summary.json")
RB4 = jload("results/basepaper/rb4/summary.json")


def src(*names):
    return git_loc([f"src/{n}.{e}" for n in names for e in ("c", "h")])


MOD_LOC = {
    "M1": src("regex_lex", "regex_parse", "ast"),
    "M2": src("nfa", "dfa", "dot", "engine_dfa"),
    "M3": src("main", "cli", "rules", "reader", "normalize", "timestamp", "batch", "engine", "engine_posix",
              "aggregate", "route", "json", "pipeline", "types"),
}
if "sizes" in M1["tests"]:      # results/milestone1/tests.json written by an older script has no line counts
    TOOLS_BP = M1["tests"]["sizes"]["basepaper_tool_lines"]
    assert sum(MOD_LOC.values()) == M1["tests"]["sizes"]["src_lines"], (MOD_LOC, M1["tests"]["sizes"])

# --------------------------------------------------------------------------------------- references


def load_refs():
    refs = {}
    for line in open(os.path.join(ROOT, "docs/research/references_ieee.md"), encoding="utf-8"):
        m = re.match(r"- \*\*([JC]\d+)\*\* (.+)", line.rstrip())
        if m:
            text = re.sub(r"\s*\*\(.*?\)\*\s*$", "", m[2])            # drop "(not confirmed)" notes
            text = escape(text)
            text = re.sub(r"\*(.+?)\*", r"<i>\1</i>", text)
            refs[m[1]] = text
    return refs


REFS = load_refs()
ORDER = []


def cite(*keys):
    nums = []
    for k in keys:
        assert k in REFS, k
        if k not in ORDER:
            ORDER.append(k)
        nums.append(ORDER.index(k) + 1)
    nums.sort()
    out, i = [], 0
    while i < len(nums):                                        # [1]–[3] for runs of 3 or more
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        if j - i >= 2:
            out.append(f"[{nums[i]}]–[{nums[j]}]")
        else:
            out += [f"[{n}]" for n in nums[i:j + 1]]
        i = j + 1
    return ", ".join(out)


# ----------------------------------------------------------------------------------- small helpers
FIGSLOT = object()      # marks where the full-width architecture page starts
SEC = 0
TAB = 0
FIG = 0
ALG = 0


def S(title):
    global SEC
    SEC += 1
    return Paragraph(f"{SEC}. {title.upper()}", sec)


def SS(num, title):
    return Paragraph(f"{num} {title}", sub)


def fx(t):
    return t.replace("<tt>", '<font name="Mono" size="8.2">').replace("</tt>", "</font>")


def P(t, st=body):
    return Paragraph(fx(t), st)


def roman(n):
    out = ""
    for v, s in ((10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")):
        while n >= v:
            out += s
            n -= v
    return out


def table(title, rows, widths, right=(), flat=False):
    global TAB
    TAB += 1
    data = []
    for i, r in enumerate(rows):
        data.append([Paragraph(fx(c), (cellhr if j in right else cellh) if i == 0 else (cellr if j in right else cell))
                     for j, c in enumerate(r)])
    t = Table(data, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle([
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 2.5), ("RIGHTPADDING", (0, 0), (-1, -1), 2.5),
        ("TOPPADDING", (0, 0), (-1, -1), 1.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 1.5),
        ("LINEABOVE", (0, 0), (-1, 0), 0.9, colors.black), ("LINEBELOW", (0, 0), (-1, 0), 0.5, colors.black),
        ("LINEBELOW", (0, -1), (-1, -1), 0.9, colors.black)]))
    parts = [Paragraph(f"TABLE {roman(TAB)}<br/>{title.upper()}", tcap), t, Spacer(1, 7)]
    return parts if flat else KeepTogether(parts)


def figure(flowable, text, wide=False):
    global FIG
    FIG += 1
    return KeepTogether([flowable, Paragraph(f"Fig. {FIG}. {text}", capc if wide else cap)])


def image(path, width):
    iw, ih = ImageReader(path).getSize()
    im = Image(path, width=width, height=width * ih / iw)
    im.hAlign = "CENTER"
    return im


def algorithm(title, lines):
    """lines: list of (indent_level, markup). Numbered pseudocode block."""
    global ALG
    ALG += 1
    rows = [[Paragraph(f"Algorithm {ALG}. {title}", alg_title), ""]]
    for n, (ind, txt) in enumerate(lines, 1):
        st = ParagraphStyle(f"a{ALG}_{n}", parent=alg_line, leftIndent=0.32 * cm * ind)
        rows.append([Paragraph(str(n), ParagraphStyle("an", parent=alg_line, alignment=2)), Paragraph(fx(txt), st)])
    t = Table(rows, colWidths=[0.55 * cm, COL_W - 0.55 * cm])
    t.setStyle(TableStyle([("SPAN", (0, 0), (1, 0)), ("VALIGN", (0, 0), (-1, -1), "TOP"),
                           ("LEFTPADDING", (0, 0), (-1, -1), 2), ("RIGHTPADDING", (0, 0), (-1, -1), 2),
                           ("TOPPADDING", (0, 0), (-1, -1), 0.6), ("BOTTOMPADDING", (0, 0), (-1, -1), 0.6),
                           ("LINEABOVE", (0, 0), (-1, 0), 0.9, colors.black),
                           ("LINEBELOW", (0, 0), (-1, 0), 0.5, colors.black),
                           ("LINEBELOW", (0, -1), (-1, -1), 0.9, colors.black)]))
    return KeepTogether([t, Spacer(1, 7)])


def kw(*w):
    return " ".join(f"<b>{x}</b>" for x in w)


def fmt(v, d=2):
    return f"{v:,.{d}f}"


def si(v):
    """Throughput with thousands separators."""
    return f"{v:,.0f}"


# --------------------------------------------------------------------------------------- charts
FONT = "Serif"


def _axes(d, x0, y0, x1, y1):
    d.add(Line(x0, y0, x1, y0, strokeColor=colors.black, strokeWidth=0.6))
    d.add(Line(x0, y0, x0, y1, strokeColor=colors.black, strokeWidth=0.6))


def hbar_log(rows, width, height, xlabel, lo, hi, ticks):
    """rows: (label, value, min, max, color). Log-scale horizontal bars."""
    import math
    d = Drawing(width, height)
    left, right, bottom, top = 78, 12, 24, 6
    x0, x1, y0, y1 = left, width - right, bottom, height - top
    _axes(d, x0, y0, x1, y1)
    sx = lambda v: x0 + (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (x1 - x0)
    for t, lab in ticks:
        d.add(Line(sx(t), y0, sx(t), y0 - 2.5, strokeColor=colors.black, strokeWidth=0.5))
        d.add(Line(sx(t), y0, sx(t), y1, strokeColor=colors.HexColor("#dddddd"), strokeWidth=0.4))
        d.add(String(sx(t), y0 - 10, lab, fontName=FONT, fontSize=6.5, textAnchor="middle"))
    n = len(rows)
    slot = (y1 - y0) / n
    for i, (label, v, vmin, vmax, col) in enumerate(rows):
        yc = y1 - slot * (i + 0.5)
        bh = slot * 0.62
        d.add(Rect(x0, yc - bh / 2, sx(v) - x0, bh, fillColor=col, strokeColor=colors.black, strokeWidth=0.4))
        d.add(String(x0 - 4, yc - 2.2, label, fontName=FONT, fontSize=6.8, textAnchor="end"))
        d.add(String(sx(v) + 3, yc - 2.2, si(v), fontName=FONT, fontSize=6.2))
    d.add(String((x0 + x1) / 2, 2, xlabel, fontName=FONT, fontSize=7, textAnchor="middle"))
    return d


def line_chart(series, width, height, xticks, ymax, xlabel, ylabel, ideal=True):
    """series: (label, [(x, y)], color, dash)."""
    d = Drawing(width, height)
    left, right, bottom, top = 26, 8, 24, 16
    x0, x1, y0, y1 = left, width - right, bottom, height - top
    _axes(d, x0, y0, x1, y1)
    xmin, xmax = min(xticks), max(xticks)
    sx = lambda v: x0 + (v - xmin) / (xmax - xmin) * (x1 - x0)
    sy = lambda v: y0 + v / ymax * (y1 - y0)
    for t in xticks:
        d.add(Line(sx(t), y0, sx(t), y0 - 2.5, strokeColor=colors.black, strokeWidth=0.5))
        d.add(String(sx(t), y0 - 10, str(t), fontName=FONT, fontSize=6.8, textAnchor="middle"))
    step = 1 if ymax <= 8 else 2
    for t in range(0, int(ymax) + 1, step):
        d.add(Line(x0 - 2.5, sy(t), x0, sy(t), strokeColor=colors.black, strokeWidth=0.5))
        d.add(Line(x0, sy(t), x1, sy(t), strokeColor=colors.HexColor("#e6e6e6"), strokeWidth=0.35))
        d.add(String(x0 - 4, sy(t) - 2.2, str(t), fontName=FONT, fontSize=6.8, textAnchor="end"))
    if ideal:
        d.add(Line(sx(xmin), sy(xmin), sx(xmax), sy(min(xmax, ymax)), strokeColor=GREY, strokeWidth=0.6,
                   strokeDashArray=[2, 2]))
    ly = y1 - 2
    for label, pts, col, dash in series:
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            d.add(Line(sx(xa), sy(ya), sx(xb), sy(yb), strokeColor=col, strokeWidth=1.1,
                       strokeDashArray=dash))
        for xv, yv in pts:
            d.add(Circle(sx(xv), sy(yv), 1.6, fillColor=col, strokeColor=col))
        d.add(Line(x0 + 8, ly - 1, x0 + 18, ly - 1, strokeColor=col, strokeWidth=1.1, strokeDashArray=dash))
        d.add(String(x0 + 21, ly - 3, label, fontName=FONT, fontSize=6.6))
        ly -= 8.5
    if ideal:
        d.add(Line(x0 + 8, ly - 1, x0 + 18, ly - 1, strokeColor=GREY, strokeWidth=0.6, strokeDashArray=[2, 2]))
        d.add(String(x0 + 21, ly - 3, "ideal (linear)", fontName=FONT, fontSize=6.6))
    d.add(String((x0 + x1) / 2, 2, xlabel, fontName=FONT, fontSize=7, textAnchor="middle"))
    d.add(String(2, y1 + 5, ylabel, fontName=FONT, fontSize=6.8))
    return d


def vbars(groups, series_cols, width, height, ymax, ylabel, labels_fmt="{:.2f}", series_names=None, legend=True,
          top_text=None):
    """groups: (label, [values per series]). Grouped vertical bars."""
    d = Drawing(width, height)
    left, right, bottom, top = 26, 6, 22, 20
    x0, x1, y0, y1 = left, width - right, bottom, height - top
    _axes(d, x0, y0, x1, y1)
    sy = lambda v: y0 + v / ymax * (y1 - y0)
    for i in range(0, 6):
        t = ymax * i / 5
        d.add(Line(x0 - 2.5, sy(t), x0, sy(t), strokeColor=colors.black, strokeWidth=0.5))
        d.add(Line(x0, sy(t), x1, sy(t), strokeColor=colors.HexColor("#e6e6e6"), strokeWidth=0.35))
        d.add(String(x0 - 4, sy(t) - 2.2, f"{t:g}", fontName=FONT, fontSize=6.5, textAnchor="end"))
    ng = len(groups)
    slot = (x1 - x0) / ng
    ns = len(series_cols)
    bw = slot * 0.74 / ns
    for gi, (label, vals) in enumerate(groups):
        gx = x0 + slot * gi + slot * 0.13
        for si_, v in enumerate(vals):
            d.add(Rect(gx + si_ * bw, y0, bw - 0.6, sy(v) - y0, fillColor=series_cols[si_], strokeColor=colors.black, strokeWidth=0.4))
            if labels_fmt:
                d.add(String(gx + si_ * bw + bw / 2 - 0.3, sy(v) + 1.5, labels_fmt.format(v), fontName=FONT,
                             fontSize=5.4, textAnchor="middle"))
        d.add(String(x0 + slot * (gi + 0.5), y0 - 9, label, fontName=FONT, fontSize=6.4, textAnchor="middle"))
        if top_text:
            d.add(String(x0 + slot * (gi + 0.5), y1 + 1, top_text[gi], fontName=FONT, fontSize=6.2,
                         textAnchor="middle"))
    if legend and series_names:
        lx = x0 + 4
        for name, col in zip(series_names, series_cols):
            d.add(Rect(lx, y0 - 20, 6, 5, fillColor=col, strokeColor=colors.black, strokeWidth=0.4))
            d.add(String(lx + 8, y0 - 20, name, fontName=FONT, fontSize=6.4))
            lx += 8 + pdfmetrics.stringWidth(name, FONT, 6.4) + 8
    d.add(String(2, y1 + 11, ylabel, fontName=FONT, fontSize=6.8))
    return d


def hbar_err(rows, width, height, xlabel, xmax):
    """rows: (label, median, min, max, color): linear horizontal bars with min-max whiskers."""
    d = Drawing(width, height)
    left, right, bottom, top = 62, 14, 24, 6
    x0, x1, y0, y1 = left, width - right, bottom, height - top
    _axes(d, x0, y0, x1, y1)
    sx = lambda v: x0 + v / xmax * (x1 - x0)
    for i in range(0, 6):
        t = xmax * i / 5
        d.add(Line(sx(t), y0, sx(t), y0 - 2.5, strokeColor=colors.black, strokeWidth=0.5))
        d.add(Line(sx(t), y0, sx(t), y1, strokeColor=colors.HexColor("#e6e6e6"), strokeWidth=0.35))
        d.add(String(sx(t), y0 - 10, f"{t / 1000:g}K", fontName=FONT, fontSize=6.5, textAnchor="middle"))
    slot = (y1 - y0) / len(rows)
    for i, (label, v, lo, hi, col) in enumerate(rows):
        yc = y1 - slot * (i + 0.5)
        bh = slot * 0.5
        d.add(Rect(x0, yc - bh / 2, sx(v) - x0, bh, fillColor=col, strokeColor=colors.black, strokeWidth=0.4))
        d.add(Line(sx(lo), yc, sx(hi), yc, strokeColor=colors.black, strokeWidth=0.7))
        d.add(String(x0 - 4, yc - 2.2, label, fontName=FONT, fontSize=6.8, textAnchor="end"))
        d.add(String(sx(v) + 3, yc + bh / 2 + 1, si(v) + " lines/s", fontName=FONT, fontSize=6.4))
    d.add(String((x0 + x1) / 2, 2, xlabel, fontName=FONT, fontSize=7, textAnchor="middle"))
    return d


# ------------------------------------------------------------------------------ page templates
def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Serif", 8)
    canvas.drawCentredString(PAGE_W / 2, 1.2 * cm, str(doc.page))
    canvas.restoreState()


def build_doc(fig_h):
    doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=LM, rightMargin=RM, topMargin=TOP, bottomMargin=BOT,
                          title="Real-Time Parallel Log / Event Triage Engine: Review 1 Report",
                          author="Adithya Santhosh P, Anvesha Tripathi",
                          subject="Compiler Design project review 1 (IEEE format), state at commit " + COMMIT)
    title_h = 4.2 * cm
    col_h = PAGE_H - TOP - BOT
    x1, x2 = LM, LM + COL_W + GUTTER

    def F(i, x, y, w, h):
        return Frame(x, y, w, h, id=i, leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    first = PageTemplate(id="first", onPage=footer, frames=[
        F("t", LM, PAGE_H - TOP - title_h, FULL_W, title_h),
        F("c1", x1, BOT, COL_W, col_h - title_h), F("c2", x2, BOT, COL_W, col_h - title_h)])
    later = PageTemplate(id="later", onPage=footer, frames=[F("l1", x1, BOT, COL_W, col_h),
                                                             F("l2", x2, BOT, COL_W, col_h)])
    figpage = PageTemplate(id="figpage", onPage=footer, frames=[
        F("f", LM, PAGE_H - TOP - fig_h, FULL_W, fig_h),
        F("g1", x1, BOT, COL_W, col_h - fig_h), F("g2", x2, BOT, COL_W, col_h - fig_h)])
    doc.addPageTemplates([first, later, figpage])
    return doc


def title_block():
    t = ParagraphStyle("t", fontName="Serif-Bold", fontSize=18, leading=21.5, alignment=TA_CENTER)
    st = ParagraphStyle("st", fontName="Serif-Italic", fontSize=10.5, leading=12.8, alignment=TA_CENTER)
    au = ParagraphStyle("au", fontName="Serif", fontSize=10.5, leading=12.5, alignment=TA_CENTER)
    af = ParagraphStyle("af", fontName="Serif-Italic", fontSize=9, leading=11, alignment=TA_CENTER)
    authors = Table([[Paragraph("Adithya Santhosh P<br/><font size=9>Reg. No. 24BCI0099</font>", au),
                      Paragraph("Anvesha Tripathi<br/><font size=9>Reg. No. 24BCT0055</font>", au)]],
                    colWidths=[FULL_W / 2] * 2)
    authors.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                                 ("RIGHTPADDING", (0, 0), (-1, -1), 0)]))
    return [Spacer(1, 0.1 * cm), P("Real-Time Parallel Log / Event Triage Engine", t),
            P("A Regex-to-DFA Compiler for Streaming Log Classification: Review 1 Report", st),
            Spacer(1, 0.4 * cm), authors, Spacer(1, 0.12 * cm),
            P("Team 1, Compiler Design Course Project, VIT  ·  Review 1  ·  October 2026", af), FrameBreak()]


# ------------------------------------------------------------------------------------------- text
def content(arch_png):
    s = []
    tp = M1["throughput"]
    dfa_lps, pos_lps = tp["dfa"]["lines_per_s_median"], tp["posix"]["lines_per_s_median"]
    speed = tp["speedup_dfa_over_posix"]
    spread = max((tp[e]["lines_per_s_max"] - tp[e]["lines_per_s_min"]) / tp[e]["lines_per_s_median"]
                 for e in ("dfa", "posix")) * 100
    extra = tp["dfa"]["median_startup_s"] - tp["posix"]["median_startup_s"]
    breakeven = extra / (1 / pos_lps - 1 / dfa_lps)
    sweep = {r["max_states"]: r for r in M1["state_sweep"]}
    single = sweep[1000000]["dfa_states"]
    grouped = sweep[20000]
    T = M1["tests"]
    checks = sum(T["unit_test_checks"].values())
    dt = T["difftest"]

    def r2(batch, variant):
        return [x for x in RB2 if x["batch"] == batch and x["variant"] == variant][0]["inputs_per_s"]
    thr = {v: r2(10000, v) for v in ("fork", "seq-recompile", "seq-precompiled", "threads-pattern",
                                      "threads-data_t5", "dfa_t5")}
    sp = {}
    for x in RB3:
        sp.setdefault(x["variant"], {})[x["k"]] = x["speedup_vs_same_variant_k1"]
    fork_vs_rec = thr["seq-recompile"]["median"] / thr["fork"]["median"]
    fork_vs_pre = thr["seq-precompiled"]["median"] / thr["fork"]["median"]
    dfa_vs_pre = thr["dfa_t5"]["median"] / thr["seq-precompiled"]["median"]
    tdata_vs_pre = thr["threads-data_t5"]["median"] / thr["seq-precompiled"]["median"]

    # ---------------------------------------------------------------------------- Abstract
    s.append(P("<b><i>Abstract</i></b>—Servers emit thousands of log lines per second, and the few that matter—"
               "repeated failed logins, injection attempts, crashes, leaked personal data—must be found as the "
               "lines arrive. This report presents the first milestone of a real-time log triage engine: a "
               "purpose-built regular-expression compiler and a baseline classification pipeline. Twenty-six "
               "detection rules are parsed by a recursive-descent parser, converted to a Thompson NFA, and "
               "compiled by subset construction into combined DFAs that match every rule in one linear pass over "
               "a line. The compiled DFA agrees with the C library's regex engine on all "
               f"{dt['file_lines']:,} corpus lines and {dt['random_strings']:,} generated strings "
               f"({dt['mismatches']} mismatches), and it processes {si(dfa_lps)} lines/s against "
               f"{si(pos_lps)} lines/s for the POSIX engine ({speed:.1f}× faster, one thread, end to end). "
               "Splitting the rules into groups reduces the DFA from "
               f"{single:,} states to {grouped['dfa_states']:,}. The report also reproduces and measures the "
               "course's base paper, whose process-per-pattern design is slower than a sequential loop, while "
               "splitting the input across cores scales. Checksum validation, confidence scoring, multi-core "
               "execution and attack correlation are the next milestones.", abstract))
    s.append(P("<b><i>Index Terms</i></b>—regular expressions, finite automata, subset construction, log "
               "classification, deep packet inspection, multicore processing.",
               ParagraphStyle("it", parent=abstract, fontName="Serif", spaceAfter=2)))

    # ---------------------------------------------------------------------- 1 Introduction
    s.append(S("Introduction"))
    s.append(SS("1.1", "Motivation"))
    s.append(P("Modern services write logs faster than people can read them. Hidden in the stream are lines that "
               "need action: repeated authentication failures, SQL-injection or path-traversal requests, "
               "application crashes, slow queries, and personal data that must not be stored in clear. "
               "<i>Triage</i>—assigning every line a category and a severity, and routing urgent lines to an "
               "alert stream—has to keep up with the input rate. Hand-written regular-expression rules are the "
               f"traditional way to encode this knowledge {cite('J14')}, which makes the matching engine the "
               "performance-critical part.", body0))
    s.append(P("The base paper given for this course classifies short text inputs by running one regular "
               "expression per forked process on separate cores. We treat its design and claims as hypotheses "
               "and test them. This report follows a different route to speed: compile all rules into one "
               "deterministic automaton, then (in later milestones) parallelise across input lines rather than "
               "across patterns."))
    s.append(SS("1.2", "Problem Statement and Objectives"))
    s.append(P("<i>Problem.</i> Given a stream of log lines and a set of at most 64 rules, each a regular "
               "expression with a category, a severity and a base score, assign every line the categories "
               "whose rules match it, a primary category and a severity, at a rate that keeps up with the "
               "input. The matching semantics must equal POSIX extended regular expressions on the supported "
               "subset, so that the C library can serve as an independent reference, and a rule outside the "
               "subset must be reported, never silently approximated.", body0))
    s.append(P("<i>Objectives.</i> (O1) Build a regex-to-DFA compiler from first principles: lexer, parser, "
               "Thompson NFA and subset construction. (O2) Verify it against the C library by differential "
               "testing. (O3) Provide a single-threaded pipeline that classifies real logs with two "
               "interchangeable engines. (O4) Measure the base paper's design and fairer alternatives, "
               "including our DFA, under a documented protocol. Later milestones add validation, confidence "
               "scoring, multi-core execution and attack correlation."))
    s.append(SS("1.3", "Contributions of This Milestone"))
    s.append(P("1)&nbsp;a <b>regex front end</b>: lexer and recursive-descent parser for the regular POSIX-ERE "
               "subset that rejects anything outside it;", item))
    s.append(P("2)&nbsp;a <b>regex-to-DFA compiler</b>: Thompson NFA, subset construction with accept masks "
               "and automatic rule grouping, and a table-driven matcher;", item))
    s.append(P("3)&nbsp;a <b>baseline pipeline</b>: rule loader, normaliser, batching reader, two "
               "interchangeable engines (our DFA and the C library), first classifier and JSON-lines output;", item))
    s.append(P("4)&nbsp;a faithful <b>reproduction and benchmark</b> of the base paper, with fairer "
               "baselines and our DFA as one of the variants.", item))
    s.append(SS("1.4", "Organization"))
    s.append(P("Fig. 1 gives an overview of the architecture: the rules are compiled once at start-up by the front end "
               "and the DFA compiler, the pipeline then streams lines through matching and classification, and "
               "the evaluation harness measures everything. Section 2 surveys related work, Section 3 describes each "
               "module with pseudocode, Section 4 gives the implementation and per-module results, Section 5 "
               "compares existing and proposed methods, and Section 6 concludes.", body0))
    s.append(SS("1.5", "Scope and Limitations"))
    s.append(P(f"This report covers the project at commit <tt>{COMMIT}</tt> only. Classification at this stage "
               "is rule based: a matched rule contributes its category, with no validation of the matched "
               "text, no confidence score and no masking of personal data, and the effect is visible in "
               "Section 4.4, where digit identifiers are labelled as personal data. The pipeline is "
               "single-threaded, so the multi-core results of Section 5 concern the five-pattern benchmark of "
               "the base paper, not the log pipeline itself. Accuracy figures exist for the five-pattern task, "
               "for which a labelled set was built; for the log rules we report agreement with annotated "
               "lines and with the C library, not precision and recall against human labels. The sample "
               "logs are small (2,000 lines each), so the throughput experiment repeats them, and all absolute "
               "numbers depend on the machine described in Section 4.", body0))
    s.append(FIGSLOT)

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
               f"is the option adopted for the next milestone. Two performance-theory points frame the "
               f"experiments: Gustafson's scaled-speedup argument {cite('J22')}, and Lamport's work on "
               f"specifying the interfaces of concurrent program modules {cite('J23')}, relevant to the "
               f"planned hand-off between pipeline stages. Baumann <i>et al.</i> argue that "
               f"<tt>fork()</tt> has become a poor primitive for modern systems {cite('C10')}, which is "
               f"relevant to a base-paper design that forks per pattern per input.", body0))
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
               f"studied by Gumm {cite('J21')} allow identifier-like numbers to be validated by arithmetic.",
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
        [cite("C10", "C11"), "fork cost; benchmarking", "Base-paper analysis, method"]],
        [1.7 * cm, 3.3 * cm, 3.6 * cm]))
    s.append(P("In the surveyed work, regex automata research targets network payloads, while log research "
               "targets parsing, clustering and anomaly detection. A compiled multi-rule DFA applied to "
               "per-line triage of heterogeneous logs, evaluated against a process-parallel baseline, is the gap "
               "this project addresses.", body0))

    # ------------------------------------------------------------------ 3 Proposed system
    s.append(S("Proposed System"))
    s.append(P("The engine is one C11 program with two phases (Fig. 1). At start-up, the rule file is compiled "
               "into DFA tables by the regex front end and the DFA compiler. At run time, the baseline pipeline reads log lines, "
               "normalises them, matches them with either engine, assigns a category and severity, and writes "
               "JSON-lines records. The evaluation harness reproduces the base paper and "
               "measures it together with our DFA. Table II gives the module split.", body0))
    s.append(table("Module split at this milestone", [
        ["Module", "Source files", "New", "Lines"],
        ["Regex front end", "regex_lex, regex_parse, ast", "yes", fmt(MOD_LOC["M1"], 0)],
        ["NFA/DFA compiler", "nfa, dfa, dot, engine_dfa", "yes", fmt(MOD_LOC["M2"], 0)],
        ["Baseline pipeline", "rules, reader, normalize, timestamp, batch, engine, engine_posix, "
         "aggregate, route, json, pipeline, cli, main, types", "no", fmt(MOD_LOC["M3"], 0)],
        ["Reproduction and benchmark", "tools/basepaper/ (fork, variants, runner, evaluation)", "yes",
         fmt(TOOLS_BP, 0)]],
        [2.2 * cm, 3.9 * cm, 0.8 * cm, 1.2 * cm], right=(3,)))
    s.append(P(f"The regex front end and the DFA compiler are the new contributions of the compiler and carry full pseudocode; the pipeline and the evaluation harness are "
               f"described with shorter pseudocode. The engine core totals {sum(MOD_LOC.values()):,} lines of C "
               f"in {M1['tests']['sizes']['src_files']} files.", body0))

    s.append(SS("3.1", "Regex Front End"))
    s.append(P(f"The front end turns the text of a rule's regular expression into an abstract syntax tree (AST). The "
               f"grammar is the POSIX-ERE subset—literals, <tt>.</tt>, bracket expressions, "
               f"<tt>* + ? {{n,m}}</tt>, alternation, grouping and the anchors <tt>^ $</tt>—so the same text "
               f"also runs on the C library, which makes differential testing possible. A lexer produces "
               f"tokens and the parser has one function per non-terminal (Algorithm 1). Character classes are "
               f"256-bit sets, and case-insensitive rules fold the set once at parse time. Every construct "
               f"outside the regular subset is rejected with the byte offset of the offending character; "
               f"nothing is approximated, because an approximation would silently change what a rule matches.",
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

    s.append(SS("3.2", "NFA and Combined-DFA Construction"))
    s.append(P(f"The DFA compiler compiles the ASTs of all rules into one automaton. Thompson's construction {cite('J3')} gives "
               f"every AST node a small NFA fragment with one entry and one exit (Algorithm 2); a bounded "
               f"repetition <tt>x{{n,m}}</tt> is expanded into copies. The fragments of all rules are joined "
               f"under one start state, and each accept state is tagged with its rule number. Subset "
               f"construction {cite('J1')} then builds a DFA whose states are sets of NFA states "
               f"(Algorithm 3). Three design points matter. First, each DFA state stores an <i>accept mask</i>, "
               f"a 64-bit word whose bit <i>r</i> says that rule <i>r</i> has matched, so one pass over a line "
               f"reports every matching rule. Second, to find a match anywhere in the line, the set <i>U</i> of "
               f"start states of all non-anchored rules is added to every transition; anchors are handled by "
               f"assertion edges that only fire at offset 0 (<tt>^</tt>) or at the end of the line "
               f"(<tt>$</tt>). Third, bytes that no character set of the NFA distinguishes are merged into "
               f"byte classes, so the transition function is computed once per class. When a DFA would exceed "
               f"a state cap, the rule set is halved and each half is built separately, recursively; the group "
               f"results are OR-ed at match time. Matching is the loop of Algorithm 4.", body0))
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
    s.append(algorithm("Subset construction with accept masks", [
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

    s.append(SS("3.3", "Baseline Pipeline"))
    s.append(P("The baseline is a single-threaded pipeline that makes the compiler usable and gives an independent "
               "reference. The <i>rule loader</i> reads a tab-separated file (id, category, severity, base "
               "score, flags, validator, extract list, context keywords, regex) and rejects malformed rows with "
               "file and line number. The <i>reader</i> splits a file or stdin into lines, handles CRLF, a final "
               "line without newline and very long lines, parses syslog or ISO-8601 timestamps and fills "
               "batches. The <i>normaliser</i> makes the match copy of each line: percent escapes are decoded "
               "(so <tt>%27</tt> is seen as an apostrophe), '+' becomes a space and control characters are "
               "removed, because attack strings in web logs are usually URL-encoded. Two engines sit behind one "
               "interface: the DFA engine described above and a POSIX engine that calls the C library's <tt>regexec</tt> "
               "once per rule. The first classifier assigns each matched line the categories of its rules, the "
               "primary category by highest severity, and writes JSON-lines records to a store file and an "
               "alert file (severity HIGH or above).", body0))
    s.append(algorithm("Baseline pipeline", [
        (0, f"{kw('function')} Run(<i>rulefile</i>, <i>input</i>, <i>engine</i>):"),
        (1, "<i>rules</i> ← LoadRules(<i>rulefile</i>); <i>E</i> ← Compile(<i>rules</i>, <i>engine</i>)"),
        (1, kw("while") + " <i>batch</i> ← ReadBatch(<i>input</i>, B) " + kw("do")),
        (2, kw("for each") + " line <i>i</i> in <i>batch</i> " + kw("do")),
        (3, "<i>norm</i> ← Normalize(raw<sub>i</sub>)   // %xx, '+', control chars"),
        (3, "<i>mask</i> ← <i>E</i>.Match(<i>norm</i>)"),
        (3, "<i>labels</i> ← categories of rules in <i>mask</i>"),
        (3, "<i>primary</i> ← label of highest severity (tie order fixed)"),
        (3, "write JSON record to store; " + kw("if") + " severity ≥ HIGH: also to alerts")]))

    s.append(SS("3.4", "Base-Paper Reproduction and Benchmark"))
    s.append(P("The evaluation harness rebuilds the base paper's program as described and measures it. For each input the program "
               "creates a pipe, forks one child per pattern, pins each child to a core, and the child "
               "compiles its regular expression, matches it and reports the result through the pipe "
               "(Algorithm 6); the parent collects the five records and reaps the children. The paper's "
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
    sizes = M1["tests"]["sizes"]
    s.append(P(f"All results come from commit <tt>{COMMIT}</tt>, built with gcc 15.2 and "
               f"<tt>-O2 -Wall -Wextra -Wpedantic -pthread</tt> without warnings, on a 13th-generation Intel "
               f"Core i7-13650HX (14 cores) under Linux. The code base has {sizes['src_lines']:,} lines of "
               f"engine source, {sizes['test_lines']:,} lines of tests and {sizes['basepaper_tool_lines']:,} "
               f"lines of base-paper tooling. Every measurement is stored under <tt>results/</tt>.", body0))
    s.append(SS("4.1", "Dataset Description"))
    corpus = {c["file"]: c for c in M1["corpus"]}
    lab = RB4["none"]["lines"]
    s.append(table("Data sets", [
        ["Data set", "Source", "Lines", "Use"],
        ["OpenSSH_2k", "Loghub sample", f"{corpus['OpenSSH_2k.log']['lines']:,}", "AUTH rules"],
        ["Apache_2k", "Loghub sample", f"{corpus['Apache_2k.log']['lines']:,}", "web-server log"],
        ["Linux_2k", "Loghub sample", f"{corpus['Linux_2k.log']['lines']:,}", "syslog / PAM"],
        ["BGL_2k", "Loghub sample", f"{corpus['BGL_2k.log']['lines']:,}", "ERROR rules"],
        ["HDFS_2k", "Loghub sample", f"{corpus['HDFS_2k.log']['lines']:,}", "large-volume log"],
        ["synthetic_small", "hand-written", f"{corpus['synthetic_small.log']['lines']}",
         f"{M1['tests']['annotated_lines_synthetic_small']} annotated lines: positives, near-misses"],
        ["labelled set", "generated, fixed seed", f"{lab:,}", "five base-paper categories"]],
        [2.3 * cm, 2.3 * cm, 1.0 * cm, 3.0 * cm], right=(2,)))
    s.append(P(f"The first five are the 2,000-line samples published with Loghub {cite('C8')}. They carry no "
               f"labels, so for them we report what the engine finds. The hand-written corpus annotates each "
               f"line with the exact set of rules that must match it, including look-alikes such as a "
               f"12-digit order number or the word “errors”. The labelled set of {lab:,} inputs is generated "
               f"from templates with a fixed seed, balanced over e-mail, phone, question, greeting and name "
               f"plus none, multi-category and near-miss lines; its ground truth describes what a line "
               f"<i>is</i>, not what the regexes match.", body0))

    s.append(SS("4.2", "Regex Front End"))
    rej = [("a\\d+", r"\d shorthand not supported (write [0-9])", 1), ("a\\bb", r"\b word boundary not supported", 1),
           ("(a)\\1", "backreference \\1 (not a regular language)", 3),
           ("(?=a)b", "\"(?\" groups: lookaround not supported", 0),
           ("a*?b", "lazy / possessive quantifier", 2), ("a{,3}", "{,n} not supported; write {0,n}", 1),
           ("a^b", "'^' only allowed at pattern start", 1), ("(ab", "unbalanced '('", 0),
           ("a{2,1}", "invalid interval, m &lt; n", 1)]
    s.append(table("Rejected syntax (message and byte offset)", [["Pattern", "Compiler message", "Offset"]] +
                   [[f"<font name='Mono' size='7'>{escape(p)}</font>", escape(m).replace("&amp;lt;", "&lt;"),
                     str(o)] for p, m, o in rej],
                   [1.5 * cm, 6.0 * cm, 1.0 * cm], right=(2,)))
    s.append(P(f"All 26 default rules parse. The parser test has 97 valid and invalid cases "
               f"({T['unit_test_checks']['test_parse']} checks). Table IV shows that every unsupported construct "
               f"is refused with a message that names a regular alternative where one exists. <i>Inference:</i> "
               f"the front end enforces the regular subset strictly; a rule author is told how to rewrite the "
               f"rule instead of getting silently different behaviour.", body0))

    s.append(SS("4.3", "NFA and DFA Construction"))
    s.append(P(f"The 26 rules give a combined NFA of {M1['state_sweep'][0]['nfa_states']:,} states. Table V and "
               f"Fig. 2 show what the state cap does. Without a cap the combined DFA has {single:,} states and "
               f"takes {sweep[1000000]['build_ms']:.0f} ms to build; with the default cap of 20,000 it is "
               f"split into {grouped['groups']} groups with {grouped['dfa_states']:,} states in total "
               f"({grouped['build_ms']:.0f} ms), a {single / grouped['dfa_states']:.1f}× reduction in table "
               f"states. Tighter caps give more groups: at 500 states the DFA needs "
               f"{sweep[500]['groups']} groups of {sweep[500]['dfa_states']:,} states in total, built in "
               f"{sweep[500]['build_ms']:.0f} ms.", body0))
    s.append(table("DFA size versus state cap (26 rules)", [["Cap", "Groups", "DFA states", "Build (ms)"]] + [
        [f"{r['max_states']:,}", str(r["groups"]), f"{r['dfa_states']:,}", f"{r['build_ms']:.0f}"]
        for r in M1["state_sweep"] if r["max_states"] != 100000],
        [2.0 * cm, 1.6 * cm, 2.2 * cm, 2.0 * cm], right=(0, 1, 2, 3)))
    cols = {c["max_states"]: c for c in M1["state_sweep"]}
    order = [500, 1000, 2000, 5000, 20000, 1000000]
    grp = [(("1M" if k == 1000000 else f"{k // 1000}K" if k >= 1000 else str(k)),
            [cols[k]["dfa_states"] / 1000]) for k in order]
    s.append(figure(vbars(grp, [BLUE], COL_W, 100, 50, "DFA states (thousands)", "{:.1f}", legend=False,
                          top_text=[f"{cols[k]['groups']} grp" for k in order]),
                    "Total DFA states against the state cap; the label above each bar is the number of groups."))
    s.append(P(f"<i>Correctness.</i> The differential tester compares the DFA's rule mask with the C library's "
               f"for every rule on every line of the {dt['files']} corpus files ({dt['file_lines']:,} lines) "
               f"and on {dt['random_strings']:,} generated strings: <b>{dt['mismatches']} mismatches</b>. The "
               f"DFA test (<tt>{T['unit_test_checks']['test_dfa']:,}</tt> checks) additionally compares the DFA "
               f"with an NFA simulator, and the NFA test ({T['unit_test_checks']['test_nfa']:,} checks) "
               f"compares that simulator with <tt>regexec</tt>. <i>Inference:</i> the combined DFA is "
               f"semantically equal to the reference, and grouping trades a few more table lookups per byte "
               f"(one pass per group) for much smaller tables.", body0))

    s.append(SS("4.4", "Baseline Pipeline"))
    rows = [["File", "Lines", "Rule hits", "Primary label counts"]]
    short = {"AUTH_FAILURE": "AUTH", "SECURITY_THREAT": "SEC", "ERROR": "ERR", "PERF_WARNING": "PERF",
             "PII": "PII"}
    for c in M1["corpus"]:
        pl = ", ".join(f"{short[k]} {v}" for k, v in sorted(c["primary_label_counts"].items(),
                                                          key=lambda kv: -kv[1])) or "none"
        rows.append([c["file"].replace(".log", ""), f"{c['lines']:,}", f"{c['lines_with_rule_match']:,}", pl])
    s.append(table("Classification output at this milestone", rows, [2.4 * cm, 1.0 * cm, 1.3 * cm, 3.9 * cm],
                   right=(1, 2)))
    s.append(P(f"The two engines produce the same labels, and both reproduce the annotated rule set of all "
               f"{T['annotated_lines_synthetic_small']} annotated lines of the hand-written corpus; the "
               f"end-to-end test compares the pipeline output with an expected file for both engines. "
               f"Table VI shows the distribution on real logs: OpenSSH is dominated by authentication "
               f"failures ({corpus['OpenSSH_2k.log']['primary_label_counts']['AUTH_FAILURE']:,} of "
               f"{corpus['OpenSSH_2k.log']['lines']:,} lines) and BGL by errors. <i>Inference and limitation:</i> "
               f"the {corpus['HDFS_2k.log']['primary_label_counts']['PII']} lines labelled PII in HDFS are "
               f"block and task identifiers (for example <tt>blk_278357163850888</tt>), long digit strings "
               f"that match the card and Aadhaar patterns. Pattern matching alone cannot tell an identifier "
               f"from a card number; checksum validation of the matched digits is the planned remedy, and "
               f"the same effect explains the PII hits in the Linux sample.", body0))

    s.append(SS("4.5", "Base-Paper Reproduction"))
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
        rows.append([f"<font name='Mono' size='6.8'>{escape(name)}</font>"] +
                    [(f"{v[k][i][1]:.0f}" if v[k][i][1] >= 100 else f"{v[k][i][1]:.3g}")
                     for k in ("fork", "seq-recompile", "seq-precompiled", "threads-pattern", "dfa_t1")])
    s.append(table("Latency per input, median (µs)", rows,
                   [2.6 * cm, 1.1 * cm, 1.2 * cm, 1.2 * cm, 1.2 * cm, 1.1 * cm], right=(1, 2, 3, 4, 5)))
    fk = [v["fork"][i][1] for i in v["fork"]]
    s.append(P(f"The reproduced program classifies one input in {min(fk):.0f}–{max(fk):.0f} µs, while a "
               f"sequential loop that recompiles the five patterns needs "
               f"{min(v['seq-recompile'][i][1] for i in v['fork']):.0f}–"
               f"{max(v['seq-recompile'][i][1] for i in v['fork']):.0f} µs (Table VII). Inside each child, "
               f"<tt>regcomp</tt> takes about 20 µs and <tt>regexec</tt> about 3–5 µs, so process management "
               f"is most of the {min(fk):.0f} µs. The paper's input module also rejects "
               f"{RB4['paper']['rejected_by_input_module']} of the {lab:,} labelled lines (for example those "
               f"containing '+' or an apostrophe), because its character whitelist is narrower than the "
               f"inputs. <i>Inference:</i> the paper's central hypothesis—that process overhead dominates—is "
               f"confirmed, and it works against the paper's own speed-up claim.", body0))

    # ------------------------------------------------------------------ 5 Performance
    s.append(S("Performance and Metrics"))
    s.append(SS("5.1", "Matching Engine: Existing versus Proposed"))
    s.append(P(f"We compare the C library's POSIX engine (one <tt>regexec</tt> per rule, the approach of the "
               f"base paper's matching step) with the proposed combined DFA. Both run the same 26 rules inside "
               f"the same single-threaded pipeline on {tp['lines']:,} log lines ({tp['bytes'] / 1e6:.0f} MB, the "
               f"six corpus files repeated 100 times), output written to memory-backed files; the start-up "
               f"cost of each engine (rule compilation) is measured separately on an empty input and "
               f"subtracted. Five runs per engine were interleaved.", body0))
    s.append(figure(hbar_err([("POSIX regexec", pos_lps, tp["posix"]["lines_per_s_min"],
                               tp["posix"]["lines_per_s_max"], ORANGE),
                              ("Combined DFA", dfa_lps, tp["dfa"]["lines_per_s_min"],
                               tp["dfa"]["lines_per_s_max"], BLUE)],
                             COL_W, 62, "lines per second (median of 5 runs, whiskers = min–max)", 600000),
                    "End-to-end throughput of the two engines on real log rules, one thread."))
    s.append(P(f"The DFA engine sustains {si(dfa_lps)} lines/s and the POSIX engine {si(pos_lps)} lines/s, "
               f"a factor of <b>{speed:.1f}</b>; the spread between the five runs is at most {spread:.0f} % of the median. "
               f"<i>Inference:</i> in a loop of 26 library calls every line is scanned once per rule, and the "
               f"library's cost grows with the rule count; the DFA scans the line once whatever the rule count. "
               f"The factor is a lower bound on the matching speed-up, because the measurement includes the "
               f"unchanged reader, classifier and output. The price is the start-up cost: building the DFAs "
               f"takes {tp['dfa']['median_startup_s'] * 1000:.0f} ms against "
               f"{tp['posix']['median_startup_s'] * 1000:.0f} ms for compiling the library's regexes, which "
               f"is repaid after about {breakeven / 1000:.0f} thousand lines.",
               body0))

    s.append(SS("5.2", "Base-Paper Design versus Alternatives"))
    spreads = {x["variant"]: (x["inputs_per_s"]["max"] - x["inputs_per_s"]["min"]) / x["inputs_per_s"]["median"] * 100
               for x in RB2 if x["batch"] == 10000}
    low = sorted(v for v in spreads.values() if v <= 10)
    high = sorted(v for v in spreads.values() if v > 10)
    s.append(P(f"Fig. 4 compares six designs on the five-pattern task for a batch of 10,000 inputs (median of "
               f"five runs). The run-to-run spread (maximum minus minimum, relative to the median) is at most "
               f"{max(low):.0f} % for {len(low)} designs and {high[0]:.0f} % and {high[1]:.0f} % for the two "
               f"fastest, whose 10,000-input runs last only a few milliseconds or less and are dominated by "
               f"timing noise; the scaling runs of Fig. 5 therefore use longer runs, up to 10 million inputs.",
               body0))
    rows = [("fork (paper)", thr["fork"], ORANGE), ("sequential, recompile", thr["seq-recompile"], GREY),
            ("5 threads, one per pattern", thr["threads-pattern"], GREY),
            ("sequential, precompiled", thr["seq-precompiled"], GREY),
            ("threads over inputs (5)", thr["threads-data_t5"], GREEN),
            ("DFA, threads over inputs (5)", thr["dfa_t5"], BLUE)]
    s.append(figure(hbar_log([(a, b["median"], b["min"], b["max"], c) for a, b, c in rows], COL_W, 100,
                             "inputs per second (log scale)", 1e3, 2e8,
                             [(1e3, "1K"), (1e4, "10K"), (1e5, "100K"), (1e6, "1M"), (1e7, "10M"), (1e8, "100M")]),
                    "Throughput of six designs for 10,000 inputs (existing: first row; proposed: last row)."))
    kmax = lambda var: max(sp[var][k] for k in sp[var])
    s.append(P(f"The forking design reaches {si(thr['fork']['median'])} inputs/s, which is "
               f"{fork_vs_rec:.1f}× slower than a sequential loop that recompiles and {fork_vs_pre:.0f}× slower "
               f"than one with precompiled patterns. Splitting the <i>inputs</i> across five "
               f"threads gives {tdata_vs_pre:.1f}× the sequential precompiled loop, and our DFA with the same "
               f"split {dfa_vs_pre:.1f}×. Fig. 5 shows scaling with cores: the fork design peaks at "
               f"{kmax('fork'):.2f}× and per-pattern threads at {kmax('threads-pattern'):.2f}×, because five "
               f"patterns cannot give more than five-fold parallelism and the setup cost is paid per input; "
               f"data-parallel threads reach {max(sp['threads-data'].values()):.2f}× and the DFA variant "
               f"{max(sp['dfa'].values()):.2f}× at six cores; the eight-core point is lower "
               f"because it uses two slower efficiency cores. <i>Inference:</i> the base paper's design cannot deliver its claimed "
               f"near-linear scaling on this hardware; parallelism over input items does.", body0))
    ser = [("fork (paper)", sorted(sp["fork"].items()), ORANGE, None),
           ("5 threads / patterns", sorted(sp["threads-pattern"].items()), GREY, [3, 1.5]),
           ("threads over inputs", sorted(sp["threads-data"].items()), GREEN, None),
           ("DFA over inputs", sorted(sp["dfa"].items()), BLUE, None)]
    s.append(figure(line_chart(ser, COL_W, 98, [1, 2, 4, 6, 8], 8, "cores", "speed-up vs. 1 core"),
                    "Speed-up against the same design on one core (median of 5 runs)."))

    s.append(SS("5.3", "Accuracy: Existing versus Proposed"))
    a, b = RB4["paper"], RB4["none"]
    s.append(P(f"On the labelled set of {lab:,} inputs we compare the base paper's pipeline (its input module "
               f"followed by <tt>regexec</tt>; <i>existing</i>) with matching on the unmodified text with the "
               f"same five patterns, as done by the DFA variant (<i>proposed</i>). The DFA gives the same masks as "
               f"<tt>regexec</tt> on every line (asserted by a test), so any difference comes from the paper's "
               f"input module. Table VIII and Fig. 6 give precision (P), recall (R) and F1 per category.", body0))
    cats = ["EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"]
    rows = [["Category", "P", "R", "F1", "P", "R", "F1"]]
    rows[0][1:] = ["P<br/>exist.", "R<br/>exist.", "F1<br/>exist.", "P<br/>prop.", "R<br/>prop.", "F1<br/>prop."]
    for c in cats:
        pa, pb = a["per_category"][c], b["per_category"][c]
        nz = lambda v: "—" if v is None else f"{v:.2f}"
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
               f"templates). "
               f"<i>Inference:</i> the proposed approach does not lose accuracy relative to the library; "
               f"any accuracy gain comes from not applying a lossy input stage.", body0))
    s.append(KeepTogether([SS("5.4", "Summary of Metrics")] + table("Existing versus proposed", [
        ["Metric", "Existing", "Proposed"],
        ["Log-rule throughput, 1 thread", f"{si(pos_lps)} lines/s", f"{si(dfa_lps)} lines/s"],
        ["Inputs/s, 5 patterns, batch 10K", f"{si(thr['fork']['median'])} (fork)",
         f"{si(thr['dfa_t5']['median'])} (DFA)"],
        ["Peak scaling vs. 1 core", f"{kmax('fork'):.2f}× (fork)", f"{max(sp['dfa'].values()):.2f}× (DFA)"],
        ["Mask agreement with regexec", "(reference)", f"{dt['mismatches']} mismatches"],
        ["Exact-set accuracy", f"{a['overall']['exact_match_accuracy']:.3f}",
         f"{b['overall']['exact_match_accuracy']:.3f}"]],
        [3.8 * cm, 2.4 * cm, 2.4 * cm], flat=True)))

    # ------------------------------------------------------------------ 6 Conclusion
    s.append(S("Conclusion"))
    s.append(P(f"This milestone delivers a working regex-to-DFA compiler and a baseline log-classification "
               f"pipeline, verified against the C library on {dt['file_lines']:,} corpus lines and "
               f"{dt['random_strings']:,} random strings, together with a measured reproduction of the base "
               f"paper. On the same 26 rules the DFA engine is {speed:.1f}× faster than the library, grouping "
               f"keeps the tables at {grouped['dfa_states']:,} states instead of {single:,}, and the base "
               f"paper's process-per-pattern design is {fork_vs_rec:.0f}× slower than a sequential loop. The "
               f"main limitations seen so far are the start-up cost of DFA construction "
               f"({tp['dfa']['median_startup_s'] * 1000:.0f} ms) and false PII labels from digit identifiers "
               f"when no validation is applied.", body0))
    s.append(P("The next milestones are: checksum validators and a confidence model for multi-label output; "
               "masking of personal data; a multi-core pipeline with line-level parallelism and lock-free "
               "queues; correlation of attacks across lines; and evaluation on the full data sets with "
               "hand-labelled samples.", body))

    s.append(Paragraph("REFERENCES", sec))
    for i, k in enumerate(ORDER, 1):
        s.append(Paragraph(f"[{i}]&nbsp;&nbsp;{REFS[k]}", ref))
    return s


def main():
    with tempfile.TemporaryDirectory() as tmp:
        arch = os.path.join(tmp, "arch.png")
        subprocess.run(["dot", "-Tpng", "-Gdpi=300", os.path.join(ROOT, "tools/report/r1_arch.dot"), "-o", arch],
                       check=True)
        iw, ih = ImageReader(arch).getSize()
        fig_h = FULL_W * ih / iw + 1.4 * cm
        doc = build_doc(fig_h)
        arch_fig = figure(image(arch, FULL_W), "System architecture: start-up compilation, "
                          "baseline pipeline and evaluation harness.", wide=True)   # Fig. 1
        body_flow = content(arch)
        # the full-width architecture figure goes at the top of the page where Section 3 starts
        idx = body_flow.index(FIGSLOT)
        full = [NextPageTemplate("figpage"), PageBreak(), arch_fig, FrameBreak(), NextPageTemplate("later")]
        story = title_block() + [NextPageTemplate("later")] + body_flow[:idx] + full + body_flow[idx + 1:]
        doc.build(story)
    print(OUT, "references:", len(ORDER))


if __name__ == "__main__":
    main()
