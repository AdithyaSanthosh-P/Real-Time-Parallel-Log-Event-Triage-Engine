#include "aggregate.h"

#include <string.h>

int cat_index(Category c)
{
    switch (c) {
    case CAT_AUTH: return 0;
    case CAT_SEC:  return 1;
    case CAT_ERR:  return 2;
    case CAT_PERF: return 3;
    case CAT_PII:  return 4;
    }
    return 0;
}

Category cat_from_index(int idx)
{
    static const Category tab[N_CATEGORIES] = { CAT_AUTH, CAT_SEC, CAT_ERR, CAT_PERF, CAT_PII };
    return tab[idx];
}

/* Tie-break order of §5.3, as the rank of each category index (lower wins):
 * SEC > PII > AUTH > ERR > PERF. */
static const uint8_t TIE_RANK[N_CATEGORIES] = {
    2,  /* AUTH */
    0,  /* SEC  */
    3,  /* ERR  */
    4,  /* PERF */
    1,  /* PII  */
};

void aggregator_init(Aggregator *a, const RuleSet *rs)
{
    memset(a, 0, sizeof *a);
    a->n_rules = rs->count;
    for (int i = 0; i < rs->count; i++) {
        a->cat_idx[i] = (uint8_t)cat_index(rs->rules[i].category);
        a->sev[i] = (uint8_t)rs->rules[i].severity;
        a->no_label[i] = rs->rules[i].no_label;
    }
}

void aggregator_classify(const Aggregator *a, uint64_t seq, uint64_t rule_mask, Classified *out)
{
    /* Highest severity per category; -1 = category not present. */
    int cat_sev[N_CATEGORIES];
    for (int c = 0; c < N_CATEGORIES; c++)
        cat_sev[c] = -1;

    uint32_t labels = 0;
    for (int i = 0; i < a->n_rules; i++) {
        if (!((rule_mask >> i) & 1) || a->no_label[i])
            continue;
        int c = a->cat_idx[i];
        labels |= (uint32_t)cat_from_index(c);
        if (a->sev[i] > cat_sev[c])
            cat_sev[c] = a->sev[i];
    }

    /* Primary: highest severity, ties by the fixed category order. */
    int primary = -1;
    for (int c = 0; c < N_CATEGORIES; c++) {
        if (cat_sev[c] < 0)
            continue;
        if (primary < 0 || cat_sev[c] > cat_sev[primary] ||
            (cat_sev[c] == cat_sev[primary] && TIE_RANK[c] < TIE_RANK[primary]))
            primary = c;
    }

    memset(out, 0, sizeof *out);
    out->seq = seq;
    out->rule_mask = rule_mask;
    out->label_mask = labels;
    if (primary < 0) {
        out->primary = PRIMARY_NONE;
        out->severity = SEV_INFO;
        out->confidence = 0.0f;
    } else {
        out->primary = (uint8_t)primary;
        out->severity = (uint8_t)cat_sev[primary];
        out->confidence = 1.0f;
    }
}
