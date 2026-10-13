#ifndef ROUTE_H
#define ROUTE_H

/* Router (spec §7): turns classified lines into JSONL records and writes them
 * to the sinks under the output directory:
 *   alerts.jsonl          lines with severity >= HIGH
 *   store.jsonl           every line
 *   pii_quarantine.jsonl  lines labelled PII
 * Lines whose PII values were masked (mask.h) carry the masked text in every
 * sink they go to, alerts included.
 *
 * One record per line, e.g.
 *   {"seq":1042,"ts":"2026-10-11T10:14:22","primary":"SECURITY_THREAT",
 *    "severity":"CRITICAL","confidence":1.00,"labels":["SECURITY_THREAT","PII"],
 *    "rules":["SQLI","PII_EMAIL"],"flags":[],"line":"GET /item?id=1 ..."}
 * `primary` is null and `labels` empty for a line without labels. `labels`
 * lists the primary first, then the others in category order. `ts` is the
 * line's timestamp in UTC. `line` is the raw log line as a JSON string
 * (see json_put_string for how non-UTF-8 bytes are handled). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "batch.h"
#include "json.h"
#include "rules.h"
#include "types.h"

typedef struct Router Router;

/* Report names: "AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING",
 * "PII" for a category index (aggregate.h), and "INFO".."CRITICAL". */
const char *category_label(int cat_idx);
const char *severity_name(int severity);

/* Does this line go to the alert sink? (severity >= HIGH) */
bool route_is_alert(const Classified *c);

/* Create `out_dir` (and missing parents) and open the sinks, truncating
 * existing files. `rs` must outlive the router. Returns NULL and fills `err`
 * on failure. */
Router *router_open(const char *out_dir, const RuleSet *rs, char *err, size_t err_len);

/* Append the JSON record (no trailing newline) for line `i` of `b`. `line`
 * (`line_len` bytes) replaces the raw line as the "line" field when not NULL
 * (the masked text of mask.h). */
void route_format(const RuleSet *rs, const Batch *b, uint32_t i, const Classified *c,
                  const char *line, size_t line_len, StrBuf *out);

/* Write line `i` of `b` to the sinks it belongs to; `line`/`line_len` as in
 * route_format. Returns 0, or -1 on a write error (sticky: router_close()
 * reports it again). */
int router_emit(Router *r, const Batch *b, uint32_t i, const Classified *c,
                const char *line, size_t line_len);

uint64_t router_records(const Router *r);   /* lines written to store.jsonl */
uint64_t router_alerts(const Router *r);    /* lines written to alerts.jsonl */
uint64_t router_quarantined(const Router *r); /* lines written to pii_quarantine.jsonl */

/* Flush and close all sinks and free the router. Returns 0 if every write and
 * close succeeded. */
int router_close(Router *r);

#endif /* ROUTE_H */
