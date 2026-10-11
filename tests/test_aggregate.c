#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "aggregate.h"
#include "rules.h"

static RuleSet rs;
static Aggregator agg;

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

static Classified run(uint64_t m)
{
    Classified c;
    aggregator_classify(&agg, 7, m, &c);
    return c;
}

static void test_basic(void)
{
    Classified c = run(0);
    CHECK_EQ_INT(c.seq, 7);
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
    CHECK_EQ_INT(c.severity, SEV_INFO);
    CHECK(c.confidence == 0.0f);
    CHECK_EQ_INT(c.rule_mask, 0);
    CHECK_EQ_INT(c.flags, 0);

    c = run(MASK("SSH_FAIL_PW"));
    CHECK_EQ_INT(c.label_mask, CAT_AUTH);
    CHECK_EQ_INT(c.primary, cat_index(CAT_AUTH));
    CHECK_EQ_INT(c.severity, SEV_MEDIUM);
    CHECK(c.confidence == 1.0f);
    CHECK_EQ_INT(c.rule_mask, MASK("SSH_FAIL_PW"));

    c = run(MASK("SQLI"));
    CHECK_EQ_INT(c.label_mask, CAT_SEC);
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);

    c = run(MASK("PII_EMAIL"));
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.severity, SEV_LOW);
}

static void test_no_label_rule(void)
{
    /* SSH_ACCEPTED (AUTH*, INFO) never produces a label on its own ... */
    Classified c = run(MASK("SSH_ACCEPTED"));
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
    CHECK_EQ_INT(c.severity, SEV_INFO);
    CHECK_EQ_INT(c.rule_mask, MASK("SSH_ACCEPTED"));       /* but stays in the mask */

    /* ... and does not change a line that has other labels. */
    c = run(MASK("SSH_ACCEPTED", "PII_EMAIL"));
    CHECK_EQ_INT(c.label_mask, CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_PII));
    CHECK_EQ_INT(c.severity, SEV_LOW);
}

static void test_multi_label(void)
{
    /* SQLi + email: SEC|PII, SEC wins on severity (CRITICAL vs LOW) */
    Classified c = run(MASK("SQLI", "PII_EMAIL"));
    CHECK_EQ_INT(c.label_mask, CAT_SEC | CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);

    /* ERROR line with SQL in it */
    c = run(MASK("ERR_LEVEL", "SQLI"));
    CHECK_EQ_INT(c.label_mask, CAT_ERR | CAT_SEC);
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));

    /* failed login by an email-address user */
    c = run(MASK("SSH_FAIL_PW", "PII_EMAIL"));
    CHECK_EQ_INT(c.label_mask, CAT_AUTH | CAT_PII);
    CHECK_EQ_INT(c.primary, cat_index(CAT_AUTH));          /* MEDIUM beats LOW */
    CHECK_EQ_INT(c.severity, SEV_MEDIUM);

    /* all five categories */
    c = run(MASK("SSH_FAIL_PW", "XSS", "ERR_LEVEL", "SLOW_QUERY", "PII_PAN"));
    CHECK_EQ_INT(c.label_mask, CAT_AUTH | CAT_SEC | CAT_ERR | CAT_PERF | CAT_PII);
    CHECK_EQ_INT(c.severity, SEV_HIGH);
}

static void test_ties(void)
{
    /* every row: two rules of equal category severity, expected winner */
    static const struct { const char *a, *b; Category win; int sev; } t[] = {
        { "SCANNER_UA",   "PII_PHONE_IN", CAT_SEC,  SEV_MEDIUM },   /* SEC  > PII  */
        { "XSS",          "PII_PAN",      CAT_SEC,  SEV_HIGH   },   /* SEC  > PII  */
        { "PII_PHONE_IN", "SSH_FAIL_PW",  CAT_PII,  SEV_MEDIUM },   /* PII  > AUTH */
        { "PII_PAN",      "HTTP_5XX",     CAT_PII,  SEV_HIGH   },   /* PII  > ERR  */
        { "SSH_FAIL_PW",  "ERR_LEVEL",    CAT_AUTH, SEV_MEDIUM },   /* AUTH > ERR  */
        { "SSH_FAIL_PW",  "TIMEOUT",      CAT_AUTH, SEV_MEDIUM },   /* AUTH > PERF */
        { "ERR_LEVEL",    "TIMEOUT",      CAT_ERR,  SEV_MEDIUM },   /* ERR  > PERF */
        { "PII_EMAIL",    "SLOW_QUERY",   CAT_PII,  SEV_LOW    },   /* PII  > PERF */
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        /* same winner whichever way round the rules are listed */
        Classified c1 = run(MASK(t[i].a, t[i].b));
        Classified c2 = run(MASK(t[i].b, t[i].a));
        CHECK_EQ_INT(c1.primary, cat_index(t[i].win));
        CHECK_EQ_INT(c2.primary, cat_index(t[i].win));
        CHECK_EQ_INT(c1.severity, t[i].sev);
        CHECK_EQ_INT(c1.label_mask, c2.label_mask);
        CHECK_EQ_INT(c1.label_mask & (uint32_t)t[i].win, (uint32_t)t[i].win);
    }

    /* severity beats the fixed order: ERR HIGH over SEC MEDIUM */
    Classified c = run(MASK("SCANNER_UA", "HTTP_5XX"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_ERR));
    CHECK_EQ_INT(c.severity, SEV_HIGH);

    /* category severity is the max over its rules: PERF = MEDIUM via TIMEOUT
     * (not LOW via SLOW_QUERY), so ERR (MEDIUM) wins the tie on order only. */
    c = run(MASK("SLOW_QUERY", "TIMEOUT", "ERR_LEVEL"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_ERR));
    c = run(MASK("SLOW_QUERY", "TIMEOUT"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_PERF));
    CHECK_EQ_INT(c.severity, SEV_MEDIUM);

    /* HIGH from ERR, but a higher PII rule on the same line wins */
    c = run(MASK("CRASH_OOM", "PII_EMAIL", "SLOW_QUERY"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_ERR));
    CHECK_EQ_INT(c.label_mask, CAT_ERR | CAT_PII | CAT_PERF);
    c = run(MASK("CRASH_OOM", "PII_CARD"));
    CHECK_EQ_INT(c.primary, cat_index(CAT_PII));           /* CRITICAL */
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Invariants over many random masks. */
static void test_invariants(void)
{
    const uint64_t all = ((uint64_t)1 << rs.count) - 1;
    for (int iter = 0; iter < 20000; iter++) {
        uint64_t m = rng() & rng() & rng() & all;           /* sparse masks */
        Classified c = run(m);

        uint32_t want_labels = 0;
        int want_sev = -1;
        for (int i = 0; i < rs.count; i++) {
            if (!((m >> i) & 1) || rs.rules[i].no_label)
                continue;
            want_labels |= (uint32_t)rs.rules[i].category;
            if ((int)rs.rules[i].severity > want_sev)
                want_sev = (int)rs.rules[i].severity;
        }
        if (c.label_mask != want_labels || c.rule_mask != m) {
            CHECK(0);
            break;
        }
        if (want_labels == 0) {
            if (c.primary != PRIMARY_NONE || c.severity != SEV_INFO) {
                CHECK(0);
                break;
            }
            continue;
        }
        /* the line severity is the highest labeled severity, and the primary
         * is one of the labels */
        if ((int)c.severity != want_sev || c.primary >= N_CATEGORIES ||
            !(c.label_mask & (uint32_t)cat_from_index(c.primary))) {
            CHECK(0);
            break;
        }
    }
    CHECK(1);
}

static void test_ignores_unknown_bits(void)
{
    /* bits at or above n_rules are ignored */
    Classified c = run(~(uint64_t)0 << rs.count);
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.primary, PRIMARY_NONE);
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
    aggregator_init(&agg, &rs);
    test_basic();
    test_no_label_rule();
    test_multi_label();
    test_ties();
    test_invariants();
    test_ignores_unknown_bits();
    test_cat_index();
    rules_free(&rs);
    return TEST_DONE();
}
