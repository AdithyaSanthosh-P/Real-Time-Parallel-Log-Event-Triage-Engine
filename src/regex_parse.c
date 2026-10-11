/* Recursive-descent parser: regex text -> AST (spec §3.1 grammar, §3.2 step 1).
 *
 * Each grammar nonterminal has one function below (parse_alt, parse_concat,
 * parse_repeat, parse_atom, parse_class, parse_item). Every function returns
 * a freshly allocated subtree, or NULL after recording an error; the first
 * error wins and parsing stops.
 *
 * Literals, '.', and bracket expressions all become NODE_CHARSET leaves, so
 * the NFA builder only ever sees one kind of input-consuming node. Case
 * insensitivity (rule flag 'i') is applied here by folding each charset. */

#include "regex_parse.h"

#include <stdio.h>
#include <string.h>

#include "regex_lex.h"

typedef struct {
    Lexer       lex;
    bool        icase;
    int         depth;      /* current parenthesis nesting */
    RegexError *err;
    bool        failed;
} Parser;

static void fail(Parser *p, size_t pos, const char *msg)
{
    if (p->failed)
        return;
    p->failed = true;
    p->err->pos = pos;
    snprintf(p->err->msg, sizeof p->err->msg, "%s", msg);
}

static AstNode *new_node(Parser *p, NodeKind k, size_t pos)
{
    AstNode *n = ast_new(k, pos);
    if (!n)
        fail(p, pos, "out of memory");
    return n;
}

/* Append `kid` to an n-ary node. A kid of the same kind is flattened into the
 * parent ("(ab)c" -> (cat a b c)), which does not change the language. */
static bool add_kid(Parser *p, AstNode *parent, AstNode *kid)
{
    if (kid->kind == parent->kind && (kid->kind == NODE_CONCAT || kid->kind == NODE_ALT) &&
        kid->nkids > 0) {
        for (int i = 0; i < kid->nkids; i++) {
            if (ast_add_kid(parent, kid->kids[i]) != 0) {
                /* kids already moved stay owned by parent; free the rest */
                for (int j = i; j < kid->nkids; j++)
                    ast_free(kid->kids[j]);
                kid->nkids = 0;
                ast_free(kid);
                fail(p, parent->pos, "out of memory");
                return false;
            }
        }
        kid->nkids = 0;
        ast_free(kid);
        return true;
    }
    if (ast_add_kid(parent, kid) != 0) {
        ast_free(kid);
        fail(p, parent->pos, "out of memory");
        return false;
    }
    return true;
}

/* A one-byte charset leaf, folded for icase rules. */
static AstNode *charset_leaf(Parser *p, size_t pos, const CharSet *set)
{
    AstNode *n = new_node(p, NODE_CHARSET, pos);
    if (!n)
        return NULL;
    n->set = *set;
    if (p->icase)
        cs_fold_case(&n->set);
    return n;
}

static AstNode *parse_alt(Parser *p);

/* ---- class := '[' '^'? ']'? item+ ']'   and   item := CHAR ( '-' CHAR )? ---- */

/* Read one literal byte of a bracket expression, rejecting syntax whose
 * POSIX meaning differs from what a reader would expect. Returns -1 on error. */
static int class_byte(Parser *p)
{
    int c = lex_peek_byte(&p->lex, 0);
    size_t pos = p->lex.pos;
    if (c == '\\') {
        fail(p, pos, "backslash inside [...] is not supported (POSIX treats it as a literal backslash)");
        return -1;
    }
    if (c == '[') {
        int d = lex_peek_byte(&p->lex, 1);
        if (d == ':' || d == '.' || d == '=') {
            fail(p, pos, "[:class:], [.coll.] and [=equiv=] are not supported; list the bytes or ranges");
            return -1;
        }
    }
    lex_skip_byte(&p->lex);
    return c;
}

/* item := CHAR ( '-' CHAR )?   Adds the item's bytes to `set`. A '-' is
 * literal only as the first or the last item, as in POSIX. */
static bool parse_item(Parser *p, CharSet *set, bool first)
{
    size_t pos = p->lex.pos;
    int lo = class_byte(p);
    if (lo < 0)
        return false;

    int next = lex_peek_byte(&p->lex, 0);
    if (lo == '-' && !first && next != ']' && next != -1) {
        fail(p, pos, "'-' must be the first or last character in [...]");
        return false;
    }
    /* range "lo-hi", unless the '-' is the last item ("[a-]") */
    if (lex_peek_byte(&p->lex, 0) == '-' && lex_peek_byte(&p->lex, 1) != ']' &&
        lex_peek_byte(&p->lex, 1) != -1) {
        lex_skip_byte(&p->lex);
        int hi = class_byte(p);
        if (hi < 0)
            return false;
        if (hi < lo) {
            fail(p, pos, "invalid range in [...]: end is before start");
            return false;
        }
        cs_add_range(set, (unsigned char)lo, (unsigned char)hi);
        return true;
    }
    cs_add(set, (unsigned char)lo);
    return true;
}

/* Called with the lexer just after '['. Builds the set, folds case for icase
 * rules, and complements it last for "[^...]" (so "[^a]" with icase excludes
 * both 'a' and 'A', as glibc does). */
static AstNode *parse_class(Parser *p, size_t open_pos)
{
    CharSet set;
    cs_clear(&set);
    bool negate = false;
    if (lex_peek_byte(&p->lex, 0) == '^') {
        negate = true;
        lex_skip_byte(&p->lex);
    }
    bool first = true;
    if (lex_peek_byte(&p->lex, 0) == ']') {          /* leading ']' is literal */
        cs_add(&set, ']');
        lex_skip_byte(&p->lex);
        first = false;
    }
    for (;;) {
        int c = lex_peek_byte(&p->lex, 0);
        if (c == -1) {
            fail(p, open_pos, "unterminated '[' (missing ']')");
            return NULL;
        }
        if (c == ']' && !first)
            break;
        if (!parse_item(p, &set, first))
            return NULL;
        first = false;
    }
    lex_skip_byte(&p->lex);                          /* the closing ']' */

    AstNode *n = new_node(p, NODE_CHARSET, open_pos);
    if (!n)
        return NULL;
    if (p->icase)
        cs_fold_case(&set);
    if (negate)
        cs_negate(&set);
    n->set = set;
    return n;
}

/* ---- atom := CHAR | '.' | class | '(' alt ')' | ESCAPE | '^' | '$' ---- */

static AstNode *parse_atom(Parser *p)
{
    Token t = lex_peek(&p->lex);
    CharSet set;

    switch (t.kind) {
    case TK_ERROR:
        fail(p, t.pos, t.msg);
        return NULL;
    case TK_CHAR:
        lex_advance(&p->lex, &t);
        cs_clear(&set);
        cs_add(&set, t.c);
        return charset_leaf(p, t.pos, &set);
    case TK_DOT:                                     /* '.' = every byte except '\n' (§3.2) */
        lex_advance(&p->lex, &t);
        cs_fill(&set);
        set.w['\n' >> 6] &= ~((uint64_t)1 << ('\n' & 63));
        return charset_leaf(p, t.pos, &set);
    case TK_LBRACKET:
        lex_advance(&p->lex, &t);
        return parse_class(p, t.pos);
    case TK_CARET:
        lex_advance(&p->lex, &t);
        return new_node(p, NODE_BOL, t.pos);
    case TK_DOLLAR:
        lex_advance(&p->lex, &t);
        return new_node(p, NODE_EOL, t.pos);
    case TK_LPAREN: {
        if (p->depth >= REGEX_DEPTH_MAX) {
            fail(p, t.pos, "parentheses nested too deeply");
            return NULL;
        }
        lex_advance(&p->lex, &t);
        p->depth++;
        AstNode *inner = parse_alt(p);
        p->depth--;
        if (!inner)
            return NULL;
        Token close = lex_peek(&p->lex);
        if (close.kind != TK_RPAREN) {
            ast_free(inner);
            fail(p, t.pos, "unbalanced '(' (missing ')')");
            return NULL;
        }
        lex_advance(&p->lex, &close);
        return inner;
    }
    case TK_STAR:
    case TK_PLUS:
    case TK_QUEST:
    case TK_LBRACE:
        fail(p, t.pos, "quantifier with nothing to repeat");
        return NULL;
    default:                                         /* '|' ')' END: parse_concat stops before these */
        fail(p, t.pos, "internal parser error: unexpected token");
        return NULL;
    }
}

/* ---- repeat := atom ( '*' | '+' | '?' | '{' INT ( ',' INT? )? '}' )* ---- */

/* Read a decimal count; -1 if there are no digits. */
static long parse_int(Parser *p)
{
    long v = -1;
    int c;
    while ((c = lex_peek_byte(&p->lex, 0)) >= '0' && c <= '9') {
        v = (v < 0 ? 0 : v) * 10 + (c - '0');
        if (v > 1000000)
            v = 1000000;                             /* clamp; rejected below */
        lex_skip_byte(&p->lex);
    }
    return v;
}

/* Called with the lexer just after '{'. */
static bool parse_interval(Parser *p, size_t open_pos, int *min, int *max)
{
    if (lex_peek_byte(&p->lex, 0) == ',') {
        fail(p, open_pos, "{,n} is not supported; write {0,n}");
        return false;
    }
    long lo = parse_int(p);
    if (lo < 0) {
        fail(p, open_pos, "invalid interval: expected {n}, {n,} or {n,m}");
        return false;
    }
    long hi = lo;
    if (lex_peek_byte(&p->lex, 0) == ',') {
        lex_skip_byte(&p->lex);
        hi = parse_int(p);                           /* -1: "{n,}" */
    }
    if (lex_peek_byte(&p->lex, 0) != '}') {
        fail(p, open_pos, "invalid interval: expected {n}, {n,} or {n,m}");
        return false;
    }
    lex_skip_byte(&p->lex);
    if (lo > REGEX_REPEAT_MAX || hi > REGEX_REPEAT_MAX) {
        fail(p, open_pos, "repeat count too large (maximum 1000)");
        return false;
    }
    if (hi >= 0 && hi < lo) {
        fail(p, open_pos, "invalid interval: {n,m} with m < n");
        return false;
    }
    *min = (int)lo;
    *max = hi < 0 ? REPEAT_INF : (int)hi;
    return true;
}

static AstNode *parse_repeat(Parser *p)
{
    AstNode *node = parse_atom(p);
    if (!node)
        return NULL;
    bool after_quantifier = false;

    for (;;) {
        Token t = lex_peek(&p->lex);
        NodeKind k;
        if (t.kind == TK_STAR)       k = NODE_STAR;
        else if (t.kind == TK_PLUS)  k = NODE_PLUS;
        else if (t.kind == TK_QUEST) k = NODE_QUEST;
        else if (t.kind == TK_LBRACE) k = NODE_REPEAT;
        else
            break;

        /* "*?" "+?" "??" "*+" ... are lazy/possessive quantifiers in PCRE;
         * POSIX would read them differently, so refuse them outright. */
        if (after_quantifier && (k == NODE_QUEST || k == NODE_PLUS)) {
            fail(p, t.pos, "lazy or possessive quantifiers (*? +? ?? *+ ...) are not supported");
            ast_free(node);
            return NULL;
        }
        if (node->kind == NODE_BOL || node->kind == NODE_EOL) {
            fail(p, t.pos, "quantifier applied to an anchor");
            ast_free(node);
            return NULL;
        }
        lex_advance(&p->lex, &t);

        AstNode *q = new_node(p, k, t.pos);
        if (!q) {
            ast_free(node);
            return NULL;
        }
        if (k == NODE_REPEAT && !parse_interval(p, t.pos, &q->min, &q->max)) {
            ast_free(q);
            ast_free(node);
            return NULL;
        }
        if (ast_add_kid(q, node) != 0) {
            ast_free(q);
            ast_free(node);
            fail(p, t.pos, "out of memory");
            return NULL;
        }
        node = q;
        after_quantifier = true;
    }
    return node;
}

/* ---- concat := repeat+ ---- */

static AstNode *parse_concat(Parser *p)
{
    Token t = lex_peek(&p->lex);
    if (t.kind == TK_PIPE || t.kind == TK_RPAREN || t.kind == TK_END) {
        fail(p, t.pos, t.kind == TK_END && t.pos == 0 ? "empty pattern"
                                                       : "empty alternative or group");
        return NULL;
    }
    AstNode *cat = new_node(p, NODE_CONCAT, t.pos);
    if (!cat)
        return NULL;
    for (;;) {
        t = lex_peek(&p->lex);
        if (t.kind == TK_PIPE || t.kind == TK_RPAREN || t.kind == TK_END)
            break;
        AstNode *r = parse_repeat(p);
        if (!r || !add_kid(p, cat, r)) {
            ast_free(cat);
            return NULL;
        }
    }
    if (cat->nkids == 1) {                           /* no wrapper for a single item */
        AstNode *only = cat->kids[0];
        cat->nkids = 0;
        ast_free(cat);
        return only;
    }
    return cat;
}

/* ---- alt := concat ( '|' concat )* ---- */

static AstNode *parse_alt(Parser *p)
{
    AstNode *first = parse_concat(p);
    if (!first)
        return NULL;
    Token t = lex_peek(&p->lex);
    if (t.kind != TK_PIPE)
        return first;

    AstNode *alt = new_node(p, NODE_ALT, first->pos);
    if (!alt || !add_kid(p, alt, first)) {
        if (alt)
            ast_free(alt);
        else
            ast_free(first);
        return NULL;
    }
    while ((t = lex_peek(&p->lex)).kind == TK_PIPE) {
        lex_advance(&p->lex, &t);
        AstNode *next = parse_concat(p);
        if (!next || !add_kid(p, alt, next)) {
            ast_free(alt);
            return NULL;
        }
    }
    return alt;
}

/* ---- anchor placement ----
 *
 * '^' and '$' are zero-width assertions (true only at offset 0 / at the end
 * of the line). §3.1 allows them only "at rule start" / "at rule end"; we read
 * that as: no byte can have been consumed on any path before a '^', and none
 * can be consumed on any path after a '$'. That accepts "^a", "(^|[^0-9])x",
 * "x([^0-9]|$)" and rejects "a^b", "a$b", "(^a)*". */

/* `before`: could a byte have been consumed before this node is entered?
 * Returns false (after fail()) on a misplaced '^'. */
static bool check_bol(Parser *p, const AstNode *n, bool before)
{
    switch (n->kind) {
    case NODE_BOL:
        if (before) {
            fail(p, n->pos, "'^' is only allowed at the start of the pattern");
            return false;
        }
        return true;
    case NODE_CONCAT:
        for (int i = 0; i < n->nkids; i++) {
            if (!check_bol(p, n->kids[i], before))
                return false;
            before = before || ast_can_consume(n->kids[i]);
        }
        return true;
    case NODE_ALT:
        for (int i = 0; i < n->nkids; i++)
            if (!check_bol(p, n->kids[i], before))
                return false;
        return true;
    case NODE_STAR:
    case NODE_PLUS:
    case NODE_REPEAT:
        /* a second iteration starts after the first one consumed bytes */
        return check_bol(p, n->kids[0], before || ast_can_consume(n->kids[0]));
    case NODE_QUEST:
        return check_bol(p, n->kids[0], before);
    default:
        return true;
    }
}

/* Mirror image: `after` = could a byte be consumed after this node? */
static bool check_eol(Parser *p, const AstNode *n, bool after)
{
    switch (n->kind) {
    case NODE_EOL:
        if (after) {
            fail(p, n->pos, "'$' is only allowed at the end of the pattern");
            return false;
        }
        return true;
    case NODE_CONCAT:
        for (int i = n->nkids - 1; i >= 0; i--) {
            if (!check_eol(p, n->kids[i], after))
                return false;
            after = after || ast_can_consume(n->kids[i]);
        }
        return true;
    case NODE_ALT:
        for (int i = 0; i < n->nkids; i++)
            if (!check_eol(p, n->kids[i], after))
                return false;
        return true;
    case NODE_STAR:
    case NODE_PLUS:
    case NODE_REPEAT:
        return check_eol(p, n->kids[0], after || ast_can_consume(n->kids[0]));
    case NODE_QUEST:
        return check_eol(p, n->kids[0], after);
    default:
        return true;
    }
}

/* ---- regex := alt END ---- */

AstNode *regex_parse(const char *pattern, size_t len, bool icase, RegexError *err)
{
    Parser p;
    memset(&p, 0, sizeof p);
    lex_init(&p.lex, pattern, len);
    p.icase = icase;
    p.err = err;
    err->pos = 0;
    err->msg[0] = '\0';

    AstNode *root = parse_alt(&p);
    if (!root)
        return NULL;

    Token t = lex_peek(&p.lex);
    if (t.kind != TK_END) {
        /* parse_alt only stops early at ')' (or after an error) */
        fail(&p, t.pos, t.kind == TK_RPAREN ? "unmatched ')'" : (t.kind == TK_ERROR ? t.msg : "unexpected character"));
        ast_free(root);
        return NULL;
    }
    if (!check_bol(&p, root, false) || !check_eol(&p, root, false)) {
        ast_free(root);
        return NULL;
    }
    return root;
}
