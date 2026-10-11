#include "ast.h"

#include <stdlib.h>
#include <string.h>

/* ---- CharSet ---- */

void cs_clear(CharSet *s)  { memset(s, 0, sizeof *s); }
void cs_fill(CharSet *s)   { memset(s, 0xff, sizeof *s); }

void cs_add(CharSet *s, unsigned char c)
{
    s->w[c >> 6] |= (uint64_t)1 << (c & 63);
}

void cs_add_range(CharSet *s, unsigned char lo, unsigned char hi)
{
    for (unsigned c = lo; c <= hi; c++)
        cs_add(s, (unsigned char)c);
}

void cs_union(CharSet *s, const CharSet *t)
{
    for (int i = 0; i < 4; i++)
        s->w[i] |= t->w[i];
}

void cs_negate(CharSet *s)
{
    for (int i = 0; i < 4; i++)
        s->w[i] = ~s->w[i];
}

bool cs_has(const CharSet *s, unsigned char c)
{
    return (s->w[c >> 6] >> (c & 63)) & 1;
}

int cs_count(const CharSet *s)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        n += __builtin_popcountll(s->w[i]);
    return n;
}

bool cs_equal(const CharSet *a, const CharSet *b)
{
    return memcmp(a->w, b->w, sizeof a->w) == 0;
}

bool cs_empty(const CharSet *s)
{
    return (s->w[0] | s->w[1] | s->w[2] | s->w[3]) == 0;
}

void cs_fold_case(CharSet *s)
{
    for (unsigned c = 'A'; c <= 'Z'; c++) {
        unsigned lower = c + ('a' - 'A');
        if (cs_has(s, (unsigned char)c) || cs_has(s, (unsigned char)lower)) {
            cs_add(s, (unsigned char)c);
            cs_add(s, (unsigned char)lower);
        }
    }
}

static void put_byte(StrBuf *out, unsigned c, bool in_class)
{
    bool special = in_class ? (c == ']' || c == '\\' || c == '-' || c == '^')
                            : (c == '\'' || c == '\\');
    if (c > 0x20 && c < 0x7f && !special)
        sb_putc(out, (char)c);
    else
        sb_printf(out, "\\x%02x", c);
}

/* Bytes of `s` as ranges: a-z0-9 ... */
static void put_ranges(const CharSet *s, StrBuf *out)
{
    for (unsigned c = 0; c < 256; ) {
        if (!cs_has(s, (unsigned char)c)) {
            c++;
            continue;
        }
        unsigned end = c;
        while (end + 1 < 256 && cs_has(s, (unsigned char)(end + 1)))
            end++;
        put_byte(out, c, true);
        if (end > c + 1)
            sb_putc(out, '-');
        if (end > c)
            put_byte(out, end, true);
        c = end + 1;
    }
}

void cs_format(const CharSet *s, StrBuf *out)
{
    int n = cs_count(s);
    if (n == 1) {
        for (unsigned c = 0; c < 256; c++) {
            if (cs_has(s, (unsigned char)c)) {
                sb_putc(out, '\'');
                put_byte(out, c, false);
                sb_putc(out, '\'');
                return;
            }
        }
    }
    sb_putc(out, '[');
    if (n > 128) {
        CharSet t = *s;
        cs_negate(&t);
        sb_putc(out, '^');
        put_ranges(&t, out);
    } else {
        put_ranges(s, out);
    }
    sb_putc(out, ']');
}

/* ---- nodes ---- */

AstNode *ast_new(NodeKind kind, size_t pos)
{
    AstNode *n = calloc(1, sizeof *n);
    if (n) {
        n->kind = kind;
        n->pos = pos;
    }
    return n;
}

int ast_add_kid(AstNode *parent, AstNode *kid)
{
    AstNode **k = realloc(parent->kids, (size_t)(parent->nkids + 1) * sizeof *k);
    if (!k)
        return -1;
    parent->kids = k;
    parent->kids[parent->nkids++] = kid;
    return 0;
}

void ast_free(AstNode *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nkids; i++)
        ast_free(n->kids[i]);
    free(n->kids);
    free(n);
}

static void format_kids(const AstNode *n, const char *name, StrBuf *out)
{
    sb_putc(out, '(');
    sb_puts(out, name);
    for (int i = 0; i < n->nkids; i++) {
        sb_putc(out, ' ');
        ast_format(n->kids[i], out);
    }
    sb_putc(out, ')');
}

void ast_format(const AstNode *n, StrBuf *out)
{
    switch (n->kind) {
    case NODE_CHARSET: cs_format(&n->set, out); break;
    case NODE_CONCAT:  format_kids(n, "cat", out); break;
    case NODE_ALT:     format_kids(n, "alt", out); break;
    case NODE_STAR:    format_kids(n, "star", out); break;
    case NODE_PLUS:    format_kids(n, "plus", out); break;
    case NODE_QUEST:   format_kids(n, "quest", out); break;
    case NODE_BOL:     sb_puts(out, "bol"); break;
    case NODE_EOL:     sb_puts(out, "eol"); break;
    case NODE_REPEAT:
        if (n->max == REPEAT_INF)
            sb_printf(out, "(rep %d inf ", n->min);
        else
            sb_printf(out, "(rep %d %d ", n->min, n->max);
        ast_format(n->kids[0], out);
        sb_putc(out, ')');
        break;
    }
}

void ast_print(const AstNode *n, FILE *f)
{
    StrBuf sb;
    sb_init(&sb);
    ast_format(n, &sb);
    if (sb.data)
        fprintf(f, "%s\n", sb.data);
    sb_free(&sb);
}

bool ast_can_consume(const AstNode *n)
{
    switch (n->kind) {
    case NODE_CHARSET:
        return !cs_empty(&n->set);
    case NODE_BOL:
    case NODE_EOL:
        return false;
    case NODE_REPEAT:
        if (n->max == 0)
            return false;
        return ast_can_consume(n->kids[0]);
    default:
        for (int i = 0; i < n->nkids; i++)
            if (ast_can_consume(n->kids[i]))
                return true;
        return false;
    }
}
