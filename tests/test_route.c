#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_main.h"
#include "aggregate.h"
#include "batch.h"
#include "json.h"
#include "route.h"
#include "rules.h"

static RuleSet rs;

/* ---- JSON string escaping ---- */

static void esc(const char *in, size_t n, const char *want, int src_line)
{
    StrBuf sb;
    sb_init(&sb);
    json_put_string(&sb, in, n);
    tm_checks++;
    if (sb.oom || strcmp(sb.data, want) != 0) {
        tm_failures++;
        fprintf(stderr, "%s:%d: json_put_string gave %s, want %s\n", __FILE__, src_line,
                sb.data, want);
    }
    sb_free(&sb);
}
#define ESC(s, want)  esc((s), strlen(s), (want), __LINE__)

static void test_json_escape(void)
{
    ESC("", "\"\"");
    ESC("plain text 123", "\"plain text 123\"");
    ESC("say \"hi\"", "\"say \\\"hi\\\"\"");
    ESC("back\\slash", "\"back\\\\slash\"");
    ESC("/not/escaped", "\"/not/escaped\"");
    ESC("a\nb\rc\td\be\ff", "\"a\\nb\\rc\\td\\be\\ff\"");
    ESC("\x01\x1f\x7f", "\"\\u0001\\u001f\\u007f\"");
    esc("a\0b", 3, "\"a\\u0000b\"", __LINE__);

    /* well-formed UTF-8 passes through unchanged */
    ESC("caf\xc3\xa9", "\"caf\xc3\xa9\"");                    /* U+00E9 */
    ESC("\xe2\x82\xac", "\"\xe2\x82\xac\"");                  /* U+20AC */
    ESC("\xf0\x9f\x98\x80", "\"\xf0\x9f\x98\x80\"");          /* U+1F600 */
    ESC("\xf4\x8f\xbf\xbf", "\"\xf4\x8f\xbf\xbf\"");          /* U+10FFFF */
    ESC("\xed\x9f\xbf", "\"\xed\x9f\xbf\"");                  /* U+D7FF, just below surrogates */

    /* anything else becomes U+FFFD, one per offending byte */
    ESC("\x80", "\"\\ufffd\"");                               /* stray continuation */
    ESC("\xff\xfe", "\"\\ufffd\\ufffd\"");
    ESC("\xc3", "\"\\ufffd\"");                               /* truncated at the end */
    ESC("\xc3(", "\"\\ufffd(\"");                             /* bad continuation */
    ESC("\xc0\x80", "\"\\ufffd\\ufffd\"");                    /* overlong NUL */
    ESC("\xc1\xbf", "\"\\ufffd\\ufffd\"");                    /* overlong */
    ESC("\xe0\x80\x80", "\"\\ufffd\\ufffd\\ufffd\"");         /* overlong 3-byte */
    ESC("\xed\xa0\x80", "\"\\ufffd\\ufffd\\ufffd\"");         /* surrogate U+D800 */
    ESC("\xf4\x90\x80\x80", "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");  /* above U+10FFFF */
    ESC("\xf8\x88\x80\x80\x80", "\"\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\"");
    ESC("ok\xe2\x82", "\"ok\\ufffd\\ufffd\"");                /* truncated 3-byte */
}

static void test_strbuf(void)
{
    StrBuf sb;
    sb_init(&sb);
    sb_puts(&sb, "abc");
    sb_putc(&sb, 'd');
    sb_printf(&sb, "-%d-%s", 42, "x");
    CHECK_STR_EQ(sb.data, "abcd-42-x");
    CHECK_EQ_INT(sb.len, 9);

    char big[1000];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    sb_clear(&sb);
    sb_printf(&sb, "<%s>", big);                        /* longer than the printf scratch */
    CHECK_EQ_INT(sb.len, 1001);
    CHECK(sb.data[0] == '<' && sb.data[1000] == '>' && sb.data[1001] == '\0');

    for (int i = 0; i < 10000; i++)                    /* growth */
        sb_puts(&sb, "0123456789");
    CHECK_EQ_INT(sb.len, 1001 + 100000);
    CHECK(!sb.oom);
    sb_free(&sb);
}

/* ---- record format and sinks ---- */

static Batch batch;
static Aggregator agg;

static Classified classify(uint32_t i, const char *first, ...)
{
    uint64_t m = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, first);
    for (const char *id = first; id; id = __builtin_va_arg(ap, const char *)) {
        int k = rules_find(&rs, id);
        if (k < 0) {
            fprintf(stderr, "test bug: no rule %s\n", id);
            exit(1);
        }
        m |= (uint64_t)1 << k;
    }
    __builtin_va_end(ap);
    MatchResult mr = { .seq = batch.seq_start + i, .rule_mask = m, .valid_mask = m };
    Classified c;
    aggregator_classify(&agg, &mr, 0, &c);
    return c;
}

static void test_format(void)
{
    StrBuf sb;
    sb_init(&sb);

    /* SQLi + e-mail: primary SEC, PII second; ts 2026-10-11 10:14:22 UTC */
    Classified c = classify(0, "SQLI", "PII_EMAIL", NULL);
    route_format(&rs, &batch, 0, &c, &sb);
    CHECK_STR_EQ(sb.data,
        "{\"seq\":100,\"ts\":\"2026-10-11T10:14:22\",\"primary\":\"SECURITY_THREAT\","
        "\"severity\":\"CRITICAL\",\"confidence\":0.60,"
        "\"labels\":[\"SECURITY_THREAT\",\"PII\"],\"rules\":[\"SQLI\",\"PII_EMAIL\"],"
        "\"flags\":[],\"line\":\"GET /item?id=1 UNION SELECT \\\"x\\\" a@b.com\"}");

    /* no label: primary null, empty labels; SSH_ACCEPTED still listed in rules */
    sb_clear(&sb);
    c = classify(1, "SSH_ACCEPTED", NULL);
    route_format(&rs, &batch, 1, &c, &sb);
    CHECK_STR_EQ(sb.data,
        "{\"seq\":101,\"ts\":\"2026-10-11T10:14:23\",\"primary\":null,"
        "\"severity\":\"INFO\",\"confidence\":0.00,\"labels\":[],"
        "\"rules\":[\"SSH_ACCEPTED\"],\"flags\":[],\"line\":\"tab\\there\\\\ and \\u0001\"}");

    /* flags */
    sb_clear(&sb);
    c = classify(2, "SSH_FAIL_PW", NULL);
    c.flags = FLAG_BRUTE_FORCE | FLAG_ERROR_STORM;
    route_format(&rs, &batch, 2, &c, &sb);
    CHECK(strstr(sb.data, "\"flags\":[\"ESCALATED_BRUTE_FORCE\",\"ERROR_STORM\"]") != NULL);
    CHECK(strstr(sb.data, "\"primary\":\"AUTH_FAILURE\"") != NULL);
    CHECK(strstr(sb.data, "\"line\":\"\"}") != NULL);          /* empty line */
    sb_free(&sb);
}

static void test_names(void)
{
    CHECK_STR_EQ(category_label(0), "AUTH_FAILURE");
    CHECK_STR_EQ(category_label(1), "SECURITY_THREAT");
    CHECK_STR_EQ(category_label(2), "ERROR");
    CHECK_STR_EQ(category_label(3), "PERF_WARNING");
    CHECK_STR_EQ(category_label(4), "PII");
    CHECK_STR_EQ(severity_name(SEV_INFO), "INFO");
    CHECK_STR_EQ(severity_name(SEV_CRITICAL), "CRITICAL");
    Classified c = classify(0, "SSH_FAIL_PW", NULL);          /* MEDIUM */
    CHECK(!route_is_alert(&c));
    c = classify(0, "XSS", NULL);                             /* HIGH */
    CHECK(route_is_alert(&c));
    c = classify(0, "SQLI", NULL);                            /* CRITICAL */
    CHECK(route_is_alert(&c));
}

static size_t count_lines(const char *path, char **content)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return (size_t)-1;
    char *buf = malloc(1 << 16);
    size_t n = fread(buf, 1, (1 << 16) - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t lines = 0;
    for (size_t i = 0; i < n; i++)
        lines += buf[i] == '\n';
    if (content)
        *content = buf;
    else
        free(buf);
    return lines;
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static void test_sinks(void)
{
    char tmpl[] = "/tmp/triage_test_route_XXXXXX";
    char *base = mkdtemp(tmpl);
    CHECK(base != NULL);
    if (!base)
        return;
    char out[512], err[256];
    snprintf(out, sizeof out, "%s/nested/out", base);           /* parents are created */

    Router *r = router_open(out, &rs, err, sizeof err);
    if (!r)
        fprintf(stderr, "router_open: %s\n", err);
    CHECK(r != NULL);
    if (r) {
        Classified c0 = classify(0, "SQLI", "PII_EMAIL", NULL);   /* CRITICAL -> alert */
        Classified c1 = classify(1, "SSH_ACCEPTED", NULL);        /* INFO */
        Classified c2 = classify(2, "SSH_FAIL_PW", NULL);         /* MEDIUM */
        CHECK_EQ_INT(router_emit(r, &batch, 0, &c0), 0);
        CHECK_EQ_INT(router_emit(r, &batch, 1, &c1), 0);
        CHECK_EQ_INT(router_emit(r, &batch, 2, &c2), 0);
        CHECK_EQ_INT(router_records(r), 3);
        CHECK_EQ_INT(router_alerts(r), 1);
        CHECK_EQ_INT(router_close(r), 0);

        char path[600];
        char *content = NULL;
        snprintf(path, sizeof path, "%s/store.jsonl", out);
        CHECK_EQ_INT(count_lines(path, &content), 3);
        free(content);
        snprintf(path, sizeof path, "%s/alerts.jsonl", out);
        CHECK_EQ_INT(count_lines(path, &content), 1);
        CHECK(content && strstr(content, "\"seq\":100,") != NULL);
        free(content);

        /* reopening truncates */
        r = router_open(out, &rs, err, sizeof err);
        CHECK(r != NULL);
        CHECK_EQ_INT(router_close(r), 0);
        snprintf(path, sizeof path, "%s/store.jsonl", out);
        CHECK_EQ_INT(count_lines(path, NULL), 0);
    }

    /* an output path that is a file, not a directory */
    char file[600];
    snprintf(file, sizeof file, "%s/afile", base);
    FILE *f = fopen(file, "w");
    if (f)
        fclose(f);
    CHECK(router_open(file, &rs, err, sizeof err) == NULL);
    CHECK(strlen(err) > 0);

    snprintf(out, sizeof out, "%s/nested/out", base);
    char sub[600];
    snprintf(sub, sizeof sub, "%s/nested", base);
    remove_dir(out);
    remove_dir(sub);
    unlink(file);
    rmdir(base);
}

int main(void)
{
    char err[256];
    if (rules_load("rules/default.rules", &rs, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load rules: %s\n", err);
        return 1;
    }
    aggregator_init(&agg, &rs, 0.5f);

    if (batch_init(&batch) != 0)
        return 1;
    batch_reset(&batch);
    batch.seq_start = 100;
    const char *l0 = "GET /item?id=1 UNION SELECT \"x\" a@b.com";
    const char *l1 = "tab\there\\ and \x01";
    CHECK_EQ_INT(batch_add_line(&batch, l0, strlen(l0), 1791713662LL, 1), 0);
    CHECK_EQ_INT(batch_add_line(&batch, l1, strlen(l1), 1791713663LL, 2), 0);
    CHECK_EQ_INT(batch_add_line(&batch, "", 0, 1791713664LL, 3), 0);

    test_json_escape();
    test_strbuf();
    test_format();
    test_names();
    test_sinks();

    batch_free(&batch);
    rules_free(&rs);
    return TEST_DONE();
}
