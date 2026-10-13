#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "engine_posix.h"
#include "extract.h"
#include "mask.h"
#include "normalize.h"
#include "rules.h"

static RuleSet rs;
static Extractor *ext;
static EnginePosix *pe;

/* Masked form of `text` through the same steps as the pipeline; returns a malloc'd string
 * ("" copy of the input when nothing was masked) and the number of spans masked. */
static char *masked(const char *text, int *nmasked)
{
    char norm[4096];
    size_t n = normalize_line(text, strlen(text), norm);
    MatchResult m = { 0 };
    m.rule_mask = posix_match(pe, norm, n);
    extract_line(ext, norm, n, m.rule_mask, &m);
    StrBuf sb;
    sb_init(&sb);
    int k = mask_needed(&rs, m.valid_mask) ? mask_pii(ext, &rs, norm, n, m.valid_mask, &sb) : 0;
    if (nmasked)
        *nmasked = k;
    char *out = malloc(sb.len + 1);
    if (k) {
        memcpy(out, sb.data, sb.len);
        out[sb.len] = '\0';
    } else {
        out[0] = '\0';
    }
    sb_free(&sb);
    return out;
}

static void expect(const char *text, const char *want, int want_n)
{
    int k = -1;
    char *got = masked(text, &k);
    if (strcmp(got, want) != 0 || k != want_n)
        fprintf(stderr, "  input: %s\n  got:   %s (%d)\n  want:  %s (%d)\n", text, got, k, want, want_n);
    CHECK_STR_EQ(got, want);
    CHECK_EQ_INT(k, want_n);
    free(got);
}

static void test_formats(void)
{
    /* spec §7 formats */
    expect("payment captured card=4111 1111 1111 1111 amount=499.00",
           "payment captured card=XXXX\xE2\x80\xA6" "1111 amount=499.00", 1);
    expect("card 4012888888881881.", "card XXXX\xE2\x80\xA6" "1881.", 1);
    expect("uid 2341 2341 2346 verified", "uid XXXX XXXX 2346 verified", 1);
    expect("aadhaar=234123412346 stored", "aadhaar=XXXX XXXX 2346 stored", 1);
    expect("pan ABCPE1234F submitted", "pan XXXXX1234X submitted", 1);
    expect("GET /login?user=bob&password=hunter2 HTTP/1.1", "GET /login?user=bob&password=*** HTTP/1.1", 1);
    expect("GET /v1/data?api_key=sk_live_abc123&format=json", "GET /v1/data?api_key=***&format=json", 1);
    expect("signup completed for jane.doe@example.com via web", "signup completed for ***@example.com via web", 1);
    expect("call me on 9876543210 now", "call me on XXXXXX3210 now", 1);
    /* the normalizer turns a literal '+' into a space, so "+91" is not part of the number */
    expect("mobile: +91 9876543210.", "mobile:  91 XXXXXX3210.", 1);
}

static void test_not_pii(void)
{
    /* the number fails its checksum: not masked (nothing to hide), nothing reported */
    expect("payment declined card=4111 1111 1111 1112 amount=99.00", "", 0);
    expect("order id 523456789012 shipped", "", 0);
    expect("uid 234123412347 rejected", "", 0);
    expect("ts=20261011100000 ok", "", 0);
    expect("nothing sensitive here", "", 0);
    expect("password= is empty", "", 0);
}

static void test_several(void)
{
    /* two values, one valid and one not */
    expect("a 4111111111111111 b 4111111111111112 c", "a XXXX\xE2\x80\xA6" "1111 b 4111111111111112 c", 1);
    /* two different kinds on a line, in order */
    expect("x=jane@example.com&password=pw1&card 4111-1111-1111-1111",
           "x=***@example.com&password=***&card XXXX\xE2\x80\xA6" "1111", 3);
    /* adjacent numbers separated by one space are both found */
    expect("4111111111111111 5500005555555559", "XXXX\xE2\x80\xA6" "1111 XXXX\xE2\x80\xA6" "5559", 2);
    /* the value is masked after URL decoding (the text masked is the normalized line) */
    expect("GET /p?card=4111%201111%201111%201111&x=1", "GET /p?card=XXXX\xE2\x80\xA6" "1111&x=1", 1);
    expect("GET /p?password=a%26b", "GET /p?password=***&b", 1);   /* decoded & ends the value */
}

/* no original value survives in the output of any rule-matched PII line of the corpus-like text */
static void test_no_leak(void)
{
    static const char *const lines[] = {
        "card=4111 1111 1111 1111", "aadhaar 2341 2341 2346", "PAN ABCPE1234F", "tel 9876543210",
        "pwd=correcthorse", "mail bob.smith@example.org",
    };
    static const char *const secrets[] = {
        "4111 1111 1111 1111", "1111 1111 1111", "2341 2341", "ABCPE", "9876543", "correcthorse", "bob.smith",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        char *m = masked(lines[i], NULL);
        for (size_t k = 0; k < sizeof secrets / sizeof secrets[0]; k++)
            CHECK(strstr(m, secrets[k]) == NULL);
        free(m);
    }
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    ext = extractor_new(&rs, err, sizeof err);
    pe = posix_new(&rs, err, sizeof err);
    if (!ext || !pe) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    test_formats();
    test_not_pii();
    test_several();
    test_no_leak();
    posix_free(pe);
    extractor_free(ext);
    rules_free(&rs);
    return TEST_DONE();
}
