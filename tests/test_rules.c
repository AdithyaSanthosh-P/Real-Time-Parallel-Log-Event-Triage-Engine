#include <stdio.h>
#include <string.h>

#include "test_main.h"
#include "rules.h"

#define FIXTURES "tests/fixtures/"

static const Rule *get(const RuleSet *rs, const char *id)
{
    int i = rules_find(rs, id);
    CHECK(i >= 0);
    return i >= 0 ? &rs->rules[i] : NULL;
}

static void test_default_rules(void)
{
    RuleSet rs;
    char err[256];
    int rc = rules_load("rules/default.rules", &rs, err, sizeof err);
    if (rc != 0)
        fprintf(stderr, "load failed: %s\n", err);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(rs.count, 26);

    int per_cat[5] = {0};
    for (int i = 0; i < rs.count; i++) {
        Category c = rs.rules[i].category;
        per_cat[c == CAT_AUTH ? 0 : c == CAT_SEC ? 1 : c == CAT_ERR ? 2 :
                c == CAT_PERF ? 3 : 4]++;
    }
    CHECK_EQ_INT(per_cat[0], 6);   /* AUTH */
    CHECK_EQ_INT(per_cat[1], 5);   /* SEC  */
    CHECK_EQ_INT(per_cat[2], 5);   /* ERR  */
    CHECK_EQ_INT(per_cat[3], 4);   /* PERF */
    CHECK_EQ_INT(per_cat[4], 6);   /* PII  */

    /* Rule order is file order: rule r is bit r of every mask. */
    CHECK_STR_EQ(rs.rules[0].id, "SSH_FAIL_PW");
    CHECK_STR_EQ(rs.rules[25].id, "PII_SECRET_QS");

    const Rule *r = get(&rs, "SSH_FAIL_PW");
    if (r) {
        CHECK_EQ_INT(r->category, CAT_AUTH);
        CHECK_EQ_INT(r->severity, SEV_MEDIUM);
        CHECK(r->base > 0.59f && r->base < 0.61f);
        CHECK(!r->icase && !r->no_label && !r->anchored_start && !r->anchored_end);
        CHECK_EQ_INT(r->validator, VAL_NONE);
        CHECK_EQ_INT(r->extract, EXTRACT_IP | EXTRACT_USER);
        CHECK_EQ_INT(r->n_context, 0);
        CHECK_STR_EQ(r->regex, "Failed password for (invalid user )?[^ ]+ from [0-9.]+");
    }

    r = get(&rs, "SSH_ACCEPTED");
    if (r) {
        CHECK_EQ_INT(r->category, CAT_AUTH);
        CHECK_EQ_INT(r->severity, SEV_INFO);
        CHECK(r->no_label);
    }
    r = get(&rs, "PAM_AUTH_FAIL");
    if (r) {
        CHECK(!r->no_label);
        CHECK_EQ_INT(r->extract, EXTRACT_USER);
    }

    r = get(&rs, "SQLI");
    if (r) {
        CHECK_EQ_INT(r->category, CAT_SEC);
        CHECK_EQ_INT(r->severity, SEV_CRITICAL);
        CHECK(r->icase);
        CHECK_EQ_INT(r->n_context, 3);
        CHECK_STR_EQ(r->context[0], "select");
        CHECK_STR_EQ(r->context[1], "union");
        CHECK_STR_EQ(r->context[2], "where");
    }

    r = get(&rs, "PII_CARD");
    if (r) {
        CHECK_EQ_INT(r->category, CAT_PII);
        CHECK_EQ_INT(r->validator, VAL_LUHN);
        CHECK(r->base > 0.34f && r->base < 0.36f);
        CHECK_EQ_INT(r->n_context, 3);
        CHECK_STR_EQ(r->context[2], "payment");
        /* ^ and $ inside a group are not rule-level anchors */
        CHECK(!r->anchored_start && !r->anchored_end);
    }
    r = get(&rs, "PII_AADHAAR");
    if (r) {
        CHECK_EQ_INT(r->validator, VAL_VERHOEFF);
        CHECK_EQ_INT(r->n_context, 3);
        CHECK_STR_EQ(r->context[1], "aadhar");
    }
    r = get(&rs, "PII_PAN");
    if (r)
        CHECK_EQ_INT(r->validator, VAL_PAN);

    /* The regex column keeps tabs and trailing spaces verbatim. */
    r = get(&rs, "JAVA_STACK");
    if (r) {
        CHECK(r->anchored_start);
        CHECK(!r->anchored_end);
        CHECK(strchr(r->regex, '\t') != NULL);
        CHECK_STR_EQ(r->regex, "^[ \t]+at [A-Za-z0-9_.$]+\\(");
    }
    r = get(&rs, "JAVA_EXCEPTION");
    if (r) {
        size_t n = strlen(r->regex);
        CHECK(n > 0 && r->regex[n - 1] == ' ');
    }

    rules_free(&rs);
    CHECK_EQ_INT(rs.count, 0);
}

static void test_anchors_and_edge_syntax(void)
{
    static const char text[] =
        "# comment\r\n"
        "\r\n"
        "A1\tERR\tLOW\t1\ti\t-\tip\ta,b\t^abc$\r\n"       /* CRLF line, both anchors */
        "A2\tERR\tLOW\t0.5\t-\t-\t-\t-\tabc\\$\n"          /* escaped $ is a literal  */
        "A3\tERR\tLOW\t0.5\t-\t-\t-\t-\tabc\\\\$\n"        /* \\$ : escaped backslash, anchor */
        "A4\tPERF*\tINFO\t0.5\t-\t-\t-\t-\tx";             /* no trailing newline     */
    RuleSet rs;
    char err[256];
    int rc = rules_parse(text, sizeof text - 1, "inline", &rs, err, sizeof err);
    if (rc != 0)
        fprintf(stderr, "parse failed: %s\n", err);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(rs.count, 4);
    if (rs.count == 4) {
        CHECK(rs.rules[0].anchored_start && rs.rules[0].anchored_end);
        CHECK(rs.rules[0].icase);
        CHECK_EQ_INT(rs.rules[0].n_context, 2);
        CHECK_STR_EQ(rs.rules[0].regex, "^abc$");       /* \r removed */
        CHECK(!rs.rules[1].anchored_end);
        CHECK(rs.rules[2].anchored_end);
        CHECK(rs.rules[3].no_label);
        CHECK_EQ_INT(rs.rules[3].category, CAT_PERF);
        CHECK_STR_EQ(rs.rules[3].regex, "x");
    }
    rules_free(&rs);
}

static const struct {
    const char *file;
    const char *message;      /* expected after "<file>:<line>: " */
    int         line;
} bad[] = {
    { "bad_columns.rules",     "expected 9 tab-separated columns, found 8", 3 },
    { "bad_category.rules",    "unknown category 'AUTHX'", 3 },
    { "bad_severity.rules",    "unknown severity 'SEVERE'", 3 },
    { "bad_validator.rules",   "unknown validator 'crc32'", 3 },
    { "bad_duplicate.rules",   "duplicate rule id 'DUP'", 3 },
    { "bad_too_many.rules",    "too many rules (maximum 64)", 65 },
    { "bad_base.rules",        "bad base '1.5'", 3 },
    { "bad_flags.rules",       "unknown flag 'x'", 3 },
    { "bad_extract.rules",     "unknown extract item 'host'", 3 },
    { "bad_id.rules",          "bad rule id 'bad_id'", 3 },
    { "bad_empty_regex.rules", "empty regex", 3 },
    { "bad_context.rules",     "empty item in context list", 3 },
};

static void test_bad_fixtures(void)
{
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char path[128], want[256], err[256];
        snprintf(path, sizeof path, FIXTURES "%s", bad[i].file);
        RuleSet rs;
        int rc = rules_load(path, &rs, err, sizeof err);
        CHECK_EQ_INT(rc, -1);
        CHECK_EQ_INT(rs.count, 0);
        snprintf(want, sizeof want, "%s:%d: ", path, bad[i].line);
        CHECK(strncmp(err, want, strlen(want)) == 0);
        CHECK(strstr(err, bad[i].message) != NULL);
        if (strncmp(err, want, strlen(want)) != 0 || !strstr(err, bad[i].message))
            fprintf(stderr, "  %s: got \"%s\"\n  want \"%s%s\"\n", bad[i].file, err, want,
                    bad[i].message);
        rules_free(&rs);
    }
}

static void test_missing_file(void)
{
    RuleSet rs;
    char err[256];
    CHECK_EQ_INT(rules_load("tests/fixtures/does_not_exist.rules", &rs, err, sizeof err), -1);
    CHECK(strstr(err, "does_not_exist.rules") != NULL);
    CHECK_EQ_INT(rs.count, 0);
}

int main(void)
{
    test_default_rules();
    test_anchors_and_edge_syntax();
    test_bad_fixtures();
    test_missing_file();
    return TEST_DONE();
}
