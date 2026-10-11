#ifndef AGGREGATE_H
#define AGGREGATE_H

#include "rules.h"
#include "types.h"

/* Aggregator v0 (spec §5.3, without the confidence model of §5.1/5.2):
 *   - label_mask  = categories of the matched rules, except rules marked
 *                   no_label ("AUTH*", e.g. SSH_ACCEPTED), which only feed the
 *                   correlator;
 *   - severity(c) = max severity over the matched rules of category c;
 *   - primary     = the label with the highest severity; ties go to higher
 *                   confidence (all equal in v0), then the fixed order
 *                   SEC > PII > AUTH > ERR > PERF;
 *   - line severity = primary's severity, SEV_INFO when there is no label;
 *   - confidence  = 1.0 for a labeled line, 0.0 otherwise (placeholder until
 *                   the evidence model arrives). */

#define N_CATEGORIES 5

/* Category <-> index (0 = AUTH, 1 = SEC, 2 = ERR, 3 = PERF, 4 = PII). */
int      cat_index(Category c);
Category cat_from_index(int idx);

/* Per-rule facts, extracted once from the rule set; read-only afterwards, so
 * one Aggregator can be shared. */
typedef struct {
    int     n_rules;
    uint8_t cat_idx[RULES_MAX];
    uint8_t sev[RULES_MAX];
    uint8_t no_label[RULES_MAX];
} Aggregator;

void aggregator_init(Aggregator *a, const RuleSet *rs);

/* Classify one line from its rule mask. Bits at or above n_rules are ignored. */
void aggregator_classify(const Aggregator *a, uint64_t seq, uint64_t rule_mask, Classified *out);

#endif /* AGGREGATE_H */
