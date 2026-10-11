#include <stdio.h>
#include <string.h>

#include "test_main.h"
#include "timestamp.h"

/* 2026-10-11 10:14:22 UTC */
#define T_REF 1791713662LL

static void ok(const char *line, int year, long long want, int src_line)
{
    int64_t got = 0;
    int parsed = ts_parse(line, strlen(line), year, &got);
    tm_checks++;
    if (!parsed || got != want) {
        tm_failures++;
        fprintf(stderr, "%s:%d: ts_parse(\"%s\") = %s %lld, want %lld\n", __FILE__, src_line,
                line, parsed ? "ok" : "fail", (long long)got, want);
    }
}

static void no(const char *line, int year, int src_line)
{
    int64_t got = 0;
    int parsed = ts_parse(line, strlen(line), year, &got);
    tm_checks++;
    if (parsed) {
        tm_failures++;
        fprintf(stderr, "%s:%d: ts_parse(\"%s\") unexpectedly parsed to %lld\n", __FILE__,
                src_line, line, (long long)got);
    }
}

#define OK(line, year, want) ok((line), (year), (want), __LINE__)
#define NO(line, year)       no((line), (year), __LINE__)

int main(void)
{
    /* syslog */
    OK("Oct 11 10:14:22 srv1 sshd[1]: hello", 2026, T_REF);
    OK("Oct  1 05:06:07 srv1 sshd[1]: padded day", 2026, 1790831167LL);
    OK("Feb 29 23:59:59 host x", 2024, 1709251199LL);
    OK("[Oct 11 10:14:22] bracketed", 2026, T_REF);
    OK("Oct 11 10:14:22", 2026, T_REF);                   /* line ends right after */
    NO("Feb 29 23:59:59 host x", 2026);                   /* not a leap year */
    NO("Foo 11 10:14:22 x", 2026);
    NO("Oct 32 10:14:22 x", 2026);
    NO("Oct 0 10:14:22 x", 2026);
    NO("Oct 11 25:00:00 x", 2026);
    NO("Oct 11 10:61:00 x", 2026);
    NO("Oct 11 10:14 x", 2026);
    NO("Oct11 10:14:22 x", 2026);
    NO("oct 11 10:14:22 x", 2026);                        /* month names are capitalized */

    /* ISO-8601 */
    OK("2026-10-11T10:14:22 x", 0, T_REF);
    OK("2026-10-11 10:14:22 x", 0, T_REF);
    OK("2026-10-11T10:14:22Z x", 0, T_REF);
    OK("2026-10-11 10:14:22,123 INFO x", 0, T_REF);       /* log4j fraction */
    OK("2026-10-11T10:14:22.5Z", 0, T_REF);
    OK("2026-10-11T10:14:22+05:30 x", 0, T_REF - 19800);
    OK("2026-10-11T10:14:22+0530 x", 0, T_REF - 19800);
    OK("2026-10-11T10:14:22+05 x", 0, T_REF - 18000);
    OK("2026-10-11T10:14:22-05:00 x", 0, T_REF + 18000);
    OK("2026-10-11T10:14:22.250-05:00 x", 0, T_REF + 18000);
    OK("[2026-10-11 10:14:22] bracketed", 0, T_REF);
    OK("2026-10-11 10:14:22 +0530 detached zone is ignored", 0, T_REF);
    OK("2026-10-11 10:14:22-nginx", 0, T_REF);            /* '-' not followed by a zone */
    NO("2026-13-01 10:14:22 x", 0);
    NO("2026-10-32 10:14:22 x", 0);
    NO("2026-02-29 10:14:22 x", 0);
    NO("2026-10-11 24:00:00 x", 0);
    NO("2026-10-11T10:14 x", 0);
    NO("2026-10-11 x", 0);
    NO("2026/10/11 10:14:22 x", 0);

    /* not at the start of the line, or not a timestamp at all */
    NO("", 2026);
    NO("[", 2026);
    NO("198.51.100.23 - - [11/Oct/2026:10:07:00 +0530] \"GET / HTTP/1.1\" 200 1", 2026);
    NO("INFO Oct 11 10:14:22 later in the line", 2026);
    NO("   Oct 11 10:14:22 leading spaces", 2026);
    NO("12345", 2026);

    CHECK_EQ_INT(ts_year_of(T_REF), 2026);
    CHECK_EQ_INT(ts_year_of(1709251199LL), 2024);
    CHECK_EQ_INT(ts_year_of(0), 1970);
    return TEST_DONE();
}
