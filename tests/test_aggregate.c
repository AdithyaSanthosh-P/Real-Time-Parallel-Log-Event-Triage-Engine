#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "aggregate.h"
#include "engine_posix.h"
#include "extract.h"
#include "normalize.h"
#include "rules.h"

static RuleSet rs;
static Aggregator agg;
static EnginePosix *posix;
static Extractor *ext;

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-5)

/* Mask of the named rules (a NULL-terminated list). */
static uint64_t mask_of(const char *first, ...)
{
    uint64_t m = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, first);
    for (const char *id = first; id; id = __builtin_va_arg(ap, const char *)) {
        int i = rules_find(&rs, id);
        if (i < 0) {
            fprintf(stderr, "test bug: no rule %s\n", id);
            exit(1);
        }
        m |= (uint64_t)1 << i;
    }
    __builtin_va_end(ap);
    return m;
}

#define MASK(...) mask_of(__VA_ARGS__, (const char *)NULL)

/* Classify from masks: matched, validator-passed, context-present. */
static Classified run3(uint64_t matched, uint64_t valid, uint64_t ctx)
{
    MatchResult m = { .seq = 7, .rule_mask = matched, .valid_mask = valid, .ctx_mask = ctx };
    Classified c;
    aggregator_classify(&agg, &m, 0, &c);
    return c;
}

static Classified run(uint64_t m) { return run3(m, m, 0); }

/* The full post-match path on real text, as the pipeline does it. */
static Classified classify_text(const char *text)
{
    char norm[4096];
    size_t n = normalize_line(text, strlen(text), norm);
    MatchResult m = { .seq = 1 };
    m.rule_mask = posix_match(posix, norm, n);
    uint64_t sub = 0;
    if (m.rule_mask) {
        extract_line(ext, norm, n, m.rule_mask, &m);
        m.ctx_mask = context_mask(&rs, norm, n, m.rule_mask);
        sub = extract_subsumed(ext, norm, n, m.valid_mask);
    }
    Classified c;
    aggregator_classify(&agg, &m, sub, &c);
    return c;
}

/* ---- the worked examples of the plan (T3.4 "done when") -------------------------- */

static void test_worked_examples(void)
{
    /* card line: PII_CARD kept, the overlapping PII_AADHAAR match discarded by Verhoeff */
    Classified c = classify_text("2026-10-11 10:51:00,100 INFO  PaymentService - payment captured "
                                 "card=4111 1111 1111 1111 amount=499.00");
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_PII));
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);
    CHECK_EQ_INT(c.rule_mask, MASK("PII_CARD"));
    CHECK(NEAR(c.confidence, 0.35 + 0.25 + 0.15));            /* base + checksum + keyword */

    /* SQLi + e-mail: SEC|PII, primary SEC */
    c = classify_text("203.0.113.51 - - [11/Oct/2026:10:08:10 +0530] \"GET /lookup?email=a@b.com'+OR+'1'='1 "
                      "HTTP/1.1\" 200 12 \"-\" \"Mozilla/5.0\"");
    CHECK_EQ_INT(c.label_mask, CAT_SEC | CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);
    CHECK_EQ_INT(c.rule_mask, MASK("SQLI", "PII_EMAIL"));

    /* Aadhaar-shaped numbers that fail Verhoeff: no PII label, nothing to explain */
    c = classify_text("2026-10-11 10:52:10,100 INFO  KycService - uid 234123412347 rejected");
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
    CHECK_EQ_INT(c.severity, SEV_INFO);
    CHECK_EQ_INT(c.rule_mask, 0);
    c = classify_text("2026-10-11 10:52:15,100 INFO  OrderService - order id 523456789012 shipped");
    CHECK_EQ_INT(c.label_mask, 0);

    /* and the valid one, with its keyword */
    c = classify_text("2026-10-11 10:52:00,100 INFO  KycService - uid 2341 2341 2346 verified");
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.severity, SEV_HIGH);
    CHECK(NEAR(c.confidence, 0.75));
}

/* ---- §5.1 scores, §5.2 noisy-OR, §5.3 tau ---------------------------------------- */

static void test_scores(void)
{
    int card = rules_find(&rs, "PII_CARD"), pan = rules_find(&rs, "PII_PAN");
    int sqli = rules_find(&rs, "SQLI"), http = rules_find(&rs, "HTTP_401_403");
    CHECK(NEAR(aggregator_rule_score(&agg, card, false), 0.60));
    CHECK(NEAR(aggregator_rule_score(&agg, card, true), 0.75));
    CHECK(NEAR(aggregator_rule_score(&agg, pan, true), 1.00));      /* 0.6 + 0.25 + 0.15 */
    CHECK(NEAR(aggregator_rule_score(&agg, sqli, false), 0.60));
    CHECK(NEAR(aggregator_rule_score(&agg, sqli, true), 0.75));
    CHECK(NEAR(aggregator_rule_score(&agg, http, false), 0.35));

    /* clamping to 1 needs a base above 0.6; use a one-rule set */
    static const char text[] = "HI\tPII\tHIGH\t0.9\t-\tpan\t-\tpan\tX\n";
    RuleSet one;
    char err[256];
    CHECK_EQ_INT(rules_parse(text, sizeof text - 1, "inline", &one, err, sizeof err), 0);
    Aggregator a1;
    aggregator_init(&a1, &one, 0.5f);
    CHECK(NEAR(aggregator_rule_score(&a1, 0, true), 1.0));
    rules_free(&one);
}

static void test_noisy_or_and_tau(void)
{
    /* a weak rule alone stays below tau = 0.5 ... */
    Classified c = run(MASK("HTTP_401_403"));
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
    CHECK_EQ_INT(c.rule_mask, MASK("HTTP_401_403"));        /* still explained */
    c = run(MASK("TIMEOUT"));
    CHECK_EQ_INT(c.label_mask, 0);

    /* ... two evidence sources combine: 1 - (1-0.6)(1-0.35) = 0.74 */
    c = run(MASK("SSH_FAIL_PW", "HTTP_401_403"));
    CHECK_EQ_INT(c.label_mask, CAT_AUTH);
    CHECK(NEAR(c.confidence, 0.74));
    c = run(MASK("ERR_LEVEL", "JAVA_EXCEPTION"));
    CHECK(NEAR(c.confidence, 0.84));
    c = run(MASK("ERR_LEVEL", "JAVA_EXCEPTION", "JAVA_STACK", "HTTP_5XX"));
    CHECK(NEAR(c.confidence, 1.0 - 0.4 * 0.4 * 0.4 * 0.4));
    CHECK_EQ_INT(c.severity, SEV_HIGH);

    /* exactly at tau: 0.35 + 0.15 = 0.5 (float base 0.35f is a hair low; CONF_EPS keeps it) */
    uint64_t ph = MASK("PII_PHONE_IN");
    c = run3(ph, ph, ph);
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK(NEAR(c.confidence, 0.5));
    c = run3(ph, ph, 0);
    CHECK_EQ_INT(c.label_mask, 0);

    /* tau moves the threshold */
    Aggregator a0 = agg, a9 = agg;
    a0.tau = 0.0f;
    a9.tau = 0.9f;
    MatchResult m = { .rule_mask = MASK("TIMEOUT", "SQLI"), .valid_mask = MASK("TIMEOUT", "SQLI") };
    Classified d;
    aggregator_classify(&a0, &m, 0, &d);
    CHECK_EQ_INT(d.label_mask, CAT_PERF | CAT_SEC);
    aggregator_classify(&a9, &m, 0, &d);
    CHECK_EQ_INT(d.label_mask, 0);
    m.ctx_mask = MASK("SQLI");
    aggregator_classify(&a9, &m, 0, &d);
    CHECK_EQ_INT(d.label_mask, 0);                          /* 0.75 < 0.9 */
}

static void test_discard(void)
{
    /* a matched rule whose validator failed is gone: no score, no severity, no explanation */
    uint64_t both = MASK("PII_CARD", "PII_EMAIL");
    Classified c = run3(both, MASK("PII_EMAIL"), MASK("PII_CARD"));
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.severity, SEV_LOW);                      /* not CRITICAL from the card */
    CHECK(NEAR(c.confidence, 0.6));
    CHECK_EQ_INT(c.rule_mask, MASK("PII_EMAIL"));
    c = run3(MASK("PII_CARD"), 0, MASK("PII_CARD"));
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.rule_mask, 0);
    /* valid bits of rules that did not match mean nothing */
    c = run3(0, ~(uint64_t)0, ~(uint64_t)0);
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.rule_mask, 0);
}

static void test_no_label_rule(void)
{
    Classified c = run(MASK("SSH_ACCEPTED"));
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
    CHECK_EQ_INT(c.severity, SEV_INFO);
    CHECK_EQ_INT(c.rule_mask, MASK("SSH_ACCEPTED"));       /* kept for the correlator */
    c = run(MASK("SSH_ACCEPTED", "PII_EMAIL"));
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK(NEAR(c.confidence, 0.6));                         /* SSH_ACCEPTED adds nothing to PII or AUTH */
}

/* ---- §5.3 primary ------------------------------------------------------------------ */

static void test_primary(void)
{
    /* severity first */
    Classified c = run(MASK("SQLI", "PII_EMAIL"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    c = run(MASK("SCANNER_UA", "HTTP_5XX"));                 /* ERR HIGH > SEC MEDIUM */
    CHECK_EQ_INT(c.primary, cat_index(CAT_ERR));
    CHECK_EQ_INT(c.severity, SEV_HIGH);

    /* equal severity: higher confidence beats the fixed order */
    uint64_t m = MASK("SSH_FAIL_PW", "PII_PHONE_IN");        /* both MEDIUM; AUTH 0.6 vs PII 0.5 */
    c = run3(m, m, MASK("PII_PHONE_IN"));
    CHECK_EQ_INT(c.label_mask, CAT_AUTH | CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_AUTH));           /* although PII > AUTH in the order */
    c = run(MASK("SCANNER_UA", "ERR_LEVEL", "JAVA_EXCEPTION")); /* SEC 0.6 vs ERR 0.84, MEDIUM */
    CHECK_EQ_INT(c.primary, cat_index(CAT_ERR));
    CHECK(NEAR(c.confidence, 0.84));

    /* equal severity and confidence: SEC > PII > AUTH > ERR > PERF */
    static const struct { const char *a, *b; Category win; } t[] = {
        { "SCANNER_UA",  "ERR_LEVEL",   CAT_SEC  },
        { "XSS",         "HTTP_5XX",    CAT_SEC  },
        { "SSH_FAIL_PW", "ERR_LEVEL",   CAT_AUTH },
        { "ERR_LEVEL",   "RESOURCE_PRESSURE", CAT_ERR },
        { "PII_EMAIL",   "SLOW_QUERY",  CAT_PII  },
        { "SSH_FAIL_PW", "RESOURCE_PRESSURE", CAT_AUTH },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        Classified c1 = run(MASK(t[i].a, t[i].b));
        Classified c2 = run(MASK(t[i].b, t[i].a));
        CHECK_EQ_INT(c1.primary, cat_index(t[i].win));
        CHECK_EQ_INT(c2.primary, cat_index(t[i].win));
    }

    /* the category severity is the max over its surviving rules */
    c = run(MASK("SLOW_QUERY", "RESOURCE_PRESSURE"));
    CHECK_EQ_INT(c.severity, SEV_MEDIUM);
    /* a label below tau never becomes primary even with the higher severity */
    c = run(MASK("PII_EMAIL", "TIMEOUT"));                   /* PERF MEDIUM 0.35, PII LOW 0.6 */
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_PII));
    CHECK_EQ_INT(c.severity, SEV_LOW);
}

/* ---- correlator hook ------------------------------------------------------------- */

static void test_escalation(void)
{
    MatchResult m = { .seq = 3, .rule_mask = MASK("HTTP_401_403"), .valid_mask = MASK("HTTP_401_403") };
    Evidence ev;
    aggregator_evidence(&agg, &m, &ev);
    CHECK(NEAR(ev.conf[cat_index(CAT_AUTH)], 0.35));
    CHECK_EQ_INT(ev.sev[cat_index(CAT_SEC)], -1);
    aggregator_escalate(&ev, CAT_SEC, SEV_HIGH);
    Classified c;
    aggregator_decide(&agg, &ev, m.seq, ev.kept, FLAG_BRUTE_FORCE, &c);
    CHECK_EQ_INT(c.label_mask, CAT_SEC);                    /* AUTH is still below tau */
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    CHECK_EQ_INT(c.severity, SEV_HIGH);
    CHECK(NEAR(c.confidence, 0.95));
    CHECK_EQ_INT(c.flags, FLAG_BRUTE_FORCE);
    CHECK_EQ_INT(c.seq, 3);

    /* escalation never lowers a stronger category, nor its severity */
    m.rule_mask = m.valid_mask = MASK("SQLI", "CMD_INJECTION", "XSS");
    m.ctx_mask = MASK("SQLI");                                /* 1 - 0.25 * 0.4 * 0.4 = 0.96 */
    aggregator_evidence(&agg, &m, &ev);
    float before = ev.conf[cat_index(CAT_SEC)];
    CHECK(before > 0.95f);
    aggregator_escalate(&ev, CAT_SEC, SEV_HIGH);
    CHECK(ev.conf[cat_index(CAT_SEC)] == before);
    CHECK_EQ_INT(ev.sev[cat_index(CAT_SEC)], SEV_CRITICAL);
}

/* ---- subsumption: explanation only ----------------------------------------------------- */

static void test_subsumption(void)
{
    static const char text[] =
        "OUTER\tSEC\tHIGH\t0.6\t-\t-\t-\t-\tdrop table [a-z]+\n"
        "INNER\tSEC\tMEDIUM\t0.6\t-\t-\t-\t-\ttable\n"
        "OTHERCAT\tERR\tLOW\t0.6\t-\t-\t-\t-\ttable [a-z]+\n"
        "SAME1\tPERF\tLOW\t0.6\t-\t-\t-\t-\tslow\n"
        "SAME2\tPERF\tLOW\t0.6\t-\t-\t-\t-\ts[l]ow\n";
    RuleSet r2;
    char err[256];
    CHECK_EQ_INT(rules_parse(text, sizeof text - 1, "inline", &r2, err, sizeof err), 0);
    Extractor *x = extractor_new(&r2, err, sizeof err);
    Aggregator a2;
    aggregator_init(&a2, &r2, 0.5f);
    const char *line = "x drop table users; slow";
    uint64_t all = 0x1F;
    uint64_t sub = extract_subsumed(x, line, strlen(line), all);
    CHECK_EQ_INT(sub, (1u << 1) | (1u << 4));    /* INNER inside OUTER; SAME2 equals SAME1 (higher index drops) */
    MatchResult m = { .rule_mask = all, .valid_mask = all };
    Classified with, without;
    aggregator_classify(&a2, &m, sub, &with);
    aggregator_classify(&a2, &m, 0, &without);
    CHECK_EQ_INT(with.rule_mask, all & ~sub);
    CHECK_EQ_INT(with.label_mask, without.label_mask);      /* labels, confidence, severity unchanged */
    CHECK(with.confidence == without.confidence);
    CHECK_EQ_INT(with.severity, without.severity);
    CHECK(NEAR(with.confidence, 0.84));                     /* SEC: OUTER and INNER both count */
    /* a lone rule in its category, or a kept set without INNER, has nothing to drop */
    CHECK_EQ_INT(extract_subsumed(x, line, strlen(line), (1u << 0) | (1u << 2)), 0);
    extractor_free(x);
    rules_free(&r2);
}

/* ---- invariants over random evidence --------------------------------------------- */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_invariants(void)
{
    const uint64_t all = ((uint64_t)1 << rs.count) - 1;
    int bad = 0;
    for (int iter = 0; iter < 20000 && !bad; iter++) {
        uint64_t m = rng() & rng() & all, v = m & (rng() | rng()), x = m & rng();
        Classified c = run3(m, v, x);
        /* explanation = surviving rules; labels only from surviving, labelled rules */
        if (c.rule_mask != (m & v))
            bad = 1;
        uint32_t possible = 0;
        for (int i = 0; i < rs.count; i++)
            if ((m & v) >> i & 1 && !rs.rules[i].no_label)
                possible |= (uint32_t)rs.rules[i].category;
        if (c.label_mask & ~possible)
            bad = 2;
        if (c.label_mask == 0 && (c.primary != PRIMARY_NONE || c.severity != SEV_INFO || c.confidence != 0.0f))
            bad = 3;
        if (c.label_mask && (c.primary >= N_CATEGORIES || !(c.label_mask & (uint32_t)cat_from_index(c.primary)) ||
                             c.confidence + CONF_EPS < agg.tau || c.confidence > 1.0f))
            bad = 4;
        /* more evidence never lowers a category: adding context to everything keeps every label */
        Classified more = run3(m, v, m);
        if ((c.label_mask & more.label_mask) != c.label_mask)
            bad = 5;
    }
    if (bad)
        fprintf(stderr, "invariant %d broken\n", bad);
    CHECK_EQ_INT(bad, 0);
}

static void test_cat_index(void)
{
    for (int i = 0; i < N_CATEGORIES; i++)
        CHECK_EQ_INT(cat_index(cat_from_index(i)), i);
    CHECK_EQ_INT(cat_index(CAT_AUTH), 0);
    CHECK_EQ_INT(cat_index(CAT_PII), 4);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load rules: %s\n", err);
        return 1;
    }
    aggregator_init(&agg, &rs, 0.5f);
    posix = posix_new(&rs, err, sizeof err);
    ext = extractor_new(&rs, err, sizeof err);
    if (!posix || !ext) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    test_worked_examples();
    test_scores();
    test_noisy_or_and_tau();
    test_discard();
    test_no_label_rule();
    test_primary();
    test_escalation();
    test_subsumption();
    test_invariants();
    test_cat_index();
    extractor_free(ext);
    posix_free(posix);
    rules_free(&rs);
    return TEST_DONE();
}
