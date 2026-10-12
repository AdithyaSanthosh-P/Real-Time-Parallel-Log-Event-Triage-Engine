#!/usr/bin/env python3
"""Minimal plotting for the Phase R experiments (plan R4); T6.5 extends it for E1-E11.

  plot.py --results results/basepaper --out DIR

Reads <results>/rbN/summary.json (written by bp_eval.py summarize) and writes one SVG per
experiment into DIR: rb1.svg ... rb5.svg. Points are medians; bars show min and max over
runs. Standard library only (no matplotlib on the build machine), so the figures are plain
SVG: open them in a browser or convert for the report."""
import argparse
import json
import math
import os
import sys
from xml.sax.saxutils import escape

PALETTE = ["#2f6db5", "#d9822b", "#3a9d5d", "#b8434f", "#7b5ea7", "#8c8c3a", "#3aa7b0"]
W, H = 900, 520
ML, MR, MT, MB = 80, 190, 50, 90          # margins: left, right (legend), top, bottom


def fmt(v):
    if v >= 1e9: return f"{v / 1e9:g}G"
    if v >= 1e6: return f"{v / 1e6:g}M"
    if v >= 1e3: return f"{v / 1e3:g}k"
    return f"{v:g}"


class Chart:
    def __init__(self, title, xlabel, ylabel, logy=True):
        self.title, self.xlabel, self.ylabel, self.logy = title, xlabel, ylabel, logy
        self.items = []             # drawn after the axes are known
        self.ymin = self.ymax = None

    def span(self, *vals):
        for v in vals:
            if v is None or (self.logy and v <= 0):
                continue
            self.ymin = v if self.ymin is None else min(self.ymin, v)
            self.ymax = v if self.ymax is None else max(self.ymax, v)

    def y(self, v):
        lo, hi = self.lo, self.hi
        if self.logy:
            v = max(v, lo)
            t = (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
        else:
            t = (v - lo) / (hi - lo)
        return H - MB - t * (H - MB - MT)

    def axes(self):
        if self.ymin is None:
            self.ymin, self.ymax = 1, 10
        if self.logy:
            self.lo = 10 ** math.floor(math.log10(self.ymin))
            self.hi = 10 ** math.ceil(math.log10(self.ymax))
            if self.hi == self.lo:
                self.hi *= 10
            ticks = [10 ** e for e in range(round(math.log10(self.lo)), round(math.log10(self.hi)) + 1)]
        else:
            self.lo = 0
            step = 10 ** math.floor(math.log10(self.ymax))
            self.hi = math.ceil(self.ymax / step) * step
            ticks = [self.lo + i * (self.hi - self.lo) / 5 for i in range(6)]
        out = []
        for t in ticks:
            yy = self.y(t)
            out.append(f'<line x1="{ML}" x2="{W - MR}" y1="{yy:.1f}" y2="{yy:.1f}" stroke="#ddd"/>')
            out.append(f'<text x="{ML - 8}" y="{yy + 4:.1f}" text-anchor="end" font-size="11">{fmt(t)}</text>')
        out.append(f'<line x1="{ML}" x2="{ML}" y1="{MT}" y2="{H - MB}" stroke="#333"/>')
        out.append(f'<line x1="{ML}" x2="{W - MR}" y1="{H - MB}" y2="{H - MB}" stroke="#333"/>')
        out.append(f'<text x="{(ML + W - MR) / 2}" y="{H - 12}" text-anchor="middle" font-size="12">{escape(self.xlabel)}</text>')
        out.append(f'<text x="18" y="{(MT + H - MB) / 2}" text-anchor="middle" font-size="12" '
                   f'transform="rotate(-90 18 {(MT + H - MB) / 2})">{escape(self.ylabel)}</text>')
        out.append(f'<text x="{ML}" y="28" font-size="15" font-weight="bold">{escape(self.title)}</text>')
        return out

    def svg(self, legend):
        body = self.axes() + [f(self) for f in self.items]
        for i, name in enumerate(legend):
            yy = MT + 8 + i * 18
            body.append(f'<rect x="{W - MR + 14}" y="{yy - 9}" width="12" height="12" fill="{PALETTE[i % len(PALETTE)]}"/>')
            body.append(f'<text x="{W - MR + 32}" y="{yy + 2}" font-size="12">{escape(name)}</text>')
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
                f'font-family="sans-serif"><rect width="100%" height="100%" fill="white"/>'
                + "".join(body) + "</svg>\n")


def errbar(c, x, row, color):
    """median marker/bar with a min-max whisker; row = {median,min,max}."""
    def draw(ch):
        y0, y1 = ch.y(row["min"]), ch.y(row["max"])
        return f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{y0:.1f}" y2="{y1:.1f}" stroke="#222" stroke-width="1.5"/>'
    c.items.append(draw)
    c.span(row["min"], row["median"], row["max"])


def grouped_bars(title, ylabel, groups, series, value, logy=True):
    """groups: x categories; series: bar colors/legend; value(g, s) -> {median,min,max} or None."""
    c = Chart(title, "", ylabel, logy)
    gw = (W - ML - MR) / max(len(groups), 1)
    bw = gw * 0.8 / max(len(series), 1)
    for gi, g in enumerate(groups):
        for si, s in enumerate(series):
            row = value(g, s)
            if not row:
                continue
            x = ML + gi * gw + gw * 0.1 + si * bw
            c.span(row["min"], row["median"], row["max"])
            def bar(ch, x=x, row=row, si=si):
                top, base = ch.y(row["median"]), H - MB
                r = f'<rect x="{x:.1f}" y="{top:.1f}" width="{bw * 0.9:.1f}" height="{max(base - top, 0.5):.1f}" fill="{PALETTE[si % len(PALETTE)]}"/>'
                xm = x + bw * 0.45
                r += f'<line x1="{xm:.1f}" x2="{xm:.1f}" y1="{ch.y(row["min"]):.1f}" y2="{ch.y(row["max"]):.1f}" stroke="#222" stroke-width="1.5"/>'
                return r
            c.items.append(bar)
        def label(ch, gi=gi, g=g):
            return (f'<text x="{ML + gi * gw + gw / 2:.1f}" y="{H - MB + 16}" text-anchor="middle" '
                    f'font-size="11">{escape(str(g))}</text>')
        c.items.append(label)
    return c.svg(series)


def lines(title, xlabel, ylabel, xs, series, value, logx=False, logy=True):
    """series: names; value(s, x) -> {median,min,max} or None. x positions evenly spaced (categorical)."""
    c = Chart(title, xlabel, ylabel, logy)
    step = (W - ML - MR) / max(len(xs), 1)
    xpos = {x: ML + step * (i + 0.5) for i, x in enumerate(xs)}
    for si, s in enumerate(series):
        pts = [(xpos[x], value(s, x)) for x in xs if value(s, x)]
        for _, r in pts:
            c.span(r["min"], r["median"], r["max"])
        def draw(ch, pts=pts, si=si):
            col = PALETTE[si % len(PALETTE)]
            path = " ".join(f'{px:.1f},{ch.y(r["median"]):.1f}' for px, r in pts)
            out = f'<polyline points="{path}" fill="none" stroke="{col}" stroke-width="2"/>'
            for px, r in pts:
                out += (f'<line x1="{px:.1f}" x2="{px:.1f}" y1="{ch.y(r["min"]):.1f}" y2="{ch.y(r["max"]):.1f}" stroke="{col}"/>'
                        f'<circle cx="{px:.1f}" cy="{ch.y(r["median"]):.1f}" r="3.5" fill="{col}"/>')
            return out
        c.items.append(draw)
    for x in xs:
        c.items.append(lambda ch, x=x: f'<text x="{xpos[x]:.1f}" y="{H - MB + 16}" text-anchor="middle" font-size="11">{escape(str(x))}</text>')
    return c.svg(series)


# ---- one figure per experiment -----------------------------------------------------------------

def load(res, name):
    p = os.path.join(res, name, "summary.json")
    if not os.path.exists(p):
        return None
    with open(p) as f:
        return json.load(f)


def fig_rb1(rows):
    variants = sorted({r["variant"] for r in rows})
    inputs = sorted({r["input_index"] for r in rows})
    names = {r["input_index"]: r["input"] for r in rows}
    idx = {(r["input_index"], r["variant"]): r for r in rows}
    return grouped_bars("RB1 single-input latency (median per run; bars = min..max of run medians)",
                        "latency per input, ns (log)", [names[i] for i in inputs], variants,
                        lambda g, v: idx.get((inputs[[names[i] for i in inputs].index(g)], v), {}).get("p50_ns"))


def fig_rb2(rows):
    variants = sorted({r["variant"] for r in rows})
    sizes = sorted({r["batch"] for r in rows})
    idx = {(r["variant"], r["batch"]): r["inputs_per_s"] for r in rows}
    return lines("RB2 throughput vs batch size", "inputs per batch", "inputs / s (log)", sizes, variants,
                 lambda v, n: idx.get((v, n)))


def fig_rb3(rows, vs_seq=False):
    """Speedup of inputs/s: vs the same variant at k=1, or vs seq-precompiled at k=1."""
    variants = sorted({r["variant"] for r in rows if r["variant"] != "seq-precompiled"})
    ks = sorted({r["k"] for r in rows})
    idx = {(r["variant"], r["k"]): r for r in rows}
    def v(var, k):
        r = idx.get((var, k))
        base_row = idx.get(("seq-precompiled", 1)) if vs_seq else idx.get((var, 1))
        if not r or not base_row:
            return None
        base = base_row["inputs_per_s"]["median"]
        x = r["inputs_per_s"]
        return {"median": x["median"] / base, "min": x["min"] / base, "max": x["max"] / base}
    what = "seq-precompiled on 1 core" if vs_seq else "the same variant on 1 core"
    return lines(f"RB3 core scaling: speedup vs {what}", "cores (one logical CPU per core)",
                 "speedup (log)", ks, variants, v)


def fig_rb4(res):
    cats = ["EMAIL", "PHONE", "QUESTION", "GREETING", "NAME"]
    modes = [m for m in ("none", "paper") if m in res]
    def v(cat, mode):
        f = res[mode]["per_category"][cat]["f1"]
        return None if f is None else {"median": f, "min": f, "max": f}
    return grouped_bars("RB4 per-category F1 on the labeled set (deterministic: single run)", "F1",
                        cats, ["normalize=" + m for m in modes],
                        lambda g, s: v(g, s.split("=")[1]), logy=False)


def fig_rb5(rows):
    rows = [r for r in rows if "variant" in r]
    metrics = [("rusage_user_s", "user (self)"), ("rusage_sys_s", "sys (self)"),
               ("rusage_children_user_s", "user (children)"), ("rusage_children_sys_s", "sys (children)")]
    return grouped_bars("RB5 CPU time over the run, seconds (getrusage)", "seconds (log)",
                        [r["variant"] for r in rows], [m[1] for m in metrics],
                        lambda g, s: next(r for r in rows if r["variant"] == g)[
                            next(k for k, n in metrics if n == s)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    made = 0
    for name, fn in (("rb1", fig_rb1), ("rb2", fig_rb2), ("rb3", fig_rb3), ("rb4", fig_rb4), ("rb5", fig_rb5)):
        data = load(args.results, name)
        if not data:
            continue
        figs = [(name, fn(data))]
        if name == "rb3":
            figs.append(("rb3_vs_seq", fig_rb3(data, vs_seq=True)))
        for fname, svg in figs:
            with open(os.path.join(args.out, fname + ".svg"), "w") as f:
                f.write(svg)
            made += 1
    print(f"plot: wrote {made} figure(s) to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
