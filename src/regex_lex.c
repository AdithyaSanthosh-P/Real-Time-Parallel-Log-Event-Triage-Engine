#include "regex_lex.h"

#include <stdio.h>
#include <string.h>

void lex_init(Lexer *L, const char *s, size_t n)
{
    L->s = s;
    L->n = n;
    L->pos = 0;
}

int lex_peek_byte(const Lexer *L, size_t ahead)
{
    return L->pos + ahead < L->n ? (unsigned char)L->s[L->pos + ahead] : -1;
}

void lex_skip_byte(Lexer *L)
{
    if (L->pos < L->n)
        L->pos++;
}

void lex_advance(Lexer *L, const Token *t)
{
    L->pos = t->pos + t->len;
}

static Token make(TokKind k, size_t pos, size_t len)
{
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = k;
    t.pos = pos;
    t.len = len;
    return t;
}

/* Escapes allowed by §3.1: a backslash before a metacharacter makes it
 * literal. ')' ']' '}' are included so they can be written literally too. */
static const char ESCAPABLE[] = ".()[]{}\\+?*|$^";

/* Lex "\x" at `pos`. */
static Token lex_escape(const Lexer *L, size_t pos)
{
    if (pos + 1 >= L->n) {
        Token t = make(TK_ERROR, pos, 1);
        snprintf(t.msg, sizeof t.msg, "trailing backslash");
        return t;
    }
    unsigned char c = (unsigned char)L->s[pos + 1];
    if (c != '\0' && strchr(ESCAPABLE, c)) {
        Token t = make(TK_CHAR, pos, 2);
        t.c = c;
        return t;
    }

    Token t = make(TK_ERROR, pos, 2);
    if (strchr("dDsSwW", c))
        snprintf(t.msg, sizeof t.msg, "\\%c shorthand is not supported; write a bracket expression such as [0-9]", c);
    else if (c == 'b' || c == 'B')
        snprintf(t.msg, sizeof t.msg, "\\%c word boundary is not supported; write an explicit boundary such as ([^0-9]|$)", c);
    else if (c >= '0' && c <= '9')
        snprintf(t.msg, sizeof t.msg, "backreference \\%c is not supported (not a regular language)", c);
    else if (c >= 0x21 && c < 0x7f)
        snprintf(t.msg, sizeof t.msg, "unsupported escape \\%c", c);
    else
        snprintf(t.msg, sizeof t.msg, "unsupported escape \\x%02x", c);
    return t;
}

Token lex_peek(const Lexer *L)
{
    size_t p = L->pos;
    if (p >= L->n)
        return make(TK_END, p, 0);

    unsigned char c = (unsigned char)L->s[p];
    switch (c) {
    case '\\': return lex_escape(L, p);
    case '.':  return make(TK_DOT, p, 1);
    case '*':  return make(TK_STAR, p, 1);
    case '+':  return make(TK_PLUS, p, 1);
    case '?':  return make(TK_QUEST, p, 1);
    case '{':  return make(TK_LBRACE, p, 1);
    case '|':  return make(TK_PIPE, p, 1);
    case ')':  return make(TK_RPAREN, p, 1);
    case '[':  return make(TK_LBRACKET, p, 1);
    case '^':  return make(TK_CARET, p, 1);
    case '$':  return make(TK_DOLLAR, p, 1);
    case '(':
        if (p + 1 < L->n && L->s[p + 1] == '?') {
            Token t = make(TK_ERROR, p, 2);
            snprintf(t.msg, sizeof t.msg, "\"(?\" groups (lookaround, non-capturing, flags) are not supported");
            return t;
        }
        return make(TK_LPAREN, p, 1);
    case ']':
    case '}': {
        Token t = make(TK_ERROR, p, 1);
        snprintf(t.msg, sizeof t.msg, "unmatched '%c' (write \\%c for a literal)", c, c);
        return t;
    }
    default: {
        Token t = make(TK_CHAR, p, 1);
        t.c = c;
        return t;
    }
    }
}
