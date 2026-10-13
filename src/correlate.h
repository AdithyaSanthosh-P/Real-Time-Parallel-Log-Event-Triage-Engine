#ifndef CORRELATE_H
#define CORRELATE_H

/* Correlator (spec §6, plan T5.1): sliding windows over log time that turn
 * series of lines into escalations no single line shows.
 *
 *   BRUTE_FORCE        >= maxretry AUTH failures from one source IP within
 *                      findtime -> SECURITY_THREAT, HIGH, conf 0.95, once per
 *                      findtime window per IP (on the line that crosses it)
 *   PASSWORD_SPRAY     AUTH failures from one IP against >= spray_users
 *                      distinct users within spray_window -> SECURITY_THREAT,
 *                      HIGH, once per spray_window per IP
 *   FAIL_THEN_SUCCESS  a successful login (a matched no-label AUTH rule,
 *                      "AUTH*" in the rule file: SSH_ACCEPTED) from an IP that
 *                      is over the BRUTE_FORCE threshold right now ->
 *                      SECURITY_THREAT, CRITICAL, every time
 *   ERROR_STORM        >= storm_lines ERROR lines (any source) within
 *                      storm_window -> ERROR raised to HIGH on the line that
 *                      starts the storm; no further storm alert until a later
 *                      ERROR line finds fewer than storm_lines in its window
 *                      (one alert per storm)
 *
 * An AUTH failure is a line labelled AUTH_FAILURE (after tau) with an
 * extracted source IP. Time is the line's timestamp (Batch.ts: parsed log
 * time, else ingest time); "within W" means W seconds back from the current
 * line, inclusive. Lines must be fed in input (seq) order.
 *
 * State: an open-addressing hash map keyed by source IP; each entry keeps a
 * ring of its last 64 failures (time + hash of the user name), which serves
 * both the failure count and the distinct-user count. One thread only. */

#include <stdint.h>

#include "aggregate.h"
#include "rules.h"
#include "types.h"

#define CORR_RING 64           /* failures remembered per IP (spec §6) */

typedef struct {
    int     maxretry;          /* fail2ban name; default 5 */
    int64_t findtime;          /* seconds; default 60 */
    int     spray_users;       /* K; default 5 */
    int64_t spray_window;      /* T; default 300 s */
    int     storm_lines;       /* N; default 100 (not given by the spec) */
    int64_t storm_window;      /* T; default 60 s (not given by the spec) */
} CorrelatorConfig;

void correlator_default_config(CorrelatorConfig *cfg);

typedef struct Correlator Correlator;

/* NULL on out of memory or an invalid config (maxretry or spray_users
 * outside 1..CORR_RING, storm_lines < 1, a negative window). */
Correlator *correlator_new(const CorrelatorConfig *cfg, const RuleSet *rs);
void        correlator_free(Correlator *k);

/* Feed one line: its time, match result, and labels as classified without
 * escalation. Updates the windows, applies any escalation to `ev`
 * (aggregator_escalate) and returns the FLAG_* bits raised (0 = none; the
 * caller then re-runs aggregator_decide with those flags). */
uint32_t correlator_observe(Correlator *k, int64_t ts, const MatchResult *m,
                            uint32_t label_mask, Evidence *ev);

/* Escalations raised so far, per flag bit index (0..3). */
uint64_t correlator_count(const Correlator *k, int flag_bit);

#endif /* CORRELATE_H */
