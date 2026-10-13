#ifndef AGGREGATE_H
#define AGGREGATE_H

/* Classification of one matched line (spec §5.1-5.3).
 *
 *   §5.1  per-rule evidence score: a rule whose validator failed on every
 *         candidate span is discarded; otherwise s = base, +0.25 if it has a
 *         validator (which passed), +0.15 if one of its context keywords is on
 *         the line, clamped to [0, 1];
 *   §5.2  per-category confidence by noisy-OR over the surviving rules of the
 *         category: conf(c) = 1 - prod(1 - s_i); a correlator escalation sets
 *         the category to (at least) 0.95;
 *   §5.3  labels = categories with conf >= tau; severity(c) = max severity of
 *         its surviving rules; primary = label with the highest severity, ties
 *         by higher confidence, then SEC > PII > AUTH > ERR > PERF; line
 *         severity = primary's, INFO without labels.
 * Rules marked no_label ("AUTH*", SSH_ACCEPTED) survive into the explanation
 * mask but never contribute to a category: they only feed the correlator.
 *
 * Done in two steps so the correlator (T5.1) can escalate in between:
 * aggregator_evidence() -> [aggregator_escalate()] -> aggregator_decide().
 * aggregator_classify() runs both steps without escalation. */

#include <stdbool.h>

#include "rules.h"
#include "types.h"

#define N_CATEGORIES 5

/* Category <-> index (0 = AUTH, 1 = SEC, 2 = ERR, 3 = PERF, 4 = PII). */
int      cat_index(Category c);
Category cat_from_index(int idx);

/* Per-rule facts, extracted once from the rule set; read-only afterwards, so
 * one Aggregator can be shared by all workers. */
typedef struct {
    int     n_rules;
    float   tau;                      /* --tau */
    uint8_t cat_idx[RULES_MAX];
    uint8_t sev[RULES_MAX];
    uint8_t no_label[RULES_MAX];
    uint8_t has_validator[RULES_MAX];
    float   base[RULES_MAX];
} Aggregator;

/* Evidence for one line after §5.1/§5.2. */
typedef struct {
    float    conf[N_CATEGORIES];      /* noisy-OR confidence; 0 when absent */
    int8_t   sev[N_CATEGORIES];       /* max severity of surviving rules; -1 = absent */
    uint64_t kept;                    /* matched rules that survived validation (incl. no_label) */
} Evidence;

/* Confidence comparisons allow this much slack: base values are stored as
 * float, and 0.35f + 0.15 is a hair below the documented 0.5. */
#define CONF_EPS 1e-6f

void aggregator_init(Aggregator *a, const RuleSet *rs, float tau);

/* §5.1 score of rule `r` (validator assumed passed when it has one). */
float aggregator_rule_score(const Aggregator *a, int r, bool ctx);

/* §5.1 + §5.2 from the post-match facts of a line (rule_mask, valid_mask,
 * ctx_mask of `m`; bits at or above n_rules are ignored). */
void aggregator_evidence(const Aggregator *a, const MatchResult *m, Evidence *ev);

/* Correlator hook (§5.2, §6): category `c` gets confidence max(conf, 0.95)
 * and at least severity `sev`. */
#define ESCALATION_CONF 0.95f
void aggregator_escalate(Evidence *ev, Category c, Severity sev);

/* §5.3. `explain` becomes Classified.rule_mask (the rules listed as the
 * explanation); `flags` are the correlator's FLAG_* bits. */
void aggregator_decide(const Aggregator *a, const Evidence *ev, uint64_t seq, uint64_t explain,
                       uint32_t flags, Classified *out);

/* Evidence + decide, no escalation. The explanation is the surviving rules
 * minus `subsumed` (rules whose span lies inside the span of another rule of
 * the same category, see extract_subsumed(); reporting only, §5.3). */
void aggregator_classify(const Aggregator *a, const MatchResult *m, uint64_t subsumed, Classified *out);

#endif /* AGGREGATE_H */
