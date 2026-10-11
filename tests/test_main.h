#ifndef TEST_MAIN_H
#define TEST_MAIN_H

/* Tiny test runner: no framework. Each tests/test_*.c is its own executable;
 * it calls CHECK()/CHECK_EQ_INT()/CHECK_STR_EQ() and returns TEST_DONE()
 * from main(). A failed check prints file:line and the test exits non-zero
 * at the end (checks keep running so one run reports every failure). */

#include <stdio.h>
#include <string.h>

static int tm_checks;
static int tm_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        tm_checks++;                                                         \
        if (!(cond)) {                                                       \
            tm_failures++;                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                     \
                    __FILE__, __LINE__, #cond);                              \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        long long tm_a = (long long)(actual);                                \
        long long tm_e = (long long)(expected);                              \
        tm_checks++;                                                         \
        if (tm_a != tm_e) {                                                  \
            tm_failures++;                                                   \
            fprintf(stderr, "%s:%d: CHECK_EQ_INT failed: %s == %lld, "       \
                    "expected %lld\n", __FILE__, __LINE__, #actual,          \
                    tm_a, tm_e);                                             \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                       \
    do {                                                                     \
        const char *tm_a = (actual);                                         \
        const char *tm_e = (expected);                                       \
        tm_checks++;                                                         \
        if (tm_a == NULL || strcmp(tm_a, tm_e) != 0) {                       \
            tm_failures++;                                                   \
            fprintf(stderr, "%s:%d: CHECK_STR_EQ failed: %s == \"%s\", "     \
                    "expected \"%s\"\n", __FILE__, __LINE__, #actual,        \
                    tm_a ? tm_a : "(null)", tm_e);                           \
        }                                                                    \
    } while (0)

/* Print a summary line and evaluate to the process exit status. */
#define TEST_DONE()                                                          \
    (fprintf(stderr, "%s: %d checks, %d failed\n", __FILE__, tm_checks,      \
             tm_failures),                                                   \
     tm_failures ? 1 : 0)

#endif /* TEST_MAIN_H */
