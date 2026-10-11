#include <glob.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "batch.h"
#include "dfa.h"
#include "engine_posix.h"
#include "nfa.h"
#include "reader.h"
#include "regex_parse.h"
#include "rules.h"

static uint64_t rng_state = 0x13198a2e03707344ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static RuleSet rs;
static Nfa nfa;
static EnginePosix *posix;

/* Test strings: every corpus line (normalized), edge strings, fragment mixes. */
typedef struct { char *s; size_t n; } Str;
static Str *strs;
static size_t nstrs, capstrs;

static void add_str(const char *s, size_t n)
{
    if (nstrs == capstrs) {
        capstrs = capstrs ? capstrs * 2 : 1024;
        strs = realloc(strs, capstrs * sizeof *strs);
    }
    strs[nstrs].s = malloc(n + 1);
    memcpy(strs[nstrs].s, s, n);
    strs[nstrs].s[n] = '\0';
    strs[nstrs++].n = n;
}

static void collect_strings(void)
{
    glob_t g;
    if (glob("tests/corpus/*.log", 0, NULL, &g) == 0) {
        Batch b;
        batch_init(&b);
        for (size_t f = 0; f < g.gl_pathc; f++) {
            Reader *r = reader_open_path(g.gl_pathv[f], 0);
            if (!r)
                continue;
            while (reader_next_batch(r, &b, 256) > 0)
                for (uint32_t i = 0; i < b.count; i++)
                    add_str(batch_norm(&b, i), batch_norm_len(&b, i));
            reader_close(r);
        }
        batch_free(&b);
        globfree(&g);
    }
    static const char *const edge[] = {
        "", " ", "ERROR", "xERROR", "ERRORx", "SEVERE", "4111 1111 1111 1111", "234123412346",
        "9876543210", "+91-9876543210", "\tat a.b(", "at x(", "a@b.co", "ABCPE1234F",
        "took 1234ms", "union  select", "' or '1'='1", "; wget ", "../", "\" 403 -", "\" 599 -",
        "password=x", "disk usage 95%", "Out of memory: Kill process", "timed out",
    };
    for (size_t i = 0; i < sizeof edge / sizeof edge[0]; i++)
        add_str(edge[i], strlen(edge[i]));
    static const char *const frag[] = {
        "ERROR", " ", "x", "1", "9", "-", "+91", "@", ".com", "'", "=", " or ", "union",
        "select", "/", "..", "\\", "<script", "(", "$(", ";", "wget ", "took ", "ms", "1234",
        "4111", "9876543210", "Failed password for ", "from ", "1.2.3.4", "\" 403 ", "\t", "at ",
        "Exception: ", "password=", "ABCPE",
    };
    char buf[256];
    for (int it = 0; it < 5000; it++) {
        size_t len = 0;
        int parts = (int)(rng() % 8);
        for (int p = 0; p < parts; p++) {
            const char *f = frag[rng() % (sizeof frag / sizeof frag[0])];
            size_t fl = strlen(f);
            if (len + fl >= sizeof buf)
                break;
            memcpy(buf + len, f, fl);
            len += fl;
        }
        add_str(buf, len);
    }
}

/* DFA set vs NFA simulation (and regexec) on every test string. */
static void compare_all(const DfaSet *ds, const char *label)
{
    size_t bad = 0;
    for (size_t i = 0; i < nstrs; i++) {
        uint64_t d = dfaset_match(ds, strs[i].s, strs[i].n);
        uint64_t n = nfa_simulate(&nfa, strs[i].s, strs[i].n);
        uint64_t p = posix_match(posix, strs[i].s, strs[i].n);
        tm_checks++;
        if (d != n || d != p) {
            tm_failures++;
            if (++bad <= 10)
                fprintf(stderr, "%s: dfa %#llx nfa %#llx regexec %#llx: \"%s\"\n", label,
                        (unsigned long long)d, (unsigned long long)n, (unsigned long long)p,
                        strs[i].s);
        }
    }
    fprintf(stderr, "%s: %zu strings, %zu mismatches\n", label, nstrs, bad);
}

static void test_default_rules(void)
{
    DfaSet ds;
    char err[256];
    int rc = dfaset_build(&nfa, 0, &ds, err, sizeof err);
    CHECK_EQ_INT(rc, 0);
    if (rc != 0) {
        fprintf(stderr, "dfaset_build: %s\n", err);
        return;
    }
    fprintf(stderr, "default rules: %d group(s), %lld states, %.1f ms\n", ds.ngroups,
            (long long)ds.total_states, ds.build_ms);
    /* All 26 rules in one DFA need ~40k states, so under the default cap of
     * 20000 the builder halves the rule list: expect a few groups, each under
     * the cap, together covering every rule exactly once. */
    CHECK(ds.ngroups >= 1 && ds.ngroups <= 4);
    uint64_t covered = 0;
    for (int g = 0; g < ds.ngroups; g++) {
        const Dfa *d = &ds.groups[g];
        CHECK(d->nstates <= DFA_DEFAULT_MAX_STATES);
        CHECK((covered & d->rules) == 0);
        covered |= d->rules;
        /* table sanity: every transition lands on a valid state */
        int bad = 0;
        for (int64_t i = 0; i < (int64_t)d->nstates * 256; i++)
            bad += d->next[i] < 0 || d->next[i] >= d->nstates;
        CHECK_EQ_INT(bad, 0);
    }
    CHECK_EQ_INT(covered, ((uint64_t)1 << rs.count) - 1);

    compare_all(&ds, "default rules, default cap");
    dfaset_free(&ds);
}

/* A small cap forces the halving into several groups; results must not change. */
static void test_groups(void)
{
    static const int caps[] = { 5000, 300, 100 };   /* largest single rule: 78 states */
    for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
        DfaSet ds;
        char err[256];
        int rc = dfaset_build(&nfa, caps[c], &ds, err, sizeof err);
        CHECK_EQ_INT(rc, 0);
        if (rc != 0) {
            fprintf(stderr, "cap %d: %s\n", caps[c], err);
            continue;
        }
        uint64_t covered = 0;
        for (int g = 0; g < ds.ngroups; g++) {
            CHECK(ds.groups[g].nstates <= caps[c]);
            CHECK((covered & ds.groups[g].rules) == 0);   /* groups are disjoint */
            covered |= ds.groups[g].rules;
        }
        CHECK_EQ_INT(covered, ((uint64_t)1 << rs.count) - 1);
        CHECK(ds.ngroups > 1);
        char label[64];
        snprintf(label, sizeof label, "cap %d, %d groups", caps[c], ds.ngroups);
        compare_all(&ds, label);
        dfaset_free(&ds);
    }

    /* a cap no single rule fits under is an error naming a rule */
    DfaSet ds;
    char err[256];
    CHECK_EQ_INT(dfaset_build(&nfa, 3, &ds, err, sizeof err), -1);
    CHECK(strstr(err, "alone needs more than 3 DFA states") != NULL);
    CHECK_EQ_INT(ds.ngroups, 0);
}

/* Small patterns: single-rule DFA vs regexec on random strings. */
static const struct { const char *re; int icase; } PATTERNS[] = {
    { "a", 0 }, { "ab", 0 }, { "a|b", 0 }, { "a*", 0 }, { "a+b", 0 }, { "(ab)*a", 0 },
    { "a{2}", 0 }, { "a{1,3}b", 0 }, { "a{2,}", 0 }, { "(a|b){2,3}", 0 }, { "^a", 0 },
    { "a$", 0 }, { "^$", 0 }, { "^a*$", 0 }, { "(^|b)a", 0 }, { "a(b|$)", 0 },
    { "(^|[^0-9])1{2}([^0-9]|$)", 0 }, { "[^a]+b", 0 }, { ".", 0 }, { "(a*)*", 0 },
    { "((a|b)*1)+", 0 }, { "x?(a|b)?$", 0 }, { "^[ \t]+a", 0 }, { "a{0}b", 0 },
    { "(a{0,2}b){1,2}", 0 }, { "^(a|b)+$", 0 }, { "^a|b$", 0 }, { "(a|^b)(a|b$)", 0 },
    { "a*$", 0 }, { "^", 0 }, { "$", 0 }, { "(^|a)($|b)", 0 },
    { "A", 1 }, { "[^a]", 1 }, { "AB|b1", 1 }, { "^a+$", 1 },
};

static void test_small_patterns(void)
{
    static const char alphabet[] = "ab01 .-\txA";
    for (size_t p = 0; p < sizeof PATTERNS / sizeof PATTERNS[0]; p++) {
        const char *re = PATTERNS[p].re;
        RegexError err;
        AstNode *ast = regex_parse(re, strlen(re), PATTERNS[p].icase, &err);
        CHECK(ast != NULL);
        if (!ast) {
            fprintf(stderr, "pattern \"%s\": %s\n", re, err.msg);
            continue;
        }
        Nfa one;
        nfa_init(&one);
        CHECK_EQ_INT(nfa_add_rule(&one, ast, 0), 0);
        CHECK_EQ_INT(nfa_finish(&one), 0);
        ast_free(ast);
        Dfa d;
        CHECK_EQ_INT(dfa_build(&one, 1, 0, &d), 0);
        regex_t rx;
        CHECK_EQ_INT(regcomp(&rx, re, REG_EXTENDED | REG_NOSUB | (PATTERNS[p].icase ? REG_ICASE : 0)), 0);

        int bad = 0;
        char s[16];
        for (int it = 0; it < 3000 && bad < 3; it++) {
            size_t len = rng() % 11;
            for (size_t i = 0; i < len; i++)
                s[i] = alphabet[rng() % (sizeof alphabet - 1)];
            regmatch_t range = { 0, (regoff_t)len };
            uint64_t want = regexec(&rx, s, 0, &range, REG_STARTEND) == 0;
            uint64_t got = dfa_match(&d, s, (uint32_t)len);
            tm_checks++;
            if (got != want) {
                tm_failures++;
                bad++;
                fprintf(stderr, "pattern \"%s\" on \"%.*s\": dfa %d, regexec %d\n", re, (int)len, s,
                        (int)got, (int)want);
            }
        }
        regfree(&rx);
        dfa_free(&d);
        nfa_free(&one);
    }
}

/* Hand-checked automaton: "ab" unanchored. States: {} start, after 'a', after
 * "ab" (accepting); every other byte returns to the restart state. */
static void test_tiny_dfa(void)
{
    RegexError err;
    AstNode *ast = regex_parse("ab", 2, false, &err);
    Nfa one;
    nfa_init(&one);
    CHECK_EQ_INT(nfa_add_rule(&one, ast, 0), 0);
    CHECK_EQ_INT(nfa_finish(&one), 0);
    ast_free(ast);
    Dfa d;
    CHECK_EQ_INT(dfa_build(&one, 1, 0, &d), 0);
    /* start (flagged), restart = U, after 'a', after "ab" */
    CHECK_EQ_INT(d.nstates, 4);
    CHECK_EQ_INT(d.accept_mask[0], 0);
    int sa = d.next[0 * 256 + 'a'];
    int sab = d.next[sa * 256 + 'b'];
    CHECK_EQ_INT(d.accept_mask[sab], 1);
    CHECK_EQ_INT(d.accept_mask[sa], 0);
    CHECK_EQ_INT(d.next[sab * 256 + 'a'], sa);
    CHECK_EQ_INT(d.next[sa * 256 + 'a'], sa);        /* "aa": still after an 'a' */
    CHECK(d.next[sa * 256 + 'x'] != sa && d.next[sa * 256 + 'x'] != sab);
    CHECK_EQ_INT(dfa_match(&d, "xxaby", 5), 1);
    CHECK_EQ_INT(dfa_match(&d, "a b", 3), 0);
    dfa_free(&d);
    nfa_free(&one);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0 ||
        nfa_build_rules(&nfa, &rs, err, sizeof err) != 0 ||
        !(posix = posix_new(&rs, err, sizeof err))) {
        fprintf(stderr, "setup failed: %s\n", err);
        return 1;
    }
    collect_strings();
    CHECK(nstrs > 15000);

    test_tiny_dfa();
    test_default_rules();
    test_groups();
    test_small_patterns();

    for (size_t i = 0; i < nstrs; i++)
        free(strs[i].s);
    free(strs);
    posix_free(posix);
    nfa_free(&nfa);
    rules_free(&rs);
    return TEST_DONE();
}
