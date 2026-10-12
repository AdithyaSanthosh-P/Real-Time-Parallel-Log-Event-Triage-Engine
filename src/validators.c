#include "validators.h"

#include <ctype.h>
#include <string.h>

/* Verhoeff tables, verbatim from spec §4.3: V_D is the multiplication table of
 * the dihedral group D5, V_P the position-dependent permutation (period 8). */
static const unsigned char V_D[10][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,2,3,4,0,6,7,8,9,5},{2,3,4,0,1,7,8,9,5,6},
    {3,4,0,1,2,8,9,5,6,7},{4,0,1,2,3,9,5,6,7,8},{5,9,8,7,6,0,4,3,2,1},
    {6,5,9,8,7,1,0,4,3,2},{7,6,5,9,8,2,1,0,4,3},{8,7,6,5,9,3,2,1,0,4},
    {9,8,7,6,5,4,3,2,1,0}};
static const unsigned char V_P[8][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,5,7,6,2,8,3,0,9,4},{5,8,0,3,7,9,6,1,4,2},
    {8,9,1,6,0,4,3,5,2,7},{9,4,5,3,1,2,6,8,7,0},{4,2,8,6,5,7,3,9,0,1},
    {2,7,9,3,8,0,6,4,1,5},{7,0,4,6,9,1,3,2,5,8}};

static bool all_digits(const char *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (d[i] < '0' || d[i] > '9')
            return false;
    return true;
}

/* Verhoeff: walk the digits right to left; c <- D[c][P[i mod 8][digit]].
 * The number is valid when the running value ends at 0. */
bool verhoeff_ok(const char *d, size_t n)
{
    if (n == 0 || !all_digits(d, n))
        return false;
    int c = 0;
    for (size_t i = 0; i < n; i++)
        c = V_D[c][V_P[i % 8][d[n - 1 - i] - '0']];
    return c == 0;
}

/* Luhn: from the right, double every second digit (subtracting 9 when the
 * product exceeds 9) and sum; valid when the sum is a multiple of 10. */
bool luhn_ok(const char *d, size_t n)
{
    if (n < 13 || n > 19 || !all_digits(d, n))
        return false;
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        int x = d[n - 1 - i] - '0';
        if (i & 1) {
            x *= 2;
            if (x > 9)
                x -= 9;
        }
        sum += x;
    }
    return sum % 10 == 0;
}

bool pan_ok(const char *s, size_t n)
{
    if (n != 10)
        return false;
    for (size_t i = 0; i < 10; i++) {
        bool digit = i >= 5 && i <= 8;
        if (digit ? (s[i] < '0' || s[i] > '9') : (s[i] < 'A' || s[i] > 'Z'))
            return false;
    }
    return strchr("ABCFGHJLPT", s[3]) != NULL;
}

size_t strip_separators(const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
        if (in[i] != ' ' && in[i] != '-')
            out[o++] = in[i];
    return o;
}

/* Drop the regex's boundary characters: everything before the first and after
 * the last character that can belong to the number (alphanumerics). */
static void trim_boundary(const char **s, size_t *n)
{
    while (*n > 0 && !isalnum((unsigned char)(*s)[0])) {
        (*s)++;
        (*n)--;
    }
    while (*n > 0 && !isalnum((unsigned char)(*s)[*n - 1]))
        (*n)--;
}

bool validate_span(Validator v, const char *span, size_t n)
{
    if (v == VAL_NONE)
        return true;
    trim_boundary(&span, &n);
    char buf[64];
    if (n >= sizeof buf)
        return false;                       /* far longer than any number we validate */
    size_t m = strip_separators(span, n, buf);
    switch (v) {
    case VAL_LUHN:
        return luhn_ok(buf, m);
    case VAL_VERHOEFF:
        return m == 12 && buf[0] >= '2' && verhoeff_ok(buf, m);
    case VAL_PAN:
        return pan_ok(buf, m);
    default:
        return true;
    }
}
