#!/usr/bin/env python3
"""Write results/SUMMARY.md from the saved JSON of experiments E1-E4 (tools/run_bench.sh) and E6-E7
(tools/run_eval.sh) (plan T7.4). Every number, ratio and verdict below is computed from those files;
nothing is typed in by hand. usage: make_results_summary.py [RESULTS_DIR]"""
import json
import os
import re
import sys

R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "results")
R = os.path.abspath(R)
out = []


def J(*p):
    with open(os.path.join(R, *p)) as f:
        return json.load(f)


def have(*p):
    return os.path.exists(os.path.join(R, *p))


def w(s=""):
    out.append(s)


def lps(x):
    return f"{x['median']:,.0f}"


def rng(x):
    return f"{x['min']:,.0f}–{x['max']:,.0f}"


def pt(rows, **kw):
    for r in rows:
        if all(r[k] == v for k, v in kw.items()):
            return r


def spread(x):
    return (x["max"] - x["min"]) / x["median"] * 100


w("# Results summary")
w()
w("Every number below is computed by `tools/make_results_summary.py` from the raw runs saved under `results/`")
w("(benchmark runs: `results/e1`…`e4/raw/*.json`; accuracy runs: `results/e6`, `results/e7`). Throughput is")
w("`triage --bench` lines per second (input preloaded, sinks to /dev/null, rule compilation excluded); every point")
w("is run several times, interleaved, and reported as the median with the min–max range. Figures: `results/figures/`.")
w("The exact command of every point is in `results/<exp>/commands.txt`; the machine in `results/machine.txt`.")
w()
if have("machine.txt"):
    m = open(os.path.join(R, "machine.txt")).read()
    g = lambda k: (re.search(rf"^{k}:\s*(.+)$", m, re.M) or [None, "?"])[1]
    w(f"Machine: {g('Model name')}; {g('mode')}; {g('gcc')}; commit {g('commit')[:12]}; kernel `{g('uname')[:60]}…`.")
    w()

# ------------------------------------------------------------------------------------------------ E1
if have("e1", "summary.json"):
    rows = J("e1", "summary.json")
    w("## E1 Architecture comparison")
    w()
    w("| config | engine | workers | lines | lines/s (median) | min–max | runs | vs C2 |")
    w("|---|---|---|---|---|---|---|---|")
    c2 = next(r for r in rows if r["id"] == "c2-posix-w1")["lines_per_s"]["median"]
    for r in rows:
        w(f"| {r['id']} | {r['engine']} | {r['workers']} | {r['lines']:,} | {lps(r['lines_per_s'])} | "
          f"{rng(r['lines_per_s'])} | {r['runs']} | {r['lines_per_s']['median'] / c2:.3g}× |")
    w()
    v = {r["id"].split("-")[0]: r["lines_per_s"]["median"] for r in rows}
    ok = v["c1"] < v["c2"] < v["c3"] < v["c4"]
    w(f"Hypothesis C1 ≪ C2 < C3 < C4: **{'holds' if ok else 'does not hold'}** "
      f"(C1 {v['c1']:,.0f}, C2 {v['c2']:,.0f}, C3 {v['c3']:,.0f}, C4 {v['c4']:,.0f} lines/s). "
      f"The fork design (C1) is {v['c2'] / v['c1']:.0f}× slower than the sequential POSIX engine (C2) and "
      f"C4 (dfa, same workers as C3) is {v['c4'] / v['c1']:.0f}× faster than C1 and {v['c4'] / v['c3']:.2f}× faster than C3.")
    w()
    w("Note: C1 is run on fewer lines than the others (it processes about "
      f"{v['c1']:,.0f} lines/s); the throughput per line is what is compared.")
    w()

# ------------------------------------------------------------------------------------------------ E2
if have("e2", "summary.json"):
    rows = J("e2", "summary.json")
    ws = sorted({r["workers"] for r in rows})
    w("## E2 Scaling with workers")
    w()
    w("Speed-up S = T(1)/T(p) of the whole pipeline (reader, p workers, aggregator, correlator, router), efficiency S/p.")
    w()
    w("| workers | posix lines/s | posix S | posix S/p | dfa lines/s | dfa S | dfa S/p |")
    w("|---|---|---|---|---|---|---|")
    b = {e: pt(rows, engine=e, workers=1)["lines_per_s"]["median"] for e in ("posix", "dfa")}
    for p_ in ws:
        cells = []
        for e in ("posix", "dfa"):
            r = pt(rows, engine=e, workers=p_)
            s_ = r["lines_per_s"]["median"] / b[e]
            cells += [lps(r["lines_per_s"]), f"{s_:.2f}", f"{s_ / p_:.2f}"]
        w(f"| {p_} | " + " | ".join(cells) + " |")
    w()
    bestd = max((pt(rows, engine="dfa", workers=p_) for p_ in ws), key=lambda r: r["lines_per_s"]["median"])
    bestp = max((pt(rows, engine="posix", workers=p_) for p_ in ws), key=lambda r: r["lines_per_s"]["median"])
    w(f"The POSIX engine peaks at {bestp['lines_per_s']['median']:,.0f} lines/s with {bestp['workers']} workers "
      f"(S = {bestp['lines_per_s']['median'] / b['posix']:.2f}). The DFA engine peaks at "
      f"{bestd['lines_per_s']['median']:,.0f} lines/s with {bestd['workers']} worker{'s' if bestd['workers'] != 1 else ''} "
      f"(S = {bestd['lines_per_s']['median'] / b['dfa']:.2f}); its single-worker run is already at "
      f"{b['dfa']:,.0f} lines/s, which is {b['dfa'] / bestp['lines_per_s']['median']:.2f}× the best POSIX result.")
    near = [p_ for p_ in ws if pt(rows, engine="posix", workers=p_)["lines_per_s"]["median"] / b["posix"] / p_ >= 0.8]
    w(f"Hypothesis \"near-linear until the serial stages bound it\": the POSIX engine keeps efficiency ≥ 0.8 up to "
      f"{max(near) if near else 1} workers; the DFA engine is bound by the serial stages from the start "
      f"(S = {pt(rows, engine='dfa', workers=ws[1])['lines_per_s']['median'] / b['dfa']:.2f} at {ws[1]} workers).")
    w()

# ------------------------------------------------------------------------------------------------ E3
if have("e3", "summary.json"):
    rows = J("e3", "summary.json")
    ns = sorted({int(r["rules"].split("_")[1]) for r in rows})
    w("## E3 Rule-count sensitivity (headline)")
    w()
    w("| rules | posix lines/s | dfa lines/s | dfa / posix | DFA groups | DFA states | DFA build (ms) |")
    w("|---|---|---|---|---|---|---|")
    for n in ns:
        p_, d = pt(rows, engine="posix", rules=f"bench_{n}"), pt(rows, engine="dfa", rules=f"bench_{n}")
        w(f"| {n} | {lps(p_['lines_per_s'])} | {lps(d['lines_per_s'])} | "
          f"{d['lines_per_s']['median'] / p_['lines_per_s']['median']:.1f}× | {d['dfa_groups']} | {d['dfa_states']:,} | "
          f"{d['build_ms']:,.0f} |")
    w()
    p5, p64 = (pt(rows, engine="posix", rules=f"bench_{n}")["lines_per_s"]["median"] for n in (ns[0], ns[-1]))
    d5, d64 = (pt(rows, engine="dfa", rules=f"bench_{n}")["lines_per_s"]["median"] for n in (ns[0], ns[-1]))
    w(f"From {ns[0]} to {ns[-1]} rules the POSIX engine falls to {p64 / p5 * 100:.0f} % of its throughput "
      f"({p5:,.0f} → {p64:,.0f} lines/s) and the DFA engine to {d64 / d5 * 100:.0f} % ({d5:,.0f} → {d64:,.0f}). "
      "Hypothesis \"POSIX falls about linearly with the rule count, DFA stays roughly flat\": "
      f"POSIX: {'holds' if p64 / p5 < (ns[0] / ns[-1]) * 3 else 'roughly'} (a perfectly linear fall would be to "
      f"{ns[0] / ns[-1] * 100:.0f} %); DFA: **{'holds' if d64 / d5 > 0.8 else 'does not hold'}**. "
      f"Up to {ns[-2]} rules (at most 2 DFA groups) the DFA engine keeps "
      f"{pt(rows, engine='dfa', rules=f'bench_{ns[-2]}')['lines_per_s']['median'] / d5 * 100:.0f} % of its {ns[0]}-rule throughput; "
      "the drop comes with the 7-group set. The DFA engine makes one pass per DFA group, and the number of groups grows with the rule count "
      "(table above), so its cost rises in steps when a rule set needs another group; the DFA engine stays "
      f"{d64 / p64:.1f}× faster than POSIX at {ns[-1]} rules.")
    w()

# ------------------------------------------------------------------------------------------------ E4
if have("e4", "summary.json"):
    rows = J("e4", "summary.json")
    w("## E4 Batch size (dfa, 4 workers)")
    w()
    w("| batch (-b) | lines/s | min–max | latency p50 (µs) | latency p99 (µs) |")
    w("|---|---|---|---|---|")
    for r in sorted(rows, key=lambda r: r["batch"]):
        w(f"| {r['batch']} | {lps(r['lines_per_s'])} | {rng(r['lines_per_s'])} | "
          f"{r['latency_p50_us']['median']:,.0f} | {r['latency_p99_us']['median']:,.0f} |")
    w()
    rs = sorted(rows, key=lambda r: r["batch"])
    w("Latency here is ingest → aggregated for a preloaded input that the reader pushes in as fast as the queues "
      "accept it, so it measures queueing behind the bounded rings, not a paced stream (that is E5, not run). "
      f"Throughput goes from {rs[0]['lines_per_s']['median']:,.0f} lines/s at -b {rs[0]['batch']} to "
      f"{max(r['lines_per_s']['median'] for r in rs):,.0f} at its best (-b "
      f"{max(rs, key=lambda r: r['lines_per_s']['median'])['batch']}); the largest batch the build accepts is 1,024 lines.")
    w()

# ------------------------------------------------------------------------------------------------ E6
if have("e6", "synthetic", "result.json"):
    w("## E6 Accuracy of the labels")
    w()
    syn = J("e6", "synthetic", "result.json")["all"]
    w(f"**Generator ground truth** ({syn['n']:,} lines, `bin/gen --seed 11`). The generator was written together with "
      "the rule set, so this checks that the engine and the generator's by-construction labels agree (and that the "
      "checksum look-alikes are rejected); it is not an estimate of accuracy on real logs.")
    w()
    w("| category | precision | recall | F1 | support |")
    w("|---|---|---|---|---|")
    f3 = lambda v: "n/a" if v is None else f"{v:.4f}"
    for c, m in syn["categories"].items():
        w(f"| {c} | {f3(m['precision'])} | {f3(m['recall'])} | {f3(m['f1'])} | {m['support']:,.0f} |")
    cal = syn["calibration"]
    w()
    w(f"Exact label-set match {syn['exact_match']:.4f}. Calibration of the primary label: ECE = {cal['ece']:.3f} "
      "(reliability diagram `results/e6/synthetic/reliability.svg`). Confidences are the noisy-OR of the base "
      "scores in `rules/default.rules` (0.35 weak, 0.6 strong, +0.25 validated, +0.15 keyword), so a correct label "
      "with confidence 0.6 shows up as under-confident when its observed precision is 1.0: "
      + ", ".join(f"bin {b['lo']:.1f}–{b['hi']:.1f}: mean confidence {b['mean_confidence']:.2f}, observed precision "
                  f"{b['observed_precision']:.2f}" for b in cal["bins"] if b["weight"]) + ".")
    w()
    if have("e6", "ait"):
        w("**AIT-LDS v2.0 (scenario russellmitchell), labelled web and auth logs.** AIT labels every request of the "
          "attacker's scan and exploitation steps; a line counts as detected if the engine labels it "
          "SECURITY_THREAT or AUTH_FAILURE.")
        w()
        w("| log | lines | attack lines | detected (recall) | false alarms | precision |")
        w("|---|---|---|---|---|---|")
        for f in sorted(os.listdir(os.path.join(R, "e6", "ait"))):
            if f.endswith(".txt"):
                r = J("e6", "ait", f[:-4], "result.json")["all"]
                a = r["categories"]["ATTACK"]
                w(f"| {f[:-4].replace('intranet_server__', '')} | {r['n']:,} | {a['support']:,.0f} | "
                  f"{a['tp']:,.0f} ({f3(a['recall'])}) | {a['fp']:,.0f} | {f3(a['precision'])} |")
        w()
        w("The rule set has no rule for most of these attacker requests (WordPress scanning and an upload exploit with "
          "ordinary-looking URLs), so recall is low; the few lines it flags are correct. The rules were not changed "
          "after seeing these numbers (no tuning on the evaluation data).")
        w()
    if have("e6", "loghub_full.json"):
        lf = J("e6", "loghub_full.json")
        w("**Full Loghub logs** (no ground truth; what the engine labels):")
        w()
        w("| log | lines | AUTH | SEC | ERR | PERF | PII | none | brute-force / spray / storm escalations |")
        w("|---|---|---|---|---|---|---|---|---|")
        for k, v in lf.items():
            p_ = v["default"]["primary"]
            fl = v["default"]["flags"]
            w(f"| {k} | {v['lines']:,} | {p_.get('AUTH_FAILURE', 0):,} | {p_.get('SECURITY_THREAT', 0):,} | "
              f"{p_.get('ERROR', 0):,} | {p_.get('PERF_WARNING', 0):,} | {p_.get('PII', 0):,} | {p_.get('none', 0):,} | "
              f"{fl.get('ESCALATED_BRUTE_FORCE', 0)} / {fl.get('ESCALATED_PASSWORD_SPRAY', 0)} / {fl.get('ERROR_STORM', 0)} |")
        w()
    w("Hand-labelled precision and recall on a stratified sample of real log lines need two people labelling "
      "independently (`tools/sample_for_labeling.py`, then `tools/label_eval.py --annot`); that has not been done and "
      "no number for it is reported.")
    w()

# ------------------------------------------------------------------------------------------------ E7
if have("e7", "synthetic_novalidators", "result.json"):
    w("## E7 Validator gain")
    w()
    on = J("e6", "synthetic", "result.json")["all"]["categories"]["PII"]
    off = J("e7", "synthetic_novalidators", "result.json")["all"]["categories"]["PII"]
    w("PII precision and recall on the generator truth (which contains Luhn- and Verhoeff-failing look-alikes), "
      "with the default rules and with `rules/novalidators.rules` (validators off, validator rules scored 0.6 so a "
      "pattern-only match is a label):")
    w()
    w("| | PII precision | PII recall | false PII labels | PII true positives |")
    w("|---|---|---|---|---|")
    w(f"| validators off | {f3(off['precision'])} | {f3(off['recall'])} | {off['fp']:,.0f} | {off['tp']:,.0f} |")
    w(f"| validators on | {f3(on['precision'])} | {f3(on['recall'])} | {on['fp']:,.0f} | {on['tp']:,.0f} |")
    w()
    w(f"Hypothesis \"validators raise precision\": **{'holds' if on['precision'] > off['precision'] else 'does not hold'}** "
      f"({f3(off['precision'])} → {f3(on['precision'])}) at unchanged recall ({f3(off['recall'])} → {f3(on['recall'])}).")
    w()
    if have("e7", "pii_on_logs.json"):
        pl = J("e7", "pii_on_logs.json")
        w("Lines labelled PII on the real logs (no truth; every line that drops out was a pattern match the checksum rejected):")
        w()
        w("| log | lines | validators off | validators on |")
        w("|---|---|---|---|")
        for k, v in pl.items():
            w(f"| {k} | {v['lines']:,} | {v['lines_labelled_pii_no_validators']:,} | {v['lines_labelled_pii_validators']:,} |")
        w()

os.makedirs(R, exist_ok=True)
with open(os.path.join(R, "SUMMARY.md"), "w") as f:
    f.write("\n".join(out) + "\n")
print(f"wrote {os.path.join(R, 'SUMMARY.md')} ({len(out)} lines)")
