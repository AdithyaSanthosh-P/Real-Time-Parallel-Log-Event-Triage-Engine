#include "route.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "aggregate.h"

struct Router {
    const RuleSet *rs;
    FILE          *alerts;
    FILE          *store;
    StrBuf         buf;
    uint64_t       n_records;
    uint64_t       n_alerts;
    bool           failed;
};

const char *category_label(int cat_idx)
{
    static const char *const names[N_CATEGORIES] = {
        "AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII",
    };
    return (cat_idx >= 0 && cat_idx < N_CATEGORIES) ? names[cat_idx] : "UNKNOWN";
}

const char *severity_name(int severity)
{
    static const char *const names[] = { "INFO", "LOW", "MEDIUM", "HIGH", "CRITICAL" };
    return (severity >= 0 && severity <= SEV_CRITICAL) ? names[severity] : "UNKNOWN";
}

bool route_is_alert(const Classified *c)
{
    return c->severity >= SEV_HIGH;
}

static void put_flags(StrBuf *sb, uint32_t flags)
{
    static const struct { uint32_t bit; const char *name; } tab[] = {
        { FLAG_BRUTE_FORCE,       "ESCALATED_BRUTE_FORCE" },
        { FLAG_PASSWORD_SPRAY,    "ESCALATED_PASSWORD_SPRAY" },
        { FLAG_FAIL_THEN_SUCCESS, "ESCALATED_FAIL_THEN_SUCCESS" },
        { FLAG_ERROR_STORM,       "ERROR_STORM" },
    };
    sb_putc(sb, '[');
    bool first = true;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (!(flags & tab[i].bit))
            continue;
        if (!first)
            sb_putc(sb, ',');
        json_put_string(sb, tab[i].name, strlen(tab[i].name));
        first = false;
    }
    sb_putc(sb, ']');
}

static void put_name(StrBuf *sb, const char *name)
{
    json_put_string(sb, name, strlen(name));
}

void route_format(const RuleSet *rs, const Batch *b, uint32_t i, const Classified *c, StrBuf *out)
{
    sb_printf(out, "{\"seq\":%llu,", (unsigned long long)c->seq);

    char ts[32] = "1970-01-01T00:00:00";
    time_t t = (time_t)b->ts[i];
    struct tm tm;
    if (gmtime_r(&t, &tm))
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    sb_printf(out, "\"ts\":\"%s\",\"primary\":", ts);

    if (c->primary == PRIMARY_NONE)
        sb_puts(out, "null");
    else
        put_name(out, category_label(c->primary));
    sb_puts(out, ",\"severity\":");
    put_name(out, severity_name(c->severity));
    sb_printf(out, ",\"confidence\":%.2f,\"labels\":[", (double)c->confidence);

    bool first = true;
    if (c->primary != PRIMARY_NONE) {
        put_name(out, category_label(c->primary));
        first = false;
    }
    for (int k = 0; k < N_CATEGORIES; k++) {
        if (!(c->label_mask & (uint32_t)cat_from_index(k)) || k == c->primary)
            continue;
        if (!first)
            sb_putc(out, ',');
        put_name(out, category_label(k));
        first = false;
    }

    sb_puts(out, "],\"rules\":[");
    first = true;
    for (int k = 0; k < rs->count; k++) {
        if (!((c->rule_mask >> k) & 1))
            continue;
        if (!first)
            sb_putc(out, ',');
        put_name(out, rs->rules[k].id);
        first = false;
    }

    sb_puts(out, "],\"flags\":");
    put_flags(out, c->flags);
    sb_puts(out, ",\"line\":");
    json_put_string(out, batch_raw(b, i), batch_raw_len(b, i));
    sb_putc(out, '}');
}

/* mkdir -p */
static int make_dirs(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        char saved = tmp[i];
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        tmp[i] = saved;
    }
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static FILE *open_sink(const char *dir, const char *name, char *err, size_t err_len)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_len, "%s: cannot open for writing: %s", path, strerror(errno));
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 1 << 20);      /* big buffer: few, large writes */
    return f;
}

Router *router_open(const char *out_dir, const RuleSet *rs, char *err, size_t err_len)
{
    if (make_dirs(out_dir) != 0) {
        snprintf(err, err_len, "%s: cannot create output directory: %s", out_dir, strerror(errno));
        return NULL;
    }
    Router *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    r->rs = rs;
    sb_init(&r->buf);
    r->alerts = open_sink(out_dir, "alerts.jsonl", err, err_len);
    r->store = r->alerts ? open_sink(out_dir, "store.jsonl", err, err_len) : NULL;
    if (!r->alerts || !r->store) {
        if (r->alerts)
            fclose(r->alerts);
        sb_free(&r->buf);
        free(r);
        return NULL;
    }
    return r;
}

int router_emit(Router *r, const Batch *b, uint32_t i, const Classified *c)
{
    sb_clear(&r->buf);
    route_format(r->rs, b, i, c, &r->buf);
    sb_putc(&r->buf, '\n');
    if (r->buf.oom) {
        r->failed = true;
        return -1;
    }

    if (fwrite(r->buf.data, 1, r->buf.len, r->store) != r->buf.len)
        r->failed = true;
    r->n_records++;
    if (route_is_alert(c)) {
        if (fwrite(r->buf.data, 1, r->buf.len, r->alerts) != r->buf.len)
            r->failed = true;
        r->n_alerts++;
    }
    return r->failed ? -1 : 0;
}

uint64_t router_records(const Router *r) { return r->n_records; }
uint64_t router_alerts(const Router *r)  { return r->n_alerts; }

int router_close(Router *r)
{
    if (!r)
        return 0;
    if (fclose(r->alerts) != 0)
        r->failed = true;
    if (fclose(r->store) != 0)
        r->failed = true;
    int rc = r->failed ? -1 : 0;
    sb_free(&r->buf);
    free(r);
    return rc;
}
