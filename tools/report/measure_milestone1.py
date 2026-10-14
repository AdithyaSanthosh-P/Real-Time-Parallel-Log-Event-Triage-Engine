#!/usr/bin/env python3
"""Measurements for the Review 1 report: our own engine as it was at commit 8093e0a.

Builds that commit from git into a temporary directory (nothing in the working tree is
touched) and records, under results/milestone1/:
  state_sweep.json   DFA groups / states / build time for several --max-states caps
  throughput.json    single-thread end-to-end lines/s, dfa vs posix engine, 5 runs each
  corpus.json        per corpus file: lines, lines matching a rule, labels per category
  tests.json         check counts of every unit-test program, difftest result, annotated lines
  machine.txt        commit, CPU, kernel, compiler, flags

usage: tools/report/measure_milestone1.py [COMMIT]     (default 8093e0a)
"""
import glob
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COMMIT = sys.argv[1] if len(sys.argv) > 1 else "8093e0a"
OUT = os.path.join(ROOT, "results", "milestone1")
RUNS = 5
REPS = 100            # corpus repetitions for the throughput input


def sh(cmd, cwd, **kw):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True, **kw)


def main():
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="m1_")
    shm = tempfile.mkdtemp(prefix="m1bench_", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    try:
        src = os.path.join(tmp, "src")
        os.makedirs(src)
        arch = subprocess.run(["git", "archive", COMMIT], cwd=ROOT, check=True, capture_output=True).stdout
        subprocess.run(["tar", "-x", "-C", src], input=arch, check=True)
        sh(["make", "-s"], src)
        triage = os.path.join(src, "bin", "triage")

        # ---- machine ----
        lscpu = sh(["lscpu"], src).stdout
        model = re.search(r"Model name:\s*(.+)", lscpu).group(1)
        flags = sh(["make", "print-flags"], src).stdout.strip() if os.path.exists(os.path.join(src, "Makefile")) else ""
        with open(os.path.join(OUT, "machine.txt"), "w") as f:
            f.write(f"commit: {COMMIT}\ncpu: {model}\nkernel: {platform.platform()}\n"
                    f"gcc: {sh(['gcc', '--version'], src).stdout.splitlines()[0]}\nflags: {flags}\n"
                    f"runs per point: {RUNS}\nthroughput input: corpus x{REPS} (tests/corpus/*.log)\n")

        # ---- DFA state cap sweep ----
        sweep = []
        for cap in (500, 1000, 2000, 5000, 20000, 100000, 1000000):
            r = subprocess.run([triage, "--max-states", str(cap), "-o", os.path.join(shm, "o"), "/dev/null"],
                               cwd=src, capture_output=True, text=True, check=True)
            m = re.search(r"NFA (\d+) states, (\d+) DFA groups?, (\d+) DFA states, built in ([\d.]+) ms", r.stderr)
            sweep.append({"max_states": cap, "nfa_states": int(m[1]), "groups": int(m[2]),
                          "dfa_states": int(m[3]), "build_ms": float(m[4])})
        json.dump(sweep, open(os.path.join(OUT, "state_sweep.json"), "w"), indent=1)

        # ---- throughput: dfa vs posix, end to end, one worker ----
        files = sorted(glob.glob(os.path.join(src, "tests/corpus/*.log")))
        data = b"".join(open(f, "rb").read() for f in files)
        inp = os.path.join(shm, "in.log")
        with open(inp, "wb") as f:
            for _ in range(REPS):
                f.write(data)
        empty = os.path.join(shm, "empty.log")
        open(empty, "w").close()
        lines = sum(1 for _ in open(inp, "rb"))

        def run(engine, path):
            t = time.perf_counter()
            subprocess.run([triage, "--quiet", "--engine", engine, "-w", "1", "-o", os.path.join(shm, "out"), path],
                           cwd=src, check=True)
            return time.perf_counter() - t

        res = {"dfa": [], "posix": [], "startup_dfa": [], "startup_posix": []}
        for _ in range(RUNS):                       # interleaved so drift hits both alike
            for e in ("dfa", "posix"):
                res[e].append(run(e, inp))
                res["startup_" + e].append(run(e, empty))
        tp = {"lines": lines, "bytes": os.path.getsize(inp), "runs": RUNS}
        for e in ("dfa", "posix"):
            tot, st = statistics.median(res[e]), statistics.median(res["startup_" + e])
            tp[e] = {"total_s": res[e], "startup_s": res["startup_" + e], "median_total_s": tot,
                     "median_startup_s": st, "lines_per_s_median": lines / (tot - st),
                     "lines_per_s_min": lines / (max(res[e]) - st), "lines_per_s_max": lines / (min(res[e]) - st)}
        tp["speedup_dfa_over_posix"] = tp["dfa"]["lines_per_s_median"] / tp["posix"]["lines_per_s_median"]
        json.dump(tp, open(os.path.join(OUT, "throughput.json"), "w"), indent=1)

        # ---- per-corpus classification output ----
        corpus = []
        for f in files:
            outd = os.path.join(shm, "c")
            shutil.rmtree(outd, ignore_errors=True)
            sh([triage, "--quiet", "--engine", "dfa", "-w", "1", "-o", outd, f], src)
            n = matched = 0
            prim = {}
            for line in open(os.path.join(outd, "store.jsonl")):
                r = json.loads(line)
                n += 1
                matched += bool(r["rules"])
                if r["primary"]:
                    prim[r["primary"]] = prim.get(r["primary"], 0) + 1
            corpus.append({"file": os.path.basename(f), "lines": n, "lines_with_rule_match": matched,
                           "primary_label_counts": prim})
        json.dump(corpus, open(os.path.join(OUT, "corpus.json"), "w"), indent=1)

        # ---- tests ----
        t = subprocess.run(["make", "test"], cwd=src, capture_output=True, text=True)
        checks = {m[1]: int(m[2]) for m in re.finditer(r"tests/(test_\w+)\.c: (\d+) checks, 0 failed", t.stderr + t.stdout)}
        dt = subprocess.run(["make", "difftest"], cwd=src, capture_output=True, text=True)
        m = re.search(r"difftest: (\d+) file lines \((\d+) files\) \+ (\d+) random strings checked, (\d+) mismatches",
                      dt.stdout + dt.stderr)
        annotated = sum(1 for l in open(os.path.join(src, "tests/corpus/synthetic_small.log"))
                        if l.startswith("#@ rules:"))
        json.dump({"unit_test_checks": checks, "unit_test_programs": len(checks), "all_passed": t.returncode == 0,
                   "difftest": {"file_lines": int(m[1]), "files": int(m[2]), "random_strings": int(m[3]),
                                "mismatches": int(m[4])},
                   "annotated_lines_synthetic_small": annotated},
                  open(os.path.join(OUT, "tests.json"), "w"), indent=1)
        print("wrote", OUT)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(shm, ignore_errors=True)


if __name__ == "__main__":
    main()
