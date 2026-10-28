#!/usr/bin/env python3
"""T7.2/T7.3: sample_for_labeling.py and label_eval.py on tiny inputs with hand-computed answers."""
import csv
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print(f"test_label_eval FAIL: {what}")


def run(*a):
    return subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", a[0])] + list(a[1:]),
                          capture_output=True, text=True)


def store(path, rows):
    with open(path, "w") as f:
        for seq, labels, primary, conf in rows:
            f.write(json.dumps({"seq": seq, "labels": labels, "primary": primary, "confidence": conf,
                                "line": f"line {seq}", "rules": [], "flags": []}) + "\n")


with tempfile.TemporaryDirectory() as d:
    # ---- label_eval against a truth file: 6 lines ----
    #  seq truth        predicted      primary conf
    #   0  AUTH         AUTH           AUTH    0.6   tp(AUTH)
    #   1  AUTH         -              -       0     fn(AUTH)
    #   2  ERROR        ERROR,PII      ERROR   0.9   tp(ERROR), fp(PII)
    #   3  -            AUTH           AUTH    0.6   fp(AUTH)
    #   4  PII          PII            PII     0.8   tp(PII)
    #   5  -            -              -       0
    store(f"{d}/p.jsonl", [(0, ["AUTH_FAILURE"], "AUTH_FAILURE", 0.6), (1, [], None, 0.0),
                           (2, ["ERROR", "PII"], "ERROR", 0.9), (3, ["AUTH_FAILURE"], "AUTH_FAILURE", 0.6),
                           (4, ["PII"], "PII", 0.8), (5, [], None, 0.0)])
    with open(f"{d}/t.tsv", "w") as f:
        for seq, lab in enumerate(["AUTH_FAILURE", "AUTH_FAILURE", "ERROR", "-", "PII", "-"]):
            f.write(f"{seq}\t{lab}\tk\n")
    r = run("label_eval.py", "--truth", f"{d}/t.tsv", "--pred", f"{d}/p.jsonl", "--out", f"{d}/o")
    check(r.returncode == 0, "label_eval runs: " + r.stderr)
    res = json.load(open(f"{d}/o/result.json"))["all"]
    c = res["categories"]
    check((c["AUTH_FAILURE"]["tp"], c["AUTH_FAILURE"]["fp"], c["AUTH_FAILURE"]["fn"]) == (1, 1, 1), "AUTH counts")
    check(abs(c["AUTH_FAILURE"]["precision"] - 0.5) < 1e-9 and abs(c["AUTH_FAILURE"]["recall"] - 0.5) < 1e-9, "AUTH P/R")
    check((c["ERROR"]["tp"], c["ERROR"]["fp"], c["ERROR"]["fn"]) == (1, 0, 0) and c["ERROR"]["f1"] == 1.0, "ERROR")
    check((c["PII"]["tp"], c["PII"]["fp"], c["PII"]["fn"]) == (1, 1, 0) and abs(c["PII"]["precision"] - 0.5) < 1e-9, "PII")
    check(c["SECURITY_THREAT"]["precision"] is None and c["SECURITY_THREAT"]["support"] == 0, "empty category -> n/a")
    check(abs(res["exact_match"] - 3 / 6) < 1e-9, "exact match = 3 of 6 (lines 0, 4, 5)")
    cm = res["confusion"]["matrix"]
    check(cm["AUTH_FAILURE"]["AUTH_FAILURE"] == 1 and cm["AUTH_FAILURE"]["none"] == 1 and cm["none"]["AUTH_FAILURE"] == 1
          and cm["ERROR"]["ERROR"] == 1 and cm["PII"]["PII"] == 1 and cm["none"]["none"] == 1, "confusion matrix")
    # calibration: bin 6 (0.6): lines 0 and 3: mean conf 0.6, precision 1/2; bin 9 (0.9): line 2: precision 1;
    # bin 8 (0.8): line 4: precision 1.  ECE = (2*|0.5-0.6| + 1*|1-0.9| + 1*|1-0.8|) / 4 = 0.125
    check(abs(res["calibration"]["ece"] - 0.125) < 1e-9, f"ECE {res['calibration']['ece']}")
    check(os.path.getsize(f"{d}/o/reliability.svg") > 500, "reliability svg written")

    # attack map: truth ATTACK vs predicted SEC or AUTH
    with open(f"{d}/t2.tsv", "w") as f:
        for seq, lab in enumerate(["ATTACK", "-", "ATTACK", "-", "-", "-"]):
            f.write(f"{seq}\t{lab}\tk\n")
    r = run("label_eval.py", "--truth", f"{d}/t2.tsv", "--pred", f"{d}/p.jsonl", "--attack-map",
            "ATTACK=SECURITY_THREAT+AUTH_FAILURE", "--out", f"{d}/o2")
    a = json.load(open(f"{d}/o2/result.json"))["all"]["categories"]["ATTACK"]
    check(r.returncode == 0 and (a["tp"], a["fp"], a["fn"]) == (1, 1, 1), "attack map: lines 0 tp, 3 fp, 2 fn")
    # mismatching line counts are refused
    with open(f"{d}/short.tsv", "w") as f:
        f.write("0\t-\tk\n")
    check(run("label_eval.py", "--truth", f"{d}/short.tsv", "--pred", f"{d}/p.jsonl").returncode != 0, "length mismatch refused")

    # ---- sampling: strata, quotas, determinism ----
    rows = [(i, ["AUTH_FAILURE"], "AUTH_FAILURE", 0.6) for i in range(500)] + \
           [(500 + i, ["PII"], "PII", 0.6) for i in range(10)] + [(510 + i, [], None, 0.0) for i in range(1000)]
    store(f"{d}/s.jsonl", rows)
    r = run("sample_for_labeling.py", "--store", f"x={d}/s.jsonl", "--n", "90", "--seed", "3", "--out", f"{d}/lab")
    check(r.returncode == 0, "sampling runs: " + r.stderr)
    key = list(csv.DictReader(open(f"{d}/lab/key.csv")))
    by = {}
    for k in key:
        by[k["stratum"]] = by.get(k["stratum"], 0) + 1
    # 3 non-empty strata, quota 30 each, but PII has only 10 -> its 20 spare go to the others (40 each)
    check(by == {"AUTH_FAILURE": 40, "PII": 10, "none": 40}, f"quotas {by}")
    check(all(k["stratum_size"] and k["stratum_sampled"] for k in key), "key has stratum sizes")
    a_rows = list(csv.DictReader(open(f"{d}/lab/annotator_A.csv")))
    check(len(a_rows) == 90 and set(a_rows[0]) >= {"id", "line", "AUTH_FAILURE", "PII"}, "annotator file columns")
    check("primary" not in a_rows[0] and "confidence" not in a_rows[0], "prediction hidden from annotators")
    check(open(f"{d}/lab/annotator_A.csv").read() == open(f"{d}/lab/annotator_B.csv").read(), "A and B are the same blank form")
    run("sample_for_labeling.py", "--store", f"x={d}/s.jsonl", "--n", "90", "--seed", "3", "--out", f"{d}/lab2")
    check(open(f"{d}/lab/key.csv").read() == open(f"{d}/lab2/key.csv").read(), "same seed, same sample")

    # ---- annotator evaluation: kappa and weighting ----
    ids = [int(k["id"]) for k in key]
    truth = {int(k["id"]): (k["primary"] if k["primary"] else None) for k in key}   # annotator A agrees with the engine
    def write(path, flip):
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["id", "source", "line", "AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"])
            for i in ids:
                v = [1 if truth[i] == c else 0 for c in ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"]]
                if i in flip:
                    v[0] = 1 - v[0]
                w.writerow([i, "x", f"line {i}"] + v)
    write(f"{d}/A.csv", set())
    write(f"{d}/B.csv", set(ids[:9]))                  # B differs from A on AUTH for 9 lines
    r = run("label_eval.py", "--annot", f"{d}/A.csv", f"{d}/B.csv", "--key", f"{d}/lab/key.csv", "--out", f"{d}/o3")
    check(r.returncode == 0, "annotator eval runs: " + r.stderr)
    res = json.load(open(f"{d}/o3/result.json"))
    check(res["vs_A"]["exact_match"] == 1.0 and res["vs_A"]["categories"]["AUTH_FAILURE"]["f1"] == 1.0, "engine equals annotator A")
    check(res["vs_B"]["categories"]["AUTH_FAILURE"]["f1"] < 1.0, "engine differs from annotator B")
    check(0 < res["kappa"]["AUTH_FAILURE"] < 1 and res["kappa"]["PII"] == 1.0, f"kappa {res['kappa']}")
    check(res["agreement"]["identical_label_sets"] == 90 - 9, "agreement count")
    # weighting: AUTH stratum is 500 lines, 40 sampled -> weight 12.5 per sampled line
    check(abs(res["vs_A"]["weighted_n"] - (40 * 500 / 40 + 10 * 10 / 10 + 40 * 1000 / 40)) < 1e-6, "weighted line count")
    bad = open(f"{d}/A.csv").read().replace(",1,0,0,0,0", ",x,0,0,0,0", 1)
    open(f"{d}/bad.csv", "w").write(bad)
    check(run("label_eval.py", "--annot", f"{d}/bad.csv", "--key", f"{d}/lab/key.csv").returncode != 0, "non 0/1 cell refused")

print(f"tests/test_label_eval.py: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
