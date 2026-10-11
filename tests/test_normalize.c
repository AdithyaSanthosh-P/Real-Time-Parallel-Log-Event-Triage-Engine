#include <stdio.h>
#include <string.h>

#include "test_main.h"
#include "normalize.h"

/* Normalize `in` (length n, may contain NULs) and compare with `want` (length wn). */
static void expect_n(const char *in, size_t n, const char *want, size_t wn, int line)
{
    char out[512];
    size_t got = normalize_line(in, n, out);
    tm_checks++;
    if (got > n || got != wn || memcmp(out, want, wn) != 0) {
        tm_failures++;
        fprintf(stderr, "%s:%d: normalize mismatch: got %zu bytes \"%.*s\", want %zu bytes \"%.*s\"\n",
                __FILE__, line, got, (int)got, out, wn, (int)wn, want);
    }
}

#define EXPECT(in, want) expect_n((in), strlen(in), (want), strlen(want), __LINE__)

static void test_percent_decoding(void)
{
    EXPECT("id=1%27 OR 1=1", "id=1' OR 1=1");
    EXPECT("%2e%2e%2f", "../");
    EXPECT("%2E%2e%2F", "../");                 /* either case */
    EXPECT("..%2f..%2fetc%2fpasswd", "../../etc/passwd");
    EXPECT("%3Cscript%3E", "<script>");
    EXPECT("%41%42%43", "ABC");
    EXPECT("a%20b", "a b");
    EXPECT("%FF%80", "\xff\x80");               /* high bytes are kept */
}

static void test_invalid_escapes(void)
{
    EXPECT("%zz", "%zz");
    EXPECT("%", "%");
    EXPECT("100%", "100%");
    EXPECT("50%4", "50%4");                     /* truncated at the end */
    EXPECT("%4", "%4");
    EXPECT("%4z", "%4z");                       /* second digit invalid */
    EXPECT("%z4", "%z4");                       /* first digit invalid */
    EXPECT("%%41", "%A");                       /* '%' then a valid escape */
    EXPECT("a%zzb%41", "a%zzbA");
    EXPECT("%G1", "%G1");
}

static void test_plus_and_single_pass(void)
{
    EXPECT("a+b", "a b");
    EXPECT("+++", "   ");
    EXPECT("union+select", "union select");
    EXPECT("%2b", "+");                         /* decoded '+' is not turned into a space */
    EXPECT("1%2B1", "1+1");
    EXPECT("%2541", "%41");                     /* no double decoding */
    EXPECT("%252e", "%2e");
}

static void test_control_characters(void)
{
    EXPECT("a\tb", "a\tb");                     /* tab survives */
    EXPECT("a\x01" "b\x1f" "c", "abc");
    EXPECT("a\rb\nc", "abc");
    EXPECT("a\x7f" "b", "ab");
    EXPECT("%00", "");                          /* decoded controls are dropped too */
    EXPECT("un%00ion", "union");
    EXPECT("a%0ab%0Dc", "abc");
    EXPECT("a%09b", "a\tb");                    /* decoded tab survives */
    EXPECT("\x1b[31mred\x1b[0m", "[31mred[0m"); /* ANSI escape char removed */
    expect_n("a\0b", 3, "ab", 2, __LINE__);     /* embedded NUL */
    EXPECT("\x01\x02\x03", "");
}

static void test_edges_and_length(void)
{
    EXPECT("", "");
    EXPECT("plain text, nothing to do", "plain text, nothing to do");
    EXPECT("No Lowercasing HERE", "No Lowercasing HERE");

    /* Output is never longer than input: all-escape input shrinks 3:1. */
    const char *in = "%61%62%63%64%65%66";
    char out[64];
    size_t got = normalize_line(in, strlen(in), out);
    CHECK_EQ_INT(got, 6);
    CHECK(memcmp(out, "abcdef", 6) == 0);
}

static void test_in_place(void)
{
    char buf[] = "GET /a%2e%2e%2fb+c%00d%zz HTTP/1.1";
    size_t n = strlen(buf);
    size_t got = normalize_line(buf, n, buf);
    CHECK_EQ_INT(got, strlen("GET /a../b cd%zz HTTP/1.1"));
    CHECK(memcmp(buf, "GET /a../b cd%zz HTTP/1.1", got) == 0);
}

int main(void)
{
    test_percent_decoding();
    test_invalid_escapes();
    test_plus_and_single_pass();
    test_control_characters();
    test_edges_and_length();
    test_in_place();
    return TEST_DONE();
}
