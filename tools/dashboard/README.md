# Triage Desk

A live dashboard for the triage engine. Standard library only (Python server, plain HTML/CSS/JS, hand-drawn SVG and
canvas charts, no downloads), so it works offline.

    make dashboard                 # then open http://127.0.0.1:8765/
    python3 tools/dashboard/server.py --out out/live --port 8765

**Live** follows `store.jsonl`, `alerts.jsonl` and `pii_quarantine.jsonl` in `out/live` four times a second and pushes
an update every second: a strip chart per category (a tick per second, height = lines, darkness = worst severity),
the classified lines, alerts and escalations (brute force, spray, fail-then-success, error storm), severity, rate,
category mix, busiest rules and the confidence histogram.

Two ways to feed it:
- press **Start**: the server runs a paced source through `bin/triage` (the generator, or a sample log replayed at the
  chosen rate); the OpenSSH sample shows the correlator's escalations;
- or run the engine yourself into the same folder: `bin/gen -n 100000 --rate 500 | bin/triage -o out/live -`.

The engine flushes its output files after every batch when its input is a stream (stdin), which is what makes the page
live; a run from a file is written in large blocks and shows up in bursts.

**Experiments** and **Accuracy** draw the saved runs under `results/` (E1–E4, the base-paper runs, E6, E7, the AIT-LDS
and AI-annotator checks); nothing there is computed by the page. Links: `/?tab=exp`, `/?tab=acc`.
