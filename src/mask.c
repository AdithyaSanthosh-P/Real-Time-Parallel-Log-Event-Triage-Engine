#include "mask.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "validators.h"

int mask_needed(const RuleSet *rs, uint64_t kept)
{
    for (int r = 0; r < rs->count; r++)
        if (((kept >> r) & 1) && rs->rules[r].category == CAT_PII)
            return 1;
    return 0;
}

typedef struct {
    uint32_t so, eo;
    int      rule;
} MSpan;

/* Drop the regex's boundary characters (non-alphanumerics at both ends). */
static void trim(const char *line, uint32_t *so, uint32_t *eo)
{
    while (*so < *eo && !isalnum((unsigned char)line[*so]))
        (*so)++;
    while (*eo > *so && !isalnum((unsigned char)line[*eo - 1]))
        (*eo)--;
}

static size_t digits_of(const char *s, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k < cap; i++)
        if (s[i] >= '0' && s[i] <= '9')
            out[k++] = s[i];
    return k;
}

/* The replacement text for one span. */
static void put_masked(StrBuf *out, const Rule *rule, const char *s, size_t n)
{
    char d[64];
    size_t nd = digits_of(s, n, d, sizeof d);
    switch (rule->validator) {
    case VAL_LUHN:
        sb_puts(out, "XXXX\xE2\x80\xA6");
        sb_append(out, d + (nd >= 4 ? nd - 4 : 0), nd >= 4 ? 4 : nd);
        return;
    case VAL_VERHOEFF:
        sb_puts(out, "XXXX XXXX ");
        sb_append(out, d + (nd >= 4 ? nd - 4 : 0), nd >= 4 ? 4 : nd);
        return;
    case VAL_PAN:
        sb_puts(out, "XXXXX");
        sb_append(out, s + 5, 4);                  /* pan_ok guarantees 10 chars, digits at 5..8 */
        sb_putc(out, 'X');
        return;
    default:
        break;
    }
    const char *eq = memchr(s, '=', n);
    const char *at = memchr(s, '@', n);
    if (eq) {
        sb_append(out, s, (size_t)(eq - s) + 1);   /* keep "key=" */
        sb_puts(out, "***");
    } else if (at) {
        sb_puts(out, "***");
        sb_append(out, at, n - (size_t)(at - s));
    } else if (nd >= 10) {
        sb_puts(out, "XXXXXX");
        sb_append(out, d + nd - 4, 4);
    } else {
        sb_puts(out, "***");
    }
}

static int cmp_span(const void *a, const void *b)
{
    const MSpan *x = a, *y = b;
    if (x->so != y->so)
        return x->so < y->so ? -1 : 1;
    return x->eo < y->eo ? 1 : x->eo > y->eo ? -1 : 0;
}

int mask_pii(Extractor *x, const RuleSet *rs, const char *line, size_t len, uint64_t kept, StrBuf *out)
{
    MSpan all[RULES_MAX * MAX_SPANS];
    int n = 0;
    for (int r = 0; r < rs->count; r++) {
        const Rule *rule = &rs->rules[r];
        if (!((kept >> r) & 1) || rule->category != CAT_PII)
            continue;
        Span sp[MAX_SPANS];
        int k = extractor_spans(x, r, line, len, sp, MAX_SPANS);
        for (int i = 0; i < k; i++) {
            if (!validate_span(rule->validator, line + sp[i].so, sp[i].eo - sp[i].so))
                continue;                          /* e.g. a card-shaped number that fails Luhn */
            uint32_t so = sp[i].so, eo = sp[i].eo;
            trim(line, &so, &eo);
            if (eo > so)
                all[n++] = (MSpan){ so, eo, r };
        }
    }
    sb_clear(out);
    qsort(all, (size_t)n, sizeof all[0], cmp_span);

    size_t pos = 0;
    int masked = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].so < pos)
            continue;                              /* inside a span already masked (outer wins) */
        sb_append(out, line + pos, all[i].so - pos);
        put_masked(out, &rs->rules[all[i].rule], line + all[i].so, all[i].eo - all[i].so);
        pos = all[i].eo;
        masked++;
    }
    sb_append(out, line + pos, len - pos);
    return masked;
}
