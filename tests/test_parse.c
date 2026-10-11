#include <stdio.h>
#include <string.h>

#include "test_main.h"
#include "ast.h"
#include "regex_parse.h"
#include "rules.h"

/* ---- valid patterns: expected AST in ast_format() notation ---- */

static const struct {
    const char *pattern;
    int         icase;
    const char *ast;
} VALID[] = {
    /* literals, concatenation, alternation */
    { "a",              0, "'a'" },
    { "abc",            0, "(cat 'a' 'b' 'c')" },
    { "a|b",            0, "(alt 'a' 'b')" },
    { "ab|cd|e",        0, "(alt (cat 'a' 'b') (cat 'c' 'd') 'e')" },
    { "(ab)c",          0, "(cat 'a' 'b' 'c')" },             /* groups are flattened */
    { "((a))",          0, "'a'" },
    { "(a|b)|c",        0, "(alt 'a' 'b' 'c')" },
    { "x.y",            0, "(cat 'x' [^\\x0a] 'y')" },
    { "\xff",           0, "'\\xff'" },                        /* high byte literal */
    { "a b",            0, "(cat 'a' '\\x20' 'b')" },
    /* quantifiers */
    { "a*",             0, "(star 'a')" },
    { "a+",             0, "(plus 'a')" },
    { "a?",             0, "(quest 'a')" },
    { "a{3}",           0, "(rep 3 3 'a')" },
    { "a{2,}",          0, "(rep 2 inf 'a')" },
    { "a{1,4}",         0, "(rep 1 4 'a')" },
    { "a{0}",           0, "(rep 0 0 'a')" },
    { "a{1000}",        0, "(rep 1000 1000 'a')" },
    { "(ab)*",          0, "(star (cat 'a' 'b'))" },
    { "a**",            0, "(star (star 'a'))" },              /* stacked * is plain POSIX */
    { "a{2}*",          0, "(star (rep 2 2 'a'))" },
    { "ab+c",           0, "(cat 'a' (plus 'b') 'c')" },       /* binds to the last atom */
    /* bracket expressions */
    { ".",              0, "[^\\x0a]" },
    { "[abc]",          0, "[a-c]" },
    { "[a-z0-9]",       0, "[0-9a-z]" },
    { "[0-9.]",         0, "[.0-9]" },
    { "[^ ]",           0, "[^\\x20]" },
    { "[]a]",           0, "[\\x5da]" },                       /* leading ] is literal */
    { "[^]a]",          0, "[^\\x5da]" },
    { "[a-]",           0, "[\\x2da]" },                       /* trailing - is literal */
    { "[-a]",           0, "[\\x2da]" },
    { "[ \t]",          0, "[\\x09\\x20]" },
    { "[.*+?(|]",       0, "[(*+.?|]" },                       /* metachars are literal inside */
    { "[a]",            0, "'a'" },
    /* escapes */
    { "\\.",            0, "'.'" },
    { "\\(\\)",         0, "(cat '(' ')')" },
    { "\\\\",           0, "'\\x5c'" },
    { "\\$\\^",         0, "(cat '$' '^')" },
    { "\\[\\]\\{\\}",   0, "(cat '[' ']' '{' '}')" },
    { "\\*\\+\\?\\|",   0, "(cat '*' '+' '?' '|')" },
    /* case folding */
    { "a",              1, "[Aa]" },
    { "aB1",            1, "(cat [Aa] [Bb] '1')" },
    { "[a-c]",          1, "[A-Ca-c]" },
    { "[^a]",           1, "[^Aa]" },                          /* fold, then complement */
    { ".",              1, "[^\\x0a]" },
    /* anchors */
    { "^abc",           0, "(cat bol 'a' 'b' 'c')" },
    { "abc$",           0, "(cat 'a' 'b' 'c' eol)" },
    { "^$",             0, "(cat bol eol)" },
    { "(^|[^0-9])x",    0, "(cat (alt bol [^0-9]) 'x')" },
    { "x([^0-9]|$)",    0, "(cat 'x' (alt [^0-9] eol))" },
    { "a|^b",           0, "(alt 'a' (cat bol 'b'))" },
    { "(a|b$)",         0, "(alt 'a' (cat 'b' eol))" },
    { "^a{0}b",         0, "(cat bol (rep 0 0 'a') 'b')" },
};

/* ---- invalid patterns: expected error offset and message fragment ---- */

static const struct {
    const char *pattern;
    size_t      pos;
    const char *msg;
} INVALID[] = {
    { "\\d",          0, "\\d shorthand" },
    { "a\\s",         1, "\\s shorthand" },
    { "\\w+",         0, "\\w shorthand" },
    { "x\\b",         1, "word boundary" },
    { "(a)\\1",       3, "backreference" },
    { "(?=a)",        0, "\"(?\" groups" },
    { "(?:a)",        0, "\"(?\" groups" },
    { "a*?",          2, "lazy or possessive" },
    { "a+?",          2, "lazy or possessive" },
    { "a??",          2, "lazy or possessive" },
    { "a{2}+",        4, "lazy or possessive" },
    { "a{,3}",        1, "{,n} is not supported" },
    { "a^b",          1, "'^' is only allowed at the start" },
    { "a*^",          2, "'^' is only allowed at the start" },
    { "(^a)*",        1, "'^' is only allowed at the start" },
    { "a$b",          1, "'$' is only allowed at the end" },
    { "$a",           0, "'$' is only allowed at the end" },
    { "^*",           1, "quantifier applied to an anchor" },
    { "(a",           0, "unbalanced '('" },
    { "a)",           1, "unmatched ')'" },
    { "[abc",         0, "unterminated '['" },
    { "[a-",          0, "unterminated '['" },
    { "[]",           0, "unterminated '['" },
    { "a]",           1, "unmatched ']'" },
    { "a}",           1, "unmatched '}'" },
    { "*a",           0, "nothing to repeat" },
    { "a|*",          2, "nothing to repeat" },
    { "a|{2}",        2, "nothing to repeat" },
    { "",             0, "empty pattern" },
    { "a|",           2, "empty alternative" },
    { "|a",           0, "empty alternative" },
    { "()",           1, "empty alternative or group" },
    { "a{2",          1, "invalid interval" },
    { "a{x}",         1, "invalid interval" },
    { "a{3,2}",       1, "m < n" },
    { "a{1001}",      1, "too large" },
    { "[z-a]",        1, "invalid range" },
    { "[\\d]",        1, "backslash inside [...]" },
    { "[[:alpha:]]",  1, "[:class:]" },
    { "[a-z-0]",      4, "'-' must be the first or last" },
    { "a\\",          1, "trailing backslash" },
    { "\\n",          0, "unsupported escape \\n" },
    { "\\t",          0, "unsupported escape \\t" },
    { "a\\-",         1, "unsupported escape \\-" },
};

static void test_valid(void)
{
    StrBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < sizeof VALID / sizeof VALID[0]; i++) {
        RegexError err;
        AstNode *n = regex_parse(VALID[i].pattern, strlen(VALID[i].pattern), VALID[i].icase, &err);
        tm_checks++;
        if (!n) {
            tm_failures++;
            fprintf(stderr, "valid[%zu] \"%s\": parse error at %zu: %s\n", i, VALID[i].pattern,
                    err.pos, err.msg);
            continue;
        }
        sb_clear(&sb);
        ast_format(n, &sb);
        tm_checks++;
        if (strcmp(sb.data, VALID[i].ast) != 0) {
            tm_failures++;
            fprintf(stderr, "valid[%zu] \"%s\"%s:\n   got  %s\n   want %s\n", i, VALID[i].pattern,
                    VALID[i].icase ? " (icase)" : "", sb.data, VALID[i].ast);
        }
        ast_free(n);
    }
    sb_free(&sb);
}

static void test_invalid(void)
{
    for (size_t i = 0; i < sizeof INVALID / sizeof INVALID[0]; i++) {
        RegexError err;
        AstNode *n = regex_parse(INVALID[i].pattern, strlen(INVALID[i].pattern), false, &err);
        tm_checks++;
        if (n) {
            tm_failures++;
            StrBuf sb;
            sb_init(&sb);
            ast_format(n, &sb);
            fprintf(stderr, "invalid[%zu] \"%s\" was accepted: %s\n", i, INVALID[i].pattern, sb.data);
            sb_free(&sb);
            ast_free(n);
            continue;
        }
        tm_checks++;
        if (err.pos != INVALID[i].pos || !strstr(err.msg, INVALID[i].msg)) {
            tm_failures++;
            fprintf(stderr, "invalid[%zu] \"%s\": got pos %zu \"%s\", want pos %zu \"...%s...\"\n",
                    i, INVALID[i].pattern, err.pos, err.msg, INVALID[i].pos, INVALID[i].msg);
        }
    }
}

/* Every default rule parses; spot-check a few shapes. */
static void test_default_rules(void)
{
    RuleSet rs;
    char e[256];
    CHECK_EQ_INT(rules_load("rules/default.rules", &rs, e, sizeof e), 0);
    CHECK_EQ_INT(rs.count, 26);

    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < rs.count; i++) {
        const Rule *r = &rs.rules[i];
        RegexError err;
        AstNode *n = regex_parse(r->regex, strlen(r->regex), r->icase, &err);
        tm_checks++;
        if (!n) {
            tm_failures++;
            fprintf(stderr, "rule %s: error at %zu: %s\n", r->id, err.pos, err.msg);
            continue;
        }
        sb_clear(&sb);
        ast_format(n, &sb);
        /* the loader's anchor flags agree with where the parser put bol/eol */
        CHECK(r->anchored_start == (strncmp(sb.data, "(cat bol", 8) == 0));
        if (strcmp(r->id, "JAVA_STACK") == 0)
            CHECK(strncmp(sb.data, "(cat bol (plus [\\x09\\x20]) 'a' 't' '\\x20'", 40) == 0);
        if (strcmp(r->id, "SQLI") == 0)                 /* icase rule: letters folded */
            CHECK(strstr(sb.data, "[Uu] [Nn] [Ii] [Oo] [Nn]") != NULL);
        if (strcmp(r->id, "PII_CARD") == 0)
            CHECK(strncmp(sb.data, "(cat (alt bol [^0-9]) (rep 4 4 [0-9])", 37) == 0);
        if (strcmp(r->id, "PII_SECRET_QS") == 0)
            CHECK(strstr(sb.data, "(plus [^\\x20\"&])") != NULL);
        ast_free(n);
    }
    sb_free(&sb);
    rules_free(&rs);
}

static void test_charset_ops(void)
{
    CharSet s;
    cs_clear(&s);
    CHECK(cs_empty(&s));
    cs_add_range(&s, 'a', 'z');
    CHECK_EQ_INT(cs_count(&s), 26);
    CHECK(cs_has(&s, 'm') && !cs_has(&s, 'M'));
    cs_fold_case(&s);
    CHECK_EQ_INT(cs_count(&s), 52);
    cs_negate(&s);
    CHECK_EQ_INT(cs_count(&s), 256 - 52);
    CHECK(!cs_has(&s, 'Q') && cs_has(&s, '0') && cs_has(&s, 0xff) && cs_has(&s, 0));
    CharSet t;
    cs_fill(&t);
    CHECK_EQ_INT(cs_count(&t), 256);
    cs_negate(&t);
    CHECK(cs_empty(&t));
}

static void test_deep_nesting(void)
{
    char pat[512];
    size_t n = 0;
    for (int i = 0; i < 150; i++)
        pat[n++] = '(';
    pat[n++] = 'a';
    for (int i = 0; i < 150; i++)
        pat[n++] = ')';
    RegexError err;
    AstNode *a = regex_parse(pat, n, false, &err);
    CHECK(a == NULL);
    CHECK(strstr(err.msg, "nested too deeply") != NULL);

    n = 0;
    for (int i = 0; i < 50; i++)
        pat[n++] = '(';
    pat[n++] = 'a';
    for (int i = 0; i < 50; i++)
        pat[n++] = ')';
    a = regex_parse(pat, n, false, &err);
    CHECK(a != NULL);
    ast_free(a);
}

int main(void)
{
    CHECK(sizeof VALID / sizeof VALID[0] + sizeof INVALID / sizeof INVALID[0] >= 40);
    test_valid();
    test_invalid();
    test_default_rules();
    test_charset_ops();
    test_deep_nesting();
    return TEST_DONE();
}
