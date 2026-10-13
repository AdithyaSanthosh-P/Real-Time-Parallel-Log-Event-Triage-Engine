/* T5.1: correlator (spec §6). Crafted sequences through the real
 * classification path: lines are built as MatchResults for the default rules,
 * classified (aggregator_evidence/decide), fed to the correlator, and
 * re-decided when it escalates, exactly as pipeline.c does. */

#include <stdio.h>
#include <string.h>

#include "test_main.h"
#include "aggregate.h"
#include "correlate.h"
#include "rules.h"

static RuleSet    rs;
static Aggregator agg;

typedef struct {
    Correlator *k;
    uint64_t    seq;
} Run;

static uint32_t ip4(int a, int b, int c, int d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

/* Feed one line matching `rule` (by id; NULL = no rule) and return the final
 * classification. */
static Classified feed(Run *r, int64_t ts, const char *rule, uint32_t ip, const char *user)
{
    MatchResult m;
    memset(&m, 0, sizeof m);
    m.seq = r->seq++;
    if (rule) {
        int id = rules_find(&rs, rule);
        CHECK(id >= 0);
        m.rule_mask = m.valid_mask = 1ull << id;
    }
    m.src_ip = ip;
    snprintf(m.user, sizeof m.user, "%s", user ? user : "");
    Evidence ev;
    Classified c;
    aggregator_evidence(&agg, &m, &ev);
    aggregator_decide(&agg, &ev, m.seq, ev.kept, 0, &c);
    uint32_t flags = correlator_observe(r->k, ts, &m, c.label_mask, &ev);
    if (flags)
        aggregator_decide(&agg, &ev, m.seq, c.rule_mask, flags, &c);
    return c;
}

static Run start(const CorrelatorConfig *cfg)
{
    CorrelatorConfig def;
    correlator_default_config(&def);
    Run r = { correlator_new(cfg ? cfg : &def, &rs), 0 };
    CHECK(r.k != NULL);
    return r;
}

static void test_config(void)
{
    CorrelatorConfig c;
    correlator_default_config(&c);
    CHECK_EQ_INT(c.maxretry, 5);
    CHECK_EQ_INT(c.findtime, 60);
    CHECK_EQ_INT(c.spray_users, 5);
    CHECK_EQ_INT(c.spray_window, 300);
    CorrelatorConfig bad = c;
    bad.maxretry = 0;
    CHECK(correlator_new(&bad, &rs) == NULL);
    bad = c;
    bad.maxretry = CORR_RING + 1;
    CHECK(correlator_new(&bad, &rs) == NULL);
    bad = c;
    bad.findtime = -1;
    CHECK(correlator_new(&bad, &rs) == NULL);
    bad = c;
    bad.storm_lines = 0;
    CHECK(correlator_new(&bad, &rs) == NULL);
}

/* The plan's cases. */
static void test_brute_force(void)
{
    uint32_t ip = ip4(203, 0, 113, 9);

    /* 4 failures -> nothing */
    Run r = start(NULL);
    for (int i = 0; i < 4; i++) {
        Classified c = feed(&r, 1000 + i, "SSH_FAIL_PW", ip, "root");
        CHECK_EQ_INT(c.flags, 0);
        CHECK_EQ_INT(c.label_mask, CAT_AUTH);
        CHECK_EQ_INT(c.severity, SEV_MEDIUM);
    }
    correlator_free(r.k);

    /* 5 within the window -> exactly one escalation, on the 5th */
    r = start(NULL);
    int escalated = 0;
    for (int i = 0; i < 12; i++) {
        Classified c = feed(&r, 1000 + 5 * i, "SSH_FAIL_PW", ip, "root");  /* 12 in 55 s */
        if (c.flags) {
            escalated++;
            CHECK_EQ_INT(i, 4);
            CHECK_EQ_INT(c.flags, FLAG_BRUTE_FORCE);
            CHECK_EQ_INT(c.label_mask, CAT_AUTH | CAT_SEC);
            CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
            CHECK_EQ_INT(c.severity, SEV_HIGH);
            CHECK(c.confidence >= 0.95f - 1e-6f);
        }
    }
    CHECK_EQ_INT(escalated, 1);                   /* once per window */
    CHECK_EQ_INT(correlator_count(r.k, 0), 1);
    /* the attack goes on: a new escalation once a full window has passed */
    Classified c = feed(&r, 1000 + 4 * 5 + 60, "SSH_FAIL_PW", ip, "root");
    CHECK_EQ_INT(c.flags, FLAG_BRUTE_FORCE);
    correlator_free(r.k);

    /* 5 spread over more than the window -> nothing */
    r = start(NULL);
    for (int i = 0; i < 10; i++) {
        c = feed(&r, 1000 + 20 * i, "SSH_FAIL_PW", ip, "root");          /* 4 per 60 s */
        CHECK_EQ_INT(c.flags, 0);
    }
    correlator_free(r.k);

    /* the window is per IP: 5 failures from 5 IPs -> nothing */
    r = start(NULL);
    for (int i = 0; i < 5; i++)
        CHECK_EQ_INT(feed(&r, 1000, "SSH_FAIL_PW", ip4(10, 0, 0, i + 1), "root").flags, 0);
    /* failures without an IP (PAM, sudo) are not counted */
    for (int i = 0; i < 6; i++)
        CHECK_EQ_INT(feed(&r, 1000, "PAM_AUTH_FAIL", 0, "root").flags, 0);
    /* both failure rules count for the same IP */
    for (int i = 0; i < 4; i++)
        CHECK_EQ_INT(feed(&r, 1001, i % 2 ? "SSH_FAIL_PW" : "SSH_INVALID_USER", ip, "x").flags, 0);
    CHECK_EQ_INT(feed(&r, 1002, "SSH_FAIL_PW", ip, "x").flags, FLAG_BRUTE_FORCE);
    correlator_free(r.k);

    /* window boundary is inclusive: failures at t, t+15, .., t+60 = 5 in 60 s */
    r = start(NULL);
    for (int i = 0; i < 4; i++)
        feed(&r, 2000 + 15 * i, "SSH_FAIL_PW", ip, "root");
    CHECK_EQ_INT(feed(&r, 2060, "SSH_FAIL_PW", ip, "root").flags, FLAG_BRUTE_FORCE);
    correlator_free(r.k);

    /* configurable: maxretry 3, findtime 10 */
    CorrelatorConfig cfg;
    correlator_default_config(&cfg);
    cfg.maxretry = 3;
    cfg.findtime = 10;
    r = start(&cfg);
    feed(&r, 0, "SSH_FAIL_PW", ip, "a");
    feed(&r, 11, "SSH_FAIL_PW", ip, "a");
    CHECK_EQ_INT(feed(&r, 15, "SSH_FAIL_PW", ip, "a").flags, 0);      /* 0 is out of [5,15] */
    CHECK_EQ_INT(feed(&r, 16, "SSH_FAIL_PW", ip, "a").flags, FLAG_BRUTE_FORCE);
    correlator_free(r.k);
}

static void test_fail_then_success(void)
{
    uint32_t ip = ip4(198, 51, 100, 7);

    /* burst then Accepted -> CRITICAL */
    Run r = start(NULL);
    for (int i = 0; i < 6; i++)
        feed(&r, 5000 + i, "SSH_FAIL_PW", ip, "admin");
    Classified c = feed(&r, 5010, "SSH_ACCEPTED", ip, "admin");
    CHECK_EQ_INT(c.flags, FLAG_FAIL_THEN_SUCCESS);
    CHECK_EQ_INT(c.label_mask, CAT_SEC);          /* SSH_ACCEPTED itself has no label */
    CHECK_EQ_INT(c.primary, cat_index(CAT_SEC));
    CHECK_EQ_INT(c.severity, SEV_CRITICAL);
    CHECK_EQ_INT(correlator_count(r.k, 2), 1);
    /* a success from another IP, or after the window, is just a login */
    c = feed(&r, 5011, "SSH_ACCEPTED", ip4(198, 51, 100, 8), "admin");
    CHECK_EQ_INT(c.flags, 0);
    CHECK_EQ_INT(c.label_mask, 0);
    CHECK_EQ_INT(c.severity, SEV_INFO);
    c = feed(&r, 5000 + 5 + 61, "SSH_ACCEPTED", ip, "admin");
    CHECK_EQ_INT(c.flags, 0);
    correlator_free(r.k);

    /* 4 failures then Accepted: under the threshold -> nothing */
    r = start(NULL);
    for (int i = 0; i < 4; i++)
        feed(&r, 7000 + i, "SSH_FAIL_PW", ip, "admin");
    CHECK_EQ_INT(feed(&r, 7005, "SSH_ACCEPTED", ip, "admin").flags, 0);
    correlator_free(r.k);
}

static void test_password_spray(void)
{
    uint32_t ip = ip4(192, 0, 2, 44);
    static const char *users[] = { "alice", "bob", "carol", "dave", "erin", "frank" };

    /* 5 distinct users within 300 s, slow enough to stay under BRUTE_FORCE */
    Run r = start(NULL);
    for (int i = 0; i < 4; i++)
        CHECK_EQ_INT(feed(&r, 100 + 61 * i, "SSH_INVALID_USER", ip, users[i]).flags, 0);
    Classified c = feed(&r, 100 + 61 * 4, "SSH_INVALID_USER", ip, users[4]);
    CHECK_EQ_INT(c.flags, FLAG_PASSWORD_SPRAY);
    CHECK_EQ_INT(c.label_mask, CAT_AUTH | CAT_SEC);
    CHECK_EQ_INT(c.severity, SEV_HIGH);
    CHECK_EQ_INT(feed(&r, 100 + 61 * 4 + 1, "SSH_INVALID_USER", ip, users[5]).flags, 0);  /* once */
    correlator_free(r.k);

    /* the same user over and over is brute force, not spray */
    r = start(NULL);
    uint32_t all = 0;
    for (int i = 0; i < 20; i++)
        all |= feed(&r, 100 + i, "SSH_FAIL_PW", ip, "root").flags;
    CHECK_EQ_INT(all, FLAG_BRUTE_FORCE);
    correlator_free(r.k);

    /* both at once: 5 users in 5 s */
    r = start(NULL);
    for (int i = 0; i < 4; i++)
        feed(&r, 100 + i, "SSH_FAIL_PW", ip, users[i]);
    CHECK_EQ_INT(feed(&r, 104, "SSH_FAIL_PW", ip, users[4]).flags, FLAG_BRUTE_FORCE | FLAG_PASSWORD_SPRAY);
    correlator_free(r.k);

    /* 5 users spread over more than 300 s -> nothing */
    r = start(NULL);
    for (int i = 0; i < 6; i++)
        CHECK_EQ_INT(feed(&r, 100 + 80 * i, "SSH_INVALID_USER", ip, users[i]).flags, 0);
    correlator_free(r.k);

    /* failures without a user name do not count as distinct users */
    r = start(NULL);
    for (int i = 0; i < 6; i++)
        CHECK_EQ_INT(feed(&r, 100 + 70 * i, "SSH_FAIL_PW", ip, "").flags, 0);
    correlator_free(r.k);
}

static void test_error_storm(void)
{
    CorrelatorConfig cfg;
    correlator_default_config(&cfg);
    cfg.storm_lines = 10;
    cfg.storm_window = 5;
    Run r = start(&cfg);

    /* 9 ERROR lines within 5 s: nothing; the 10th starts a storm */
    for (int i = 0; i < 9; i++)
        CHECK_EQ_INT(feed(&r, 100, "ERR_LEVEL", 0, NULL).flags, 0);
    Classified c = feed(&r, 101, "ERR_LEVEL", 0, NULL);
    CHECK_EQ_INT(c.flags, FLAG_ERROR_STORM);
    CHECK_EQ_INT(c.label_mask, CAT_ERR);
    CHECK_EQ_INT(c.severity, SEV_HIGH);           /* ERR_LEVEL alone is MEDIUM */
    /* the storm goes on: suppressed */
    for (int i = 0; i < 50; i++)
        CHECK_EQ_INT(feed(&r, 102 + i / 10, "ERR_LEVEL", 0, NULL).flags, 0);
    /* non-ERROR lines do not matter */
    CHECK_EQ_INT(feed(&r, 107, "SQLI", 0, NULL).flags, 0);
    /* calm: one ERROR line long after ends the storm, a new burst alerts again */
    CHECK_EQ_INT(feed(&r, 200, "ERR_LEVEL", 0, NULL).flags, 0);
    for (int i = 0; i < 9; i++)
        CHECK_EQ_INT(feed(&r, 201, "ERR_LEVEL", 0, NULL).flags, 0 + (i == 8 ? FLAG_ERROR_STORM : 0));
    CHECK_EQ_INT(correlator_count(r.k, 3), 2);
    /* a slow trickle never storms */
    correlator_free(r.k);
    r = start(&cfg);
    for (int i = 0; i < 100; i++)
        CHECK_EQ_INT(feed(&r, 1000 + i, "ERR_LEVEL", 0, NULL).flags, 0);
    correlator_free(r.k);
}

/* Many IPs (the map grows), each with its own window. */
static void test_many_ips(void)
{
    Run r = start(NULL);
    int esc = 0;
    for (int round = 0; round < 5; round++)
        for (int i = 0; i < 5000; i++)
            esc += feed(&r, 3000 + round, "SSH_FAIL_PW", ip4(10, 1, i / 256, i % 256 + 0) + 1, "u").flags != 0;
    CHECK_EQ_INT(esc, 5000);                      /* every IP escalates once, on its 5th */
    CHECK_EQ_INT(correlator_count(r.k, 0), 5000);
    correlator_free(r.k);
}

/* A line's time going backwards (rotated or merged logs) is handled. */
static void test_time_backwards(void)
{
    uint32_t ip = ip4(203, 0, 113, 50);
    Run r = start(NULL);
    for (int i = 0; i < 4; i++)
        feed(&r, 10000 + i, "SSH_FAIL_PW", ip, "root");
    /* an older line: its window [t-60, t] holds only itself */
    CHECK_EQ_INT(feed(&r, 5000, "SSH_FAIL_PW", ip, "root").flags, 0);
    CHECK_EQ_INT(feed(&r, 10004, "SSH_FAIL_PW", ip, "root").flags, FLAG_BRUTE_FORCE);
    correlator_free(r.k);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    aggregator_init(&agg, &rs, 0.5f);
    test_config();
    test_brute_force();
    test_fail_then_success();
    test_password_spray();
    test_error_storm();
    test_many_ips();
    test_time_backwards();
    rules_free(&rs);
    return TEST_DONE();
}
