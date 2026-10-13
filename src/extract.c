#include "extract.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "validators.h"

struct Extractor {
    const RuleSet *rs;
    bool           has[RULES_MAX];      /* re[r] is compiled */
    regex_t        re[RULES_MAX];
};

static bool needs_spans(const Rule *r)
{
    return r->validator != VAL_NONE || r->extract != 0;
}

void extractor_free(Extractor *x)
{
    if (x == NULL)
        return;
    for (int i = 0; i < x->rs->count; i++)
        if (x->has[i])
            regfree(&x->re[i]);
    free(x);
}

Extractor *extractor_new(const RuleSet *rs, char *err, size_t err_len)
{
    Extractor *x = calloc(1, sizeof *x);
    if (x == NULL) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    x->rs = rs;
    for (int i = 0; i < rs->count; i++) {
        const Rule *r = &rs->rules[i];
        int rc = regcomp(&x->re[i], r->regex, REG_EXTENDED | (r->icase ? REG_ICASE : 0));
        if (rc != 0) {
            char msg[128];
            regerror(rc, &x->re[i], msg, sizeof msg);
            snprintf(err, err_len, "rule %s: regcomp failed: %s", r->id, msg);
            extractor_free(x);
            return NULL;
        }
        x->has[i] = true;
    }
    return x;
}

/* ---- spans ------------------------------------------------------------------ */

int extractor_spans(Extractor *x, int rule, const char *line, size_t len, Span *out, int max)
{
    if (rule < 0 || rule >= x->rs->count || !x->has[rule])
        return 0;
    int n = 0;
    size_t from = 0;
    while (n < max && from <= len) {
        /* REG_STARTEND: search line[from..len) without a NUL terminator. After the first
         * search REG_NOTBOL stops '^' from matching at the new window start. */
        regmatch_t m;
        m.rm_so = (regoff_t)from;
        m.rm_eo = (regoff_t)len;
        if (regexec(&x->re[rule], line, 1, &m, REG_STARTEND | (from > 0 ? REG_NOTBOL : 0)) != 0)
            break;
        out[n].so = (uint32_t)m.rm_so;
        out[n].eo = (uint32_t)m.rm_eo;
        n++;
        from = (size_t)m.rm_so + 1;
    }
    return n;
}

/* ---- IPv4 --------------------------------------------------------------------- */

bool parse_ipv4(const char *s, size_t n, uint32_t *ip)
{
    uint32_t v = 0;
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        size_t digits = 0;
        uint32_t o = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 4) {
            o = o * 10 + (uint32_t)(s[i] - '0');
            i++;
            digits++;
        }
        if (digits == 0 || digits > 3 || o > 255)
            return false;
        v = (v << 8) | o;
        if (part < 3) {
            if (i >= n || s[i] != '.')
                return false;
            i++;
        }
    }
    if (i != n)
        return false;
    *ip = v;
    return true;
}

/* The rules' regexes end in `from [0-9.]+`, so the address is the last
 * space-delimited token of the span. Scan tokens right to left and take the
 * first one that parses; a token like "1.2.3" (a truncated address that the
 * rule's [0-9.]+ would still match) is rejected by parse_ipv4. */
bool extract_ip(const char *line, Span span, uint32_t *ip)
{
    size_t end = span.eo;
    while (end > span.so) {
        while (end > span.so && line[end - 1] == ' ')
            end--;
        size_t start = end;
        while (start > span.so && line[start - 1] != ' ')
            start--;
        if (start == end)
            break;
        if (parse_ipv4(line + start, end - start, ip))
            return true;
        end = start;
    }
    return false;
}

/* ---- username ----------------------------------------------------------------- */

static void put_token(const char *s, size_t n, int stop_semicolon, char out[32])
{
    size_t k = 0;
    while (k < n && s[k] != ' ' && !(stop_semicolon && s[k] == ';') && k < 31) {
        out[k] = s[k];
        k++;
    }
    out[k] = '\0';
}

/* Find `needle` (case-sensitive unless `ci`) in line[from..to). Returns the offset of its first
 * byte or SIZE_MAX. */
static size_t find_str(const char *line, size_t from, size_t to, const char *needle, bool ci)
{
    size_t m = strlen(needle);
    for (size_t i = from; i + m <= to; i++) {
        size_t k = 0;
        while (k < m && (ci ? tolower((unsigned char)line[i + k]) == tolower((unsigned char)needle[k])
                            : line[i + k] == needle[k]))
            k++;
        if (k == m)
            return i;
    }
    return SIZE_MAX;
}

/* Username forms, tried in this order (first hit wins):
 *   1. "invalid user NAME"          inside the span  (sshd: Failed password for invalid user NAME from ...,
 *                                                     Invalid user NAME from ...)
 *   2. " for NAME"                  inside the span  (Failed/Accepted password for NAME from ...)
 *   3. "sudo: NAME :"               anywhere         (sudo: alice : 3 incorrect password attempts ...)
 *   4. " user=NAME" / start-of-line  anywhere        (pam_unix(...): authentication failure; ... user=root)
 * Form 4 needs the key to start a token ("ruser=" does not count) and is case-sensitive,
 * so sudo's "USER=root" (the target account) is never taken for the login name. */
void extract_user(const char *line, size_t len, Span span, char out[32])
{
    size_t at = find_str(line, span.so, span.eo, "invalid user ", true);
    if (at != SIZE_MAX) {
        put_token(line + at + 13, span.eo - (at + 13), 0, out);
        return;
    }
    at = find_str(line, span.so, span.eo, " for ", false);
    if (at != SIZE_MAX) {
        put_token(line + at + 5, span.eo - (at + 5), 0, out);
        return;
    }
    at = find_str(line, 0, len, "sudo: ", false);
    if (at != SIZE_MAX) {
        size_t s = at + 6, e = s;
        while (e < len && line[e] != ' ')
            e++;
        if (e + 2 < len && line[e + 1] == ':' && line[e + 2] == ' ') {
            put_token(line + s, e - s, 0, out);
            return;
        }
    }
    for (size_t from = 0; (at = find_str(line, from, len, "user=", false)) != SIZE_MAX; from = at + 1) {
        if (at == 0 || line[at - 1] == ' ' || line[at - 1] == ';') {
            put_token(line + at + 5, len - (at + 5), 1, out);
            return;
        }
    }
    out[0] = '\0';
}

/* ---- per-line driver ------------------------------------------------------------ */

void extract_line(Extractor *x, const char *line, size_t len, uint64_t rule_mask, MatchResult *out)
{
    out->rule_mask = rule_mask;
    out->valid_mask = 0;
    out->src_ip = 0;
    out->user[0] = '\0';
    bool got_ip = false, got_user = false;
    Span spans[MAX_SPANS];

    for (int r = 0; r < x->rs->count; r++) {
        if (!((rule_mask >> r) & 1))
            continue;
        const Rule *rule = &x->rs->rules[r];
        if (!needs_spans(rule)) {
            out->valid_mask |= (uint64_t)1 << r;
            continue;
        }
        int n = extractor_spans(x, r, line, len, spans, MAX_SPANS);

        /* §5.1: a validator must pass on at least one candidate span. */
        bool valid = rule->validator == VAL_NONE;
        for (int i = 0; i < n && !valid; i++)
            valid = validate_span(rule->validator, line + spans[i].so, spans[i].eo - spans[i].so);
        if (valid)
            out->valid_mask |= (uint64_t)1 << r;

        /* Entities come from the first span of the first matched rule that asks for them. */
        if (n > 0 && (rule->extract & EXTRACT_IP) && !got_ip)
            got_ip = extract_ip(line, spans[0], &out->src_ip);
        if (n > 0 && (rule->extract & EXTRACT_USER) && !got_user) {
            extract_user(line, len, spans[0], out->user);
            got_user = out->user[0] != '\0';
        }
    }
}

/* ---- subsumption (§5.3) ------------------------------------------------------------ */

uint64_t extract_subsumed(Extractor *x, const char *line, size_t len, uint64_t kept)
{
    Span first[RULES_MAX];
    bool have[RULES_MAX] = { false };
    int per_cat[32] = { 0 };
    uint64_t drop = 0;

    /* only categories with two or more kept rules need spans */
    for (int r = 0; r < x->rs->count; r++)
        if ((kept >> r) & 1)
            per_cat[__builtin_ctz((unsigned)x->rs->rules[r].category)]++;
    for (int r = 0; r < x->rs->count; r++) {
        if (!((kept >> r) & 1) || per_cat[__builtin_ctz((unsigned)x->rs->rules[r].category)] < 2)
            continue;
        have[r] = extractor_spans(x, r, line, len, &first[r], 1) == 1;
    }
    for (int i = 0; i < x->rs->count; i++) {
        for (int j = 0; j < x->rs->count; j++) {
            if (i == j || !have[i] || !have[j] || ((drop >> i) & 1) ||
                x->rs->rules[i].category != x->rs->rules[j].category)
                continue;
            bool inside = first[i].so <= first[j].so && first[j].eo <= first[i].eo;
            bool equal = first[i].so == first[j].so && first[i].eo == first[j].eo;
            if (inside && (!equal || i < j))
                drop |= (uint64_t)1 << j;          /* j is explained by the outer rule i */
        }
    }
    return drop;
}

/* ---- context keywords ------------------------------------------------------------- */

uint64_t context_mask(const RuleSet *rs, const char *line, size_t len, uint64_t rule_mask)
{
    uint64_t m = 0;
    for (int r = 0; r < rs->count; r++) {
        if (!((rule_mask >> r) & 1))
            continue;
        const Rule *rule = &rs->rules[r];
        for (int k = 0; k < rule->n_context; k++) {
            if (find_str(line, 0, len, rule->context[k], true) != SIZE_MAX) {
                m |= (uint64_t)1 << r;
                break;
            }
        }
    }
    return m;
}
