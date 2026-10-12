#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_main.h"
#include "validators.h"

#define LUHN(s)  luhn_ok((s), strlen(s))
#define VERH(s)  verhoeff_ok((s), strlen(s))

static void test_luhn(void)
{
    /* spec §4.3 known value, then well-known test numbers of several brands/lengths */
    static const char *const good[] = {
        "4111111111111111", "5500005555555559", "5555555555554444", "378282246310005",
        "6011111111111117", "3530111333300000", "4012888888881881", "30569309025904",
        "4222222222222", "6200000000000005",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK(LUHN(good[i]));
    static const char *const bad[] = {
        "4111111111111112", "5500005555555558", "378282246310006", "6011111111111118",
        "4012888888881882", "1234567812345678", "0000000000000001",
        "411111111111",                       /* 12 digits: too short even if the sum works */
        "41111111111111111111",               /* 20 digits: too long */
        "4111 1111 1111 1111",                /* separators are the caller's job */
        "41111111111111a1", "", "4",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!LUHN(bad[i]));
    CHECK(LUHN("0000000000000"));             /* sum 0, 13 digits: valid by Luhn */
    CHECK(!luhn_ok("4111111111111111", 15));  /* length argument is honoured */

    /* property: of the ten possible last digits exactly one makes a 16-digit prefix valid,
     * and every single-digit substitution of a valid number is invalid */
    srand(7);
    for (int t = 0; t < 500; t++) {
        char d[17];
        for (int i = 0; i < 15; i++)
            d[i] = (char)('0' + rand() % 10);
        d[16] = '\0';
        int valid = 0;
        for (int c = 0; c < 10; c++) {
            d[15] = (char)('0' + c);
            valid += LUHN(d);
        }
        CHECK_EQ_INT(valid, 1);
        for (int c = 0; c < 10; c++) {
            d[15] = (char)('0' + c);
            if (!LUHN(d))
                continue;
            int p = rand() % 16;
            char old = d[p];
            d[p] = (char)('0' + (old - '0' + 1 + rand() % 9) % 10);
            CHECK(!LUHN(d));
            d[p] = old;
        }
    }
}

static void test_verhoeff(void)
{
    /* spec §4.3: Aadhaar-format examples; Wikipedia's check-digit examples */
    CHECK(VERH("234123412346"));
    CHECK(!VERH("234123412347"));
    CHECK(VERH("2363"));                      /* 236 + check digit 3 */
    CHECK(VERH("123451"));                    /* 12345 + 1 */
    CHECK(VERH("1428570"));                   /* 142857 + 0 */
    CHECK(!VERH("2362"));
    CHECK(!VERH("123450"));
    CHECK(!VERH("1428571"));
    CHECK(!VERH(""));
    CHECK(!VERH("23412341234a"));
    CHECK(!VERH("234 123 412 346"));
    CHECK(VERH("0"));                         /* a lone check digit 0 is the empty number's check */
    CHECK(!VERH("1"));

    /* properties of the Verhoeff code, independent of any remembered value: for every 11-digit
     * prefix exactly one check digit works; every single-digit error and every adjacent
     * transposition of a valid 12-digit number is detected (Verhoeff's design goal) */
    srand(11);
    for (int t = 0; t < 400; t++) {
        char d[13];
        for (int i = 0; i < 11; i++)
            d[i] = (char)('0' + rand() % 10);
        d[12] = '\0';
        int valid = 0, check = 0;
        for (int c = 0; c < 10; c++) {
            d[11] = (char)('0' + c);
            if (VERH(d)) {
                valid++;
                check = c;
            }
        }
        CHECK_EQ_INT(valid, 1);
        d[11] = (char)('0' + check);
        for (int p = 0; p < 12; p++) {
            char old = d[p];
            for (int v = 0; v < 10; v++) {
                if (v == old - '0')
                    continue;
                d[p] = (char)('0' + v);
                if (VERH(d)) {
                    CHECK(0);
                    break;
                }
            }
            d[p] = old;
        }
        for (int p = 0; p < 11; p++) {
            if (d[p] == d[p + 1])
                continue;
            char a = d[p];
            d[p] = d[p + 1];
            d[p + 1] = a;
            CHECK(!VERH(d));
            d[p + 1] = d[p];
            d[p] = a;
        }
    }
}

static void test_pan(void)
{
    static const char *const good[] = {
        "ABCPE1234F", "AAAAA0000A", "ZZZCZ9999Z", "ABCFE1234F", "ABCHE1234F", "ABCTE1234F",
        "ABCAE1234F", "ABCBE1234F", "ABCGE1234F", "ABCJE1234F", "ABCLE1234F",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK(pan_ok(good[i], strlen(good[i])));
    static const char *const bad[] = {
        "ABCXE1234F",                         /* 4th letter is not a holder type */
        "ABCDE1234F", "ABCEE1234F", "ABCZE1234F",
        "abcpe1234f", "ABCPE123F", "ABCPE12345F", "ABCPE12340", "1BCPE1234F", "ABCPE1234",
        "ABCP1E234F", "", "ABCPE1234F ",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!pan_ok(bad[i], strlen(bad[i])));
}

static void test_strip(void)
{
    char out[64];
    const char *s = "4111 1111-1111 1111";
    size_t n = strip_separators(s, strlen(s), out);
    CHECK_EQ_INT(n, 16);
    out[n] = '\0';
    CHECK_STR_EQ(out, "4111111111111111");
    CHECK_EQ_INT(strip_separators("", 0, out), 0);
    CHECK_EQ_INT(strip_separators(" - ", 3, out), 0);
    n = strip_separators("a b-c", 5, out);
    out[n] = '\0';
    CHECK_STR_EQ(out, "abc");
}

#define SPAN(v, s) validate_span((v), (s), strlen(s))

static void test_validate_span(void)
{
    /* regex-style spans: boundary characters and separators around the number */
    CHECK(SPAN(VAL_LUHN, " 4111-1111-1111-1111 "));
    CHECK(SPAN(VAL_LUHN, "=4111 1111 1111 1111,"));
    CHECK(SPAN(VAL_LUHN, "4111111111111111"));
    CHECK(!SPAN(VAL_LUHN, " 4111-1111-1111-1112 "));
    CHECK(!SPAN(VAL_LUHN, "1234 5678 1234 5678"));
    CHECK(SPAN(VAL_VERHOEFF, " 2341 2341 2346 "));
    CHECK(SPAN(VAL_VERHOEFF, "234123412346"));
    CHECK(SPAN(VAL_VERHOEFF, "(2341-2341-2346)"));
    CHECK(!SPAN(VAL_VERHOEFF, "2341 2341 2347"));
    CHECK(!SPAN(VAL_VERHOEFF, "234123412346 7"));             /* 13 digits */
    CHECK(!SPAN(VAL_VERHOEFF, "0000 0000 0000"));             /* first digit must be 2-9 */
    CHECK(!SPAN(VAL_VERHOEFF, "123412341239"));               /* valid check digit, first digit 1 */
    CHECK(SPAN(VAL_PAN, " ABCPE1234F "));
    CHECK(SPAN(VAL_PAN, "ABCPE1234F"));
    CHECK(!SPAN(VAL_PAN, " ABCXE1234F "));
    CHECK(SPAN(VAL_NONE, ""));
    CHECK(SPAN(VAL_NONE, "anything"));
    CHECK(!SPAN(VAL_LUHN, ""));
    CHECK(!SPAN(VAL_LUHN, "   "));
    char longspan[200];
    memset(longspan, '1', sizeof longspan - 1);
    longspan[sizeof longspan - 1] = '\0';
    CHECK(!SPAN(VAL_LUHN, longspan));
}

int main(void)
{
    test_luhn();
    test_verhoeff();
    test_pan();
    test_strip();
    test_validate_span();
    return TEST_DONE();
}
