#include <glob.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "batch.h"
#include "engine_posix.h"
#include "nfa.h"
#include "normalize.h"
#include "reader.h"
#include "regex_parse.h"
#include "rules.h"

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- default rules: NFA vs regexec on corpus + synthetic strings ---- */

static RuleSet rs;
static Nfa nfa;
static EnginePosix *posix;
static size_t compared, mismatches;

static void compare(const char *s, size_t n, const char *where)
{
    uint64_t want = posix_match(posix, s, n);
    uint64_t got = nfa_simulate(&nfa, s, n);
    compared++;
    tm_checks++;
    if (got != want) {
        tm_failures++;
        if (++mismatches <= 10) {
            fprintf(stderr, "%s: nfa %#llx != regexec %#llx:", where,
                    (unsigned long long)got, (unsigned long long)want);
            for (int r = 0; r < rs.count; r++)
                if (((got ^ want) >> r) & 1)
                    fprintf(stderr, " %s", rs.rules[r].id);
            fprintf(stderr, "\n    \"%.*s\"\n", (int)n, s);
        }
    }
}

static void test_corpus(void)
{
    glob_t g;
    CHECK_EQ_INT(glob("tests/corpus/*.log", 0, NULL, &g), 0);
    Batch b;
    CHECK_EQ_INT(batch_init(&b), 0);
    for (size_t f = 0; f < g.gl_pathc; f++) {
        Reader *r = reader_open_path(g.gl_pathv[f], 0);
        CHECK(r != NULL);
        if (!r)
            continue;
        while (reader_next_batch(r, &b, 256) > 0)
            for (uint32_t i = 0; i < b.count; i++)
                compare(batch_norm(&b, i), batch_norm_len(&b, i), g.gl_pathv[f]);
        reader_close(r);
    }
    batch_free(&b);
    globfree(&g);
}

static void test_edge_strings(void)
{
    static const char *const s[] = {
        "", " ", "ERROR", "xERROR", "ERRORx", "ERROR:", "(ERROR)", "SEVERE", "FATAL\t",
        "4111 1111 1111 1111", "4111111111111111", "x4111-1111-1111-1111", "41111111111111111111",
        "234123412346", "1234123412346", "2341 2341 2346x", "9876543210", "+91 9876543210",
        "+91-9876543210", "19876543210", "\tat a.b(", "  at x(", "at x(", "x\tat a(",
        "a@b.co", "a@b.c", "@b.com", "ABCPE1234F", "ABCPE1234", "Failed password for x from 1",
        "Failed password for invalid user y from 1.2", "Invalid user  from 9", "took 1234ms",
        "took=12345.6 ms", "elapsed: 999 ms", "union  select", "UNION+ALL+SELECT", "' or '1'='1",
        "'or 1=1", "sleep(10)", "BENCHMARK(", "; wget ", "&&curl/", "$(sh ", "||nc x",
        "../", "..\\", "/etc/passwd", "<SCRIPT", "javascript:", "onerror =", "disk usage 95%",
        "queue is full", "High CPU usage", "password=x", "TOKEN=abc&", "api_key=\"",
        "\" 401 1", "\" 403 -", "\" 500 12", "\" 599 -", "Query_time: 3", "timed out",
        "segfault at", "Out of memory: Kill process", "oom-killer",
    };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        compare(s[i], strlen(s[i]), "edge");
}

/* Strings glued together from fragments of the rules, to hit near-misses. */
static void test_fragment_mix(void)
{
    static const char *const frag[] = {
        "ERROR", "error", " ", "x", "1", "9", "0", "-", "+91", "@", ".com", "a", "Z", "'",
        "=", "1=1", " or ", "union", "select", "/", "..", "\\", "<script", "(", ")", "$(",
        ";", "&&", "wget ", "sh/", "took ", "ms", "1234", "4111", "2341", "9876543210",
        "Failed password for ", "from ", "1.2.3.4", "Invalid user ", "\" 403 ", "\" 503 ",
        "\t", "at ", "Exception: ", "%", "password=", "&", "ABCPE", "PAN", "card", ":",
    };
    const size_t nf = sizeof frag / sizeof frag[0];
    char buf[256];
    for (int iter = 0; iter < 5000; iter++) {
        size_t len = 0;
        int parts = (int)(rng() % 8);
        for (int p = 0; p < parts; p++) {
            const char *f = frag[rng() % nf];
            size_t fl = strlen(f);
            if (len + fl >= sizeof buf)
                break;
            memcpy(buf + len, f, fl);
            len += fl;
        }
        compare(buf, len, "mix");
    }
}

/* ---- small patterns: single-rule NFA vs regexec on random strings ---- */

static const struct { const char *re; int icase; } PATTERNS[] = {
    { "a", 0 }, { "ab", 0 }, { "a|b", 0 }, { "a*", 0 }, { "a+b", 0 }, { "(ab)*a", 0 },
    { "a?b?a", 0 }, { "a{2}", 0 }, { "a{1,3}b", 0 }, { "a{2,}", 0 }, { "(a|b){2,3}", 0 },
    { "^a", 0 }, { "a$", 0 }, { "^$", 0 }, { "^a*$", 0 }, { "(^|b)a", 0 }, { "a(b|$)", 0 },
    { "(^|[^0-9])1{2}([^0-9]|$)", 0 }, { "[0-9]+", 0 }, { "[^a]+b", 0 }, { ".", 0 },
    { "a.b", 0 }, { "(a*)*", 0 }, { "(a*)+b", 0 }, { "((a|b)*1)+", 0 }, { "x?(a|b)?$", 0 },
    { "[ \t]+a", 0 }, { "^[ \t]+a", 0 }, { "[ab-]{3}", 0 }, { "a{0}b", 0 },
    { "(a{0,2}b){1,2}", 0 }, { "0+1?$", 0 }, { "^(a|b)+$", 0 }, { "^a|b$", 0 },
    { "(a|^b)(a|b$)", 0 }, { "a{0,1}{2}", 0 }, { "\\.-", 0 }, { "[.]a", 0 },
    { "A", 1 }, { "[^a]", 1 }, { "AB|b1", 1 }, { "^a+$", 1 }, { "[a-b]x", 1 },
};

static void test_small_patterns(void)
{
    static const char alphabet[] = "ab01 .-\txA";
    for (size_t p = 0; p < sizeof PATTERNS / sizeof PATTERNS[0]; p++) {
        RegexError err;
        const char *re = PATTERNS[p].re;
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

        regex_t rx;
        CHECK_EQ_INT(regcomp(&rx, re, REG_EXTENDED | REG_NOSUB | (PATTERNS[p].icase ? REG_ICASE : 0)), 0);

        int bad = 0;
        char s[16];
        for (int iter = 0; iter < 3000 && bad < 3; iter++) {
            size_t len = rng() % 11;
            for (size_t i = 0; i < len; i++)
                s[i] = alphabet[rng() % (sizeof alphabet - 1)];
            regmatch_t range = { 0, (regoff_t)len };
            uint64_t want = regexec(&rx, s, 0, &range, REG_STARTEND) == 0;
            uint64_t got = nfa_simulate(&one, s, len);
            tm_checks++;
            if (got != want) {
                tm_failures++;
                bad++;
                fprintf(stderr, "pattern \"%s\"%s on \"%.*s\": nfa %d, regexec %d\n", re,
                        PATTERNS[p].icase ? " (icase)" : "", (int)len, s, (int)got, (int)want);
            }
        }
        regfree(&rx);
        nfa_free(&one);
    }
}

/* ---- structure ---- */

static void test_structure(void)
{
    /* every state is one of: one charset edge, <= 2 epsilon edges, one
     * assertion edge, or an accept; each rule has exactly one accept. */
    int accepts[RULES_MAX] = {0};
    for (int i = 0; i < nfa.n; i++) {
        const NfaState *s = &nfa.st[i];
        switch (s->kind) {
        case NFA_CHAR:
            CHECK(s->out1 >= 0 && s->out2 == -1 && !cs_empty(&s->set));
            break;
        case NFA_BOL:
        case NFA_EOL:
            CHECK(s->out1 >= 0 && s->out2 == -1);
            break;
        case NFA_EPS:
            break;
        case NFA_ACCEPT:
            CHECK(s->rule >= 0 && s->rule < rs.count && s->out1 == -1);
            if (s->rule >= 0 && s->rule < rs.count)
                accepts[s->rule]++;
            break;
        default:
            CHECK(0);
        }
        CHECK(s->out1 < nfa.n && s->out2 < nfa.n);
    }
    for (int r = 0; r < rs.count; r++) {
        CHECK_EQ_INT(accepts[r], 1);
        CHECK_EQ_INT(nfa.st[nfa.rule_accept[r]].rule, r);
    }
    CHECK(nfa.start >= 0 && nfa.start < nfa.n);
    CHECK_EQ_INT(nfa.nrules, rs.count);

    /* only JAVA_STACK starts with '^' on every path */
    for (int r = 0; r < rs.count; r++)
        CHECK(nfa.rule_anchored[r] == (strcmp(rs.rules[r].id, "JAVA_STACK") == 0));
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
    fprintf(stderr, "combined NFA: %d states for %d rules\n", nfa.n, rs.count);

    test_structure();
    test_corpus();
    test_edge_strings();
    test_fragment_mix();
    test_small_patterns();
    fprintf(stderr, "default rules: %zu strings compared, %zu mismatches\n", compared, mismatches);
    CHECK(compared > 10000);

    posix_free(posix);
    nfa_free(&nfa);
    rules_free(&rs);
    return TEST_DONE();
}
