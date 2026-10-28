# AI-annotator check on 100 real log lines (not human ground truth)

- Sample: `tools/sample_for_labeling.py --n 100 --seed 2` over the full Loghub OpenSSH, Apache and Linux logs
  (equal quota per stratum: AUTH 20, SEC 20, ERROR 20, PII 20, none 20; PERF has no lines).
- Labels: `annotator_AI_claude.csv`, written by Claude (the model that also wrote the rules and the classifier) from
  the log lines and the criteria of `docs/llm_labelling_instructions.md` only; the engine's answers (`key.csv`) were
  not looked at while labelling. Judgement calls: lines that state repetition ("message repeated 5 times", "Too many
  authentication failures", "5 more authentication failures") are SECURITY_THREAT too; sshd `error:` lines are ERROR;
  probes for web applications in Apache's "File does not exist" lines are SECURITY_THREAT; anonymous-FTP and kernel
  banner e-mail addresses are PII; masked all-zero memory addresses are not.
- `result.json`, `eval.txt`, `reliability.svg`: output of `tools/label_eval.py --annot annotator_AI_claude.csv --key key.csv`
  (strata weighted back to the full logs).
- It is replaced or complemented by human labels (two annotators) and by a second LLM when those are done.
