#include <glob.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "annot.h"
#include "batch.h"
#include "engine_posix.h"
#include "normalize.h"
#include "reader.h"
#include "rules.h"

static RuleSet rs;

/* On synthetic_small.log every annotated line must get exactly its annotated
 * rule mask, and every one of the 26 rules must have a positive line. */
static void test_synthetic_small(void)
{
    char err[256];
    EnginePosix *e = posix_new(&rs, err, sizeof err);
    CHECK(e != NULL);
    if (!e) {
        fprintf(stderr, "posix_new: %s\n", err);
        return;
    }

    AnnotCorpus c;
    int rc = annot_load("tests/corpus/synthetic_small.log", &rs, &c);
    CHECK_EQ_INT(rc, 0);
    if (rc != 0) {
        posix_free(e);
        return;
    }
    CHECK(c.n >= 80);                         /* one entry per annotation */

    uint64_t all = (rs.count == 64) ? ~(uint64_t)0 : (((uint64_t)1 << rs.count) - 1);
    CHECK(c.covered == all);                 /* a positive line for every rule */

    size_t negatives = 0;
    char *norm = malloc(65536);
    for (size_t i = 0; i < c.n; i++) {
        const AnnotEntry *a = &c.e[i];
        size_t nlen = normalize_line(a->text, a->len, norm);
        uint64_t got = posix_match(e, norm, nlen);
        negatives += a->expect == 0;
        tm_checks++;
        if (got != a->expect) {
            tm_failures++;
            fprintf(stderr, "synthetic_small.log:%zu: mask %#llx, expected %#llx: %s\n",
                    a->file_line, (unsigned long long)got, (unsigned long long)a->expect, a->text);
        }
    }
    CHECK(negatives >= 20);
    free(norm);
    annot_free(&c);
    posix_free(e);
}

/* The engine's REG_STARTEND matching must agree with plain regexec() on a
 * NUL-terminated copy, for every line of every corpus file. */
static void test_startend_vs_terminated(void)
{
    char err[256];
    EnginePosix *e = posix_new(&rs, err, sizeof err);
    CHECK(e != NULL);
    if (!e)
        return;
    regex_t re[RULES_MAX];
    for (int i = 0; i < rs.count; i++)
        CHECK_EQ_INT(regcomp(&re[i], rs.rules[i].regex,
                             REG_EXTENDED | REG_NOSUB | (rs.rules[i].icase ? REG_ICASE : 0)), 0);

    glob_t g;
    CHECK_EQ_INT(glob("tests/corpus/*.log", 0, NULL, &g), 0);
    Batch b;
    CHECK_EQ_INT(batch_init(&b), 0);
    size_t lines = 0, with_match = 0;
    for (size_t f = 0; f < g.gl_pathc; f++) {
        Reader *r = reader_open_path(g.gl_pathv[f], 0);
        CHECK(r != NULL);
        if (!r)
            continue;
        while (reader_next_batch(r, &b, 256) > 0) {
            for (uint32_t i = 0; i < b.count; i++) {
                const char *s = batch_norm(&b, i);
                size_t len = batch_norm_len(&b, i);
                char *z = malloc(len + 1);
                memcpy(z, s, len);
                z[len] = '\0';
                uint64_t want = 0;
                for (int k = 0; k < rs.count; k++)
                    if (regexec(&re[k], z, 0, NULL, 0) == 0)
                        want |= (uint64_t)1 << k;
                free(z);
                uint64_t got = posix_match(e, s, len);
                lines++;
                with_match += want != 0;
                tm_checks++;
                if (got != want) {
                    tm_failures++;
                    fprintf(stderr, "%s: seq %llu: STARTEND mask %#llx != terminated mask %#llx\n",
                            g.gl_pathv[f], (unsigned long long)(b.seq_start + i),
                            (unsigned long long)got, (unsigned long long)want);
                }
            }
        }
        reader_close(r);
    }
    CHECK(lines > 10000);
    CHECK(with_match > 100);
    batch_free(&b);
    globfree(&g);
    for (int i = 0; i < rs.count; i++)
        regfree(&re[i]);
    posix_free(e);
}

static void test_edge_cases(void)
{
    char err[256];
    EnginePosix *e = posix_new(&rs, err, sizeof err);
    CHECK(e != NULL);
    if (!e)
        return;
    int stack = rules_find(&rs, "JAVA_STACK");
    int fail_pw = rules_find(&rs, "SSH_FAIL_PW");
    int email = rules_find(&rs, "PII_EMAIL");

    /* empty line: nothing matches */
    CHECK_EQ_INT(posix_match(e, "", 0), 0);

    /* only the first `len` bytes are examined (no NUL terminator needed) */
    const char *buf = "Failed password for root from 1.2.3.4 trailing";
    size_t cut = strlen("Failed password for root from 1.");
    CHECK(posix_match(e, buf, cut) & ((uint64_t)1 << fail_pw));
    CHECK(!(posix_match(e, buf, strlen("Failed password for root fro")) & ((uint64_t)1 << fail_pw)));

    /* ^ binds to the start of the given range */
    CHECK(posix_match(e, "\tat a.b(c)", 10) & ((uint64_t)1 << stack));
    CHECK(!(posix_match(e, "x\tat a.b(c)", 11) & ((uint64_t)1 << stack)));

    /* bytes after `len` are ignored even when they would match */
    const char two[] = "no match here a@b.com";
    CHECK(!(posix_match(e, two, 13) & ((uint64_t)1 << email)));
    CHECK(posix_match(e, two, sizeof two - 1) & ((uint64_t)1 << email));

    /* non-ASCII bytes must not crash or match by accident */
    CHECK_EQ_INT(posix_match(e, "\xff\xfe\x80 caf\xc3\xa9", 10), 0);

    posix_free(e);
}

static void test_independent_instances(void)
{
    char err[256];
    EnginePosix *a = posix_new(&rs, err, sizeof err);
    EnginePosix *b = posix_new(&rs, err, sizeof err);
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        const char *line = "Oct 11 10:02:11 srv1 sshd[2210]: Failed password for root from 203.0.113.5";
        CHECK(posix_match(a, line, strlen(line)) == posix_match(b, line, strlen(line)));
        CHECK(posix_match(a, line, strlen(line)) != 0);
    }
    posix_free(a);
    posix_free(b);
    posix_free(NULL);                                  /* tolerated */
}

static void test_bad_regex(void)
{
    static const char text[] = "BAD\tERR\tLOW\t0.5\t-\t-\t-\t-\ta(b\n";
    RuleSet bad;
    char err[256];
    CHECK_EQ_INT(rules_parse(text, sizeof text - 1, "inline", &bad, err, sizeof err), 0);
    EnginePosix *e = posix_new(&bad, err, sizeof err);
    CHECK(e == NULL);
    CHECK(strstr(err, "BAD") != NULL);
    rules_free(&bad);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load rules: %s\n", err);
        return 1;
    }
    test_synthetic_small();
    test_startend_vs_terminated();
    test_edge_cases();
    test_independent_instances();
    test_bad_regex();
    rules_free(&rs);
    return TEST_DONE();
}
