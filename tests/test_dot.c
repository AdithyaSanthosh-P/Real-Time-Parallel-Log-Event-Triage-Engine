#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "dfa.h"
#include "dot.h"
#include "nfa.h"
#include "rules.h"

/* Render `rs` to a string. */
static char *render(const RuleSet *rs, int *ngroups)
{
    Nfa nfa;
    DfaSet ds;
    char err[256];
    if (nfa_build_rules(&nfa, rs, err, sizeof err) != 0 ||
        dfaset_build(&nfa, 0, &ds, err, sizeof err) != 0) {
        fprintf(stderr, "build: %s\n", err);
        return NULL;
    }
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    CHECK_EQ_INT(dot_write(f, rs, &nfa, &ds), 0);
    fclose(f);
    *ngroups = ds.ngroups;
    dfaset_free(&ds);
    nfa_free(&nfa);
    return buf;
}

/* Syntax sanity a DOT parser relies on: quotes balanced on every line
 * (ignoring escaped ones) and braces balanced outside quotes. */
static void check_syntax(const char *s)
{
    int depth = 0, bad_lines = 0;
    bool in_q = false;
    for (const char *p = s; *p; p++) {
        if (in_q) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_q = false;
            else if (*p == '\n')
                bad_lines++;                      /* a string ran over a line end */
            continue;
        }
        if (*p == '"')
            in_q = true;
        else if (*p == '{')
            depth++;
        else if (*p == '}')
            depth--;
        if (depth < 0)
            break;
    }
    CHECK(!in_q);
    CHECK_EQ_INT(depth, 0);
    CHECK_EQ_INT(bad_lines, 0);
}

static size_t count(const char *s, const char *needle)
{
    size_t n = 0;
    for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

static void test_tiny(void)
{
    RuleSet rs;
    char err[256];
    CHECK_EQ_INT(rules_load("tests/fixtures/tiny.rules", &rs, err, sizeof err), 0);
    CHECK_EQ_INT(rs.count, 2);
    int ng = 0;
    char *dot = render(&rs, &ng);
    CHECK(dot != NULL);
    if (dot) {
        CHECK(strncmp(dot, "digraph triage {\n", 17) == 0);
        CHECK(strstr(dot, "subgraph cluster_nfa {") != NULL);
        CHECK(strstr(dot, "subgraph cluster_dfa0 {") != NULL);
        CHECK_EQ_INT(ng, 1);
        /* NFA: accept states for both rules, ^ and $ assertion edges */
        CHECK(strstr(dot, "shape=doublecircle, label=\"7\\nTINY_AB\"") != NULL);
        CHECK(strstr(dot, "\\nTINY_X\"") != NULL);
        CHECK(strstr(dot, "[label=\"^\", style=dashed]") != NULL);
        CHECK(strstr(dot, "[label=\"$\", style=dashed]") != NULL);
        CHECK(strstr(dot, "[label=\"'b'\"]") != NULL);
        /* DFA: 7 states, icase folded label, end-of-line accept, ranges */
        CHECK(strstr(dot, "DFA group 0 (7 states): TINY_AB TINY_X") != NULL);
        CHECK(strstr(dot, "[label=\"[Xx]\"]") != NULL);
        CHECK(strstr(dot, "$: TINY_X") != NULL);
        CHECK(strstr(dot, "[label=\"[0-9]\"]") != NULL);
        CHECK(strstr(dot, "[label=\"[^a-c]\"]") != NULL);
        /* the DFA is complete: every state has edges covering all 256 bytes,
         * merged per target; 7 states here give 3+2+3+3+2+4+2 = 19 edges */
        CHECK_EQ_INT(count(dot, "    d0_") - 7, 19);
        check_syntax(dot);
    }
    free(dot);
    rules_free(&rs);
}

/* Quotes and backslashes in charsets must be escaped in DOT labels. */
static void test_escaping(void)
{
    static const char text[] = "Q\tSEC\tLOW\t0.5\t-\t-\t-\t-\t\"\\\\\n";   /* regex: "\\ */
    RuleSet rs;
    char err[256];
    CHECK_EQ_INT(rules_parse(text, sizeof text - 1, "inline", &rs, err, sizeof err), 0);
    int ng = 0;
    char *dot = render(&rs, &ng);
    CHECK(dot != NULL);
    if (dot) {
        CHECK(strstr(dot, "[label=\"'\\\"'\"]") != NULL);      /* '"'  -> '\"'     */
        CHECK(strstr(dot, "[label=\"'\\\\x5c'\"]") != NULL);   /* '\x5c' -> '\\x5c' */
        check_syntax(dot);
    }
    free(dot);
    rules_free(&rs);
}

static void test_default_rules_render(void)
{
    RuleSet rs;
    char err[256];
    CHECK_EQ_INT(rules_load("rules/default.rules", &rs, err, sizeof err), 0);
    int ng = 0;
    char *dot = render(&rs, &ng);
    CHECK(dot != NULL);
    if (dot) {
        check_syntax(dot);
        char name[32];
        for (int g = 0; g < ng; g++) {
            snprintf(name, sizeof name, "cluster_dfa%d {", g);
            CHECK(strstr(dot, name) != NULL);
        }
    }
    free(dot);
    rules_free(&rs);
}

int main(void)
{
    test_tiny();
    test_escaping();
    test_default_rules_render();
    return TEST_DONE();
}
