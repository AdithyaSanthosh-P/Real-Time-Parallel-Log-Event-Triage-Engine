# Instructions for labelling log lines (for a human or a second LLM annotator)

Use this when a second, independent annotator (another LLM, or a person) labels the sample. Give the annotator
ONLY this file and the `line` column of the sheet. Do NOT give it `key.csv`, the engine's output, the rules in
`rules/*.rules`, or anyone else's labels. Record the annotator's identity (model name and version, or person),
the date, and the exact prompt, and report it as such in the results ("LLM annotator", never "human").

## Prompt to paste into the LLM

> You are labelling log lines for a research evaluation. For each row below, decide independently whether the
> line belongs to each of five categories. A line can belong to several categories or to none.
>
> - AUTH_FAILURE: the line shows a failed login or authentication (wrong password, invalid user, PAM
>   authentication failure, sudo failure, "Auth fail", too many authentication failures).
> - SECURITY_THREAT: the line shows an attack or probe (SQL, script or command injection, path traversal, a
>   scanner tool, probing for web applications, or repeated break-in attempts that the line itself states, such as
>   "message repeated 5 times: [Failed password ...]", "Too many authentication failures", "5 more authentication
>   failures").
> - ERROR: the line reports an error or failure (ERROR or FATAL level, a message the program itself prefixes with
>   "error:", an exception, a crash, an HTTP 5xx, out of memory, forbidden or missing resource logged at error level).
> - PERF_WARNING: slowness or resource pressure (slow query, very long duration, pool or queue exhausted, high CPU,
>   memory or disk use).
> - PII: personal or secret data is visible (card number, Aadhaar, PAN, e-mail address, phone number, password or
>   token in the text). Text such as `***@example.com` or `XXXX XXXX 1234` is a masked value: the original was
>   personal data, so label PII = 1; a hexadecimal or all-zero memory address is not personal data.
>
> Decide from what the line says, not from guessing context you cannot see. If unsure, choose 0.
> Output only a CSV with the header `id,AUTH_FAILURE,SECURITY_THREAT,ERROR,PERF_WARNING,PII` and one row per
> input line, with the id from the input and 0 or 1 in every category column. No other text.
>
> Lines:
> `id<TAB>line` rows, pasted from the sheet

## After the annotator answers

1. Put its 0/1 columns into a copy of `annotator_A.csv` (keep `id`, `source`, `line`), or let the checker build it:

       tools/check_labels_csv.py SHEET.csv ANSWER.csv --out LLM_labels.csv

   The checker refuses anything that is not exactly the sheet's ids with 0/1 cells.
2. Evaluate against the engine and against the other annotators:

       tools/label_eval.py --annot LLM_labels.csv --key key.csv                       # engine vs this annotator
       tools/label_eval.py --annot A.csv LLM_labels.csv --key key.csv                 # kappa between two annotators

3. Report each annotator separately, with the weighted strata explained (see `tools/sample_for_labeling.py`).
