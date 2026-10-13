#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "annot.h"
#include "engine_posix.h"
#include "extract.h"
#include "normalize.h"
#include "rules.h"

static RuleSet rs;

static uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static void test_parse_ipv4(void)
{
    uint32_t ip = 0;
    CHECK(parse_ipv4("203.0.113.5", 11, &ip) && ip == ip4(203, 0, 113, 5));
    CHECK(parse_ipv4("0.0.0.0", 7, &ip) && ip == 0);
    CHECK(parse_ipv4("255.255.255.255", 15, &ip) && ip == 0xFFFFFFFFu);
    CHECK(parse_ipv4("010.001.1.1", 11, &ip) && ip == ip4(10, 1, 1, 1));
    static const char *const bad[] = {
        "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.", ".1.2.3", "1..2.3", "1.2.3.4a", "a.b.c.d",
        "1.2.3.1000", "1,2,3,4", " 1.2.3.4", "1.2.3.4 ", "1.2.3.-4",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!parse_ipv4(bad[i], strlen(bad[i]), &ip));
    CHECK(parse_ipv4("1.2.3.4xyz", 7, &ip) && ip == ip4(1, 2, 3, 4));   /* the length argument is honoured */
}

static void test_ip_and_user_helpers(void)
{
    uint32_t ip = 0;
    char u[32];
    const char *l = "x Failed password for bob from 10.1.2.3";
    Span sp = { 2, (uint32_t)strlen(l) };
    CHECK(extract_ip(l, sp, &ip) && ip == ip4(10, 1, 2, 3));
    extract_user(l, strlen(l), sp, u);
    CHECK_STR_EQ(u, "bob");

    /* a truncated address that [0-9.]+ would accept is not an IP */
    const char *t = "Failed password for bob from 10.1.2";
    Span st = { 0, (uint32_t)strlen(t) };
    CHECK(!extract_ip(t, st, &ip));

    /* empty username (Invalid user  from ...) */
    const char *e = "Invalid user  from 1.2.3.4";
    Span se = { 0, (uint32_t)strlen(e) };
    extract_user(e, strlen(e), se, u);
    CHECK_STR_EQ(u, "");
    CHECK(extract_ip(e, se, &ip) && ip == ip4(1, 2, 3, 4));

    /* PAM: the name is outside the span; "ruser=" is not "user=" */
    const char *p = "pam_unix(sshd:auth): authentication failure; ruser= rhost=1.2.3.4  user=root";
    Span sq = { 0, 43 };                                /* up to "failure" */
    extract_user(p, strlen(p), sq, u);
    CHECK_STR_EQ(u, "root");
    const char *p2 = "pam_unix(sshd:auth): authentication failure; ruser=evil rhost=1.2.3.4";
    extract_user(p2, strlen(p2), sq, u);
    CHECK_STR_EQ(u, "");

    /* sudo: login name before " : ", not the USER= target */
    const char *s = "sudo: alice : 3 incorrect password attempts ; TTY=pts/0 ; USER=root ; COMMAND=/bin/ls";
    Span ss = { 14, 41 };
    extract_user(s, strlen(s), ss, u);
    CHECK_STR_EQ(u, "alice");

    /* long names are cut at 31 bytes */
    char longline[128];
    snprintf(longline, sizeof longline, "Invalid user %040d from 1.2.3.4", 7);
    extract_user(longline, strlen(longline), (Span){ 0, (uint32_t)strlen(longline) }, u);
    CHECK_EQ_INT(strlen(u), 31);
}

/* ---- the AUTH example lines of the corpus -------------------------------------- */

typedef struct {
    const char *needle;      /* substring identifying the corpus line */
    const char *rule;        /* matching rule that carries the extract flags */
    uint32_t    ip;          /* 0: the rule does not extract an address */
    const char *user;
} AuthCase;

static const AuthCase auth_cases[] = {
    { "Failed password for root from 203.0.113.5 port",              "SSH_FAIL_PW",     0xCB007105u, "root" },
    { "Failed password for invalid user oracle from 203.0.113.5",    "SSH_FAIL_PW",     0xCB007105u, "oracle" },
    { "Invalid user admin from 203.0.113.9",                         "SSH_INVALID_USER", 0xCB007109u, "admin" },
    { "authentication failure; logname= uid=0",                      "PAM_AUTH_FAIL",   0,           "root" },
    { "sudo: alice : 3 incorrect password attempts",                 "SUDO_BAD_PW",     0,           "alice" },
    { "Accepted password for alice from 198.51.100.7",               "SSH_ACCEPTED",    0xC6336407u, "alice" },
    { "Accepted publickey for deploy from 198.51.100.8",             "SSH_ACCEPTED",    0xC6336408u, "deploy" },
    { "Failed password for jane@example.com from 203.0.113.9",       "SSH_FAIL_PW",     0xCB007109u, "jane@example.com" },
    /* the correlator section (T5.1) */
    { "Failed password for root from 192.0.2.66",                    "SSH_FAIL_PW",     0xC0000242u, "root" },
    { "Accepted password for root from 192.0.2.66",                  "SSH_ACCEPTED",    0xC0000242u, "root" },
    { "Invalid user alice from 192.0.2.77",                          "SSH_INVALID_USER", 0xC000024Du, "alice" },
    { "Invalid user bob from 192.0.2.77",                            "SSH_INVALID_USER", 0xC000024Du, "bob" },
    { "Invalid user carol from 192.0.2.77",                          "SSH_INVALID_USER", 0xC000024Du, "carol" },
    { "Invalid user dave from 192.0.2.77",                           "SSH_INVALID_USER", 0xC000024Du, "dave" },
    { "Invalid user erin from 192.0.2.77",                           "SSH_INVALID_USER", 0xC000024Du, "erin" },
    { "Failed password for test from 192.0.2.88",                    "SSH_FAIL_PW",     0xC0000258u, "test" },
    { "Accepted password for test from 192.0.2.88",                  "SSH_ACCEPTED",    0xC0000258u, "test" },
};

static void test_auth_corpus(void)
{
    AnnotCorpus c;
    CHECK_EQ_INT(annot_load("tests/corpus/synthetic_small.log", &rs, &c), 0);
    char err[256];
    Extractor *x = extractor_new(&rs, err, sizeof err);
    CHECK(x != NULL);
    char *norm = malloc(65536);
    unsigned wants_entities = 0;
    for (int r = 0; r < rs.count; r++)
        if (rs.rules[r].extract)
            wants_entities |= 1u << r;

    size_t covered = 0, seen = 0;
    for (size_t i = 0; x && i < c.n; i++) {
        uint64_t exp = c.e[i].expect;
        if (!(exp & wants_entities))
            continue;
        seen++;
        size_t n = normalize_line(c.e[i].text, c.e[i].len, norm);
        MatchResult m;
        memset(&m, 0xAB, sizeof m);
        extract_line(x, norm, n, exp, &m);
        const AuthCase *ac = NULL;
        for (size_t k = 0; k < sizeof auth_cases / sizeof auth_cases[0]; k++)
            if (strstr(c.e[i].text, auth_cases[k].needle))
                ac = &auth_cases[k];
        CHECK(ac != NULL);
        if (!ac) {
            fprintf(stderr, "no expectation for AUTH line %zu: %s\n", c.e[i].file_line, c.e[i].text);
            continue;
        }
        covered++;
        CHECK(exp & ((uint64_t)1 << rules_find(&rs, ac->rule)));
        CHECK_EQ_INT(m.src_ip, ac->ip);
        CHECK_STR_EQ(m.user, ac->user);
        CHECK_EQ_INT(m.valid_mask, exp);               /* none of these rules has a validator */
        CHECK_EQ_INT(m.rule_mask, exp);
    }
    CHECK(seen >= 8);
    CHECK_EQ_INT(covered, seen);
    free(norm);
    extractor_free(x);
    annot_free(&c);
}

/* A real log: every "Failed password" / "Invalid user" line of the OpenSSH sample gives the
 * same IP and user as a plain sscanf-style reading of the line. */
static void test_openssh_sample(void)
{
    char err[256];
    Extractor *x = extractor_new(&rs, err, sizeof err);
    EnginePosix *pe = posix_new(&rs, err, sizeof err);
    CHECK(x != NULL && pe != NULL);
    FILE *f = fopen("tests/corpus/OpenSSH_2k.log", "r");
    CHECK(f != NULL);
    char line[2048], norm[2048];
    int checked_fail = 0, checked_inv = 0, with_ip = 0;
    while (x && pe && f && fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        size_t n = normalize_line(line, len, norm);
        norm[n] = '\0';          /* (some Loghub lines carry stray NUL bytes; strstr below needs a terminator) */
        uint64_t mask = posix_match(pe, norm, n);
        MatchResult m;
        extract_line(x, norm, n, mask, &m);

        char want_user[64] = "", want_ip[64] = "";
        const char *a;
        if ((mask >> rules_find(&rs, "SSH_FAIL_PW") & 1) && (a = strstr(norm, "Failed password for "))) {
            a += strlen("Failed password for ");
            if (strncmp(a, "invalid user ", 13) == 0)
                a += 13;
            sscanf(a, "%63s", want_user);
            const char *fr = strstr(a, " from ");
            sscanf(fr + 6, "%63s", want_ip);
            checked_fail++;
        } else if ((mask >> rules_find(&rs, "SSH_INVALID_USER") & 1) && (a = strstr(norm, "Invalid user "))) {
            sscanf(a + 13, "%63s", want_user);
            const char *fr = strstr(a, " from ");
            sscanf(fr + 6, "%63s", want_ip);
            checked_inv++;
        } else {
            continue;
        }
        uint32_t want = 0;
        bool ok = parse_ipv4(want_ip, strlen(want_ip), &want);
        CHECK(ok);
        CHECK_EQ_INT(m.src_ip, ok ? want : 0);
        CHECK_STR_EQ(m.user, want_user);
        with_ip += m.src_ip != 0;
    }
    CHECK(checked_fail > 400);
    CHECK(checked_inv > 50);
    CHECK_EQ_INT(with_ip, checked_fail + checked_inv);
    if (f)
        fclose(f);
    posix_free(pe);
    extractor_free(x);
}

/* ---- validators through extract_line, spans, context ------------------------------ */

typedef struct {
    const char *needle;
    const char *rule;
    int         valid;       /* expected valid_mask bit of `rule` */
} PiiCase;

static const PiiCase pii_cases[] = {
    { "card=4111 1111 1111 1111", "PII_CARD",    1 },
    { "card=4111 1111 1111 1111", "PII_AADHAAR", 0 },   /* the first 12 digits also match the Aadhaar shape */
    { "card=4012888888881881",    "PII_CARD",    1 },
    { "card=4111 1111 1111 1112", "PII_CARD",    0 },   /* fails Luhn */
    { "card=4111 1111 1111 1112", "PII_AADHAAR", 0 },
    { "uid 2341 2341 2346",       "PII_AADHAAR", 1 },
    { "aadhaar=234123412346",     "PII_AADHAAR", 1 },
    { "uid 234123412347",         "PII_AADHAAR", 0 },   /* fails Verhoeff */
    { "order id 523456789012",    "PII_AADHAAR", 0 },
    { "pan ABCPE1234F",           "PII_PAN",     1 },
    { "ts=20261011100000",        "PII_CARD",    0 },   /* a timestamp that looks like a card number */
};

static void test_validators_in_corpus(void)
{
    AnnotCorpus c;
    CHECK_EQ_INT(annot_load("tests/corpus/synthetic_small.log", &rs, &c), 0);
    char err[256];
    Extractor *x = extractor_new(&rs, err, sizeof err);
    char *norm = malloc(65536);
    int hits = 0;
    for (size_t k = 0; x && k < sizeof pii_cases / sizeof pii_cases[0]; k++) {
        const PiiCase *pc = &pii_cases[k];
        int rule = rules_find(&rs, pc->rule);
        CHECK(rule >= 0);
        int found = 0;
        for (size_t i = 0; i < c.n; i++) {
            if (!strstr(c.e[i].text, pc->needle))
                continue;
            found = 1;
            size_t n = normalize_line(c.e[i].text, c.e[i].len, norm);
            MatchResult m;
            extract_line(x, norm, n, c.e[i].expect, &m);
            CHECK(c.e[i].expect >> rule & 1);
            CHECK_EQ_INT((m.valid_mask >> rule) & 1, pc->valid);
            hits++;
        }
        CHECK(found);
    }
    CHECK_EQ_INT(hits, sizeof pii_cases / sizeof pii_cases[0]);

    /* every matched rule without a validator is valid; unmatched rules never are */
    for (size_t i = 0; x && i < c.n; i++) {
        size_t n = normalize_line(c.e[i].text, c.e[i].len, norm);
        MatchResult m;
        extract_line(x, norm, n, c.e[i].expect, &m);
        CHECK_EQ_INT(m.valid_mask & ~c.e[i].expect, 0);
        for (int r = 0; r < rs.count; r++)
            if ((c.e[i].expect >> r & 1) && rs.rules[r].validator == VAL_NONE)
                CHECK(m.valid_mask >> r & 1);
    }
    free(norm);
    extractor_free(x);
    annot_free(&c);
}

static void test_spans(void)
{
    char err[256];
    Extractor *x = extractor_new(&rs, err, sizeof err);
    CHECK(x != NULL);
    int card = rules_find(&rs, "PII_CARD");
    Span sp[MAX_SPANS];

    /* two numbers one space apart: both found (the second needs the shared space as its boundary),
     * and only the second is valid */
    const char *l = "a 4111111111111111 4111111111111112 z";
    int n = extractor_spans(x, card, l, strlen(l), sp, MAX_SPANS);
    CHECK_EQ_INT(n, 2);
    if (n == 2) {
        CHECK(sp[0].so < sp[1].so);
        CHECK(memmem(l + sp[0].so, sp[0].eo - sp[0].so, "4111111111111111", 16) != NULL);
        CHECK(memmem(l + sp[1].so, sp[1].eo - sp[1].so, "4111111111111112", 16) != NULL);
    }
    MatchResult m;
    extract_line(x, l, strlen(l), (uint64_t)1 << card, &m);
    CHECK((m.valid_mask >> card) & 1);                 /* one valid span is enough (§5.1) */

    /* '^' only matches at the real start of the line, also when later windows are searched */
    const char *s = "4111111111111111";
    n = extractor_spans(x, card, s, strlen(s), sp, MAX_SPANS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(sp[0].so, 0);
    CHECK_EQ_INT(sp[0].eo, 16);

    /* the span cap */
    char many[600];
    size_t o = 0;
    for (int i = 0; i < 30; i++)
        o += (size_t)snprintf(many + o, sizeof many - o, "4111111111111111 ");
    n = extractor_spans(x, card, many, o, sp, 5);
    CHECK_EQ_INT(n, 5);
    n = extractor_spans(x, rules_find(&rs, "SQLI"), l, strlen(l), sp, MAX_SPANS);
    CHECK_EQ_INT(n, 0);                                /* no capture regex for rules that need none */
    extractor_free(x);
}

static void test_context(void)
{
    int card = rules_find(&rs, "PII_CARD"), pan = rules_find(&rs, "PII_PAN");
    uint64_t both = ((uint64_t)1 << card) | ((uint64_t)1 << pan);
    const char *a = "payment captured card=4111111111111111";
    CHECK_EQ_INT(context_mask(&rs, a, strlen(a), both), (uint64_t)1 << card);
    const char *b = "Customer PAN is ABCPE1234F and CC 4111111111111111";
    CHECK_EQ_INT(context_mask(&rs, b, strlen(b), both), both);     /* case-insensitive: PAN, CC */
    const char *c = "no keywords, just 4111111111111111";
    CHECK_EQ_INT(context_mask(&rs, c, strlen(c), both), 0);
    CHECK_EQ_INT(context_mask(&rs, b, strlen(b), 0), 0);           /* only rules in the mask count */
    CHECK_EQ_INT(context_mask(&rs, b, strlen(b), (uint64_t)1 << card), (uint64_t)1 << card);
    CHECK_EQ_INT(context_mask(&rs, a, 6, both), 0);                /* "paymen": the length argument is honoured */
    CHECK_EQ_INT(context_mask(&rs, a, 7, both), (uint64_t)1 << card);   /* "payment" */
    /* a rule without context keywords never sets its bit */
    int sqli = rules_find(&rs, "SQLI");
    CHECK_EQ_INT(context_mask(&rs, a, strlen(a), (uint64_t)1 << sqli), 0);
    /* a keyword cut by the end of the buffer does not match */
    CHECK_EQ_INT(context_mask(&rs, "card", 3, (uint64_t)1 << card), 0);
    CHECK_EQ_INT(context_mask(&rs, "CARD", 4, (uint64_t)1 << card), (uint64_t)1 << card);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    test_parse_ipv4();
    test_ip_and_user_helpers();
    test_auth_corpus();
    test_openssh_sample();
    test_validators_in_corpus();
    test_spans();
    test_context();
    rules_free(&rs);
    return TEST_DONE();
}
