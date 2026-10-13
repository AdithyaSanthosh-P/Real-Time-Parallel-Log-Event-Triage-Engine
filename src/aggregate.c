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

void aggregator_init(Aggregator *a, const RuleSet *rs, float tau)
{
    memset(a, 0, sizeof *a);
    a->n_rules = rs->count;
    a->tau = tau;
    for (int i = 0; i < rs->count; i++) {
        const Rule *r = &rs->rules[i];
        a->cat_idx[i] = (uint8_t)cat_index(r->category);
        a->sev[i] = (uint8_t)r->severity;
        a->no_label[i] = r->no_label;
        a->has_validator[i] = r->validator != VAL_NONE;
        a->base[i] = r->base;
    }
}

float aggregator_rule_score(const Aggregator *a, int r, bool ctx)
{
    float s = a->base[r];
    if (a->has_validator[r])
        s += 0.25f;                     /* pattern + checksum */
    if (ctx)
        s += 0.15f;                     /* + nearby keyword */
    return s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
}

void aggregator_evidence(const Aggregator *a, const MatchResult *m, Evidence *ev)
{
    /* noisy-OR is accumulated as the product of the complements */
    float miss[N_CATEGORIES];
    for (int c = 0; c < N_CATEGORIES; c++) {
        miss[c] = 1.0f;
        ev->sev[c] = -1;
    }
    ev->kept = 0;
    for (int r = 0; r < a->n_rules; r++) {
        uint64_t bit = (uint64_t)1 << r;
        if (!(m->rule_mask & bit))
            continue;
        if (!(m->valid_mask & bit))
            continue;                   /* validator failed on every span: discard (§5.1) */
        ev->kept |= bit;
        if (a->no_label[r])
            continue;
        int c = a->cat_idx[r];
        miss[c] *= 1.0f - aggregator_rule_score(a, r, (m->ctx_mask & bit) != 0);
        if (a->sev[r] > ev->sev[c])
            ev->sev[c] = (int8_t)a->sev[r];
    }
    for (int c = 0; c < N_CATEGORIES; c++)
        ev->conf[c] = ev->sev[c] < 0 ? 0.0f : 1.0f - miss[c];
}

void aggregator_escalate(Evidence *ev, Category cat, Severity sev)
{
    int c = cat_index(cat);
    if (ev->conf[c] < ESCALATION_CONF)
        ev->conf[c] = ESCALATION_CONF;
    if ((int)sev > ev->sev[c])
        ev->sev[c] = (int8_t)sev;
}

void aggregator_decide(const Aggregator *a, const Evidence *ev, uint64_t seq, uint64_t explain,
                       uint32_t flags, Classified *out)
{
    uint32_t labels = 0;
    int primary = -1;
    for (int c = 0; c < N_CATEGORIES; c++) {
        if (ev->sev[c] < 0 || ev->conf[c] + CONF_EPS < a->tau)
            continue;
        labels |= (uint32_t)cat_from_index(c);
        if (primary < 0 || ev->sev[c] > ev->sev[primary] ||
            (ev->sev[c] == ev->sev[primary] &&
             (ev->conf[c] > ev->conf[primary] + CONF_EPS ||
              (ev->conf[c] + CONF_EPS >= ev->conf[primary] && TIE_RANK[c] < TIE_RANK[primary]))))
            primary = c;
    }

    memset(out, 0, sizeof *out);
    out->seq = seq;
    out->rule_mask = explain;
    out->label_mask = labels;
    out->flags = flags;
    if (primary < 0) {
        out->primary = PRIMARY_NONE;
        out->severity = SEV_INFO;
        out->confidence = 0.0f;
    } else {
        out->primary = (uint8_t)primary;
        out->severity = (uint8_t)ev->sev[primary];
        out->confidence = ev->conf[primary];
    }
}

void aggregator_classify(const Aggregator *a, const MatchResult *m, uint64_t subsumed, Classified *out)
{
    Evidence ev;
    aggregator_evidence(a, m, &ev);
    aggregator_decide(a, &ev, m->seq, ev.kept & ~subsumed, 0, out);
}
