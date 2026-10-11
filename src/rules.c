#include "rules.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCOLS 9

/* Everything a parse error needs to report "file:line: message". */
typedef struct {
    const char *name;
    int         line;
    char       *err;
    size_t      err_len;
} Ctx;

static void fail(const Ctx *c, const char *fmt, ...)
{
    int n = snprintf(c->err, c->err_len, "%s:%d: ", c->name, c->line);
    if (n < 0 || (size_t)n >= c->err_len)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err + n, c->err_len - (size_t)n, fmt, ap);
    va_end(ap);
}

/* ---- column parsers: each returns 0 on success, -1 after calling fail() ---- */

static int parse_id(const Ctx *c, const char *s, Rule *r)
{
    size_t n = strlen(s);
    if (n == 0 || n >= RULE_ID_MAX) {
        fail(c, "rule id must be 1..%d characters", RULE_ID_MAX - 1);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_')) {
            fail(c, "bad rule id '%s' (use A-Z, 0-9, '_')", s);
            return -1;
        }
    }
    memcpy(r->id, s, n + 1);
    return 0;
}

static int parse_category(const Ctx *c, const char *s, Rule *r)
{
    static const struct { const char *name; Category cat; } tab[] = {
        { "AUTH", CAT_AUTH }, { "SEC", CAT_SEC }, { "ERR", CAT_ERR },
        { "PERF", CAT_PERF }, { "PII", CAT_PII },
    };
    size_t n = strlen(s);
    r->no_label = n > 0 && s[n - 1] == '*';
    if (r->no_label)
        n--;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strlen(tab[i].name) == n && strncmp(s, tab[i].name, n) == 0) {
            r->category = tab[i].cat;
            return 0;
        }
    }
    fail(c, "unknown category '%s' (expected AUTH, SEC, ERR, PERF or PII)", s);
    return -1;
}

static int parse_severity(const Ctx *c, const char *s, Rule *r)
{
    static const struct { const char *name; Severity sev; } tab[] = {
        { "INFO", SEV_INFO }, { "LOW", SEV_LOW }, { "MEDIUM", SEV_MEDIUM },
        { "HIGH", SEV_HIGH }, { "CRITICAL", SEV_CRITICAL },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strcmp(s, tab[i].name) == 0) {
            r->severity = tab[i].sev;
            return 0;
        }
    }
    fail(c, "unknown severity '%s' (expected INFO, LOW, MEDIUM, HIGH or CRITICAL)", s);
    return -1;
}

static int parse_base(const Ctx *c, const char *s, Rule *r)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (*s == '\0' || *end != '\0' || !(v > 0.0 && v <= 1.0)) {
        fail(c, "bad base '%s' (expected a number in (0, 1])", s);
        return -1;
    }
    r->base = (float)v;
    return 0;
}

static int parse_flags(const Ctx *c, const char *s, Rule *r)
{
    r->icase = false;
    if (strcmp(s, "-") == 0)
        return 0;
    for (const char *p = s; *p; p++) {
        if (*p != 'i') {
            fail(c, "unknown flag '%c' in '%s' (only 'i', or '-' for none)", *p, s);
            return -1;
        }
        r->icase = true;
    }
    if (*s == '\0') {
        fail(c, "empty flags column (use '-' for none)");
        return -1;
    }
    return 0;
}

static int parse_validator(const Ctx *c, const char *s, Rule *r)
{
    if (strcmp(s, "-") == 0)            r->validator = VAL_NONE;
    else if (strcmp(s, "luhn") == 0)     r->validator = VAL_LUHN;
    else if (strcmp(s, "verhoeff") == 0) r->validator = VAL_VERHOEFF;
    else if (strcmp(s, "pan") == 0)      r->validator = VAL_PAN;
    else {
        fail(c, "unknown validator '%s' (expected luhn, verhoeff, pan or '-')", s);
        return -1;
    }
    return 0;
}

/* Walk a comma-separated list in place, calling cb(item, arg) per item.
 * "-" alone means an empty list. Empty items ("a,,b") are errors. */
typedef int (*ItemFn)(const Ctx *c, const char *item, void *arg);

static int for_each_item(const Ctx *c, const char *what, char *s, ItemFn cb, void *arg)
{
    if (strcmp(s, "-") == 0)
        return 0;
    while (1) {
        char *comma = strchr(s, ',');
        if (comma)
            *comma = '\0';
        if (*s == '\0') {
            fail(c, "empty item in %s list", what);
            return -1;
        }
        if (cb(c, s, arg) != 0)
            return -1;
        if (!comma)
            return 0;
        s = comma + 1;
    }
}

static int extract_item(const Ctx *c, const char *item, void *arg)
{
    unsigned *bits = arg;
    if (strcmp(item, "ip") == 0)        *bits |= EXTRACT_IP;
    else if (strcmp(item, "user") == 0) *bits |= EXTRACT_USER;
    else {
        fail(c, "unknown extract item '%s' (expected ip or user)", item);
        return -1;
    }
    return 0;
}

static int context_item(const Ctx *c, const char *item, void *arg)
{
    Rule *r = arg;
    if (r->n_context >= RULE_CTX_MAX) {
        fail(c, "more than %d context keywords", RULE_CTX_MAX);
        return -1;
    }
    if (strlen(item) >= RULE_CTX_LEN) {
        fail(c, "context keyword '%s' longer than %d characters", item, RULE_CTX_LEN - 1);
        return -1;
    }
    strcpy(r->context[r->n_context++], item);
    return 0;
}

/* anchored_start: first char is '^'. anchored_end: last char is a '$' that is
 * not escaped, i.e. preceded by an even number of backslashes (§3.1). */
static void detect_anchors(Rule *r)
{
    size_t n = strlen(r->regex);
    r->anchored_start = n > 0 && r->regex[0] == '^';
    r->anchored_end = false;
    if (n > 0 && r->regex[n - 1] == '$') {
        size_t slashes = 0;
        while (slashes < n - 1 && r->regex[n - 2 - slashes] == '\\')
            slashes++;
        r->anchored_end = (slashes % 2) == 0;
    }
}

/* Split `line` in place into NCOLS columns. The first NCOLS-1 columns end at
 * a tab; the last one is the remainder. Returns the number of columns found. */
static int split_columns(char *line, char *col[NCOLS])
{
    int n = 0;
    col[n++] = line;
    while (n < NCOLS) {
        char *tab = strchr(col[n - 1], '\t');
        if (!tab)
            break;
        *tab = '\0';
        col[n++] = tab + 1;
    }
    return n;
}

static int parse_rule_line(const Ctx *c, char *line, const RuleSet *rs, Rule *r)
{
    char *col[NCOLS];
    int ncols = split_columns(line, col);
    if (ncols != NCOLS) {
        fail(c, "expected %d tab-separated columns, found %d", NCOLS, ncols);
        return -1;
    }

    memset(r, 0, sizeof *r);
    if (parse_id(c, col[0], r) != 0 ||
        parse_category(c, col[1], r) != 0 ||
        parse_severity(c, col[2], r) != 0 ||
        parse_base(c, col[3], r) != 0 ||
        parse_flags(c, col[4], r) != 0 ||
        parse_validator(c, col[5], r) != 0 ||
        for_each_item(c, "extract", col[6], extract_item, &r->extract) != 0 ||
        for_each_item(c, "context", col[7], context_item, r) != 0)
        return -1;

    if (rules_find(rs, r->id) >= 0) {
        fail(c, "duplicate rule id '%s'", r->id);
        return -1;
    }
    if (col[8][0] == '\0') {
        fail(c, "empty regex");
        return -1;
    }
    r->regex = strdup(col[8]);
    if (!r->regex) {
        fail(c, "out of memory");
        return -1;
    }
    detect_anchors(r);
    return 0;
}

int rules_parse(const char *text, size_t len, const char *name,
                RuleSet *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof *out);
    if (err_len > 0)
        err[0] = '\0';
    Ctx c = { name, 0, err, err_len };

    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(text + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - text) : len;
        size_t n = end - pos;
        c.line++;

        if (n > 0 && text[pos + n - 1] == '\r')
            n--;
        if (memchr(text + pos, '\0', n)) {
            fail(&c, "NUL byte in rule file");
            goto error;
        }

        if (n > 0 && text[pos] != '#') {
            if (out->count >= RULES_MAX) {
                fail(&c, "too many rules (maximum %d)", RULES_MAX);
                goto error;
            }
            char *line = malloc(n + 1);
            if (!line) {
                fail(&c, "out of memory");
                goto error;
            }
            memcpy(line, text + pos, n);
            line[n] = '\0';
            int rc = parse_rule_line(&c, line, out, &out->rules[out->count]);
            free(line);
            if (rc != 0)
                goto error;
            out->count++;
        }
        pos = end + 1;
    }
    return 0;

error:
    rules_free(out);
    return -1;
}

int rules_load(const char *path, RuleSet *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int ok = buf != NULL;
    while (ok) {
        if (len == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) { ok = 0; break; }
            buf = bigger;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    int read_failed = ferror(f);
    fclose(f);
    if (!ok || read_failed) {
        free(buf);
        snprintf(err, err_len, "%s: read error", path);
        return -1;
    }

    int rc = rules_parse(buf, len, path, out, err, err_len);
    free(buf);
    return rc;
}

void rules_free(RuleSet *rs)
{
    for (int i = 0; i < rs->count; i++) {
        free(rs->rules[i].regex);
        rs->rules[i].regex = NULL;
    }
    rs->count = 0;
}

int rules_find(const RuleSet *rs, const char *id)
{
    for (int i = 0; i < rs->count; i++)
        if (strcmp(rs->rules[i].id, id) == 0)
            return i;
    return -1;
}
